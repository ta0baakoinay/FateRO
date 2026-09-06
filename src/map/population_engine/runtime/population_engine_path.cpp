// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// Population wander AI and pathfinding.

#include "population_engine_path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <vector>

#include <common/random.hpp>
#include <common/showmsg.hpp>
#include <common/timer.hpp>

#include "../../battle.hpp"
#include "../../chat.hpp"
#include "../../clif.hpp"
#include "../../map.hpp"
#include "../../autocombat.hpp"
#include "../../party.hpp"
#include "../../path.hpp"
#include "../../pc.hpp"
#include "../../population_engine.hpp"
#include "../config/population_config.hpp"
#include "../config/population_yaml_types.hpp"
#include "../core/population_engine_core.hpp"
#include "population_engine_combat.hpp"
#include "../../status.hpp"
#include "../../unit.hpp"

namespace {

std::unordered_map<int32, t_tick> g_pop_wander_next_tick;
int32 g_pop_wander_timer = INVALID_TIMER;

// ---------------------------------------------------------------------------
// Wander tick instrumentation.
//
// Single-threaded (main map thread) â€” no atomics needed.  Sliding window over
// the last 60 wander ticks (=30s @ 500ms tick) to smooth per-tick noise while
// still reflecting recent behavior after a config / shell-count change.
// ---------------------------------------------------------------------------
constexpr size_t kTimingWindow = 60;

struct TickSample {
    uint64_t total_us;
    uint64_t freecell_us;
    uint64_t fallback_us;
    uint64_t unit_walktoxy_us;
    uint32_t processed;
    uint32_t examined;
};

std::array<TickSample, kTimingWindow> g_timing_window{};
size_t                                g_timing_cursor      = 0; // next slot to overwrite
size_t                                g_timing_filled      = 0; // entries written so far (capped at kTimingWindow)
uint64_t                              g_timing_total_ticks = 0;

uint64_t g_timing_worst_total    = 0;
uint32_t g_timing_worst_processed = 0;

uint64_t g_timing_freecell_calls      = 0;
uint64_t g_timing_fallback_calls      = 0;
uint64_t g_timing_unit_walktoxy_calls = 0;

inline uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
}

inline void timing_record_tick(const TickSample &s) {
    g_timing_window[g_timing_cursor] = s;
    g_timing_cursor = (g_timing_cursor + 1) % kTimingWindow;
    if (g_timing_filled < kTimingWindow)
        ++g_timing_filled;
    ++g_timing_total_ticks;
    if (s.total_us > g_timing_worst_total) {
        g_timing_worst_total     = s.total_us;
        g_timing_worst_processed = s.processed;
    }
}

} // namespace

void population_engine_path_erase_for_pc(int32 id)
{
	g_pop_wander_next_tick.erase(id);
}

void population_engine_path_clear_all()
{
	g_pop_wander_next_tick.clear();
}

void population_engine_path_register_wander_state(map_session_data *sd)
{
	if (!sd || !battle_config.population_engine_wander_enable)
		return;
	const int32 j = battle_config.population_engine_wander_cooldown_jitter_ms;
	const t_tick when = gettick() + (j > 0 ? static_cast<t_tick>(rnd() % (static_cast<uint32_t>(j) + 1u)) : 0);
	g_pop_wander_next_tick[sd->id] = when;
}

void population_engine_path_register_timer_funcs()
{
	add_timer_func_list(population_engine_wander_timer, "population_engine_wander_timer");
}

void population_engine_path_restart_wander_timer()
{
	if (g_pop_wander_timer != INVALID_TIMER) {
		const TimerData *tdw = get_timer(g_pop_wander_timer);
		if (tdw && tdw->func == population_engine_wander_timer)
			delete_timer(g_pop_wander_timer, population_engine_wander_timer);
		g_pop_wander_timer = INVALID_TIMER;
	}
	if (battle_config.population_engine_wander_enable) {
		int32 wtick = battle_config.population_engine_wander_tick_ms;
		if (wtick < 200)
			wtick = 200;
		g_pop_wander_timer = add_timer_interval(gettick() + wtick, population_engine_wander_timer, 0, 0, wtick);
	}
}

void population_engine_path_stop_wander_timer()
{
	if (g_pop_wander_timer != INVALID_TIMER) {
		const TimerData *tdw = get_timer(g_pop_wander_timer);
		if (tdw && tdw->func == population_engine_wander_timer)
			delete_timer(g_pop_wander_timer, population_engine_wander_timer);
		g_pop_wander_timer = INVALID_TIMER;
	}
}

// ---------------------------------------------------------------------------
// Proactive floor-loot scan.  Every fake player (regardless of class) collects
// nearby floor drops, not just items from its own kills (autoloot covers those).
//   * anything within pickup range: grabbed instantly, even mid-combat
//   * a genuinely FREE drop within a few cells while idle: walk over and take it
// pc_takeitem() enforces rAthena's ownership / loot-priority rules, so a real
// player's exclusive drop is skipped rather than endlessly chased.  Weight does
// not accumulate for shells (see pc_additem / status_calc_pc).
// ---------------------------------------------------------------------------
static int population_loot_pick_cb(block_list *bl, va_list ap)
{
	flooritem_data *fitem = reinterpret_cast<flooritem_data*>(bl);
	map_session_data *sd = va_arg(ap, map_session_data*);
	int *taken = va_arg(ap, int*);
	if (fitem != nullptr && sd != nullptr && pc_takeitem(sd, fitem))
		++(*taken);
	return 0;
}

struct s_pop_loot_near { map_session_data *sd; flooritem_data *best; int bestd; };

static int population_loot_near_cb(block_list *bl, va_list ap)
{
	flooritem_data *fitem = reinterpret_cast<flooritem_data*>(bl);
	s_pop_loot_near *c = va_arg(ap, s_pop_loot_near*);
	if (fitem == nullptr || c == nullptr)
		return 0;
	// Only chase genuinely free drops (unowned, or already ours). Never contest
	// a drop still reserved to another character.
	if (fitem->first_get_charid != 0 && fitem->first_get_charid != c->sd->status.char_id)
		return 0;
	const int dx = std::abs(static_cast<int>(fitem->x) - static_cast<int>(c->sd->x));
	const int dy = std::abs(static_cast<int>(fitem->y) - static_cast<int>(c->sd->y));
	const int d = std::max(dx, dy);
	if (d < c->bestd) {
		c->bestd = d;
		c->best  = fitem;
	}
	return 0;
}

static void population_shell_loot_scan(map_session_data *sd, t_tick now)
{
	if (sd == nullptr || sd->prev == nullptr || pc_isdead(sd))
		return;
	if (now < sd->pop.next_loot_scan)
		return;
	sd->pop.next_loot_scan = now + 700;

	// 1) Instant pickup of anything already in range — runs even during combat.
	int taken = 0;
	map_foreachinallrange(population_loot_pick_cb, sd, 3, BL_ITEM, sd, &taken);

	// 2) While idle (no combat target, not walking) head for the nearest free drop.
	if (sd->pop.target_id != 0) {
		sd->pop.loot_walk_target = 0;
		return;
	}
	unit_data *ud = unit_bl2ud(sd);
	if (ud == nullptr || ud->walktimer != INVALID_TIMER)
		return;

	s_pop_loot_near ctx{ sd, nullptr, 9 };
	map_foreachinallrange(population_loot_near_cb, sd, 8, BL_ITEM, &ctx);
	if (ctx.best == nullptr) {
		sd->pop.loot_walk_target = 0;
		return;
	}
	if (unit_walktoxy(sd, ctx.best->x, ctx.best->y, 1)) {
		sd->pop.loot_walk_target = ctx.best->id;
		g_pop_wander_next_tick[sd->id] = now + 1200;
	} else {
		sd->pop.loot_walk_target = 0;
	}
}

// ---------------------------------------------------------------------------
// Pub / social room support.  A "Social" town shell walks to a configured pub
// (db/population_pubs.yml) and the first to arrive opens a REAL public chatroom
// via rAthena's chat_createpcchat() (the same call Alt+C makes); the rest join
// it with chat_joinchat() and sit.  Teardown is automatic: unit_free() already
// calls chat_leavechat() when a shell despawns, and if that shell owned the room
// rAthena destroys it and kicks the members — the registry then self-heals as a
// remaining member becomes the new owner on its next tick.
// ---------------------------------------------------------------------------
static std::unordered_map<uint32_t, int32> g_pub_owner_id; ///< pub entry id -> owner shell block id.

/// Returns true if the shell's turn was consumed by pub behaviour (walking to a
/// pub, or seated inside one). false = no pub applies, fall through to normal
/// social wander.
static bool population_pub_try(map_session_data *sd, t_tick now)
{
	(void)now;
	// Already inside a chatroom -> stay put, keep seated.
	if (sd->chatID != 0) {
		if (unit_can_move(sd) && !pc_issit(sd)) {
			pc_setsit(sd);
			clif_sitting(*sd);
		}
		return true;
	}

	// Collect pubs on this map.
	const PopulationPubEntry *best = nullptr;
	int best_dist = 0;
	for (auto it = population_pub_db().begin(); it != population_pub_db().end(); ++it) {
		const std::shared_ptr<PopulationPubEntry> &e = it->second;
		if (!e || map_mapname2mapid(e->map_name.c_str()) != sd->m)
			continue;
		if (!map_getcell(sd->m, e->x, e->y, CELL_CHKPASS))
			continue; // configured spot is unwalkable — skip it
		// Skip pubs whose room already exists and is full.
		auto oit = g_pub_owner_id.find(e->id);
		if (oit != g_pub_owner_id.end()) {
			map_session_data *osd = map_id2sd(oit->second);
			if (osd != nullptr && osd->chatID != 0) {
				chat_data *cd = reinterpret_cast<chat_data*>(map_id2bl(osd->chatID));
				if (cd != nullptr && cd->users >= cd->limit)
					continue;
			}
		}
		const int d = distance_blxy(sd, e->x, e->y);
		if (best == nullptr || d < best_dist) { best = e.get(); best_dist = d; }
	}
	if (best == nullptr)
		return false; // no pub here — normal social behaviour

	// Not there yet: walk toward the pub spot.
	if (best_dist > 2) {
		if (unit_can_move(sd) && !unit_is_walking(sd)) {
			if (!unit_walktoxy(sd, best->x, best->y, 0))
				unit_walktoxy(sd, best->x, best->y, 1);
		}
		return true;
	}

	// At the pub. Find the current owner; if none/invalid, become the owner.
	map_session_data *owner_sd = nullptr;
	auto oit = g_pub_owner_id.find(best->id);
	if (oit != g_pub_owner_id.end()) {
		owner_sd = map_id2sd(oit->second);
		if (owner_sd != nullptr && (owner_sd->chatID == 0 || owner_sd->m != sd->m))
			owner_sd = nullptr;
	}

	if (owner_sd == nullptr) {
		unit_stop_walking(sd, USW_FIXPOS);
		if (pc_issit(sd)) { pc_setstand(sd, false); clif_standing(*sd); }
		chat_createpcchat(sd, best->title.c_str(), "", best->limit, true);
		if (sd->chatID != 0)
			g_pub_owner_id[best->id] = sd->id;
		return true;
	}

	// Join the existing room and sit.
	chat_joinchat(sd, owner_sd->chatID, "");
	if (sd->chatID != 0 && unit_can_move(sd) && !pc_issit(sd)) {
		pc_setsit(sd);
		clif_sitting(*sd);
	}
	return true;
}

TIMER_FUNC(population_engine_wander_timer)
{
	if (!battle_config.population_engine_wander_enable)
		return 0;

	const uint64_t t_tick_start = now_us();
	uint64_t acc_freecell_us      = 0;
	uint64_t acc_fallback_us      = 0;
	uint64_t acc_unit_walktoxy_us = 0;
	uint32_t examined_count       = 0;

	const t_tick now = gettick();
	const int max_bots = battle_config.population_engine_wander_max_per_tick;
	const int32 base_cd = battle_config.population_engine_wander_cooldown_ms;
	const int32 jit = battle_config.population_engine_wander_cooldown_jitter_ms;
	const int16 radius = static_cast<int16>(battle_config.population_engine_wander_radius);
	int processed = 0;

	{
		auto stale = population_engine_collect_stale_shells();
		for (auto *s : stale)
			population_engine_shell_release(s);
	}

	// Cursor-based iteration: instead of scanning all N shells every 500ms tick,
	// examine at most max_bots*20 shells and advance a cursor so subsequent ticks
	// cover the rest. At 5k shells and max_bots=50 the cursor rotates every ~5 ticks
	// (2.5s), keeping per-tick work bounded regardless of total shell count.
	static size_t s_wander_cursor = 0;
	const size_t n = g_population_engine_pcs.size();
	if (n == 0)
		return 0;
	if (s_wander_cursor >= n)
		s_wander_cursor = 0;

	const int max_iterate = std::min(static_cast<int>(n), std::max(max_bots * 20, 200));

	for (int ci = 0; ci < max_iterate; ++ci) {
		const size_t idx = (s_wander_cursor + static_cast<size_t>(ci)) % n;
		map_session_data *sd = g_population_engine_pcs[idx];
		++examined_count;

		if (sd == nullptr || !sd->state.active) {
			continue;
		}
		if (sd->prev == nullptr) {
			continue;
		}
		if (map_id2bl(sd->id) != sd) {
			continue;
		}

		// Cleanup stale target exclusions. The combat tick only fires near real players,
		// so shells on empty maps would never have their exclusion map cleaned otherwise.
		// Pruned BEFORE the dead-skip so corpses don't accumulate stale entries during
		// the respawn window.
		{
			auto &excl = sd->pop.recently_cleared_targets;
			for (auto eit = excl.begin(); eit != excl.end(); ) {
				if (DIFF_TICK(now, eit->second) >= PE_SHELL_TARGET_EXCLUSION_MS)
					eit = excl.erase(eit);
				else
					++eit;
			}
		}

		if (pc_isdead(sd)) {
			// Dead shells are owned by population_engine_respawn_shell_timer (5 s warp+revive
			// at spawn point). Do NOT revive in-place here â€” it would race the scheduled
			// respawn, leaving stale unit_data and skipping the warp.
			continue;
		}

		// Fake party leaders support their party members via the existing
		// AutoSupport system (heal low HP / keep buffs up). Internally gated to
		// fake-party leaders only; paced so it is not a per-tick bot.
		// (Placed before the ac_ok gate so a non-combat support leader still ticks.)
		if (party_id_is_fake(sd->status.party_id))
			autocombat_support_party(sd, now);

		// Fake-party cohesion (idle path): a non-combat member that has drifted
		// far from its leader walks back so support skills can actually reach it.
		// The detailed spacing / anti-stack pass still runs later for
		// combat-active members. Throttled via g_pop_wander_next_tick.
		if (party_id_is_fake(sd->status.party_id) && sd->pop.target_id == 0) {
			unit_data *cud = unit_bl2ud(sd);
			t_tick cnext = 0;
			if (auto itc = g_pop_wander_next_tick.find(sd->id); itc != g_pop_wander_next_tick.end())
				cnext = itc->second;
			if (cud != nullptr && cud->walktimer == INVALID_TIMER && now >= cnext && !pc_issit(sd)) {
				struct party_data *fp = party_search(sd->status.party_id);
				map_session_data *lead = (fp != nullptr) ? fp->data[0].sd : nullptr;
				if (lead != nullptr && lead != sd && lead->prev != nullptr && lead->m == sd->m) {
					const int ldx = std::abs(static_cast<int>(lead->x) - static_cast<int>(sd->x));
					const int ldy = std::abs(static_cast<int>(lead->y) - static_cast<int>(sd->y));
					if (std::max(ldx, ldy) > 7 && unit_can_move(sd)) {
						int16 nx = lead->x, ny = lead->y;
						if (map_search_freecell(lead, lead->m, &nx, &ny, 4, 4, 1 | 2, 20) &&
						    (nx != sd->x || ny != sd->y))
							unit_walktoxy(sd, nx, ny, 1);
						g_pop_wander_next_tick[sd->id] = now + 1500;
						processed++;
						continue;
					}
				}
			}
		}

		if (!population_engine_combat_shell_ac_ok(sd))
			continue;

		// Proactive loot: instant grab underfoot always; walk to a free drop when idle.
		population_shell_loot_scan(sd, now);

		if (processed >= max_bots)
			break;

		t_tick next = 0;
		{
			auto itn = g_pop_wander_next_tick.find(sd->id);
			if (itn != g_pop_wander_next_tick.end())
				next = itn->second;
		}
		if (now < next) {
			continue;
		}

		unit_data *ud = unit_bl2ud(sd);
		if (ud == nullptr || ud->walktimer != INVALID_TIMER) {
			continue;
		}

		if (sd->pop.target_id != 0)
			continue;

		// --- Town personality: Sit / Vendor shells do not wander -------------
		// Cycle a "sitter" between seated (minutes) and a short stroll on a
		// per-shell randomized timer so a townful of shells is a natural mix of
		// seated + standing + strolling, never synchronized.
		{
			const uint8_t beh_now = sd->pop.behavior;
			const uint8_t SIT     = static_cast<uint8_t>(PopulationBehavior::Sit);
			const uint8_t VENDOR  = static_cast<uint8_t>(PopulationBehavior::Vendor);
			const uint8_t SOCIAL  = static_cast<uint8_t>(PopulationBehavior::Social);
			// A "cycler" is a Sit-personality or Social-personality town shell:
			// it alternates seated (minutes) <-> short stroll (Wander for a Sit
			// base, Social for a Social base). Timer is per-shell randomised.
			const bool is_cycler = (sd->pop.behavior_base == SIT || sd->pop.behavior_base == SOCIAL);
			if (sd->pop.map_category == 1 && sd->pop.town_phase_until != 0 && is_cycler
			    && sd->chatID == 0   // not currently sitting inside a pub chatroom
			    && DIFF_TICK(now, sd->pop.town_phase_until) >= 0) {
				if (beh_now == SIT) {
					// Stand up and take a stroll (in its base activity).
					if (pc_issit(sd)) { pc_setstand(sd, false); clif_standing(*sd); }
					sd->pop.behavior = sd->pop.behavior_base; // Wander or Social
					sd->pop.town_phase_until = now + static_cast<t_tick>(40000u + rnd() % 170000u);
				} else {
					// Sit back down for a while.
					if (unit_can_move(sd) && !pc_issit(sd)) { pc_setsit(sd); clif_sitting(*sd); }
					sd->pop.behavior = SIT;
					sd->pop.town_phase_until = now + static_cast<t_tick>(90000u + rnd() % 450000u);
				}
			}
			if (sd->pop.behavior == SIT || sd->pop.behavior == VENDOR) {
				g_pop_wander_next_tick[sd->id] = now + 2000;
				processed++;
				continue; // seated / vending — no wandering
			}
			// Social town shells: try to use a configured named pub (real chatroom).
			if (sd->pop.behavior == static_cast<uint8_t>(PopulationBehavior::Social)
			    && population_pub_db().size() > 0
			    && population_pub_try(sd, now)) {
				g_pop_wander_next_tick[sd->id] = now + 2500;
				processed++;
				continue;
			}
		}

		// Reset stale canmove_tick lock â€” wander bots don't go through the combat per-tick
		// path that resets canmove_tick every 3 s, so a lock set during spawn or by a
		// status effect that has since expired would strand the bot forever.
		if (DIFF_TICK(ud->canmove_tick, gettick()) > 0)
			ud->canmove_tick = gettick();

		if (!unit_can_move(sd)) {
			g_pop_wander_next_tick[sd->id] = now + 500;
			processed++;
			continue;
		}

		// --- Stuck detection & recovery ---------------------------------------
		// If a shell's position has not changed for a long time despite being
		// eligible to move, it is boxed in (e.g. spawned in an isolated
		// CELL_CHKPASS pocket). Relocate it to a nearby REACHABLE free cell.
		// Only when genuinely stuck, on a 30 s per-shell cooldown, capped at 5
		// consecutive recoveries — never a constant teleport.
		{
			constexpr t_tick STUCK_MS = 9000;
			constexpr t_tick RELOC_CD = 30000;
			s_population &pe_s = sd->pop;
			if (sd->x == pe_s.stuck_x && sd->y == pe_s.stuck_y) {
				if (pe_s.stuck_since == 0)
					pe_s.stuck_since = now;
				if (pe_s.stuck_since != 0 && DIFF_TICK(now, pe_s.stuck_since) >= STUCK_MS
				    && DIFF_TICK(now, pe_s.last_reloc_tick) >= RELOC_CD
				    && pe_s.reloc_count < 5) {
					unit_stop_walking(sd, USW_FIXPOS);
					int16 nx = sd->x, ny = sd->y;
					bool recovered = false;
					// 1) prefer a cell the shell can actually walk a path to.
					for (int16 r = 5; r <= 17 && !recovered; r = static_cast<int16>(r + 6)) {
						nx = sd->x; ny = sd->y;
						if (map_search_freecell(sd, sd->m, &nx, &ny, r, r, 1 | 2, 30)
						    && (nx != sd->x || ny != sd->y)
						    && map_getcell(sd->m, nx, ny, CELL_CHKPASS)) {
							if (unit_walktoxy(sd, nx, ny, 1))
								recovered = true;
						}
					}
					// 2) truly boxed in: snap to a valid nearby cell (last resort).
					if (!recovered) {
						nx = sd->x; ny = sd->y;
						if (map_search_freecell(sd, sd->m, &nx, &ny, 12, 12, 1, 40)
						    && (nx != sd->x || ny != sd->y)
						    && map_getcell(sd->m, nx, ny, CELL_CHKPASS)) {
							recovered = unit_movepos(sd, nx, ny, 0, false);
						}
					}
					if (map_id2bl(sd->id) != sd)
						continue; // freed via a clif viewer side-effect during the move
					pe_s.last_reloc_tick = now;
					pe_s.reloc_count     = static_cast<uint8_t>(pe_s.reloc_count + 1);
					pe_s.stuck_since     = 0;
					pe_s.stuck_x         = sd->x;
					pe_s.stuck_y         = sd->y;
					if (!recovered)
						population_engine_stats_record_walk_failure();
					g_pop_wander_next_tick[sd->id] = now + 1500;
					processed++;
					continue;
				}
			} else {
				// Moved since the last check — reset the stuck tracker.
				pe_s.stuck_x     = sd->x;
				pe_s.stuck_y     = sd->y;
				pe_s.stuck_since = 0;
				if (pe_s.reloc_count > 0 && DIFF_TICK(now, pe_s.last_reloc_tick) > 60000)
					pe_s.reloc_count = 0; // recovered and stable — clear the cap
			}
		}

		// --- Fake-party cohesion / spacing --------------------------------------
		// Members of a map-server-local support party stay near their leader and
		// never stand on top of each other. Reuses map_search_freecell +
		// unit_walktoxy; no bespoke follow AI.
		if (party_id_is_fake(sd->status.party_id) && sd->pop.target_id == 0) {
			struct party_data *fp = party_search(sd->status.party_id);
			map_session_data *lead = (fp != nullptr) ? fp->data[0].sd : nullptr;
			if (lead != nullptr && lead != sd && lead->m == sd->m) {
				const int ldx = std::abs(static_cast<int>(lead->x) - static_cast<int>(sd->x));
				const int ldy = std::abs(static_cast<int>(lead->y) - static_cast<int>(sd->y));
				const int ld  = std::max(ldx, ldy);
				if (ld > 7) {
					int16 nx = lead->x, ny = lead->y;
					if (map_search_freecell(lead, lead->m, &nx, &ny, 3, 3, 1 | 2, 20) &&
					    (nx != sd->x || ny != sd->y))
						unit_walktoxy(sd, nx, ny, 1);
					g_pop_wander_next_tick[sd->id] = now + 1500;
					processed++;
					continue;
				}
				if (ld <= 1) { // stacked on the leader — take one spacing step
					int16 nx = sd->x, ny = sd->y;
					if (map_search_freecell(sd, sd->m, &nx, &ny, 3, 3, 1, 20) &&
					    (nx != sd->x || ny != sd->y))
						unit_walktoxy(sd, nx, ny, 1);
					g_pop_wander_next_tick[sd->id] = now + 1800;
					processed++;
					continue;
				}
			}
		}

		// Combat-style progressive-vector wander pick.
		//
		// Old algorithm (map_search_freecell + path-validation fallback) failed badly
		// in dense towns: freecell returned random walkable cells without checking
		// reachability, then unit_walktoxy rejected most of them because the A*
		// detour exceeded max_walk_path (=17). Bots ended up making 1-cell shuffles
		// or standing still.
		//
		// New algorithm matches population_shell_try_roam_step (combat roam):
		// pick a random direction vector, then try full â†’ half â†’ quarter scale,
		// then perpendicular half, then 8 adjacents. Each candidate is fed straight
		// to unit_walktoxy(flag=0|1), which is the actual reachability oracle.
		// Stops at the first walk that succeeds.
		const int32  aid_sync = sd->id;
		const int    walk_cap = std::max<int>(1, std::min<int>(static_cast<int>(radius),
		                                                        battle_config.max_walk_path - 5));
		bool sync_walk_ok = false;

		auto try_walk_at = [&](int tx, int ty) -> bool {
			if (tx == sd->x && ty == sd->y)
				return false;
			if (tx < 0 || ty < 0)
				return false;
			if (!map_getcell(sd->m, static_cast<int16>(tx), static_cast<int16>(ty), CELL_CHKPASS))
				return false;
			const uint64_t t_uw0 = now_us();
			const bool ok = (unit_walktoxy(sd, static_cast<int16>(tx), static_cast<int16>(ty), 0) ||
			                 unit_walktoxy(sd, static_cast<int16>(tx), static_cast<int16>(ty), 1)) != 0;
			acc_unit_walktoxy_us += (now_us() - t_uw0);
			++g_timing_unit_walktoxy_calls;
			return ok;
		};

		// 1. Random direction vector.
		int dx = (static_cast<int>(rnd()) % (walk_cap * 2 + 1)) - walk_cap;
		int dy = (static_cast<int>(rnd()) % (walk_cap * 2 + 1)) - walk_cap;

		// 2. Progressive scales: full â†’ half â†’ quarter.
		for (int scale = 1; scale <= 4 && !sync_walk_ok; scale *= 2) {
			const int tdx = dx / scale;
			const int tdy = dy / scale;
			if (tdx == 0 && tdy == 0)
				break;
			if (try_walk_at(sd->x + tdx, sd->y + tdy))
				sync_walk_ok = true;
		}

		// 3. Perpendicular half-scale step (sideways escape).
		if (!sync_walk_ok && (dx != 0 || dy != 0)) {
			const int half = std::max(1, walk_cap / 2);
			const int p1x = std::clamp(-dy, -half, half);
			const int p1y = std::clamp( dx, -half, half);
			const int p2x = std::clamp( dy, -half, half);
			const int p2y = std::clamp(-dx, -half, half);
			if ((p1x != 0 || p1y != 0) && try_walk_at(sd->x + p1x, sd->y + p1y))
				sync_walk_ok = true;
			else if ((p2x != 0 || p2y != 0) && try_walk_at(sd->x + p2x, sd->y + p2y))
				sync_walk_ok = true;
		}

		// 4. Last resort: 8 adjacent cells in a randomized order.
		if (!sync_walk_ok) {
			static const int step_offsets[8][2] = {
				{ 1, 0}, {-1, 0}, {0,  1}, {0, -1},
				{ 1, 1}, { 1,-1}, {-1, 1}, {-1,-1},
			};
			const int start = static_cast<int>(rnd()) & 7;
			for (int i = 0; i < 8 && !sync_walk_ok; ++i) {
				const int idxs = (start + i) & 7;
				if (try_walk_at(sd->x + step_offsets[idxs][0], sd->y + step_offsets[idxs][1]))
					sync_walk_ok = true;
			}
		}

		// Shell may have despawned during walk emission (clif viewer side-effects).
		if (map_id2bl(aid_sync) == nullptr)
			continue;

		const t_tick add = static_cast<t_tick>(base_cd + (jit > 0 ? static_cast<int32>(rnd() % (static_cast<uint32_t>(jit) + 1u)) : 0));
		if (!sync_walk_ok) {
			population_engine_stats_record_walk_failure();
			g_pop_wander_next_tick[aid_sync] = now + 500;
			processed++;
			continue;
		}

		g_pop_wander_next_tick[aid_sync] = now + add;
		processed++;
	}
	// Advance cursor so the next tick covers the next segment of shells.
	s_wander_cursor = (s_wander_cursor + static_cast<size_t>(max_iterate)) % n;

	// Record this tick's sample.
	TickSample sample;
	sample.total_us         = now_us() - t_tick_start;
	sample.freecell_us      = acc_freecell_us;
	sample.fallback_us      = acc_fallback_us;
	sample.unit_walktoxy_us = acc_unit_walktoxy_us;
	sample.processed        = static_cast<uint32_t>(processed);
	sample.examined         = examined_count;
	timing_record_tick(sample);
	return 0;
}

WanderTimingSnapshot population_engine_path_get_timing_snapshot()
{
	WanderTimingSnapshot s{};
	s.ticks_recorded = g_timing_total_ticks;
	s.window_size    = g_timing_filled;

	if (g_timing_filled > 0) {
		uint64_t sum_total = 0, sum_fc = 0, sum_fb = 0, sum_uw = 0;
		uint64_t sum_proc = 0, sum_exam = 0;
		for (size_t i = 0; i < g_timing_filled; ++i) {
			const auto &t = g_timing_window[i];
			sum_total += t.total_us;
			sum_fc    += t.freecell_us;
			sum_fb    += t.fallback_us;
			sum_uw    += t.unit_walktoxy_us;
			sum_proc  += t.processed;
			sum_exam  += t.examined;
		}
		const double inv_n = 1.0 / static_cast<double>(g_timing_filled);
		s.avg_total_us         = static_cast<double>(sum_total) * inv_n;
		s.avg_freecell_us      = static_cast<double>(sum_fc)    * inv_n;
		s.avg_fallback_us      = static_cast<double>(sum_fb)    * inv_n;
		s.avg_unit_walktoxy_us = static_cast<double>(sum_uw)    * inv_n;
		s.avg_processed        = static_cast<double>(sum_proc)  * inv_n;
		s.avg_examined         = static_cast<double>(sum_exam)  * inv_n;

		// Most recent slot = (cursor - 1) mod window.
		const size_t last_idx = (g_timing_cursor + kTimingWindow - 1) % kTimingWindow;
		const auto  &last     = g_timing_window[last_idx];
		s.last_total_us         = last.total_us;
		s.last_freecell_us      = last.freecell_us;
		s.last_fallback_us      = last.fallback_us;
		s.last_unit_walktoxy_us = last.unit_walktoxy_us;
		s.last_processed        = last.processed;
		s.last_examined         = last.examined;
	}

	s.worst_total_us         = g_timing_worst_total;
	s.worst_processed        = g_timing_worst_processed;
	s.freecell_calls         = g_timing_freecell_calls;
	s.fallback_calls         = g_timing_fallback_calls;
	s.unit_walktoxy_calls    = g_timing_unit_walktoxy_calls;
	return s;
}

void population_engine_path_reset_timing()
{
	g_timing_window.fill(TickSample{});
	g_timing_cursor              = 0;
	g_timing_filled              = 0;
	g_timing_total_ticks         = 0;
	g_timing_worst_total         = 0;
	g_timing_worst_processed     = 0;
	g_timing_freecell_calls      = 0;
	g_timing_fallback_calls      = 0;
	g_timing_unit_walktoxy_calls = 0;
}
