// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// Copyright (c) Louis T Steinhil - https://github.com/YlenXWalker
// For more information, see LICENCE in the main folder
//
// Population engine: spawns fake PC shells to fill maps with lifelike activity.
// Includes wander AI, combat AI, ambient chat, and YAML-driven equipment/skill profiles.

#include "population_engine.hpp"

#include "population_engine/runtime/population_engine_combat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <common/malloc.hpp>
#include <common/mapindex.hpp>
#include <common/mmo.hpp>
#include <common/random.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>
#include <common/utils.hpp>
#include "autocombat.hpp"
#include "battle.hpp"
#include "clif.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "npc.hpp"
#include "party.hpp"
#include "path.hpp"
#include "pc.hpp"
#include "pc_groups.hpp"
#include "script.hpp"
#include "skill.hpp"
#include "status.hpp"
#include "unit.hpp"
#include "vending.hpp"

#include "population_engine/config/population_config.hpp"
#include "population_engine/config/population_yaml_types.hpp"
#include "population_engine/core/population_engine_core.hpp"
#include "population_engine/runtime/population_engine_path.hpp"

// Unity-build: all submodule translation units compiled via the factory.
#include "population_engine/population_engine_factory.cpp"
#include "population_engine/core/pe_perf.hpp"

std::vector<map_session_data *> g_population_engine_pcs;

// ----------------------------------------------------------------------
// Dynamic vendor stock cache.
//
// Building a vendor's source-map list and walking every map's mob drop
// table on EVERY shell spawn was a measurable lag source (autosummon
// can spawn dozens of vendor shells per tick). The drop tables are
// effectively static at runtime, so we cache:
//   - the resolved source-map id list per vendor key
//   - a pre-sorted DropEntry pool per (vendor key, source map id)
// Cache is invalidated on vendor/spawn YAML reload.
// ----------------------------------------------------------------------
struct PopVendorDropEntry { t_itemid nameid; uint32_t rate; };
struct PopVendorCacheBucket {
	std::vector<int16> source_mids;
	std::unordered_map<int16, std::vector<PopVendorDropEntry>> drops_by_mid;
	bool source_mids_built = false;
};
static std::unordered_map<std::string, PopVendorCacheBucket> g_pop_vendor_dyn_cache;

void population_engine_vendor_dyn_cache_clear() {
	g_pop_vendor_dyn_cache.clear();
}

// Cache of jobs whose effective behavior is Vendor (any category override).
// Populated lazily by the autosummon timer; cleared on YAML reload.
static std::vector<uint16_t> g_pop_vendor_job_pool;
static bool                  g_pop_vendor_job_pool_built = false;
void population_engine_vendor_job_pool_clear() {
	g_pop_vendor_job_pool.clear();
	g_pop_vendor_job_pool_built = false;
}
static std::unordered_map<int32, t_tick> g_pop_chat_next_tick; ///< Per-shell next chat eligibility tick.
static int32 g_pop_chat_timer = INVALID_TIMER;
static int32 g_population_combat_global_timer = INVALID_TIMER;
static size_t g_chat_cursor = 0;    ///< Round-robin index for batched chat replies.
// Last count written to cp_population_stats; UINT32_MAX = never written.
static uint32_t g_last_db_written_count = UINT32_MAX;

static const std::string kPopulationChatProfileDefault("default");

/// If equipment omits ChatProfile:, use "default" from db/population_chat.yml (must exist).
static const std::string& population_engine_chat_profile_key(const PopulationEngine* eq)
{
	if (eq != nullptr && !eq->chat_profile.empty())
		return eq->chat_profile;
	return kPopulationChatProfileDefault;
}

static void population_engine_chat_replace_all(std::string& s, const char* needle, const std::string& repl) {
	const size_t nlen = strlen(needle);
	if (nlen == 0)
		return;
	for (size_t pos = 0; (pos = s.find(needle, pos)) != std::string::npos; pos += repl.size()) {
		s.replace(pos, nlen, repl);
	}
}

static void population_engine_format_chat_line(map_session_data* sd, const char* templ, char* out, size_t out_sz) {
	if (!out || out_sz == 0)
		return;
	out[0] = '\0';
	if (!templ || !*templ)
		return;
	std::string s(templ);
	if (sd) {
		population_engine_chat_replace_all(s, "{name}", std::string(sd->status.name));
		const char* mapn = map_mapid2mapname(sd->m);
		population_engine_chat_replace_all(s, "{map}", std::string(mapn && mapn[0] ? mapn : "?"));
		const char* jn = job_name(sd->status.class_);
		population_engine_chat_replace_all(s, "{job}", std::string(jn && jn[0] ? jn : "?"));
	}
	if (s.size() >= out_sz)
		s.resize(out_sz - 1);
	memcpy(out, s.c_str(), s.size() + 1);
}

/// Same format as player map chat (clif_process_message): "Name : message" so the client chat log shows the bot as speaker.
static void population_engine_send_public_chat_as_pc(map_session_data* bot_sd, const char* message_body)
{
	PE_PERF_SCOPE("chat.broadcast");
	if (!bot_sd || !message_body || !message_body[0] || !bot_sd->status.name[0])
		return;
	char line[CHAT_SIZE_MAX + NAME_LENGTH * 2];
	safesnprintf(line, sizeof(line), "%s : %s", bot_sd->status.name, message_body);
	clif_GlobalMessage(*bot_sd, line, AREA_CHAT_WOC);
}

/// FNV-1a hash of a chat line, used for the "don't say the same line twice in a
/// row" guard (population_shell_state.hpp: pop.last_chat_hash). Never returns 0.
static uint32_t population_engine_pop_line_hash(const std::string& s) {
	uint32_t h = 2166136261u;
	for (unsigned char c : s) { h ^= c; h *= 16777619u; }
	return h ? h : 1u;
}

static bool population_engine_chat_line_blocked(const char* msg) {
	if (!msg || !msg[0])
		return true;
	std::string lower(msg);
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });
	return population_yaml_name_hits_blocklist(lower);
}

static void population_engine_register_shell_chat_state(map_session_data* sd, const PopulationEngine* pop_cfg) {
	extern struct Battle_Config battle_config;
	if (!sd || !battle_config.population_engine_chat_enable)
		return;
	if (pop_cfg == nullptr)
		return;
	const std::vector<std::string>* pool = population_chat_db().pool_for_profile(population_engine_chat_profile_key(pop_cfg));
	if (pool == nullptr || pool->empty())
		return;
	const int32 j = battle_config.population_engine_chat_cooldown_jitter_ms;
	const t_tick when = gettick() + (j > 0 ? static_cast<t_tick>(rnd() % (static_cast<uint32_t>(j) + 1u)) : 0);
	g_pop_chat_next_tick[sd->id] = when;
}

static void population_engine_register_shell_wander_state(map_session_data *sd)
{
	population_engine_path_register_wander_state(sd);
}

static std::string generate_bot_name(uint32_t index); // defined later; used by population_roll_base_name_from_profile

static int16_t population_roll_closed_range(int16_t a, int16_t b)
{
	if (b < a)
		std::swap(a, b);
	const uint32_t span = static_cast<uint32_t>(b - a) + 1u;
	return static_cast<int16_t>(a + static_cast<int32_t>(rnd() % span));
}

static PopulationNameProfile::Strategy population_effective_name_strategy(const PopulationNameProfile* prof)
{
	if (!prof)
		return PopulationNameProfile::Strategy::BotIndex;
	if (prof->strategy != PopulationNameProfile::Strategy::None)
		return prof->strategy;

	const auto& g_syl0 = population_yaml_name_syl_start();
	const auto& g_sylm = population_yaml_name_syl_mid();
	const auto& g_syle = population_yaml_name_syl_end();
	const bool has_syl = (!prof->syllables_start.empty() || !prof->syllables_mid.empty() || !prof->syllables_end.empty()
		|| !g_syl0.empty() || !g_sylm.empty() || !g_syle.empty());
	if (has_syl)
		return PopulationNameProfile::Strategy::Syllables;
	if (!prof->pool.empty())
		return PopulationNameProfile::Strategy::PickOne;
	const auto& g_adj = population_yaml_name_adjectives();
	const auto& g_nou = population_yaml_name_nouns();
	if (!g_adj.empty() && !g_nou.empty())
		return PopulationNameProfile::Strategy::AdjectiveNoun;
	return PopulationNameProfile::Strategy::BotIndex;
}

static std::string population_roll_base_name_from_profile(PopulationNameProfile::Strategy strat, const PopulationNameProfile* prof, uint32_t index, uint32_t salt)
{
	const uint32_t variant = index + salt;

	auto pick_from = [](const std::vector<std::string>& profv, const std::vector<std::string>& glob) -> std::string {
		if (!profv.empty())
			return profv[rnd() % profv.size()];
		if (!glob.empty())
			return glob[rnd() % glob.size()];
		return std::string();
	};

	switch (strat) {
	case PopulationNameProfile::Strategy::BotIndex:
		return generate_bot_name(variant);
	case PopulationNameProfile::Strategy::PickOne: {
		if (!prof || prof->pool.empty())
			break;
		return prof->pool[rnd() % prof->pool.size()];
	}
	case PopulationNameProfile::Strategy::AdjectiveNoun: {
		const auto& g_adj = population_yaml_name_adjectives();
		const auto& g_nou = population_yaml_name_nouns();
		std::string adj = pick_from(prof != nullptr ? prof->adjectives : g_adj, g_adj);
		std::string noun = pick_from(prof != nullptr ? prof->nouns : g_nou, g_nou);
		if (adj.empty() || noun.empty())
			break;
		return adj + "_" + noun;
	}
	case PopulationNameProfile::Strategy::PrefixNumber: {
		const auto& prefs = population_yaml_name_global_prefixes();
		std::string pfx = !prefs.empty() ? prefs[rnd() % prefs.size()] : std::string("Bot");
		char buf[NAME_LENGTH];
		snprintf(buf, sizeof(buf), "%s%u", pfx.c_str(), static_cast<unsigned>(variant % 100000u));
		return std::string(buf);
	}
	case PopulationNameProfile::Strategy::Syllables: {
		const auto& g_syl0 = population_yaml_name_syl_start();
		const auto& g_sylm = population_yaml_name_syl_mid();
		const auto& g_syle = population_yaml_name_syl_end();
		std::string a, m, e;
		if (prof != nullptr) {
			a = pick_from(prof->syllables_start, g_syl0);
			m = pick_from(prof->syllables_mid, g_sylm);
			e = pick_from(prof->syllables_end, g_syle);
		} else {
			if (!g_syl0.empty())
				a = g_syl0[rnd() % g_syl0.size()];
			if (!g_sylm.empty())
				m = g_sylm[rnd() % g_sylm.size()];
			if (!g_syle.empty())
				e = g_syle[rnd() % g_syle.size()];
		}
		const std::string s = a + m + e;
		if (!s.empty())
			return s;
		break;
	}
	case PopulationNameProfile::Strategy::None:
	default:
		break;
	}
	return generate_bot_name(variant);
}

// Global state
static std::atomic<bool> g_population_engine_running(false);
static std::atomic<size_t> g_population_engine_count(0); // Atomic counter for fast access without mutex
static PopulationEngineStats g_population_engine_stats;

TIMER_FUNC(population_engine_chat_timer) {
	PE_PERF_SCOPE("timer.chat");
	extern struct Battle_Config battle_config;
	if (!battle_config.population_engine_chat_enable)
		return 0;

	const t_tick now = gettick();
	const int max_lines = battle_config.population_engine_chat_max_per_tick;
	int spoken = 0;
	size_t iterated = 0;

	const size_t n = g_population_engine_pcs.size();
	if (n == 0)
		return 0;
	if (g_chat_cursor >= n)
		g_chat_cursor = 0;

	// Periodic eviction of stale g_pop_chat_next_tick entries (shells that were released).
	// Runs a full sweep every 8 chat ticks (~4s at 500ms interval) instead of a
	// cursor-based partial scan whose position is meaningless after unordered_map rehash.
	{
		static int s_evict_countdown = 0;
		if (++s_evict_countdown >= 8) {
			s_evict_countdown = 0;
			auto it = g_pop_chat_next_tick.begin();
			while (it != g_pop_chat_next_tick.end()) {
				if (!population_engine_is_population_pc(it->first))
					it = g_pop_chat_next_tick.erase(it);
				else
					++it;
			}
		}
	}

	for (size_t i = 0; i < n && spoken < max_lines; ++i) {
		const size_t idx = (g_chat_cursor + i) % n;
		++iterated;
		map_session_data* raw_sd = g_population_engine_pcs[idx];
		if (raw_sd == nullptr || !raw_sd->state.active)
			continue;
		if (raw_sd->prev == nullptr)
			continue;
		if (map_id2bl(raw_sd->id) != raw_sd)
			continue;
		// Fake vending merchants are COMPLETELY silent — no ambient/public/bubble
		// chat, ever. A shell that has a vend open, or is Vendor-behaviour, never
		// speaks. (Real players are not in this list.)
		if (raw_sd->state.vending != 0 ||
		    static_cast<PopulationBehavior>(raw_sd->pop.behavior) == PopulationBehavior::Vendor)
			continue;

		std::shared_ptr<PopulationEngine> equipment = population_engine_db_for_shell(raw_sd).find(raw_sd->status.class_);

		t_tick next = 0;
		{
			auto it = g_pop_chat_next_tick.find(raw_sd->id);
			if (it != g_pop_chat_next_tick.end())
				next = it->second;
		}
		if (now < next)
			continue;

		// Personality + world-density gates: most shells stay silent most ticks.
		// speak_chance is the global "how chatty is the world" knob (0-100);
		// pop.chattiness is the per-shell personality weight (quiet vs talkative).
		{
			const int32 sc = battle_config.population_engine_chat_speak_chance;
			if (sc <= 0)
				continue;
			if (sc < 100 && static_cast<int32>(rnd() % 100) >= sc)
				continue;
			const uint32_t chat_w = raw_sd->pop.chattiness ? raw_sd->pop.chattiness : 50u;
			if ((rnd() % 100u) >= chat_w)
				continue;
		}

		// Context-aware pool selection: vendor shells call out, arena shells taunt,
		// shells fighting talk grind/hunt, otherwise the topic follows the map type
		// (town / field / dungeon), falling back to the profile ("personality") pool.
		const std::vector<std::string>* pool = nullptr;
		const auto beh = static_cast<PopulationBehavior>(raw_sd->pop.behavior);
		if (beh == PopulationBehavior::Vendor)
			pool = population_chat_db().lines_for_category("vendor_call");
		else if (raw_sd->pop.arena_team > 0)
			pool = population_chat_db().lines_for_category("pvp_taunt");
		else if (raw_sd->pop.target_id != 0)
			pool = population_chat_db().lines_for_category((rnd() % 2) ? "hunt" : "grind");
		else {
			// ~55% of the time follow the map context; else the personality pool.
			if ((rnd() % 100) < 55) {
				const char* ctx = nullptr;
				switch (raw_sd->pop.map_category) {
					case 1: ctx = (rnd() % 2) ? "shop" : "party"; break;   // town
					case 2: ctx = (rnd() % 2) ? "grind" : "hunt";  break;   // field
					case 3: ctx = "dungeon";                       break;   // dungeon
					default: break;
				}
				if (ctx)
					pool = population_chat_db().lines_for_category(ctx);
			}
		}
		// Fallback to profile ("personality") pool.
		if ((!pool || pool->empty()) && equipment)
			pool = population_chat_db().pool_for_profile(population_engine_chat_profile_key(equipment.get()));
		if (pool == nullptr || pool->empty())
			continue;

		// Pick a line, avoiding an immediate repeat of this shell's last line.
		std::string pick = (*pool)[rnd() % pool->size()];
		if (pool->size() > 1) {
			for (int tries = 0; tries < 3 && population_engine_pop_line_hash(pick) == raw_sd->pop.last_chat_hash; ++tries)
				pick = (*pool)[rnd() % pool->size()];
		}
		char buf[CHAT_SIZE_MAX];
		population_engine_format_chat_line(raw_sd, pick.c_str(), buf, sizeof(buf));
		if (population_engine_chat_line_blocked(buf))
			continue;

		population_engine_send_public_chat_as_pc(raw_sd, buf);
		raw_sd->pop.last_chat_hash = population_engine_pop_line_hash(pick);
		g_population_engine_stats.chat_lines_emitted++;

		const int32 base_cd = battle_config.population_engine_chat_cooldown_ms;
		const int32 jit = battle_config.population_engine_chat_cooldown_jitter_ms;
		const t_tick add = static_cast<t_tick>(base_cd + (jit > 0 ? static_cast<int32>(rnd() % (static_cast<uint32_t>(jit) + 1u)) : 0));
		g_pop_chat_next_tick[raw_sd->id] = now + add;
		spoken++;
	}
	g_chat_cursor = (g_chat_cursor + iterated) % std::max(n, size_t(1));
	return 0;
}

/// `whisper_to_sd` null = public map chat line ("Name : msg"); else whisper back.
static bool population_engine_deliver_chat_reply_locked(map_session_data* bot_sd, map_session_data* whisper_to_sd)
{
	extern struct Battle_Config battle_config;
	if (!bot_sd || !battle_config.population_engine_chat_enable || !battle_config.population_engine_chat_reply_enable)
		return false;
	if (bot_sd->prev == nullptr || map_id2bl(bot_sd->id) != bot_sd)
		return false;
	if (!bot_sd->status.name[0])
		return false;
	// Fake vending merchants never speak — including replies / whispers.
	if (bot_sd->state.vending != 0 ||
	    static_cast<PopulationBehavior>(bot_sd->pop.behavior) == PopulationBehavior::Vendor)
		return false;

	// Context-aware category selection: pick a situational category first, then fall back to profile pool.
	const std::vector<std::string>* pool = nullptr;
	const auto beh = static_cast<PopulationBehavior>(bot_sd->pop.behavior);

	// Determine context category from shell state.
	const char* ctx_category = nullptr;
	if (bot_sd->pop.arena_team > 0)
		ctx_category = "pvp_taunt";
	else if (beh == PopulationBehavior::Vendor)
		ctx_category = "shop";
	else if (bot_sd->pop.target_id != 0 || (bot_sd->pop.mob_tracker.tracked_mobs.size() > 0))
		ctx_category = "hunt";
	else if (beh == PopulationBehavior::Social || beh == PopulationBehavior::Wander)
		ctx_category = "idle";

	if (ctx_category)
		pool = population_chat_db().lines_for_category(ctx_category);

	// Fallback: profile pool (merged categories configured in population_chat.yml per profile).
	if (!pool || pool->empty()) {
		std::shared_ptr<PopulationEngine> equipment = population_engine_db_for_shell(bot_sd).find(bot_sd->status.class_);
		if (equipment == nullptr)
			return false;
		pool = population_chat_db().pool_for_profile(population_engine_chat_profile_key(equipment.get()));
	}
	if (!pool || pool->empty())
		return false;

	const t_tick now = gettick();
	t_tick next = 0;
	{
		auto it = g_pop_chat_next_tick.find(bot_sd->id);
		if (it != g_pop_chat_next_tick.end())
			next = it->second;
	}
	if (now < next)
		return false;

	// Don't reply to every nearby line: an overheard mention only gets a reply
	// some of the time (a whisper addressed directly to the shell always may).
	if (whisper_to_sd == nullptr) {
		const uint32_t chat_w = bot_sd->pop.chattiness ? bot_sd->pop.chattiness : 50u;
		// Overheard-mention reply chance scales with personality, capped modest.
		if ((rnd() % 100u) >= std::min<uint32_t>(chat_w, 45u))
			return false;
	}

	std::string pick = (*pool)[rnd() % pool->size()];
	if (pool->size() > 1) {
		for (int tries = 0; tries < 3 && population_engine_pop_line_hash(pick) == bot_sd->pop.last_chat_hash; ++tries)
			pick = (*pool)[rnd() % pool->size()];
	}
	char buf[CHAT_SIZE_MAX];
	population_engine_format_chat_line(bot_sd, pick.c_str(), buf, sizeof(buf));
	if (population_engine_chat_line_blocked(buf))
		return false;

	if (whisper_to_sd != nullptr) {
		if (!session_isActive(whisper_to_sd->fd))
			return false;
		clif_wis_message(whisper_to_sd, bot_sd->status.name, buf, strlen(buf) + 1, pc_get_group_level(bot_sd));
	} else {
		population_engine_send_public_chat_as_pc(bot_sd, buf);
	}
	bot_sd->pop.last_chat_hash = population_engine_pop_line_hash(pick);

	g_population_engine_stats.chat_lines_emitted++;
	const int32 base_cd = battle_config.population_engine_chat_cooldown_ms;
	const int32 jit = battle_config.population_engine_chat_cooldown_jitter_ms;
	const t_tick add = static_cast<t_tick>(base_cd + (jit > 0 ? static_cast<int32>(rnd() % (static_cast<uint32_t>(jit) + 1u)) : 0));
	g_pop_chat_next_tick[bot_sd->id] = now + add;
	return true;
}

static PopulationEngineConfig g_current_config;
static int32 g_autosummon_timer = INVALID_TIMER;
/// Set by population_engine_manual_fill_from_spawndb() so a single autosummon
/// pass can run on GM demand even while automatic autosummon is disabled.
static bool  g_autosummon_force_once = false;
/// 5M-slot ID pool; index in [1, POPULATION_ENGINE_INDEX_MAX) keeps account_id below POPULATION_ENGINE_ACCOUNT_ID_END.
static constexpr uint32_t POPULATION_ENGINE_INDEX_MAX = POPULATION_ENGINE_ACCOUNT_ID_END - POPULATION_ENGINE_ACCOUNT_ID_BASE;
static std::atomic<uint32_t> g_next_population_engine_index(1);

// Forward declarations.
static map_session_data* population_engine_spawn_shell(int16_t map_id, int x, int y, uint32_t index,
	uint16_t job_id, char sex, uint8_t hair_style, uint16_t hair_color,
	uint16_t weapon, uint16_t shield, uint16_t head_top, uint16_t head_mid,
	uint16_t head_bottom, uint32_t option, uint16_t cloth_color, uint16_t garment,
	struct script_code* init_script, bool skip_arrow, const PopulationEngine* pop_cfg,
	uint8_t map_category = 0,
	PopulationDbSource db_source = PopulationDbSource::Main,
	PopulationBehavior behavior_override = PopulationBehavior::None,
	const char* vendor_key_override = nullptr);
static std::string generate_bot_name(uint32_t index);
static std::string generate_population_pc_name(uint32_t index, const PopulationEngine* cfg);
static std::string population_vendor_gen_shop_name(const char* owner_name, t_itemid sample_item);
static int16_t get_random_job_id();
static char    get_job_required_sex(uint16_t job_id);
static uint16_t get_base_job(uint16_t job_id);
static uint16_t get_job_weapon(uint16_t job_id);
/// A pre-renewal weapon the given job can ALWAYS equip (low level, class-correct).
/// Returns 0 for jobs that fight unarmed by design (Taekwon / Star Gladiator).
static t_itemid population_engine_safe_job_weapon(uint16_t job_id);
static uint16_t get_random_headgear(uint8_t slot);
static uint16_t get_random_costume_robe();
static uint16_t find_valid_equip_item(uint32 equip_type);

/// Equip one item on a shell. `force_pos` overrides the item's equip bitmask — use for
/// accessories where the item supports both L and R but we need a specific slot.
static void population_engine_shell_equip_item(map_session_data* sd, t_itemid nameid, uint32_t index, const char* slot_label, uint32 force_pos = 0)
{
	if (!sd || nameid == 0)
		return;
	// Guard against max_weight reset by prior pc_equipitem → status_calc_pc calls.
	sd->max_weight = 2000000;
	struct item tmp_item = {};
	tmp_item.nameid = nameid;
	tmp_item.amount = 1;
	tmp_item.identify = 1;
	tmp_item.equip = 0;
	enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
	if (result != ADDITEM_SUCCESS) {
		ShowWarning("Population engine: Failed to add %s item %u to population shell %u (result: %d)\n",
			slot_label ? slot_label : "?", (unsigned)nameid, index, result);
		return;
	}
	for (int16 i = 0; i < MAX_INVENTORY; i++) {
		const auto& slot = sd->inventory.u.items_inventory[i];
		if (slot.nameid == nameid && slot.amount > 0 && slot.equip == 0) {
			struct item_data* id = itemdb_search(nameid);
			if (id && id->equip) {
				const uint32 pos = (force_pos != 0) ? force_pos : id->equip;
				(void)pc_equipitem(sd, i, pos, false);
			} else {
				if (!id)
					ShowWarning("Population engine: %s %u not found in itemdb for population shell %u\n",
						slot_label ? slot_label : "?", (unsigned)nameid, index);
				else if (!id->equip)
					ShowWarning("Population engine: %s %u has no equip flags for population shell %u\n",
						slot_label ? slot_label : "?", (unsigned)nameid, index);
			}
			break;
		}
	}
}
static t_itemid find_ammo_item_by_subtype(e_ammo_type sub); // First matching ammo in item_db, or 0
static t_itemid find_arrow_ammo_item(); // First arrow ammo in item_db, or common fallback
static void population_engine_sync_vd_weapon_shield(map_session_data* sd);
static void population_engine_finalize_shell_appearance(map_session_data* sd);
static void population_engine_destroy_failed_spawn(map_session_data* sd);

/// Count population shells on a specific map.
/// job_id = UINT16_MAX counts all jobs; otherwise counts only shells matching that job.
static size_t population_engine_count_shells_on_map(int16_t map_id, uint16_t job_id = UINT16_MAX) {
    size_t count = 0;
    for (map_session_data* sd : g_population_engine_pcs) {
        if (sd && sd->m == map_id) {
            if (job_id == UINT16_MAX || sd->status.class_ == job_id)
                count++;
        }
    }
    return count;
}

/// Count shells whose job belongs to a given profile job list.
/// Used by fill_category to compute the per-map deficit so the autosummon
/// timer does not keep stacking shells onto maps that are already at quota.
static size_t population_engine_count_shells_on_map_for_profile(
        int16_t map_id, const std::vector<uint16_t>& jobs)
{
    size_t count = 0;
    for (map_session_data* sd : g_population_engine_pcs) {
        if (!sd || sd->m != map_id) continue;
        const uint16_t c = static_cast<uint16_t>(sd->status.class_);
        for (uint16_t j : jobs) {
            if (c == j) { ++count; break; }
        }
    }
    return count;
}

/// Count vendor-behavior shells (state.vending == 1) currently on a map.
/// Used by the VendorPlacement-driven autosummon pass to enforce MaxVendors.
static size_t population_engine_count_vendors_on_map(int16_t map_id) {
    size_t count = 0;
    for (map_session_data* sd : g_population_engine_pcs) {
        if (sd && sd->m == map_id && sd->state.vending)
            ++count;
    }
    return count;
}



// Forward declaration — defined after population_engine_shell_release.
int32 population_engine_respawn_shell_timer(int32 tid, t_tick tick, int32 id, intptr_t data);

// Tear down a partially constructed fake PC when spawn fails before map_quit runs.
static void population_engine_destroy_failed_spawn(map_session_data* sd)
{
    if (!sd)
        return;
	g_pop_chat_next_tick.erase(sd->id);
	population_engine_path_erase_for_pc(sd->id);
    if (sd->regs.vars) {
        sd->regs.vars->destroy(sd->regs.vars, script_reg_destroy);
        sd->regs.vars = nullptr;
    }
    if (sd->regs.arrays) {
        sd->regs.arrays->destroy(sd->regs.arrays, script_free_array_db);
        sd->regs.arrays = nullptr;
    }
    sd->~map_session_data();
    aFree(sd);
}

void population_engine_shell_release(map_session_data* sd)
{
	if (!sd)
		return;
	// Defensive guard: a kill cascade (e.g. Asura Strike against a mortal shell)
	// can free `sd` out-of-band before the next stale-shell sweep runs. Touching
	// `sd->sc` (an unordered_map) on freed memory crashes deep inside _Find_last.
	// If the BL is no longer registered under this id, the shell has already been
	// torn down by another path; bail before any further access.
	if (map_id2bl(sd->id) != sd)
		return;
	// Leave (and, if last member, dissolve + free) any map-server-local fake party.
	// Also clears sd->status.party_id. No-op for shells that were never partied.
	party_fake_member_leave(sd);
	g_pop_chat_next_tick.erase(sd->id);
	population_engine_path_erase_for_pc(sd->id);
	// Cancel a pending respawn timer so it doesn't fire on a freed/reused shell.
	if (sd->pop.respawn_timer != INVALID_TIMER) {
		const TimerData* td = get_timer(sd->pop.respawn_timer);
		if (td && td->func == population_engine_respawn_shell_timer)
			delete_timer(sd->pop.respawn_timer, population_engine_respawn_shell_timer);
		sd->pop.respawn_timer = INVALID_TIMER;
	}
	// AutoCombat shells: end the real SC_AUTOCOMBAT loop cleanly (no SQL, no
	// GM-kick / warp-to-save — those paths are for real players only).
	if (static_cast<PopulationBehavior>(sd->pop.behavior) == PopulationBehavior::AutoCombat)
		autocombat_shell_stop(sd);
	population_engine_combat_shell_teardown(sd);
	// Re-check after teardown: combat_changestate / hat-effect callbacks may have
	// triggered map_quit on shells with non-zero action_on_end, freeing `sd`.
	if (map_id2bl(sd->id) != sd)
		return;
	sd->pop.flags &= ~PSF::CombatActive;
#ifdef SECURE_NPCTIMEOUT
	if (sd->npc_idle_timer != INVALID_TIMER) {
		const TimerData* td = get_timer(sd->npc_idle_timer);
		if (td && td->func == npc_secure_timeout_timer)
			delete_timer(sd->npc_idle_timer, npc_secure_timeout_timer);
		sd->npc_idle_timer = INVALID_TIMER;
	}
#endif
	sd->state.changemap = 0;
	sd->state.warping = 0;
	sd->state.connect_new = 0;

	// Only call map_quit if the shell is still registered.  Any external path that
	// calls map_quit on a shell (handle_shutdown, @kickall, etc.) will have already
	// removed it from id_db via map_deliddb; skip here to avoid double unit_free.
	if (map_id2bl(sd->id) != nullptr)
		map_quit(sd);

	// map_quit → unit_free_pc → unit_free handles sc_display, quest_log, bonus_script, etc.
	// Fake PCs never reach chrif_auth_delete, so the raw C hash tables in regs must be
	// freed here manually; unit_free does not touch them.
	// ~map_session_data destroys C++ containers; aFree releases the CREATE'd block.
	if (sd->regs.vars) {
		sd->regs.vars->destroy(sd->regs.vars, script_reg_destroy);
		sd->regs.vars = nullptr;
	}
	if (sd->regs.arrays) {
		sd->regs.arrays->destroy(sd->regs.arrays, script_free_array_db);
		sd->regs.arrays = nullptr;
	}
	sd->~map_session_data();
	aFree(sd);
}

/// Removes stale shells from g_population_engine_pcs and returns them.
/// Caller must call population_engine_shell_release on each returned pointer.
std::vector<map_session_data*> population_engine_collect_stale_shells()
{
	std::vector<map_session_data*> stale;
	auto it = g_population_engine_pcs.begin();
	while (it != g_population_engine_pcs.end()) {
		map_session_data* sd = *it;
		if (!sd || !population_engine_is_population_pc(sd->id) || !population_engine_combat_shell_ac_ok(sd)) {
			stale.push_back(sd);
			it = g_population_engine_pcs.erase(it);
			if (g_population_engine_count.load() > 0)
				g_population_engine_count--;
			if (g_population_engine_stats.active_units > 0)
				g_population_engine_stats.active_units--;
		} else {
			++it;
		}
	}
	return stale;
}

void population_engine_stats_record_walk_failure()
{
	// Called from the wander timer without the mutex; the stats struct uses plain uint32
	// but increments from the single map-main thread so no atomic needed.
	g_population_engine_stats.walk_failures++;
}

/// Allocate the next free population index from the 5 M ID pool.
/// Normal path: atomic increment, O(1).
/// Exhaustion path (rare): scans g_population_engine_pcs for used indices and returns
/// the first free slot, avoiding collision with any shell still alive.
/// Returns 0 on total pool exhaustion (impossible under normal conditions).
static uint32_t population_engine_allocate_index()
{
	uint32_t index = g_next_population_engine_index.fetch_add(1, std::memory_order_relaxed);
	if (index < POPULATION_ENGINE_INDEX_MAX)
		return index;

	// Pool counter wrapped. Build a set of in-use indices and find the first free one.
	ShowWarning("Population engine: ID counter exhausted; scanning for a free index (live shells: %zu).\n",
		g_population_engine_pcs.size());
	std::unordered_set<uint32_t> used;
	used.reserve(g_population_engine_pcs.size());
	for (const map_session_data* sd : g_population_engine_pcs) {
		if (!sd) continue;
		const uint32_t aid = sd->status.account_id;
		if (aid >= POPULATION_ENGINE_ACCOUNT_ID_BASE)
			used.insert(aid - POPULATION_ENGINE_ACCOUNT_ID_BASE);
	}
	for (uint32_t i = 1; i < POPULATION_ENGINE_INDEX_MAX; ++i) {
		if (used.find(i) == used.end()) {
			g_next_population_engine_index.store(i + 1, std::memory_order_relaxed);
			return i;
		}
	}
	ShowError("Population engine: ID pool fully exhausted (%u slots all occupied); spawn skipped.\n",
		static_cast<unsigned>(POPULATION_ENGINE_INDEX_MAX));
	return 0;
}

/// True only for a cell a normal player can properly stand on: passable, not a
/// wall/cliff, not water, not an OnTouch-NPC / Ice Wall / Land Protector cell,
/// and not an isolated one-cell pocket (needs >=2 passable orthogonal neighbours
/// so the shell is not spawned somewhere unreachable / "dead").
static bool population_cell_is_good_spawn(int16_t m, int16_t x, int16_t y)
{
	if (m < 0 || x <= 0 || y <= 0)
		return false;
	struct map_data *md = map_getmapdata(m);
	if (md == nullptr || x >= md->xs || y >= md->ys)
		return false;
	if (!map_getcell(m, x, y, CELL_CHKPASS))          return false; // wall / cliff
	if (map_getcell(m, x, y, CELL_CHKNOPASS))         return false;
	if (map_getcell(m, x, y, CELL_CHKWATER))          return false; // don't stand on water
	if (map_getcell(m, x, y, CELL_CHKNPC))            return false; // OnTouch NPC cell
	if (map_getcell(m, x, y, CELL_CHKICEWALL))        return false;
	if (map_getcell(m, x, y, CELL_CHKLANDPROTECTOR))  return false;
	int open = 0;
	if (map_getcell(m, static_cast<int16>(x + 1), y, CELL_CHKPASS)) ++open;
	if (map_getcell(m, static_cast<int16>(x - 1), y, CELL_CHKPASS)) ++open;
	if (map_getcell(m, x, static_cast<int16>(y + 1), CELL_CHKPASS)) ++open;
	if (map_getcell(m, x, static_cast<int16>(y - 1), CELL_CHKPASS)) ++open;
	return open >= 2;
}

/// Spawn up to `want` population shells on `map_id`, respecting global and per-map limits.
/// `job_hint`     — preferred job; UINT16_MAX = random.
/// `tick_budget`  — pointer to remaining spawn budget for this timer tick; nullptr = unlimited.
///                  Decremented by the number of shells actually spawned.
/// `bypass_existing_check` — when true, the per-job existing-on-map gate is skipped.
///                  Used by the VendorPlacement-driven pass which controls its own
///                  population target via PopulationVendorPlacement::max_vendors.
/// Returns the number of shells actually spawned.
static size_t autosummon_fill_map(int16_t map_id, size_t want, uint16_t job_hint = UINT16_MAX,
                                  size_t* tick_budget = nullptr, uint8_t map_category = 0,
                                  bool bypass_existing_check = false)
{
	if (want == 0)
		return 0;
	if (tick_budget != nullptr) {
		if (*tick_budget == 0)
			return 0;
		if (want > *tick_budget)
			want = *tick_budget;
	}
	struct map_data* mapdata = map_getmapdata(map_id);
	if (!mapdata || !mapdata->cell)
		return 0;

	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);

	// Only spawn shells that are missing to reach the target.
	// Count only shells of this specific job so multiple profiles can coexist on the same map.
	if (!bypass_existing_check) {
		const size_t existing = population_engine_count_shells_on_map(map_id, job_hint);
		if (existing >= want)
			return 0;
		want -= existing;
	}

	// Pre-compute vendor placement and build the vendor position snapshot once for the
	// entire batch to avoid O(N_slots × N_shells) re-walks of g_population_engine_pcs.
	const PopulationVendorPlacement *map_vp =
		population_vendor_db().vendor_placement_for_map(std::string(mapdata->name));
	std::vector<std::pair<int16, int16>> vendor_positions_here;
	if (map_vp && (map_vp->min_spacing > 0 || map_vp->max_vendors > 0)) {
		vendor_positions_here.reserve(g_population_engine_pcs.size());
		for (auto* psd : g_population_engine_pcs) {
			if (!psd || psd->m != map_id) continue;
			if (!psd->state.vending) continue;
			vendor_positions_here.emplace_back(psd->x, psd->y);
		}
	}

	size_t spawned = 0;
	for (size_t j = 0; j < want; ++j) {
		// Re-check global limit each iteration (other calls may have consumed slots).
		if (g_population_engine_count.load() >= max_global)
			break;

		// Pre-resolve the equipment/behavior for this slot so vendor placement constraints
		// can be applied to the cell pick (vendors are restricted to specific maps/areas
		// and must respect a minimum spacing from other vendor shells).
		const uint16_t pre_job_id = (job_hint != UINT16_MAX && pcdb_checkid(job_hint))
			? job_hint : get_random_job_id();
		PopulationDbSource pre_src = PopulationDbSource::Main;
		auto pre_equipment = population_engine_find_any(pre_job_id, &pre_src);
		if (!pre_equipment) {
			const uint16_t base_job = get_base_job(pre_job_id);
			if (base_job != pre_job_id)
				pre_equipment = population_engine_find_any(base_job, &pre_src);
		}
		// For town spawns (map_category==1), if the resolved entry doesn't have Vendor as its
		// town_behavior, check vendor_pop_db directly — it may have a VendorKey and the right
		// town_behavior even if the engine.yml entry shadows it in the general lookup.
		if (map_category == 1 && pre_equipment &&
		    pre_equipment->town_behavior != PopulationBehavior::Vendor) {
			if (auto vp = population_vendor_pop_db().find(pre_job_id)) {
				pre_src      = PopulationDbSource::Vendor;
				pre_equipment = vp;
			} else {
				const uint16_t base_job = get_base_job(pre_job_id);
				if (base_job != pre_job_id) {
					if (auto vp2 = population_vendor_pop_db().find(base_job)) {
						pre_src       = PopulationDbSource::Vendor;
						pre_equipment = vp2;
					}
				}
			}
		}
		PopulationBehavior eff_beh = pre_equipment ? pre_equipment->behavior : PopulationBehavior::Combat;
		if (pre_equipment) {
			PopulationBehavior cat_beh = PopulationBehavior::None;
			if      (map_category == 1) cat_beh = pre_equipment->town_behavior;
			else if (map_category == 2) cat_beh = pre_equipment->field_behavior;
			else if (map_category == 3) cat_beh = pre_equipment->dungeon_behavior;
			if (cat_beh != PopulationBehavior::None)
				eff_beh = cat_beh;
		}
		const bool is_vendor_spawn = (eff_beh == PopulationBehavior::Vendor);

		// Vendor placement — use the map-level entry pre-computed before the slot loop.
		const PopulationVendorPlacement *vp = is_vendor_spawn ? map_vp : nullptr;

		// When ANY VendorPlacement entries exist, vendor-behavior shells are
		// restricted to the listed maps only. A vendor spawn on a map with no
		// placement entry is dropped here so towns/etc. cannot accumulate vendors.
		if (is_vendor_spawn && !vp && population_vendor_db().any_vendor_placements())
			continue;

		// Enforce MaxVendors cap. vendor_positions_here was built before this loop
		// and is updated after each successful vendor spawn below.
		if (vp && vp->max_vendors > 0 &&
		    static_cast<int>(vendor_positions_here.size()) >= vp->max_vendors)
			continue; // map full of vendors already

		// Find a walkable spawn position.
		int x = 0, y = 0;
		if (mapdata->xs > 0 && mapdata->ys > 0) {
			int16_t sx = 0, sy = 0;

			// Determine search area. Default keeps a 50-cell border on large maps,
			// but shrinks the border on small maps (e.g. prt_mk is ~100x100; a
			// fixed 50-cell border would collapse the search to a single column).
			const int16_t margin_x = static_cast<int16_t>(std::min<int>(50, std::max<int>(2, mapdata->xs / 5)));
			const int16_t margin_y = static_cast<int16_t>(std::min<int>(50, std::max<int>(2, mapdata->ys / 5)));
			int16_t lo_x = margin_x, lo_y = margin_y;
			int16_t hi_x = static_cast<int16_t>(std::max<int>(margin_x + 1, mapdata->xs - margin_x));
			int16_t hi_y = static_cast<int16_t>(std::max<int>(margin_y + 1, mapdata->ys - margin_y));
			if (vp && vp->area_x1 >= 0 && vp->area_y1 >= 0 && vp->area_x2 >= vp->area_x1 && vp->area_y2 >= vp->area_y1) {
				// Clamp the placement area to the map bounds so a slightly oversized
				// box still produces a valid search range.
				lo_x = std::max<int16_t>(0, vp->area_x1);
				lo_y = std::max<int16_t>(0, vp->area_y1);
				hi_x = std::min<int16_t>(static_cast<int16_t>(mapdata->xs - 1), vp->area_x2);
				hi_y = std::min<int16_t>(static_cast<int16_t>(mapdata->ys - 1), vp->area_y2);
				if (hi_x <= lo_x) hi_x = static_cast<int16_t>(lo_x + 1);
				if (hi_y <= lo_y) hi_y = static_cast<int16_t>(lo_y + 1);
			}

			// Distance check vs the snapshot taken above (no mutex in inner loop).
			const int spacing = (vp ? vp->min_spacing : 0);
			auto cell_far_enough = [&](int16_t tx, int16_t ty) -> bool {
				if (spacing <= 0) return true;
				for (const auto &p : vendor_positions_here) {
					if (std::abs(static_cast<int>(p.first)  - tx) <= spacing &&
					    std::abs(static_cast<int>(p.second) - ty) <= spacing)
						return false;
				}
				return true;
			};

			const int max_attempts = vp ? 60 : 20;
			for (int attempt = 0; attempt < max_attempts; ++attempt) {
				const int16_t span_x = static_cast<int16_t>(std::max<int16_t>(1, hi_x - lo_x));
				const int16_t span_y = static_cast<int16_t>(std::max<int16_t>(1, hi_y - lo_y));
				sx = static_cast<int16_t>(lo_x + (rnd() % span_x));
				sy = static_cast<int16_t>(lo_y + (rnd() % span_y));
				if (sx >= mapdata->xs) sx = static_cast<int16_t>(mapdata->xs - 1);
				if (sy >= mapdata->ys) sy = static_cast<int16_t>(mapdata->ys - 1);
				if (!population_cell_is_good_spawn(map_id, sx, sy)) continue;
				if (!cell_far_enough(sx, sy)) continue;
				x = sx; y = sy;
				break;
			}
			if (x == 0 && y == 0 && !vp) {
				// Fallback: map_search_freecell (only when no placement constraints).
				if (map_search_freecell(nullptr, map_id, &sx, &sy,
					std::min(20, static_cast<int>(mapdata->xs / 2)),
					std::min(20, static_cast<int>(mapdata->ys / 2)), 1)
					&& population_cell_is_good_spawn(map_id, sx, sy)) {
					x = sx; y = sy;
				}
			}
			if (x == 0 && y == 0 && vp) {
				// Placement-constrained fallback: scan from a random center inside
				// the placement area. Honors the area but ignores spacing as a
				// last resort so vendors actually appear on small/dense maps.
				const int16_t span_x = static_cast<int16_t>(std::max<int16_t>(1, hi_x - lo_x));
				const int16_t span_y = static_cast<int16_t>(std::max<int16_t>(1, hi_y - lo_y));
				sx = static_cast<int16_t>(lo_x + (rnd() % span_x));
				sy = static_cast<int16_t>(lo_y + (rnd() % span_y));
				if (map_search_freecell(nullptr, map_id, &sx, &sy,
					std::min<int16_t>(static_cast<int16_t>(span_x / 2 + 4), 20),
					std::min<int16_t>(static_cast<int16_t>(span_y / 2 + 4), 20), 1)
					&& population_cell_is_good_spawn(map_id, sx, sy)) {
					x = sx; y = sy;
				}
			}
		}
		if (x == 0 && y == 0)
			continue; // No valid standing cell found for this slot; skip.

		// Use the pre-resolved job/equipment (may have been used to enforce vendor placement).
		uint16_t job_id = pre_job_id;
		auto equipment = pre_equipment;

		// Gender (job-locked wins; then YAML Sex; else random).
		char sex;
		const char required_sex = get_job_required_sex(job_id);
		if (required_sex != '\0') {
			sex = required_sex;
		} else if (equipment && equipment->sex_override >= 0) {
			sex = equipment->sex_override ? 'M' : 'F';
		} else {
			sex = (rnd() % 2) ? 'M' : 'F';
		}

		const uint8_t  hair_style  = MAX_HAIR_STYLE;
		const uint16_t hair_color  = static_cast<uint16_t>(rnd() % 131);
		const uint16_t cloth_color = static_cast<uint16_t>(rnd() % 699);

		// Pick one item randomly from each equipment pool (empty pool = no item in that slot).
		auto pick_pool = [](const std::vector<uint16_t>& p) -> uint16_t {
			if (p.empty()) return 0;
			return p.size() == 1 ? p[0] : p[rnd() % p.size()];
		};

		uint16_t weapon = 0, shield = 0, head_top = 0, head_mid = 0, head_bottom = 0, garment = 0;
		struct script_code* init_script = nullptr;
		bool skip_arrow = false;
		if (equipment) {
			weapon      = pick_pool(equipment->weapon_pool);
			shield      = pick_pool(equipment->shield_pool);
			head_top    = pick_pool(equipment->head_top_pool);
			head_mid    = pick_pool(equipment->head_mid_pool);
			head_bottom = pick_pool(equipment->head_bottom_pool);
			garment     = pick_pool(equipment->garment_pool);
			init_script = equipment->script;
			skip_arrow  = equipment->skip_arrow;
		} else {
			weapon = get_job_weapon(job_id);
			if (rnd() % 2 == 0) {
				struct item_data* sid = itemdb_search(2101);
				if (sid && (sid->equip & EQP_SHIELD))
					shield = 2101;
			}
			head_top    = (rnd() % 3 == 0) ? get_random_headgear(0) : 0;
			head_mid    = (rnd() % 3 == 0) ? get_random_headgear(1) : 0;
			head_bottom = (rnd() % 3 == 0) ? get_random_headgear(2) : 0;
			garment     = (rnd() % 2 == 0) ? get_random_costume_robe() : 0;
		}

		// Unique ID — collision-safe allocation from the 5 M pool.
		uint32_t index = population_engine_allocate_index();
		if (index == 0)
			continue; // pool fully exhausted, skip this shell

		const PopulationEngine* pop_cfg = equipment ? equipment.get() : nullptr;
		map_session_data* sd = population_engine_spawn_shell(
			map_id, x, y, index, job_id, sex, hair_style,
			hair_color, weapon, shield, head_top, head_mid, head_bottom,
			0 /*option*/, cloth_color, garment, init_script, skip_arrow, pop_cfg, map_category, pre_src);

		if (sd) {
			g_population_engine_pcs.push_back(sd);
			g_population_engine_count++;
			g_population_engine_stats.total_created++;
			g_population_engine_stats.active_units++;
			++spawned;
			// Keep the pre-built snapshot current so subsequent slots in this batch
			// see the just-spawned vendor when enforcing spacing and MaxVendors.
			if (is_vendor_spawn && vp)
				vendor_positions_here.emplace_back(static_cast<int16>(x), static_cast<int16>(y));
		} else {
			g_population_engine_stats.errors++;
		}
	}
	if (tick_budget != nullptr)
		*tick_budget -= spawned;
	return spawned;
}

/// Autosummon timer: fills maps to their YAML-configured population targets.
///
/// Each spawn profile declares per-category map lists and population targets.
/// Per-map target = floor(pop / map_count); first (pop % map_count) maps get +1.
/// Fires every 10s and tops up maps that are below target.
/// population_engine_autosummon_batch_size caps total spawns per tick (0 = unlimited).
/// Write current population shell count to cp_population_stats for FluxCP.
/// Guarded by g_last_db_written_count so SQL only fires on actual changes.
static void population_engine_write_count_sql(uint32_t count)
{
	if (mmysql_handle == nullptr)
		return;
	if (Sql_Query(mmysql_handle,
		"INSERT INTO `cp_population_stats` (`id`, `active_count`) VALUES (1, %u) "
		"ON DUPLICATE KEY UPDATE `active_count` = VALUES(`active_count`)",
		count) != SQL_SUCCESS)
		Sql_ShowDebug(mmysql_handle);
	g_last_db_written_count = count;
}

TIMER_FUNC(population_engine_autosummon_timer)
{
	PE_PERF_SCOPE("timer.autosummon");
	extern struct Battle_Config battle_config;
	// Defence in depth: even if the interval timer is somehow still armed, do
	// nothing while automatic population management is disabled (manual GM mode) —
	// unless a GM explicitly asked for a single distribution pass via
	// population_engine_manual_fill_from_spawndb().
	if (battle_config.population_engine_autosummon_enable == 0 && !g_autosummon_force_once)
		return 0;
	g_autosummon_force_once = false;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);

	// Collect drift candidates under lock, then call pc_setpos outside the lock.
	// to avoid deadlock via clif_* broadcast callbacks that may re-acquire the mutex.
	struct DriftEntry { map_session_data *sd; unsigned short mapindex; short x, y; };
	std::vector<DriftEntry> drift_candidates;
	for (map_session_data *sd : g_population_engine_pcs) {
		if (!sd || !sd->state.active || sd->prev == nullptr)
			continue;
		if (sd->pop.spawn_map_id < 0 || sd->m == sd->pop.spawn_map_id)
			continue;
		struct map_data *mapdata = map_getmapdata(sd->pop.spawn_map_id);
		if (!mapdata)
			continue;
		drift_candidates.push_back({sd, mapdata->index,
		    sd->pop.spawn_x, sd->pop.spawn_y});
	}
	// Map-drift check: shells that left their designated spawn map get warped back.
	for (auto &e : drift_candidates) {
		block_list *bl = map_id2bl(e.sd->id);
		if (bl && BL_CAST(BL_PC, bl) == e.sd)
			pc_setpos(e.sd, e.mapindex, e.x, e.y, CLR_TELEPORT);
	}

	// Sync active count to DB so FluxCP can display it on the website.
	{
		const uint32_t cur = static_cast<uint32_t>(g_population_engine_count.load(std::memory_order_relaxed));
		if (cur != g_last_db_written_count)
			population_engine_write_count_sql(cur);
	}

	if (!g_population_engine_running.load(std::memory_order_relaxed))
		return 0;

	if (g_population_engine_count.load() >= max_global)
		return 0;

	const int32 batch_cfg = battle_config.population_engine_autosummon_batch_size;
	size_t tick_budget = (batch_cfg > 0) ? static_cast<size_t>(batch_cfg) : 0;
	size_t* pbudget = (batch_cfg > 0) ? &tick_budget : nullptr;

	for (auto it = population_spawn_db().begin();
	     it != population_spawn_db().end(); ++it)
	{
		if (!it->second)
			continue;
		const PopulationSpawnEntry& se = *it->second;

		// Resolve Profile -> list of jobs that inherit from it. The pool is the
		// jobs declared in db/population_engine.yml whose Profile: matches; if
		// none match, the entry is silently skipped (no jobs to spawn).
		std::vector<uint16_t> profile_jobs = population_engine_db().jobs_with_profile(se.profile_name);
		if (profile_jobs.empty())
			continue;

		// Distribute `population` shells across `maps`: base = floor(pop/N),
		// first (pop%N) maps get base+1 to guarantee sum == population exactly.
		// max_per_map > 0 applies an additional per-map hard cap after distribution.
		// Jobs are picked at random per shell from `profile_jobs` so the spawn
		// is spread across every job that inherits the profile. bypass_existing_check
		// is passed to autosummon_fill_map so the per-job dedupe gate (which would
		// allow only one shell of a given job per map) does not throttle the
		// per-shell loop here.
		auto fill_category = [&](const std::vector<std::string>& maps, int32_t population, int32_t max_per_map, uint8_t category) -> bool {
			if (population <= 0 || maps.empty())
				return false;
			const size_t pop   = static_cast<size_t>(population);
			const size_t count = maps.size();
			const size_t base  = pop / count;
			const size_t extra = pop % count;
			for (size_t i = 0; i < count; ++i) {
				if (pbudget != nullptr && *pbudget == 0) return true;
				size_t target = base + (i < extra ? 1u : 0u);
				if (max_per_map > 0 && target > static_cast<size_t>(max_per_map))
					target = static_cast<size_t>(max_per_map);
				if (target == 0) continue;
				const int16 mid = map_mapname2mapid(maps[i].c_str());
				if (mid < 0) continue;
				// Only spawn the deficit so the timer never stacks more shells
				// than the YAML quota onto a map that is already at capacity.
				const size_t existing_on_map =
					population_engine_count_shells_on_map_for_profile(mid, profile_jobs);
				if (existing_on_map >= target) continue;
				const size_t deficit = target - existing_on_map;
				for (size_t s = 0; s < deficit; ++s) {
					if (pbudget != nullptr && *pbudget == 0) return true;
					if (g_population_engine_count.load() >= max_global) return true;
					const uint16_t pick = profile_jobs[rnd() % profile_jobs.size()];
					autosummon_fill_map(mid, 1, pick, pbudget, category, /*bypass_existing_check=*/true);
				}
				if (g_population_engine_count.load() >= max_global) return true;
			}
			return false;
		};

		if (fill_category(se.towns,    se.towns_population,    se.towns_max_per_map,    1)) return 0;
		if (fill_category(se.fields,   se.fields_population,   se.fields_max_per_map,   2)) return 0;
		if (fill_category(se.dungeons, se.dungeons_population, se.dungeons_max_per_map, 3)) return 0;
	}

	// VendorPlacement-driven pass: directly fill maps listed under VendorPlacement
	// up to MaxVendors using vendor-behavior jobs from population_engine.yml.
	// Without this, a map referenced ONLY by VendorPlacement (e.g. prt_mk) would
	// never receive vendors because population_spawn_db has no entry for it.
	{
		// Build (cached) list of jobs whose effective behavior is Vendor in any
		// category (base, town, field, or dungeon). Cleared on YAML reload via
		// population_engine_vendor_job_pool_clear().
		if (!g_pop_vendor_job_pool_built) {
			// Vendor jobs live in db/population_vendor_pop.yml. Falls back to the
			// main DB only if the vendor DB is empty (e.g. file missing on disk).
			PopulationEngineDatabase& vsrc = population_vendor_pop_db().size() > 0
				? population_vendor_pop_db()
				: population_engine_db();
			for (auto it = vsrc.begin(); it != vsrc.end(); ++it) {
				if (!it->second) continue;
				const PopulationEngine &pe = *it->second;
				const bool is_vendor =
					pe.behavior         == PopulationBehavior::Vendor ||
					pe.town_behavior    == PopulationBehavior::Vendor ||
					pe.field_behavior   == PopulationBehavior::Vendor ||
					pe.dungeon_behavior == PopulationBehavior::Vendor;
				if (is_vendor)
					g_pop_vendor_job_pool.push_back(it->first);
			}
			g_pop_vendor_job_pool_built = true;
		}

		if (!g_pop_vendor_job_pool.empty() && population_vendor_db().any_vendor_placements()) {
			for (const auto &kv : population_vendor_db().vendor_placements()) {
				if (pbudget != nullptr && *pbudget == 0) break;
				if (g_population_engine_count.load() >= max_global) break;

				const PopulationVendorPlacement &vp = kv.second;
				const int target = vp.max_vendors > 0 ? vp.max_vendors : 12; // sensible default
				const int16 mid  = map_mapname2mapid(vp.map.c_str());
				if (mid < 0) continue;

				const size_t cur = population_engine_count_vendors_on_map(mid);
				const size_t deficit = static_cast<size_t>(target) > cur ? static_cast<size_t>(target) - cur : 0;
				if (deficit == 0) continue;

				for (size_t d = 0; d < deficit; ++d) {
					if (pbudget != nullptr && *pbudget == 0) break;
					if (g_population_engine_count.load() >= max_global) break;
					const uint16_t vjob = g_pop_vendor_job_pool[rnd() % g_pop_vendor_job_pool.size()];
					// map_category=1: selects town_behavior override (Vendor on merchant jobs).
					// bypass_existing_check=true: placement pass owns the count via
					// population_engine_count_vendors_on_map above; the per-job gate in
					// autosummon_fill_map would otherwise stop at one shell per unique job.
					autosummon_fill_map(mid, 1, vjob, pbudget, 1, /*bypass_existing_check=*/true);
				}
			}
		}
	}

	return 0;
}

/// Tick a single bot if it qualifies. Called for each block_list within range
/// of a real PC. Dedupes via a per-pass set so two real PCs sharing view of the
/// same bot don't double-tick it.
struct s_pop_combat_tick_ctx {
	std::unordered_set<int32> ticked;
};

// Mirror of mob.cpp's ACTIVE_AI_RANGE (private there). Distance added on top of
// AREA_SIZE at which AI enters active mode.
static constexpr int POP_ACTIVE_AI_RANGE = 4;

static int32 pop_combat_tick_bot_in_range(block_list *bl, va_list ap)
{
	auto *ctx = va_arg(ap, s_pop_combat_tick_ctx *);
	map_session_data *sd = BL_CAST(BL_PC, bl);
	if (sd == nullptr)
		return 0;
	// Only bot PCs; skip real players.
	if (!population_engine_is_population_pc(sd->id))
		return 0;
	if (!sd->state.active || sd->prev == nullptr)
		return 0;
	if (!sd->state.population_combat)
		return 0;
	// Dedupe across multiple real-PC viewers.
	if (!ctx->ticked.insert(sd->id).second)
		return 0;
	population_engine_combat_per_tick(sd, true);
	return 1;
}

/// Outer callback: called for each PC in the world by map_foreachpc.
/// Skips bots; for real players, scans their viewport for bot PCs and ticks them.
static int32 pop_combat_tick_per_real_pc(map_session_data *sd, va_list ap)
{
	if (sd == nullptr)
		return 0;
	// Skip bots — only real players drive AI (mob_ai_hard pattern).
	if (population_engine_is_population_pc(sd->id))
		return 0;
	if (!sd->state.active || sd->prev == nullptr)
		return 0;
	auto *ctx = va_arg(ap, s_pop_combat_tick_ctx *);
	map_foreachinallrange(pop_combat_tick_bot_in_range, sd,
		AREA_SIZE + POP_ACTIVE_AI_RANGE, BL_PC, ctx);
	return 0;
}

/// Global combat timer: proximity-driven (mirrors mob_ai_hard).
/// Only bots within view of a real PC tick. Bots on empty maps cost ~zero,
/// so the engine scales by real-player count, not by total bot count.
TIMER_FUNC(population_engine_global_combat_timer)
{
	PE_PERF_SCOPE("timer.combat");
	{
		auto stale = population_engine_collect_stale_shells();
		for (auto *s : stale)
			population_engine_shell_release(s);
	}

	if (g_population_engine_pcs.empty())
		return 0;

	// Proximity-driven tick: each real PC scans its viewport for bots.
	// No round-robin / budget needed — work is bounded by real-player count.
	s_pop_combat_tick_ctx ctx;
	ctx.ticked.reserve(64);
	map_foreachpc(pop_combat_tick_per_real_pc, &ctx);
	return 0;
}

/// Respawn timer for mortal shells: fired ~5 s after death to teleport to spawn and revive.
/// Mob-style respawn: full state reset via unit_remove_map (target/timers/SCs) before re-add.
TIMER_FUNC(population_engine_respawn_shell_timer)
{
	PE_PERF_SCOPE("timer.respawn");
	map_session_data *sd = map_id2sd(static_cast<int32>(id));
	if (!sd || !population_engine_is_population_pc(sd->id)) {
		ShowDebug("PopEngine respawn: timer fired but shell %d not found (already released?).\n", id);
		return 0;
	}
	sd->pop.respawn_timer = INVALID_TIMER;
	if (!g_population_engine_running) {
		ShowDebug("PopEngine respawn: engine stopped; skipping revive for shell %u.\n", sd->id);
		return 0;
	}
	if (!pc_isdead(sd))
		return 0;

	const int16_t spawn_map = sd->pop.spawn_map_id;
	struct map_data *mapdata = (spawn_map >= 0) ? map_getmapdata(spawn_map) : nullptr;
	if (mapdata == nullptr) {
		ShowWarning("Population engine: respawn map invalid for shell %u (%s); despawning.\n", sd->id, sd->status.name);
		unit_remove_map(sd, CLR_OUTSIGHT);
		return 0;
	}

	// Mob-style: tear down map presence + clear stale unit_data (target, ongoing skill timers,
	// status changes) BEFORE re-spawning at the chosen point. Without this, shells revive
	// with residual SCs / locked target / canact_tick from the moment of death.
	int16 sx = sd->pop.spawn_x;
	int16 sy = sd->pop.spawn_y;
	if (!population_cell_is_good_spawn(spawn_map, sx, sy)) {
		for (int16 r = 4; r <= 16; r = static_cast<int16>(r + 6)) {
			sx = sd->pop.spawn_x; sy = sd->pop.spawn_y;
			if (map_search_freecell(nullptr, spawn_map, &sx, &sy, r, r, 1)
			    && population_cell_is_good_spawn(spawn_map, sx, sy))
				break;
		}
	}

	unit_remove_map(sd, CLR_RESPAWN);
	pc_setpos(sd, mapdata->index, sx, sy, CLR_OUTSIGHT);
	// Recalculate maxima FIRST, then revive to full, then hard-sync current HP/SP
	// to the freshly computed maxima. status_revive() derives current HP/SP from
	// whatever max_hp/max_sp happen to be at call time, so calling it before
	// status_calc_pc() can leave a shell revived at a stale (low) value — the
	// "inherited depleted HP/SP after respawn" bug. This guarantees a full,
	// valid playable state on every respawn.
	status_calc_pc(sd, SCO_FORCE);
	status_revive(sd, 100, 100);
	if (sd->battle_status.max_hp > 0) {
		sd->battle_status.hp = sd->battle_status.max_hp;
		sd->status.hp        = sd->battle_status.max_hp;
	}
	if (sd->battle_status.max_sp > 0) {
		sd->battle_status.sp = sd->battle_status.max_sp;
		sd->status.sp        = sd->battle_status.max_sp;
	}
	clif_updatestatus(*sd, SP_HP);
	clif_updatestatus(*sd, SP_SP);

	// Rebuild a valid, consistent sprite state after the mob-style teardown/re-add
	// (same authoritative path as spawn) and re-assert cloth colour 0.
	population_engine_finalize_shell_appearance(sd);
	clif_changelook(sd, LOOK_BASE,          sd->vd.look[LOOK_BASE]);
	clif_changelook(sd, LOOK_WEAPON,        sd->vd.look[LOOK_WEAPON]);
	clif_changelook(sd, LOOK_SHIELD,        sd->vd.look[LOOK_SHIELD]);
	clif_changelook(sd, LOOK_HEAD_TOP,      sd->vd.look[LOOK_HEAD_TOP]);
	clif_changelook(sd, LOOK_HEAD_MID,      sd->vd.look[LOOK_HEAD_MID]);
	clif_changelook(sd, LOOK_HEAD_BOTTOM,   sd->vd.look[LOOK_HEAD_BOTTOM]);
	clif_changelook(sd, LOOK_ROBE,          sd->vd.look[LOOK_ROBE]);
	clif_changelook(sd, LOOK_CLOTHES_COLOR, 0);

	sd->ud.canmove_tick = 0;
	sd->ud.canact_tick  = 0;
	sd->pop.target_id           = 0;
	sd->pop.sticky_target_id    = 0;
	sd->pop.last_cast_skill_id  = 0;
	sd->pop.last_damage_received = 0;
	sd->pop.last_attacked_tick  = 0;
	sd->pop.last_skill_used_on_me = 0;
	sd->pop.skill_next_use_tick.clear();

	// Re-register chat state (erased by shell_release during death processing).
	{
		std::shared_ptr<PopulationEngine> equipment = population_engine_db_for_shell(sd).find(sd->status.class_);
		if (equipment)
			population_engine_register_shell_chat_state(sd, equipment.get());
	}

	// Re-enable wander for shells that wander when not in combat.
	const auto beh = static_cast<PopulationBehavior>(sd->pop.behavior);
	if (beh == PopulationBehavior::Wander || beh == PopulationBehavior::Guard ||
	    beh == PopulationBehavior::Support || beh == PopulationBehavior::Social)
		population_engine_register_shell_wander_state(sd);

	// Re-arm the CombatActive flag (cleared by unit_remove_map / state teardown).
	sd->pop.flags |= PSF::CombatActive;

	// Restart the combat session for battle-oriented behaviors so the combat timer picks them up.
	if (beh == PopulationBehavior::Combat || beh == PopulationBehavior::Guard)
		population_engine_combat_start_session(sd, PopulationCombatStartMode::AutoCombat, 0, 0);
	// AutoCombat shells: re-seed sd->ac and re-arm SC_AUTOCOMBAT (both cleared by
	// the death / unit_remove_map teardown).
	else if (beh == PopulationBehavior::AutoCombat)
		autocombat_shell_start(sd);
	return 0;
}

void population_engine_on_shell_death(map_session_data *sd)
{
	if (!sd)
		return;
	if (!population_engine_shell_is_mortal(sd)) {
		ShowDebug("PopEngine death: shell %u (%s) has no Mortal flag — no respawn scheduled.\n",
			sd->id, sd->status.name);
		return;
	}
	// Cancel any pending respawn (shell died twice within the 5 s window) — prevents
	// double-revive and orphaned timer slots.
	if (sd->pop.respawn_timer != INVALID_TIMER) {
		const TimerData* td = get_timer(sd->pop.respawn_timer);
		if (td && td->func == population_engine_respawn_shell_timer)
			delete_timer(sd->pop.respawn_timer, population_engine_respawn_shell_timer);
		sd->pop.respawn_timer = INVALID_TIMER;
	}
	// Schedule respawn 5 s after death, matching typical mob respawn feel.
	sd->pop.respawn_timer = add_timer(gettick() + 5000, population_engine_respawn_shell_timer, sd->id, 0);
}

void population_engine_on_shell_kills_player(map_session_data *killer_sd, map_session_data *victim_sd)
{
	if (!killer_sd || !victim_sd)
		return;
	if (!battle_config.population_engine_chat_enable)
		return;

	// Pick from the pvp_kill category directly — bypasses profile lookup so any shell can trash-talk.
	const std::vector<std::string>* pool = population_chat_db().lines_for_category("pvp_kill");
	if (!pool || pool->empty()) {
		// Fallback: find the shell's profile pool.
		std::shared_ptr<PopulationEngine> equipment = population_engine_db_for_shell(killer_sd).find(killer_sd->status.class_);
		if (equipment)
			pool = population_chat_db().pool_for_profile(population_engine_chat_profile_key(equipment.get()));
	}
	if (!pool || pool->empty())
		return;

	const t_tick now = gettick();
	// Check cooldown — don't spam kill chat back-to-back.
	{
		auto it = g_pop_chat_next_tick.find(killer_sd->id);
		if (it != g_pop_chat_next_tick.end() && now < it->second)
			return;
	}

	const std::string& pick = (*pool)[rnd() % pool->size()];
	char buf[CHAT_SIZE_MAX];
	// Replace $victim with the victim's name in the trash-talk line.
	std::string line = pick;
	const std::string marker = "$victim";
	const size_t pos = line.find(marker);
	if (pos != std::string::npos)
		line.replace(pos, marker.size(), victim_sd->status.name);
	population_engine_format_chat_line(killer_sd, line.c_str(), buf, sizeof(buf));
	population_engine_send_public_chat_as_pc(killer_sd, buf);

	const int32 cd = battle_config.population_engine_chat_cooldown_ms;
	g_pop_chat_next_tick[killer_sd->id] = now + static_cast<t_tick>(cd > 0 ? cd : 5000);
}

void do_init_population_engine() {
	// Ensure the FluxCP stats table exists. This mirrors the canonical schema
	// in sql-files/population_engine.sql, which is an OPTIONAL import and is
	// therefore easy to miss on an existing database — when it is missing,
	// population_engine_write_count_sql() logs an SQL error on every autosummon
	// tick. Creating it here (same pattern as do_init_autocombat's tables)
	// makes both fresh and existing installs self-heal. Keep this in sync with
	// sql-files/population_engine.sql.
	if (mmysql_handle != nullptr && SQL_ERROR == Sql_QueryStr(mmysql_handle,
		"CREATE TABLE IF NOT EXISTS `cp_population_stats` ("
		"`id` INT UNSIGNED NOT NULL DEFAULT 1,"
		"`active_count` INT UNSIGNED NOT NULL DEFAULT 0,"
		"`last_updated` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
		"PRIMARY KEY (`id`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"))
	{
		Sql_ShowDebug(mmysql_handle);
	}

	// Single-threaded engine: bot pathing uses unit_walktoxy / unit_walktobl.
	add_timer_func_list(population_engine_autosummon_timer, "population_engine_autosummon_timer");
	add_timer_func_list(population_engine_chat_timer, "population_engine_chat_timer");
	add_timer_func_list(population_engine_global_combat_timer, "population_engine_global_combat_timer");
	add_timer_func_list(population_engine_respawn_shell_timer, "population_engine_respawn_shell_timer");
	population_engine_path_register_timer_funcs();
}

void do_init_population_engine_load_databases() {
	// Requires job_db (do_init_pc) and item_db (do_init_itemdb) to be ready.
	if (!population_names_db().load())
		ShowWarning("Population engine: population_names.yml missing or invalid; name generation falls back to Bot_<index>.\n");
	if (!population_chat_db().load())
		ShowWarning("Population engine: population_chat.yml missing or invalid; chat disabled until fixed.\n");
	if (!population_skill_db().load())
		ShowWarning("Population engine: population_skill_db.yml missing or invalid; no per-job skill overrides loaded.\n");
	// Shared templates DB MUST load before the three job DBs so GearSet/Profile
	// references in the job files can resolve via fallback lookup.
	if (!population_shared_db().load())
		ShowWarning("Population engine: population_gear_sets.yml missing or invalid; gear sets/profiles will fall back per-file.\n");
	if (!population_engine_db().load())
		ShowWarning("Population engine: equipment configuration file not found or has errors, using fallback equipment.\n");
	if (!population_pvp_db().load())
		ShowWarning("Population engine: population_pvp.yml missing or invalid; PvP arena will use main DB fallback.\n");
	if (!population_vendor_pop_db().load())
		ShowWarning("Population engine: population_vendor_pop.yml missing or invalid; vendor shells will use main DB fallback.\n");
	if (!population_spawn_db().load())
		ShowWarning("Population engine: population_spawn.yml missing or invalid; autosummon disabled until fixed.\n");
	if (!population_vendor_db().load())
		ShowWarning("Population engine: population_vendors.yml missing or invalid; vendor shells use built-in default stock.\n");
	// Optional — no warning if absent: Social town shells only use pubs when configured.
	population_pub_db().load();

	extern struct Battle_Config battle_config;

	if (g_autosummon_timer != INVALID_TIMER) {
		const TimerData* td = get_timer(g_autosummon_timer);
		if (td && td->func == population_engine_autosummon_timer)
			delete_timer(g_autosummon_timer, population_engine_autosummon_timer);
		g_autosummon_timer = INVALID_TIMER;
	}
	// Manual GM control: with population_engine_autosummon_enable off (the default)
	// the autosummon timer is NOT registered at all. Nothing spawns at boot and
	// nothing replenishes the population in the background. A GM adds/removes fake
	// players on demand with @populate add / @populate remove.
	if (battle_config.population_engine_autosummon_enable != 0) {
		constexpr int32 interval = 10000;
		// Fire immediately (100 ms) so bots are on the map as soon as the server is up,
		// then top up every 10 s like mob spawns do.
		g_autosummon_timer = add_timer_interval(gettick() + 100, population_engine_autosummon_timer, 0, 0, interval);
		if (g_autosummon_timer == INVALID_TIMER)
			ShowError("Population engine: failed to register autosummon timer; shells will not spawn automatically.\n");
		else
			ShowStatus("Population engine: autosummon ENABLED (population_engine_autosummon_enable: 1) — maps will be auto-populated from db/population_spawn.yml.\n");
	} else {
		ShowStatus("Population engine: autosummon DISABLED (manual GM mode) — use @populate add <n> [map] to create fake players.\n");
	}

	if (g_pop_chat_timer != INVALID_TIMER) {
		const TimerData* td = get_timer(g_pop_chat_timer);
		if (td && td->func == population_engine_chat_timer)
			delete_timer(g_pop_chat_timer, population_engine_chat_timer);
		g_pop_chat_timer = INVALID_TIMER;
	}
	if (battle_config.population_engine_chat_enable) {
		int32 chtick = std::max(500, battle_config.population_engine_chat_tick_ms);
		g_pop_chat_timer = add_timer_interval(gettick() + chtick, population_engine_chat_timer, 0, 0, chtick);
		if (g_pop_chat_timer == INVALID_TIMER)
			ShowError("Population engine: failed to register chat timer; ambient chat will be silent.\n");
	}

	population_engine_path_restart_wander_timer();

	if (g_population_combat_global_timer != INVALID_TIMER) {
		const TimerData *td = get_timer(g_population_combat_global_timer);
		if (td && td->func == population_engine_global_combat_timer)
			delete_timer(g_population_combat_global_timer, population_engine_global_combat_timer);
		g_population_combat_global_timer = INVALID_TIMER;
	}
	{
		int32 ctick = std::max(10, battle_config.population_engine_shell_timer_ms);
		g_population_combat_global_timer = add_timer_interval(gettick() + ctick, population_engine_global_combat_timer, 0, 0, ctick);
		if (g_population_combat_global_timer == INVALID_TIMER)
			ShowError("Population engine: failed to register combat timer; shells will not process AI.\n");
	}
	g_population_engine_running = true;
}

bool population_engine_reload_equipment(uint32_t *out_entry_count)
{
	if (out_entry_count != nullptr)
		*out_entry_count = 0;

	const bool names_ok = population_names_db().reload();
	if (names_ok)
		ShowStatus("Population engine: population_names.yml reloaded (%zu profiles).\n", population_names_db().profile_count());
	else
		ShowWarning("Population engine: population_names.yml reload failed; name lists may be empty until fixed.\n");

	// Shared templates first so cross-file GearSet/Profile references survive.
	const bool shared_re = population_shared_db().reload();
	if (shared_re)
		ShowStatus("Population engine: population_gear_sets.yml reloaded.\n");
	else
		ShowWarning("Population engine: population_gear_sets.yml reload failed.\n");

	const bool ok      = population_engine_db().reload();
	const bool pvp_ok  = population_pvp_db().reload();
	if (!pvp_ok)
		ShowWarning("Population engine: population_pvp.yml reload failed.\n");
	const bool vjob_ok = population_vendor_pop_db().reload();
	if (!vjob_ok)
		ShowWarning("Population engine: population_vendor_pop.yml reload failed.\n");

	const bool spawn_ok = population_spawn_db().reload();
	if (spawn_ok)
		ShowStatus("Population engine: population_spawn.yml reloaded (%u entries).\n",
			static_cast<uint32_t>(population_spawn_db().size()));
	else
		ShowWarning("Population engine: population_spawn.yml reload failed.\n");

	const bool skill_ok = population_skill_db().reload();
	if (skill_ok)
		ShowStatus("Population engine: population_skill_db.yml reloaded (%zu jobs).\n", population_skill_db().job_count());
	else
		ShowWarning("Population engine: population_skill_db.yml reload failed (missing or invalid).\n");

	const bool chat_re = population_chat_db().reload();
	if (chat_re)
		ShowStatus("Population engine: population_chat.yml reloaded (%zu profiles).\n", population_chat_db().profile_count());
	else
		ShowWarning("Population engine: population_chat.yml reload failed (missing or invalid).\n");

	const bool vendor_re = population_vendor_db().reload();
	if (vendor_re)
		ShowStatus("Population engine: population_vendors.yml reloaded (%zu entries).\n", population_vendor_db().entry_count());
	else
		ShowWarning("Population engine: population_vendors.yml reload failed (missing or invalid).\n");

	// Drop the dynamic-vendor cache so reloaded YAML/spawn data takes effect immediately.
	population_engine_vendor_dyn_cache_clear();
	population_engine_vendor_job_pool_clear();

	if (out_entry_count != nullptr)
		*out_entry_count = static_cast<uint32_t>(population_engine_db().size()
			+ population_pvp_db().size() + population_vendor_pop_db().size());

	if (ok)
		ShowStatus("Population engine: equipment YAML reloaded (main=%u, pvp=%u, vendor=%u job profiles).\n",
			static_cast<uint32_t>(population_engine_db().size()),
			static_cast<uint32_t>(population_pvp_db().size()),
			static_cast<uint32_t>(population_vendor_pop_db().size()));
	else
		ShowWarning("Population engine: equipment YAML reload failed (missing file or parse error).\n");

	return ok;
}

static map_session_data* population_engine_spawn_shell(int16_t map_id, int x, int y, uint32_t index,
	uint16_t job_id, char sex, uint8_t hair_style, uint16_t hair_color,
	uint16_t weapon, uint16_t shield, uint16_t head_top, uint16_t head_mid,
	uint16_t head_bottom, uint32_t option, uint16_t cloth_color, uint16_t garment,
	struct script_code* init_script, bool skip_arrow, const PopulationEngine* pop_cfg,
	uint8_t map_category,
	PopulationDbSource db_source,
	PopulationBehavior behavior_override,
	const char* vendor_key_override)
{
	PE_PERF_SCOPE("spawn_shell");
	if (map_id < 0) {
		ShowError("Population engine: Invalid map_id %d\n", map_id);
		return nullptr;
	}
	// Final gate: never create a fake player on a dead / invalid cell (wall,
	// water, cliff, OnTouch-NPC cell, isolated pocket). If the requested cell is
	// bad, try to snap to a good one nearby before giving up.
	if (!population_cell_is_good_spawn(map_id, static_cast<int16_t>(x), static_cast<int16_t>(y))) {
		int16_t nx = static_cast<int16_t>(x), ny = static_cast<int16_t>(y);
		bool fixed = false;
		for (int16_t r = 3; r <= 12 && !fixed; r = static_cast<int16_t>(r + 4)) {
			nx = static_cast<int16_t>(x); ny = static_cast<int16_t>(y);
			if (map_search_freecell(nullptr, map_id, &nx, &ny, r, r, 1)
			    && population_cell_is_good_spawn(map_id, nx, ny)) {
				x = nx; y = ny;
				fixed = true;
			}
		}
		if (!fixed) {
			ShowWarning("Population engine: no valid standing cell near (%d,%d) on map %d; skipping population shell %u\n",
				x, y, map_id, index);
			return nullptr;
		}
	}

	uint8_t eff_hair = hair_style;
	uint16_t eff_hair_color = hair_color;
	// CLOTHES colour is the ONLY palette forced for fake players: always 0
	// (equivalent to "@ccolor 0"). Hair style, hair colour and body style keep
	// their normal rolled / inherited behaviour.
	(void)cloth_color;
	const uint16_t eff_cloth = 0;
	if (pop_cfg != nullptr) {
		if (pop_cfg->hair_min >= 0) {
			const int16_t hmax = pop_cfg->hair_max >= 0 ? pop_cfg->hair_max : pop_cfg->hair_min;
			eff_hair = static_cast<uint8_t>(cap_value(population_roll_closed_range(pop_cfg->hair_min, hmax), MIN_HAIR_STYLE, MAX_HAIR_STYLE));
		}
		if (pop_cfg->hair_color_min >= 0) {
			const int16_t hmax = pop_cfg->hair_color_max >= 0 ? pop_cfg->hair_color_max : pop_cfg->hair_color_min;
			eff_hair_color = static_cast<uint16_t>(cap_value(population_roll_closed_range(pop_cfg->hair_color_min, hmax), MIN_HAIR_COLOR, MAX_HAIR_COLOR));
		}
	}

	std::string name = generate_population_pc_name(index, pop_cfg);
	const uint32_t account_id = POPULATION_ENGINE_ACCOUNT_ID_BASE + index;
	const uint32_t char_id    = POPULATION_ENGINE_CHAR_ID_BASE    + index;

	map_session_data* sd = nullptr;
	CREATE(sd, map_session_data, 1);
	new (sd) map_session_data();
	// CREATE uses memset(0) which overrides C++ default member initializers.
	// Explicitly set any fields whose sentinel value is NOT zero.
	sd->pop.respawn_timer = INVALID_TIMER;

	const t_tick tick = gettick();
	pc_setnewpc(sd, account_id, char_id, 0, tick, (sex == 'M' ? SEX_MALE : SEX_FEMALE), 0);
    
	sd->group_id = 0;
	pc_group_pc_load(sd);
	if (!sd->group) {
		ShowError("Population engine: no valid group (group_id=0) for population shell %u; using minimal defaults.\n", index);
		sd->group = std::make_shared<s_player_group>();
		sd->group->id    = 0;
		sd->group->level = 0;
		sd->group->log_commands = false;
	}
	if (sd->group->has_permission(PC_PERM_ALL_SKILL)) {
		std::bitset<PC_PERM_MAX> perms = sd->permissions;
		perms.reset(PC_PERM_ALL_SKILL);
		sd->permissions = perms;
	}
    
	memset(&sd->status, 0, sizeof(struct mmo_charstatus));
	sd->status.char_id    = char_id;
	sd->status.account_id = account_id;
	sd->status.sex = (sex == 'M' ? SEX_MALE : SEX_FEMALE);
	safestrncpy(sd->status.name, name.c_str(), sizeof(sd->status.name));

	sd->status.class_ = job_id;
	uint64 class_mapid = pc_jobid2mapid(job_id);
	if (class_mapid == (uint64)-1 || !job_db.exists(job_id)) {
		ShowError("Population engine: Invalid job %d for population shell %u; skipping spawn.\n", job_id, index);
		population_engine_destroy_failed_spawn(sd);
		return nullptr;
	} else {
		sd->class_ = class_mapid;
		int32 verify_job = pc_mapid2jobid(class_mapid, (sex == 'M' ? 1 : 0));
		if (verify_job != (int32)job_id && get_job_required_sex(job_id) != '\0') {
			ShowWarning("Population engine: Job/MAPID mismatch for population shell %u: job_id=%d, mapid=%llu, sex=%c, verify_job=%d\n",
				index, job_id, (unsigned long long)class_mapid, sex, verify_job);
			sd->status.class_ = verify_job;
		}
	}

	sd->status.hair         = cap_value(eff_hair, MIN_HAIR_STYLE, MAX_HAIR_STYLE);
	sd->status.hair_color   = cap_value(eff_hair_color, MIN_HAIR_COLOR, MAX_HAIR_COLOR);
	sd->status.clothes_color = 0; // @ccolor 0 — the only forced palette
	sd->status.body         = sd->status.class_; // original behaviour (unchanged)
	// status.weapon / status.shield / status.head_* / status.robe are VIEW (sprite)
	// ids, NOT item nameids. They are populated by pc_equipitem()->pc_set_costume_view()
	// from each equipped item's `look` field. Seeding them with a nameid here (or
	// re-assigning a nameid after equip) makes the client try to load a headgear /
	// shield sprite that does not exist -> the classic "broken fake-player sprite".
	// Leave them at 0; the equip passes below set the correct view ids.
	sd->status.weapon      = W_FIST;
	sd->status.shield      = 0;
	sd->status.head_top    = 0;
	sd->status.head_mid    = 0;
	sd->status.head_bottom = 0;
	sd->status.robe        = 0;
	sd->status.option     = option;

	// Fake players are never Level 99 (or higher). Cap every base-level source at 98.
	static const int32 POP_MAX_BASE_LEVEL = 98;
	if (pop_cfg != nullptr && pop_cfg->base_level_min >= 0) {
		const int16_t hi = pop_cfg->base_level_max >= 0 ? pop_cfg->base_level_max : pop_cfg->base_level_min;
		sd->status.base_level = cap_value(population_roll_closed_range(pop_cfg->base_level_min, hi), 1, POP_MAX_BASE_LEVEL);
	} else {
		sd->status.base_level = POP_MAX_BASE_LEVEL;
	}
	if (pop_cfg != nullptr && pop_cfg->job_level_min >= 0) {
		const int16_t hi = pop_cfg->job_level_max >= 0 ? pop_cfg->job_level_max : pop_cfg->job_level_min;
		sd->status.job_level = cap_value(population_roll_closed_range(pop_cfg->job_level_min, hi), 1, MAX_LEVEL);
	} else {
		sd->status.job_level = 70;
	}
	if (pop_cfg != nullptr && pop_cfg->str_min >= 0) {
		const int16_t hi = pop_cfg->str_max >= 0 ? pop_cfg->str_max : pop_cfg->str_min;
		sd->status.str = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->str_min, hi)), 1, 999);
	} else {
		sd->status.str = static_cast<uint16_t>(90 + (rnd() % 20));
	}
	if (pop_cfg != nullptr && pop_cfg->agi_min >= 0) {
		const int16_t hi = pop_cfg->agi_max >= 0 ? pop_cfg->agi_max : pop_cfg->agi_min;
		sd->status.agi = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->agi_min, hi)), 1, 999);
	} else {
		sd->status.agi = static_cast<uint16_t>(90 + (rnd() % 20));
	}
	if (pop_cfg != nullptr && pop_cfg->vit_min >= 0) {
		const int16_t hi = pop_cfg->vit_max >= 0 ? pop_cfg->vit_max : pop_cfg->vit_min;
		sd->status.vit = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->vit_min, hi)), 1, 999);
	} else {
		sd->status.vit = static_cast<uint16_t>(90 + (rnd() % 20));
	}
	if (pop_cfg != nullptr && pop_cfg->intl_min >= 0) {
		const int16_t hi = pop_cfg->intl_max >= 0 ? pop_cfg->intl_max : pop_cfg->intl_min;
		sd->status.int_ = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->intl_min, hi)), 1, 999);
	} else {
		sd->status.int_ = static_cast<uint16_t>(90 + (rnd() % 20));
	}
	if (pop_cfg != nullptr && pop_cfg->dex_min >= 0) {
		const int16_t hi = pop_cfg->dex_max >= 0 ? pop_cfg->dex_max : pop_cfg->dex_min;
		sd->status.dex = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->dex_min, hi)), 1, 999);
	} else {
		sd->status.dex = static_cast<uint16_t>(90 + (rnd() % 20));
	}
	if (pop_cfg != nullptr && pop_cfg->luk_min >= 0) {
		const int16_t hi = pop_cfg->luk_max >= 0 ? pop_cfg->luk_max : pop_cfg->luk_min;
		sd->status.luk = cap_value(static_cast<uint16_t>(population_roll_closed_range(pop_cfg->luk_min, hi)), 1, 999);
	} else {
		sd->status.luk = static_cast<uint16_t>(90 + (rnd() % 20));
	}

	// HP/SP placeholders — status_calc_pc() overwrites these from job_stats.yml (includes
	// job_aspd.yml and job_basepoints.yml), so the exact values here don't matter.
	sd->status.max_hp = 1;
	sd->status.hp     = 1;
	sd->status.max_sp = 1;
	sd->status.sp     = 1;

	const char* mapname = map_mapid2mapname(map_id);
	if (!mapname || !mapname[0]) {
		ShowError("Population engine: Invalid map name for map_id %d\n", map_id);
		population_engine_destroy_failed_spawn(sd);
		return nullptr;
	}
	safestrncpy(sd->status.last_point.map, mapname, sizeof(sd->status.last_point.map));
	sd->status.last_point.x = x;
	sd->status.last_point.y = y;
	safestrncpy(sd->status.save_point.map, mapname, sizeof(sd->status.save_point.map));
	sd->status.save_point.x = x;
	sd->status.save_point.y = y;

	sd->state.connect_new = 1;
	sd->followtimer             = INVALID_TIMER;
	sd->invincible_timer        = INVALID_TIMER;
	sd->npc_timer_id            = INVALID_TIMER;
	sd->pvp_timer               = INVALID_TIMER;
	sd->expiration_tid          = INVALID_TIMER;
	sd->autotrade_tid           = INVALID_TIMER;
	sd->respawn_tid             = INVALID_TIMER;
	sd->tid_queue_active        = INVALID_TIMER;
	sd->macro_detect.timer      = INVALID_TIMER;
	sd->skill_keep_using.tid    = INVALID_TIMER;
	sd->skill_keep_using.skill_id = 0;
	sd->skill_keep_using.level  = 0;
	sd->skill_keep_using.target = 0;

#ifdef SECURE_NPCTIMEOUT
	// Prevent timer cleanup errors from uninitialized npc_idle_timer.
	sd->npc_idle_timer    = INVALID_TIMER;
	sd->npc_idle_tick     = tick;
	sd->npc_idle_type     = NPCT_INPUT;
	sd->state.ignoretimeout = false;
#endif

	sd->canuseitem_tick   = 0;
	sd->canusecashfood_tick = 0;
	sd->canequip_tick     = 0;
	sd->cantalk_tick      = 0;
	sd->canskill_tick     = 0;
	sd->state.autocast    = 1; // bypass skill_isNotOk checks that prevent skill spam
	sd->cansendmail_tick  = 0;
	sd->idletime          = tick;

	sd->regen.tick.hp = tick;
	sd->regen.tick.sp = tick;

	for (int32 i = 0; i < MAX_SPIRITBALL; i++)
		sd->spirit_timer[i] = INVALID_TIMER;

	if (battle_config.item_auto_get)
		sd->state.autoloot = 10000;
	if (battle_config.disp_experience)
		sd->state.showexp = 1;
	if (battle_config.disp_zeny)
		sd->state.showzeny = 1;
	if (!(battle_config.display_skill_fail & 2))
		sd->state.showdelay = 1;

	memset(&sd->inventory, 0, sizeof(struct s_storage));
	// Non-vendor shells never access cart, storage, or premiumStorage.
	// Skipping their zero-init saves ~78 KB per shell — at 5k shells that is ~380 MB.
	{
		PopulationBehavior early_beh = pop_cfg ? pop_cfg->behavior : PopulationBehavior::Combat;
		if (pop_cfg) {
			PopulationBehavior ov = PopulationBehavior::None;
			if      (map_category == 1) ov = pop_cfg->town_behavior;
			else if (map_category == 2) ov = pop_cfg->field_behavior;
			else if (map_category == 3) ov = pop_cfg->dungeon_behavior;
			if (ov != PopulationBehavior::None) early_beh = ov;
		}
		if (early_beh == PopulationBehavior::Vendor) {
			memset(&sd->cart,           0, sizeof(struct s_storage));
			memset(&sd->storage,        0, sizeof(struct s_storage));
			memset(&sd->premiumStorage, 0, sizeof(struct s_storage));
		}
	}
	memset(&sd->equip_index,       -1, sizeof(sd->equip_index));
	memset(&sd->equip_switch_index, -1, sizeof(sd->equip_switch_index));

	sd->sc.option = sd->status.option;
	unit_dataset(sd);

	sd->guild_x = -1;
	sd->guild_y = -1;
	sd->delayed_damage = 0;

	for (int32 i = 0; i < MAX_EVENTTIMER; i++)
		sd->eventtimer[i] = INVALID_TIMER;
	sd->rental_timer = INVALID_TIMER;

	for (int32 i = 0; i < 3; i++)
		sd->hate_mob[i] = -1;

	sd->quest_log    = nullptr;
	sd->num_quests   = 0;
	sd->avail_quests = 0;
	sd->save_quest   = false;
	sd->count_rewarp = 0;
	sd->mail.pending_weight = 0;
	sd->mail.pending_zeny   = 0;
	sd->mail.pending_slots  = 0;

	sd->regs.vars   = i64db_alloc(DB_OPT_BASE);
	sd->regs.arrays = nullptr;
	sd->vars_dirty  = false;
	sd->vars_ok     = true;  // skip char-server auth
	sd->vars_received = 0x7;

	uint16 mapindex = mapindex_name2id(mapname);
	if (mapindex == 0) {
		ShowError("Population engine: Invalid mapname %s (cannot convert to mapindex)\n", mapname);
		population_engine_destroy_failed_spawn(sd);
		return nullptr;
	}
	enum e_setpos setpos_result = pc_setpos(sd, mapindex, x, y, CLR_OUTSIGHT);
	if (setpos_result != SETPOS_OK) {
		ShowError("Population engine: Failed to set position for population shell %u (error %d)\n", index, setpos_result);
		population_engine_destroy_failed_spawn(sd);
		return nullptr;
	}
	// pc_setpos sets warp handshake flags; clear them so unit_remove_map doesn't warn on cleanup.
	sd->state.changemap = 0;
	sd->state.connect_new = 0;
	sd->state.warping = 0;

	map_addiddb(sd);
	if (map_addblock(sd) != 0) {
		ShowError("Population engine: Failed to add population shell %u to map\n", index);
		map_deliddb(sd);
		population_engine_destroy_failed_spawn(sd);
		return nullptr;
	}

	// Skip clif_spawn — increment mapdata->users/users_pvp manually so unit_remove_map
	// doesn't warn about unexpected state when the shell is later released.
	{
		struct map_data* mapdata = map_getmapdata(sd->m);
		if (mapdata) {
			if (mapdata->users++ == 0 && battle_config.dynamic_mobs)
				map_spawnmobs(sd->m);
			if (!pc_isinvisible(sd))
				mapdata->users_pvp++;
		}
		sd->state.debug_remove_map = 0;
	}

	// Fix sex mismatch for gender-locked jobs before status_calc_pc.
	char required_sex_check = get_job_required_sex(sd->status.class_);
	if (required_sex_check != '\0') {
		const int32 req_sex_int = (required_sex_check == 'M') ? 1 : 0;
		if ((sd->status.sex == SEX_MALE ? 1 : 0) != req_sex_int) {
			ShowWarning("Population engine: Fixing sex mismatch for population shell %u: job=%d (requires %c), current_sex=%c\n",
				index, job_id, required_sex_check, (sd->status.sex == SEX_MALE) ? 'M' : 'F');
			sd->status.sex = (required_sex_check == 'M') ? SEX_MALE : SEX_FEMALE;
		}
	}

	// status_set_viewdata must be called before status_calc_pc.
	status_set_viewdata(sd, sd->status.class_);
	sd->vd.look[LOOK_HAIR]          = sd->status.hair;
	sd->vd.look[LOOK_HAIR_COLOR]    = sd->status.hair_color;
	sd->vd.look[LOOK_CLOTHES_COLOR] = sd->status.clothes_color;
	// LOOK_WEAPON / LOOK_SHIELD: set by pc_calcweapontype after equip.
	sd->vd.look[LOOK_HEAD_TOP]    = sd->status.head_top;
	sd->vd.look[LOOK_HEAD_MID]    = sd->status.head_mid;
	sd->vd.look[LOOK_HEAD_BOTTOM] = sd->status.head_bottom;
	sd->vd.look[LOOK_ROBE]        = sd->status.robe;
	sd->vd.sex = sd->status.sex;

	// base_status.mode must be set before status_calc_pc so MD_CANMOVE survives the copy.
	sd->base_status.mode = static_cast<e_mode>(MD_CANMOVE | MD_CANATTACK);
	status_calc_pc(sd, SCO_FIRST);

	// Sync HP/SP/AP to calculated maxima — status_calc_pc may change them without clamping current values.
	// status_isdead() checks battle_status.hp; leaving it at 0 makes the engine treat the shell as dead.
	if (sd->battle_status.max_hp > 0) {
		sd->status.hp         = sd->battle_status.max_hp;
		sd->battle_status.hp  = sd->battle_status.max_hp;
	}
	if (sd->battle_status.max_sp > 0) {
		sd->status.sp         = sd->battle_status.max_sp;
		sd->battle_status.sp  = sd->battle_status.max_sp;
	}
	if (sd->battle_status.max_ap > 0) {
		sd->status.ap         = sd->battle_status.max_ap;
		sd->battle_status.ap  = sd->battle_status.max_ap;
	}

	// status_calc_pc may flip the job for gender-specific combined MAPIDs (e.g. MAPID_SHINKIRO_SHIRANUI).
	if (required_sex_check != '\0') {
		const char cur_sex = get_job_required_sex(sd->status.class_);
		const int32 req_int = (required_sex_check == 'M') ? 1 : 0;
		if (cur_sex != required_sex_check || (sd->status.sex == SEX_MALE ? 1 : 0) != req_int
		    || sd->status.class_ != job_id)
		{
			ShowWarning("Population engine: Fixing gender/job mismatch for population shell %u: original_job=%d (requires %c), current_job=%d (requires %c), current_sex=%c\n",
				index, job_id, required_sex_check, sd->status.class_,
				cur_sex != '\0' ? cur_sex : '?',
				(sd->status.sex == SEX_MALE) ? 'M' : 'F');
			// Never restore a job that no longer exists in job_db.
			if (job_db.exists(job_id) && pc_jobid2mapid(job_id) != static_cast<uint64_t>(-1))
				sd->status.class_ = job_id;
			sd->status.sex = (required_sex_check == 'M') ? SEX_MALE : SEX_FEMALE;
			const uint64 fixed_mapid = pc_jobid2mapid(sd->status.class_);
			if (fixed_mapid != (uint64)-1)
				sd->class_ = fixed_mapid;
			status_set_viewdata(sd, sd->status.class_);
			sd->vd.sex = sd->status.sex;
			sd->base_status.mode = static_cast<e_mode>(MD_CANMOVE | MD_CANATTACK);
			status_calc_pc(sd, SCO_FORCE);
		}
	}

	sd->status.inventory_slots = MAX_INVENTORY;
	if (sd->base_status.max_ap > 0) {
		sd->status.max_ap        = sd->base_status.max_ap;
		sd->battle_status.max_ap = sd->base_status.max_ap;
		sd->status.ap            = sd->base_status.max_ap;
		sd->battle_status.ap     = sd->base_status.max_ap;
	}
	sd->status.sp         = sd->base_status.max_sp;
	sd->battle_status.sp  = sd->base_status.max_sp;
	if (sd->status.zeny < 100000)
		sd->status.zeny = 100000;
    
	// Grant full job skill tree (YAML Skills: true, default) or minimal basics (Skills: false).
	if (pop_cfg == nullptr || pop_cfg->grant_skill_tree) {
		// pc_calc_skilltree sets prerequisites but doesn't assign levels; grant each skill explicitly
		// so pc_checkskill() returns non-zero for the combat seeder. Without this all seeding passes
		// produce an empty list and shells fall back to melee-only.
		pc_calc_skilltree(sd);
		std::shared_ptr<s_skill_tree> tree = skill_tree_db.find(sd->status.class_);
		if (tree && !tree->skills.empty()) {
			for (const auto& [sid, entry] : tree->skills) {
				if (entry && entry->max_lv > 0)
					pc_skill(sd, sid, entry->max_lv, ADDSKILL_PERMANENT_GRANTED);
			}
			// Strip any extra skills pc_calc_skilltree may have added beyond this job's tree.
			std::set<uint16_t> valid_ids;
			for (const auto& e : tree->skills) valid_ids.insert(e.first);
			valid_ids.insert(NV_BASIC);
			valid_ids.insert(NV_FIRSTAID);
			for (uint16 i = 1; i < MAX_SKILL; i++) {
				if (sd->status.skill[i].id > 0 && valid_ids.find(sd->status.skill[i].id) == valid_ids.end()) {
					sd->status.skill[i].id   = 0;
					sd->status.skill[i].lv   = 0;
					sd->status.skill[i].flag = SKILL_FLAG_PERMANENT;
				}
			}
		} else {
			ShowWarning("Population engine: No skill tree for job %d (population shell %u)\n", sd->status.class_, index);
		}
	} else {
		memset(&sd->status.skill, 0, sizeof(sd->status.skill));
		pc_skill(sd, NV_BASIC,    1, ADDSKILL_PERMANENT_GRANTED);
		pc_skill(sd, NV_FIRSTAID, 1, ADDSKILL_PERMANENT_GRANTED);
	}

	// Ensure every skill listed in `Skills:` is granted at least up to its level cap.
	if (pop_cfg && !pop_cfg->shell_attack_skill_yaml.empty()) {
		for (const PopulationShellYamlSkill &e : pop_cfg->shell_attack_skill_yaml) {
			if (e.skill_id == 0 || !skill_get_index(e.skill_id))
				continue;
			const uint16_t smax = skill_get_max(e.skill_id);
			const uint16_t have = pc_checkskill(sd, e.skill_id);
			const uint16_t want = (e.level_cap > 0)
				? std::max(have, std::min<uint16_t>(e.level_cap, smax))
				: have;
			if (want > have)
				pc_skill(sd, e.skill_id, want, ADDSKILL_PERMANENT_GRANTED);
		}
	}

	// Recalc once after all pc_skill grants so passive bonuses (TF_DOUBLE, AC_OWL, etc.)
	// take effect immediately. pc_equipitem below also triggers a recalc, but a shell
	// with no equipment to wear would otherwise skip it and ship without passive stats.
	status_calc_pc(sd, SCO_FORCE);

	// max_weight must be raised before pc_additem: every pc_equipitem calls status_calc_pc
	// which resets max_weight to job_base + str*300, causing subsequent pc_additem to fail
	// with ADDITEM_OVERWEIGHT. The final status_calc_pc at the end restores the correct value.
	sd->max_weight = 2000000;

	// Bard / Clown -> Violin (Musical) ; Dancer / Gypsy -> Rope (Whip). Force the
	// class instrument regardless of the gear set: this item DB lets BardDancer
	// equip plain bows, and a Bard holding a bow cannot play any song/dance.
	switch (job_id) {
		case JOB_BARD:  case JOB_CLOWN:  case JOB_BABY_BARD:
		case JOB_DANCER: case JOB_GYPSY: case JOB_BABY_DANCER:
			weapon = population_engine_safe_job_weapon(job_id); // 1901 Violin / 1950 Rope
			break;
		default: break;
	}

	if (weapon > 0) {
		struct item tmp_item = {};
		tmp_item.nameid   = weapon;
		tmp_item.amount   = 1;
		tmp_item.identify = 1;
		enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
		if (result == ADDITEM_SUCCESS) {
			for (int16 i = 0; i < MAX_INVENTORY; i++) {
				const auto& islot = sd->inventory.u.items_inventory[i];
				if (islot.nameid == weapon && islot.amount > 0 && islot.equip == 0) {
					struct item_data* id = itemdb_search(weapon);
					if (id && id->equip) {
						(void)pc_equipitem(sd, i, id->equip, false);
					} else {
						if (!id)
							ShowWarning("Population engine: Weapon %u not found in itemdb for population shell %u\n", weapon, index);
						else if (!id->equip)
							ShowWarning("Population engine: Weapon %u has no equip flags for population shell %u\n", weapon, index);
					}
					break;
				}
			}
		} else {
			ShowWarning("Population engine: Failed to add weapon %u to population shell %u inventory (result: %d)\n", weapon, index, result);
		}
	}

	// Guaranteed-weapon safety net. If the gear-set weapon did not end up equipped
	// (empty pool, invalid item id, wrong class, or EquipLevelMin above this
	// shell's rolled BaseLevel) AND the job normally wields a weapon, equip a
	// known-safe pre-renewal weapon for the job's class. This is what stops
	// weapon-using jobs from spawning bare-handed. Taekwon/Star Gladiator return
	// 0 here and stay unarmed on purpose.
	if (sd->equip_index[EQI_HAND_R] < 0) {
		const t_itemid safe_w = population_engine_safe_job_weapon(job_id);
		if (safe_w != 0) {
			struct item_data* sw_id = itemdb_search(safe_w);
			if (sw_id != nullptr && sw_id->equip != 0) {
				struct item wi = {};
				wi.nameid   = safe_w;
				wi.amount   = 1;
				wi.identify = 1;
				sd->max_weight = 2000000;
				if (pc_additem(sd, &wi, 1, LOG_TYPE_NONE, false) == ADDITEM_SUCCESS) {
					for (int16 k = 0; k < MAX_INVENTORY; k++) {
						const auto& is = sd->inventory.u.items_inventory[k];
						if (is.nameid == safe_w && is.amount > 0 && is.equip == 0) {
							(void)pc_equipitem(sd, k, sw_id->equip, false);
							break;
						}
					}
				}
				if (sd->equip_index[EQI_HAND_R] >= 0)
					weapon = safe_w; // so the ammo pass below arms bows/guns correctly
				else
					ShowWarning("Population engine: shell %u (job %u) could not equip fallback weapon %u.\n",
						index, job_id, static_cast<unsigned>(safe_w));
			}
		}
	}

    // When a Script: block exists, setarrow/setbullet/setkunai in it run once via
    // run_script at spawn.  Skip the built-in ammo pass to avoid double-equip.
    if (!skip_arrow && init_script == nullptr && weapon > 0) {
        struct item_data* wid = itemdb_search(weapon);
        if (wid && wid->type == IT_WEAPON) {
            t_itemid ammo_id = 0;
            const char* ammo_ctx = nullptr;
            switch (wid->subtype) {
            case W_BOW:
            case W_MUSICAL:
            case W_WHIP:
                ammo_id = find_arrow_ammo_item();
                ammo_ctx = "arrow";
                break;
            case W_REVOLVER:
            case W_RIFLE:
            case W_GATLING:
                ammo_id = find_ammo_item_by_subtype(AMMO_BULLET);
                ammo_ctx = "bullet";
                break;
            case W_SHOTGUN:
                ammo_id = find_ammo_item_by_subtype(AMMO_SHELL);
                ammo_ctx = "shell";
                break;
#ifdef RENEWAL
            case W_GRENADE:
                ammo_id = find_ammo_item_by_subtype(AMMO_GRENADE);
                ammo_ctx = "grenade";
                break;
#endif
            case W_HUUMA:
                ammo_id = find_ammo_item_by_subtype(AMMO_KUNAI);
                ammo_ctx = "kunai";
                break;
            default:
                break;
            }
            if (ammo_id != 0 && ammo_ctx != nullptr) {
                struct item_data* aid = itemdb_search(ammo_id);
                if (!aid || !aid->equip) {
                    ShowWarning("Population engine: builtin %s ammo item %u invalid for population shell %u (weapon item %u)\n",
                        ammo_ctx, (unsigned)ammo_id, index, (unsigned)weapon);
                } else {
                    // Cap amount to what fits by weight; always at least 1 so equip can proceed.
                    int add_amount = 500;
                    if (aid->weight > 0) {
                        const int fits = (sd->max_weight - sd->weight) / static_cast<int>(aid->weight);
                        add_amount = fits < 1 ? 1 : (fits < 500 ? fits : 500);
                    }
                    struct item tmp_item = {};
                    tmp_item.nameid = ammo_id;
                    tmp_item.amount = add_amount;
                    tmp_item.identify = 1;
                    tmp_item.equip = 0;
                    enum e_additem_result ares = pc_additem(sd, &tmp_item, add_amount, LOG_TYPE_NONE, false);
                    if (ares != ADDITEM_SUCCESS) {
                        ShowWarning("Population engine: builtin %s pc_additem failed result=%d ammo=%u population shell %u (max_w=%d w=%d)\n",
                            ammo_ctx, (int)ares, (unsigned)ammo_id, index, sd->max_weight, sd->weight);
                    } else {
                        bool done = false;
                        for (int16 i = 0; i < MAX_INVENTORY; i++) {
                            auto& aslot = sd->inventory.u.items_inventory[i];
                            if (aslot.nameid == ammo_id && aslot.amount > 0 && aslot.equip == 0) {
                                // Bypass pc_equipitem so that class-restricted ammo (Classes: All: false)
                                // can be force-equipped on shells regardless of job. Shells are virtual
                                // and never go through normal equip validation.
                                aslot.equip = static_cast<unsigned int>(aid->equip);
                                sd->equip_index[EQI_AMMO] = i;
                                done = true;
                                break;
                            }
                        }
                        if (!done)
                            ShowWarning("Population engine: builtin %s could not equip ammo %u after additem (population shell %u)\n",
                                ammo_ctx, (unsigned)ammo_id, index);
                    }
                }
            } else if (wid->subtype == W_BOW || wid->subtype == W_MUSICAL || wid->subtype == W_WHIP
                || wid->subtype == W_REVOLVER || wid->subtype == W_RIFLE || wid->subtype == W_GATLING
                || wid->subtype == W_SHOTGUN
#ifdef RENEWAL
                || wid->subtype == W_GRENADE
#endif
                || wid->subtype == W_HUUMA) {
                ShowWarning("Population engine: no item_db ammo for weapon subtype %u (item %u) population shell %u\n",
                    (unsigned)wid->subtype, (unsigned)weapon, index);
            }
        }
    }
    
    if (shield > 0) {
        struct item tmp_item = {};
        tmp_item.nameid = shield;
        tmp_item.amount = 1;
        tmp_item.identify = 1;
        tmp_item.equip = 0;
        enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
        if (result == ADDITEM_SUCCESS) {
            for (int16 i = 0; i < MAX_INVENTORY; i++) {
                if (sd->inventory.u.items_inventory[i].nameid == shield && sd->inventory.u.items_inventory[i].amount > 0
                    && sd->inventory.u.items_inventory[i].equip == 0) {
                    struct item_data* id = itemdb_search(shield);
                    if (id && id->equip) {
                        // pc_equipitem()->pc_set_costume_view() sets the correct
                        // VIEW id (id->look). Do NOT overwrite with the nameid.
                        (void)pc_equipitem(sd, i, id->equip, false);
                        break;
                    }
                }
            }
        }
    }
    
    // Fake Players only wear "normal" headgear — items a real player could get
    // through regular play (monster drops / NPC shops). Quest-only, event-only,
    // reward-only and account-bound headgears immediately out a character as a
    // bot, so they are filtered out here (spawn path only; global item
    // availability and monster drops are untouched).
    {
        auto headgear_ok = [](uint16_t nameid) -> bool {
            if (nameid == 0)
                return true;
            struct item_data* hid = itemdb_search(nameid);
            if (hid == nullptr)
                return false;
            if (hid->flag.trade_restriction.drop)   // cannot be dropped -> not a monster-drop item
                return false;
            if (hid->flag.trade_restriction.trade)  // untradeable -> quest / event / bound
                return false;
            if (hid->flag.bindOnEquip || hid->flag.guid)
                return false;
            return true;
        };
        if (!headgear_ok(head_top))    head_top = 0;
        if (!headgear_ok(head_mid))    head_mid = 0;
        if (!headgear_ok(head_bottom)) head_bottom = 0;
        // Middle headgear (sunglasses etc.) stays rare: clear it most of the time
        // so the mid slot is usually empty rather than filled for visual variety.
        if (head_mid > 0 && (rnd() % 100u) < 82u)
            head_mid = 0;
    }

    if (head_top > 0) {
        struct item tmp_item = {};
        tmp_item.nameid = head_top;
        tmp_item.amount = 1;
        tmp_item.identify = 1;
        tmp_item.equip = 0;
        enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
        if (result == ADDITEM_SUCCESS) {
            for (int16 i = 0; i < MAX_INVENTORY; i++) {
                if (sd->inventory.u.items_inventory[i].nameid == head_top && sd->inventory.u.items_inventory[i].amount > 0
                    && sd->inventory.u.items_inventory[i].equip == 0) {
                    struct item_data* id = itemdb_search(head_top);
                    if (id && id->equip) {
                        (void)pc_equipitem(sd, i, id->equip, false); // view id set by pc_set_costume_view
                        break;
                    }
                }
            }
        }
    }

    if (head_mid > 0) {
        struct item tmp_item = {};
        tmp_item.nameid = head_mid;
        tmp_item.amount = 1;
        tmp_item.identify = 1;
        tmp_item.equip = 0;
        enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
        if (result == ADDITEM_SUCCESS) {
            for (int16 i = 0; i < MAX_INVENTORY; i++) {
                if (sd->inventory.u.items_inventory[i].nameid == head_mid && sd->inventory.u.items_inventory[i].amount > 0
                    && sd->inventory.u.items_inventory[i].equip == 0) {
                    struct item_data* id = itemdb_search(head_mid);
                    if (id && id->equip) {
                        (void)pc_equipitem(sd, i, id->equip, false); // view id set by pc_set_costume_view
                        break;
                    }
                }
            }
        }
    }

    if (head_bottom > 0) {
        struct item tmp_item = {};
        tmp_item.nameid = head_bottom;
        tmp_item.amount = 1;
        tmp_item.identify = 1;
        tmp_item.equip = 0;
        enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
        if (result == ADDITEM_SUCCESS) {
            for (int16 i = 0; i < MAX_INVENTORY; i++) {
                if (sd->inventory.u.items_inventory[i].nameid == head_bottom && sd->inventory.u.items_inventory[i].amount > 0
                    && sd->inventory.u.items_inventory[i].equip == 0) {
                    struct item_data* id = itemdb_search(head_bottom);
                    if (id && id->equip) {
                        (void)pc_equipitem(sd, i, id->equip, false); // view id set by pc_set_costume_view
                        break;
                    }
                }
            }
        }
    }

    if (garment > 0) {
        struct item tmp_item = {};
        tmp_item.nameid = garment;
        tmp_item.amount = 1;
        tmp_item.identify = 1;
        tmp_item.equip = 0;
        enum e_additem_result result = pc_additem(sd, &tmp_item, 1, LOG_TYPE_NONE, false);
        if (result == ADDITEM_SUCCESS) {
            for (int16 i = 0; i < MAX_INVENTORY; i++) {
                if (sd->inventory.u.items_inventory[i].nameid == garment && sd->inventory.u.items_inventory[i].amount > 0
                    && sd->inventory.u.items_inventory[i].equip == 0) {
                    struct item_data* id = itemdb_search(garment);
                    if (id && id->equip) {
                        (void)pc_equipitem(sd, i, id->equip, false); // view id set by pc_set_costume_view
                        break;
                    }
                }
            }
        }
    }

    if (pop_cfg != nullptr) {
        auto pick_pool_cfg = [](const std::vector<uint16_t>& p) -> uint16_t {
            if (p.empty()) return 0;
            return p.size() == 1 ? p[0] : p[rnd() % p.size()];
        };
        population_engine_shell_equip_item(sd, pick_pool_cfg(pop_cfg->armor_pool),   index, "armor");
        population_engine_shell_equip_item(sd, pick_pool_cfg(pop_cfg->shoes_pool),   index, "shoes");
        population_engine_shell_equip_item(sd, pick_pool_cfg(pop_cfg->acc_l_pool),   index, "acc_l", EQP_ACC_L);
        population_engine_shell_equip_item(sd, pick_pool_cfg(pop_cfg->acc_r_pool),   index, "acc_r", EQP_ACC_R);
    }

    // Stock consumable trap items for jobs that use trap skills.
    // Hunter/Sniper use Booby_Trap (1065); Ranger uses Special_Alloy_Trap (7940);
    // Genetic uses Seed_Of_Horny_Plant (6210).  Amount is generous to avoid running out.
    {
        struct { t_itemid nameid; int amount; } trap_stock[3] = {};
        int trap_count = 0;
        const bool is_hunter_line = (job_id == JOB_HUNTER || job_id == JOB_SNIPER
            || job_id == JOB_RANGER || job_id == JOB_RANGER_T);
        const bool is_ranger = (job_id == JOB_RANGER || job_id == JOB_RANGER_T);
        const bool is_genetic = (job_id == JOB_GENETIC || job_id == JOB_GENETIC_T);
        if (is_hunter_line) {
            trap_stock[trap_count++] = { 1065, 500 };  // Booby_Trap
        }
        if (is_ranger) {
            trap_stock[trap_count++] = { 7940, 500 };  // Special_Alloy_Trap
        }
        if (is_genetic) {
            trap_stock[trap_count++] = { 6210, 500 };  // Seed_Of_Horny_Plant
        }
        for (int ti = 0; ti < trap_count; ti++) {
            struct item tmp_item = {};
            tmp_item.nameid   = trap_stock[ti].nameid;
            tmp_item.amount   = 1;
            tmp_item.identify = 1;
            tmp_item.equip    = 0;
            pc_additem(sd, &tmp_item, trap_stock[ti].amount, LOG_TYPE_NONE, false);
        }
    }

    // Mark as active before run_script / status_calc_pc so script commands that
    // check sd->state.active (e.g. setriding, setarrow) execute correctly.
    sd->base_status.mode = static_cast<e_mode>(MD_CANMOVE | MD_CANATTACK);
    sd->state.active = 1;
    sd->state.pc_loaded = true;

    // Raise carry limit before status_calc_pc so setriding / setarrow do not fail.
    if (sd->max_weight <= 0 || sd->max_weight < 10000)
        sd->max_weight = 8000 + (sd->status.str * 300);

    // Run the full Script: once to execute side-effect commands (setriding, setfalcon, etc.).
    if (init_script != nullptr)
        run_script(init_script, 0, sd->id, fake_nd->id);

    // Register the filtered Script: (side-effect commands stripped) as a persistent
    // bonus_script so bonus commands survive every future status_calc_pc call.
    if (pop_cfg != nullptr && !pop_cfg->bonus_script_str.empty()) {
        struct s_bonus_script_entry* bse = pc_bonus_script_add(
            sd,
            pop_cfg->bonus_script_str.c_str(),
            static_cast<t_tick>(86400000LL) * 365 * 10,  // ~10-year "permanent" duration
            EFST_BLANK,
            BSF_PERMANENT,
            0);
        if (bse != nullptr)
            linkdb_insert(&sd->bonus_script.head, (void*)((intptr_t)bse), bse);
    }

    // Single status_calc_pc: rebuilds item bonuses and re-runs all bonus_script entries.
    status_calc_pc(sd, SCO_NONE);

    // Restore full SP after recalc (base SP formula for shells is often 0).
    if (sd->battle_status.max_sp > 0) {
        sd->status.sp = sd->battle_status.max_sp;
        sd->battle_status.sp = sd->battle_status.max_sp;
    }

    // Re-check carry limit after recalc in case it was overwritten.
    if (sd->max_weight <= 0 || sd->max_weight < 10000)
        sd->max_weight = 8000 + (sd->status.str * 300);
    population_engine_finalize_shell_appearance(sd);

    // Elysium stress_test fake PCs: sync paper doll to the map before spawn so observers match stock AC shells.
    clif_changelook(sd, LOOK_BASE, sd->vd.look[LOOK_BASE]);
    clif_changelook(sd, LOOK_HAIR, sd->vd.look[LOOK_HAIR]);
    clif_changelook(sd, LOOK_HAIR_COLOR, sd->vd.look[LOOK_HAIR_COLOR]);
    clif_changelook(sd, LOOK_CLOTHES_COLOR, sd->vd.look[LOOK_CLOTHES_COLOR]);
    clif_changelook(sd, LOOK_WEAPON, sd->vd.look[LOOK_WEAPON]);
    clif_changelook(sd, LOOK_SHIELD, sd->vd.look[LOOK_SHIELD]);
    clif_changelook(sd, LOOK_HEAD_TOP, sd->vd.look[LOOK_HEAD_TOP]);
    clif_changelook(sd, LOOK_HEAD_MID, sd->vd.look[LOOK_HEAD_MID]);
    clif_changelook(sd, LOOK_HEAD_BOTTOM, sd->vd.look[LOOK_HEAD_BOTTOM]);
    clif_changelook(sd, LOOK_ROBE, sd->vd.look[LOOK_ROBE]);
    clif_changeoption(sd);

    if (!population_engine_combat_try_start(sd)) {
        population_engine_shell_release(sd);
        return nullptr;
    }

    clif_spawn(sd);

    // Store map category, spawn position, and resolved behavior for the combat tick.
    sd->pop.map_category = map_category;
    sd->pop.spawn_x      = static_cast<int16_t>(x);
    sd->pop.spawn_y      = static_cast<int16_t>(y);
    sd->pop.spawn_map_id = map_id;

    // Resolve effective behavior: per-category override wins over the profile default.
    PopulationBehavior pe_beh = (pop_cfg != nullptr) ? pop_cfg->behavior : PopulationBehavior::Combat;
    if (pop_cfg != nullptr) {
        PopulationBehavior override_beh = PopulationBehavior::None;
        if (map_category == 1) override_beh = pop_cfg->town_behavior;
        else if (map_category == 2) override_beh = pop_cfg->field_behavior;
        else if (map_category == 3) override_beh = pop_cfg->dungeon_behavior;
        if (override_beh != PopulationBehavior::None)
            pe_beh = override_beh;
    }
    // Explicit caller override (e.g. @populate vending forces Vendor). Wins over
    // the profile and skips the random town-personality roll further below.
    if (behavior_override != PopulationBehavior::None)
        pe_beh = behavior_override;

    sd->pop.behavior = static_cast<uint8_t>(pe_beh);
    sd->pop.behavior_base = static_cast<uint8_t>(pe_beh);
    sd->pop.flags    = ((pop_cfg != nullptr) ? pop_cfg->flags : 0u) | PSF::CombatActive;
    sd->pop.role     = (pop_cfg != nullptr) ? static_cast<int8_t>(pop_cfg->role_type) : 0;
    sd->pop.db_source = static_cast<uint8_t>(db_source);
    sd->pop.arena_team = 0; // not in arena by default

    // ---- Class-based AI role overrides (apply to manual AND auto shells) ----
    // These make the shell behave like its class no matter what the profile's
    // Role:/Movetype:/Flags: say, so Acolyte/Priest never brawl and Mage never
    // melees by default.
    {
        const uint64 cm   = static_cast<uint64>(sd->class_);
        const uint64 base = cm & MAPID_BASEMASK;
        if (base == MAPID_ACOLYTE && !(cm & JOBL_2_2)) {
            // Acolyte / Priest / High Priest (NOT Monk/Champion): pure SUPPORT.
            // The runtime target gate rejects every non-undead monster; offensive
            // Heal vs undead is the only attack they make.
            sd->pop.role = static_cast<int8_t>(PopulationRoleType::Support);
        } else if (base == MAPID_MAGE) {
            // Mage / Wizard / High Wizard: ranged spellcaster. SkillOnly bypasses
            // population_engine_shell_basic_attack_chance so they cast spells and
            // only melee as a genuine last resort (handled by the skill picker).
            sd->pop.flags |= PSF::SkillOnly;
            if (cm & JOBL_2_2) {
                // Sage / Professor are BATTLE type only — never support. Force the
                // Attacker role so no profile can turn them into a buff-bot; the
                // skill DB rotation for these jobs is offensive-first and normal
                // attack is only the picker's last resort.
                sd->pop.role = static_cast<int8_t>(PopulationRoleType::Attacker);
            }
        }
    }

    // ---- Town personality: not everyone wanders ----------------------------
    // Only for town shells whose profile just says the generic "wander" default
    // (vendor-pop profiles keep TownBehavior: vendor). Weighted roll:
    //   ~42% Sit/idle (AFK), ~15% Social, ~13% Vendor (real vend), ~30% ShortWalk.
    // town_phase_until desyncs the sit<->stroll cycle so no two shells sit for the
    // same time and they never change phase together.
    if (map_category == 1 && pe_beh == PopulationBehavior::Wander) {
        const bool is_merchant = ((static_cast<uint64>(sd->class_) & MAPID_BASEMASK) == MAPID_MERCHANT);
        const uint32_t troll = rnd() % 100u;
        if      (troll < 40)                 pe_beh = PopulationBehavior::Sit;
        else if (troll < 58)                 pe_beh = PopulationBehavior::Social;
        else if (troll < 72 && is_merchant)  pe_beh = PopulationBehavior::Vendor; // real buyable vend
        else if (troll < 72)                 pe_beh = PopulationBehavior::Sit;     // non-merchant: sit instead
        else                                 pe_beh = PopulationBehavior::Wander;  // short purposeful walk
        sd->pop.behavior      = static_cast<uint8_t>(pe_beh);
        sd->pop.behavior_base = static_cast<uint8_t>(pe_beh);
        const t_tick tnow = gettick();
        if (pe_beh == PopulationBehavior::Sit) {
            // Spawn STANDING. Keep Sit as the personality (behavior_base) but run
            // as a normal town shell until the per-shell cycler seats it. Each
            // shell gets an independent short delay so they never sit in unison
            // and none inherit a shared sitting state.
            sd->pop.behavior = static_cast<uint8_t>(PopulationBehavior::Wander);
            sd->pop.town_phase_until = tnow + static_cast<t_tick>(4000u + rnd() % 56000u); // sits within ~1 min
        }
        else if (pe_beh == PopulationBehavior::Wander)
            sd->pop.town_phase_until = tnow + static_cast<t_tick>(40000u + rnd() % 170000u);  // ~40 s–3.5 min strolling
        else if (pe_beh == PopulationBehavior::Social)
            sd->pop.town_phase_until = tnow + static_cast<t_tick>(60000u + rnd() % 240000u);  // stroll/chat 1–5 min, then sit
        // Vendor keeps its own behaviour (opens a vend, no sit<->stroll cycling).
    }

    // Per-shell chat personality weight (0-100). Heavily skewed toward "quiet"
    // so the world is mostly silent — real players do not chatter on a timer.
    // Deterministic-ish from the id so a shell keeps the same personality.
    //   ~72% quiet      (2-8)   -> almost never speaks
    //   ~24% normal     (10-22) -> occasional short lines
    //   ~4%  talkative  (22-34) -> still infrequent, never spammy
    {
        const uint32_t r = (index * 2654435761u) % 100u;
        uint8_t chat_w;
        if      (r < 72) chat_w = static_cast<uint8_t>(2  + (r % 7));    // quiet
        else if (r < 96) chat_w = static_cast<uint8_t>(10 + (r % 13));   // normal
        else             chat_w = static_cast<uint8_t>(22 + (r % 13));   // talkative
        sd->pop.chattiness = chat_w;
    }
    sd->pop.last_chat_hash = 0;
    // NOTE: shells must NOT be given a synthetic status.party_id.  A non-zero
    // party_id that has no matching party_data in the party db makes dozens of
    // vanilla rAthena code paths call party_search(party_id) -> nullptr and then
    // dereference it (e.g. party_getavailablesd(nullptr) -> party.cpp SIGSEGV).
    // On a release build (-O2, no DEBUG) the nullpo_* guards are compiled out, so
    // this is a hard crash whenever a shell's AI touches a party code path.
    // Shell-to-shell ally heals/buffs work via s_state::population_combat detection
    // in the AI scan callbacks and do not need a party at all.  Keep party_id = 0.
    sd->status.party_id = 0;

    if (pe_beh == PopulationBehavior::Combat || pe_beh == PopulationBehavior::Guard) {
        const PopulationCombatStartResult ac = population_engine_combat_start_session(sd, PopulationCombatStartMode::AutoCombat, -1, SCSTART_NOAVOID | SCSTART_LOADED);
        if (!ac.started) {
            ShowWarning("Population engine: population combat replica did not start for shell '%s' (reject=%s); bot will wander/chat only. "
                "With Behavior: combat/guard, skills and targeting come from db/autocombat_config.yml (not db/population_engine.yml).\n",
                sd->status.name, population_combat_reject_code_name(ac.reject_code));
        }
    }

    // Send equipment list to own client only (fake PCs usually have fd<=0).
    if (sd->fd > 0) {
        clif_equiplist(sd);
    }

    // Clear act/move locks so autocombat + wander timers can run immediately after spawn.
    sd->ud.canact_tick = 0;
    sd->ud.canmove_tick = 0;
    sd->ud.skilltimer = INVALID_TIMER;

	population_engine_register_shell_chat_state(sd, pop_cfg);
	if (pe_beh == PopulationBehavior::Combat    ||
	    pe_beh == PopulationBehavior::Wander    ||
	    pe_beh == PopulationBehavior::Support   ||
	    pe_beh == PopulationBehavior::Social ||
	    pe_beh == PopulationBehavior::Sit)   // Sit-personality shells spawn standing and are cycled by the wander timer
		population_engine_register_shell_wander_state(sd);

	// Behavior-specific startup.
	if (pe_beh == PopulationBehavior::Sit) {
		// Do NOT sit at spawn — the shell spawns standing and the per-shell town
		// cycler (population_engine_path.cpp) seats it independently later.
	} else if (pe_beh == PopulationBehavior::Vendor) {
		pc_setsit(sd);
		clif_sitting(*sd);
		// Fake vending merchants are silent: no overhead "vendor_message" announce,
		// no ambient / reply chat (see population_engine_chat_timer /
		// _deliver_chat_reply_locked). The shop title is shown by the vend board
		// itself, which is not chat.

		// Vending economy: stock cart and open a real vend.
		if (battle_config.population_engine_vending_enable) {
			// Look up optional per-job vendor config from population_vendors.yml.
			// An explicit vendor_key_override (from @populate vending) wins over
			// the profile's VendorKey.
			const PopulationVendorEntry *vendor_cfg = nullptr;
			std::string vkey = (vendor_key_override && vendor_key_override[0])
				? std::string(vendor_key_override)
				: (pop_cfg ? pop_cfg->vendor_key : std::string());
			if (!vkey.empty())
				vendor_cfg = population_vendor_db().find(vkey);

			// Vend title. When the vendor config asks for randomized shop names
			// (default for RandomizeStock entries) we generate a casual,
			// player-style name AFTER the stock is built so a few can riff on
			// what is actually being sold. Otherwise: vendor_cfg Title >
			// VendorMessage > "Shop".
			std::string title_buf;
			const char *vend_title = "Shop";
			if (vendor_cfg && !vendor_cfg->title.empty())
				vend_title = vendor_cfg->title.c_str();
			else if (pop_cfg && !pop_cfg->vendor_message.empty())
				vend_title = pop_cfg->vendor_message.c_str();

			// Build the stock list to use.
			// Priority: static vendor_cfg stock → dynamic (map mob drops) → built-in defaults.
			struct TmpStock { t_itemid nameid; int16 amount; uint32_t price_override; };
			std::vector<TmpStock> stock;
			const int max_slots = vendor_cfg ? vendor_cfg->max_slots : 12;

			if (vendor_cfg && !vendor_cfg->dynamic && !vendor_cfg->stock.empty()
			    && !vendor_cfg->randomize_stock) {
				// Static vending (legacy): use exactly the YAML-defined stock.
				for (const auto &vs : vendor_cfg->stock)
					stock.push_back({ vs.nameid, vs.amount, vs.price });

			} else if (vendor_cfg && !vendor_cfg->dynamic && !vendor_cfg->stock.empty()
			           && vendor_cfg->randomize_stock) {
				// RandomizeStock: Stock: is a candidate POOL. Roll a unique-looking
				// shop for THIS vendor — item subset, per-item quantity and price —
				// so 100 vendors on the same key all look player-made. The 8000
				// Pushcart weight cap is still enforced by the cart-add loop below.
				const auto &pool = vendor_cfg->stock;
				const int pool_n = static_cast<int>(pool.size());

				// "Bulk specialist": fewer item types, bigger stacks.
				const bool specialist = (vendor_cfg->specialty_pct > 0) &&
					(static_cast<int>(rnd() % 100u) < vendor_cfg->specialty_pct);

				int lo = vendor_cfg->stock_items_min > 0 ? vendor_cfg->stock_items_min : 3;
				int hi = vendor_cfg->stock_items_max > 0 ? vendor_cfg->stock_items_max
				                                         : std::min(pool_n, max_slots);
				lo = std::max(1, std::min(lo, pool_n));
				hi = std::max(lo, std::min(std::min(hi, pool_n), max_slots));
				int want_items = specialist
					? (1 + static_cast<int>(rnd() % 3u))
					: (lo + static_cast<int>(rnd() % static_cast<uint32_t>(hi - lo + 1)));
				want_items = std::max(1, std::min(std::min(want_items, pool_n), max_slots));

				// Weighted, no-repeat selection of `want_items` pool indices.
				std::vector<int> remaining;
				remaining.reserve(pool_n);
				for (int i = 0; i < pool_n; ++i) remaining.push_back(i);
				std::vector<int> chosen;
				chosen.reserve(want_items);
				while (static_cast<int>(chosen.size()) < want_items && !remaining.empty()) {
					long total_w = 0;
					for (int idx : remaining)
						total_w += std::max<uint16_t>(1, pool[idx].pick_weight);
					long roll = static_cast<long>(rnd() % static_cast<uint32_t>(std::max<long>(1, total_w)));
					size_t pick = 0;
					for (; pick < remaining.size(); ++pick) {
						roll -= std::max<uint16_t>(1, pool[remaining[pick]].pick_weight);
						if (roll < 0) break;
					}
					if (pick >= remaining.size()) pick = remaining.size() - 1;
					chosen.push_back(remaining[pick]);
					remaining.erase(remaining.begin() + static_cast<long>(pick));
				}

				const int pv = vendor_cfg->price_variance_pct;
				for (int idx : chosen) {
					const auto &vs = pool[idx];
					std::shared_ptr<item_data> pid = item_db.find(vs.nameid);
					if (!pid) continue;
					const bool is_eq = (pid->equip != 0);

					int amt = 1;
					if (!is_eq) {
						int qlo = vs.amount_min > 0 ? vs.amount_min : std::max(1, vs.amount / 4);
						int qhi = vs.amount_max > 0 ? vs.amount_max : std::max(qlo, static_cast<int>(vs.amount));
						if (qhi < qlo) qhi = qlo;
						amt = qlo + static_cast<int>(rnd() % static_cast<uint32_t>(qhi - qlo + 1));
						if (specialist)
							amt = std::min(30000, amt + amt / 2 + static_cast<int>(rnd() % static_cast<uint32_t>(qhi + 1)));
						amt = std::max(1, std::min(30000, amt));
					}

					uint32_t base = vs.price > 0 ? vs.price
						: static_cast<uint32_t>(pid->value_buy > 0 ? pid->value_buy : 100);
					uint32_t price = base;
					if (pv > 0 && base > 0) {
						const int delta = static_cast<int>(rnd() % static_cast<uint32_t>(2 * pv + 1)) - pv;
						long p = static_cast<long>(base) + static_cast<long>(base) * delta / 100;
						price = static_cast<uint32_t>(p < 1 ? 1 : p);
					}
					stock.push_back({ vs.nameid, static_cast<int16>(amt), price });
				}

			} else if (vendor_cfg && vendor_cfg->dynamic) {
				// Dynamic vending: derive saleable items from mob drop tables across one or
				// more *source* maps (NOT the spawn map — towns have empty moblist[]).
				// Map selection priority:
				//   1) explicit SourceMaps list
				//   2) auto-discovery from population_spawn_db() filtered by SourceCategory
				//   3) legacy: spawn map only
				//
				// Both the source-map list and the per-map drop pool are cached in
				// g_pop_vendor_dyn_cache (cleared on YAML reload). This avoids
				// re-walking every map's moblist on every shell spawn.
				const uint32_t pct = vendor_cfg->price_multiplier;
				using DropEntry = PopVendorDropEntry;
				std::vector<DropEntry> drop_pool;
				std::unordered_map<t_itemid, uint32_t> seen;

				std::vector<int16> source_mids;
				{
					PopVendorCacheBucket &bucket = g_pop_vendor_dyn_cache[vendor_cfg->key];
					if (!bucket.source_mids_built) {
						if (!vendor_cfg->source_maps.empty()) {
							for (const std::string& mn : vendor_cfg->source_maps) {
								const int16 mid = map_mapname2mapid(mn.c_str());
								if (mid >= 0) bucket.source_mids.push_back(mid);
							}
						} else if (!vendor_cfg->source_category.empty()) {
							const bool want_dungeons = (vendor_cfg->source_category == "dungeon" || vendor_cfg->source_category == "both");
							const bool want_fields   = (vendor_cfg->source_category == "field"   || vendor_cfg->source_category == "both");
							std::unordered_set<int16> seen_mids;
							for (auto sit = population_spawn_db().begin(); sit != population_spawn_db().end(); ++sit) {
								if (!sit->second) continue;
								const PopulationSpawnEntry &se = *sit->second;
								auto add = [&](const std::vector<std::string> &maps) {
									for (const std::string &mn : maps) {
										const int16 mid = map_mapname2mapid(mn.c_str());
										if (mid >= 0 && seen_mids.insert(mid).second)
											bucket.source_mids.push_back(mid);
									}
								};
								if (want_dungeons) add(se.dungeons);
								if (want_fields)   add(se.fields);
							}
						} else {
							bucket.source_mids.push_back(sd->m); // legacy: spawn map only
						}
						bucket.source_mids_built = true;
					}
					source_mids = bucket.source_mids; // copy out; release lock below
				}

				// RandomizePerShell: shuffle the source list so each shell starts
				// from a different "primary" map. We still walk additional maps
				// (in shuffled order) until drop_pool reaches max_slots, so a
				// dungeon with few drops is topped up from sibling dungeons.
				if (vendor_cfg->randomize_per_shell && source_mids.size() > 1) {
					for (size_t i = source_mids.size() - 1; i > 0; --i) {
						const size_t j = static_cast<size_t>(rnd()) % (i + 1);
						std::swap(source_mids[i], source_mids[j]);
					}
				}

				// Helper: build (or fetch from cache) the drop pool for one source map.
				auto get_mid_pool = [&](int16 src_mid) -> const std::vector<DropEntry>& {
					PopVendorCacheBucket &bucket = g_pop_vendor_dyn_cache[vendor_cfg->key];
					auto it = bucket.drops_by_mid.find(src_mid);
					if (it != bucket.drops_by_mid.end()) return it->second;
					std::vector<DropEntry> built;
					struct map_data *src_map = map_getmapdata(src_mid);
					if (src_map) {
						std::unordered_map<t_itemid, uint32_t> local_seen;
						for (int i = 0; i < MAX_MOB_LIST_PER_MAP; ++i) {
							if (!src_map->moblist[i]) continue;
							const int16 mob_id = src_map->moblist[i]->id;
							if (mob_id <= 0) continue;
							auto mdb = mob_db.find(static_cast<uint32>(mob_id));
							if (!mdb) continue;
							for (const auto &drop : mdb->dropitem) {
								if (!drop || drop->nameid == 0) continue;
								auto idata = item_db.find(drop->nameid);
								if (!idata) continue;
								// Apply ItemFlags filters at cache time (filters are part of the vendor key).
								const bool is_equip = (idata->equip != 0);
								if (is_equip && !vendor_cfg->allow_equipment) continue;
								if (idata->type == IT_CARD       && !vendor_cfg->allow_cards)  continue;
								if (idata->type == IT_ETC        && !vendor_cfg->allow_etc)    continue;
								if ((idata->type == IT_HEALING ||
								     idata->type == IT_USABLE  ||
								     idata->type == IT_DELAYCONSUME) && !vendor_cfg->allow_usable) continue;
								auto sit2 = local_seen.find(drop->nameid);
								if (sit2 == local_seen.end()) {
									local_seen[drop->nameid] = drop->rate;
									built.push_back({ drop->nameid, drop->rate });
								} else if (drop->rate > sit2->second) {
									sit2->second = drop->rate;
								}
							}
						}
					}
					auto inserted = bucket.drops_by_mid.emplace(src_mid, std::move(built));
					return inserted.first->second;
				};

				// Aggregate drops from selected source maps (cached lookups).
				// We collect ~6x max_slots so the "top by drop rate" sort below
				// has a meaningful pool to pick from AND the 2x overflow stock
				// builder still has spares after rate-sorting. When randomize_
				// per_shell is set, source_mids was shuffled above so the primary
				// dungeon contributes first and siblings only top-up the deficit.
				const size_t collect_target = static_cast<size_t>(std::max(max_slots * 6, 60));
				for (int16 src_mid : source_mids) {
					if (drop_pool.size() >= collect_target) break;
					const std::vector<DropEntry> &mid_pool = get_mid_pool(src_mid);
					for (const auto &de : mid_pool) {
						if (drop_pool.size() >= collect_target) break;
						auto sit2 = seen.find(de.nameid);
						if (sit2 == seen.end()) {
							seen[de.nameid] = de.rate;
							drop_pool.push_back(de);
						} else if (de.rate > sit2->second) {
							sit2->second = de.rate;
						}
					}
				}

				// Sort by drop rate descending, take best max_slots items.
				std::sort(drop_pool.begin(), drop_pool.end(),
					[](const DropEntry &a, const DropEntry &b) { return a.rate > b.rate; });

				// MaxAmount: optional [lo,hi] range to randomize per-item stack size.
				const int dyn_lo = vendor_cfg->dyn_amount_min > 0 ? vendor_cfg->dyn_amount_min : 30;
				const int dyn_hi = vendor_cfg->dyn_amount_max > 0 ? vendor_cfg->dyn_amount_max : 30;

				// Produce a 2x overflow so the cart-add loop below has spares
				// when pc_cart_additem rejects items (NoTrade / NoCart / equipment
				// type restrictions / etc.). Without overflow a vend that loses
				// even one item to a failed add ends up below MaxSlots.
				const int stock_cap = max_slots * 2;
				int count = 0;
				for (const auto &de : drop_pool) {
					if (count >= stock_cap) break;
					auto idata = item_db.find(de.nameid);
					const uint32_t sell_val = idata ? static_cast<uint32_t>(idata->value_sell) : 0;
					const uint32_t price = std::max<uint32_t>(1, sell_val * pct / 100);
					int16 amt;
					if (idata && idata->equip != 0) {
						amt = 1; // equipment is non-stackable
					} else if (dyn_hi > dyn_lo) {
						amt = static_cast<int16>(dyn_lo + (rnd() % (dyn_hi - dyn_lo + 1)));
					} else {
						amt = static_cast<int16>(dyn_lo);
					}
					stock.push_back({ de.nameid, amt, price });
					++count;
				}
				// Fallthrough to built-in defaults if no source maps resolved any drops.
				if (stock.empty())
					vendor_cfg = nullptr; // force built-in below

			}

			if (!vendor_cfg || stock.empty()) {
				// Built-in default consumable stock.
				static const TmpStock kDefaultStock[] = {
					{ 501, 100, 0 },  // Red Potion
					{ 502, 100, 0 },  // Orange Potion
					{ 503,  50, 0 },  // Yellow Potion
					{ 504,  30, 0 },  // White Potion
					{ 506,  50, 0 },  // Green Potion
					{ 601,  50, 0 },  // Wing of Fly
					{ 602,  30, 0 },  // Wing of Butterfly
					{ 605,  50, 0 },  // Anodyne
					{ 606,  50, 0 },  // Aloevera
					{ 645,  20, 0 },  // Concentration Potion
					{ 656,  20, 0 },  // Awakening Potion
				};
				for (const auto &s : kDefaultStock)
					stock.push_back(s);
			}

			// Player-style shop name (after stock is known so a few can reference it).
			if (vendor_cfg && vendor_cfg->randomize_shop_name) {
				const t_itemid sample = stock.empty() ? 0
					: stock[rnd() % stock.size()].nameid;
				title_buf = population_vendor_gen_shop_name(sd->status.name, sample);
				if (!title_buf.empty())
					vend_title = title_buf.c_str();
			}

			// Grant MC_VENDING and cart.
			pc_skill(sd, MC_VENDING, 10, ADDSKILL_PERMANENT_GRANTED);
			pc_setcart(sd, 1);

			// Population-engine fake vendors: the TOTAL Pushcart weight must never
			// exceed 8000 (independently, per vendor). This is a restriction on
			// fake vendors only — this whole block runs solely inside the shell
			// spawn path and never for a real player. The rAthena weight formula
			// is item_data.weight * amount, accumulated into sd->cart_weight by
			// pc_cart_additem(); we gate on that BEFORE every add so the cart is
			// built correctly from the start (no post-hoc truncation).
			constexpr int32 kPopVendorCartCap = 8000;
			sd->cart_weight_max = kPopVendorCartCap; // pc_cart_additem's own guard also honours this

			// Stock cart and build vending data.
			int vend_count = 0;
			uint8 vend_data[MAX_VENDING * 8];
			memset(vend_data, 0, sizeof(vend_data));

			for (const auto &vs : stock) {
				if (vend_count >= max_slots) break;

				std::shared_ptr<item_data> id = item_db.find(vs.nameid);
				if (!id) continue;

				// Equipment is non-stackable: only 1 unit can occupy a cart slot,
				// so cap amount to 1 to avoid failed pc_cart_additem or bogus vend counts.
				const bool is_equip = (id->equip != 0);
				int16 slot_amount = is_equip ? 1 : static_cast<int16>(vs.amount);

				// rAthena weight = item_data.weight * amount, accumulated in
				// sd->cart_weight by pc_cart_additem. Keep the Pushcart <= 8000:
				// first try to REDUCE the quantity to what still fits; only if not
				// even one unit fits, skip the item and keep scanning the list.
				const int64 unit_w    = static_cast<int64>(id->weight);
				const int64 remaining = static_cast<int64>(kPopVendorCartCap) - static_cast<int64>(sd->cart_weight);
				if (unit_w > 0 && unit_w * static_cast<int64>(slot_amount) > remaining) {
					const int64 fit = remaining / unit_w; // floor
					if (fit < 1)
						continue;               // nothing fits -> skip this item
					if (is_equip)
						continue;               // equipment can't be partial
					slot_amount = static_cast<int16>(std::min<int64>(fit, 30000));
				}

				struct item tmp_item = {};
				tmp_item.nameid = vs.nameid;
				tmp_item.amount = 1;
				tmp_item.identify = 1;
				if (pc_cart_additem(sd, &tmp_item, slot_amount, LOG_TYPE_NONE) != ADDITEM_SUCCESS)
					continue;

				int cart_idx = -1;
				for (int ci = 0; ci < MAX_CART; ci++) {
					if (sd->cart.u.items_cart[ci].nameid == vs.nameid) {
						cart_idx = ci;
						break;
					}
				}
				if (cart_idx < 0) continue;

				// price_override 0 = use item buy price (prevents sell-back arbitrage).
				uint32 price = vs.price_override > 0
					? vs.price_override
					: static_cast<uint32>(id->value_buy > 0 ? id->value_buy : 100);

				// Fake Vendor pricing rule: POTIONS / USABLE CONSUMABLES sold by a
				// merchant-class vendor that HAS the Discount skill (MC_DISCOUNT)
				// are priced 10% below the item's actual configured NPC shop
				// (buy) price. Uses item_data.value_buy — no hardcoded prices.
				// Equipment, materials and every other item type keep the price
				// computed above (randomized / dynamic / YAML).
				if ((id->type == IT_HEALING || id->type == IT_USABLE || id->type == IT_DELAYCONSUME)
				    && id->value_buy > 0
				    && pc_checkskill(sd, MC_DISCOUNT) > 0) {
					price = std::max<uint32>(1, static_cast<uint32>(id->value_buy) * 90u / 100u);
				}

				*(uint16*)(vend_data + vend_count * 8 + 0) = static_cast<uint16>(cart_idx + 2);
				*(uint16*)(vend_data + vend_count * 8 + 2) = static_cast<uint16>(slot_amount);
				*(uint32*)(vend_data + vend_count * 8 + 4) = price;
				vend_count++;
			}

			// Validate the final Pushcart weight before opening the vend (req: <= 8000).
			if (sd->cart_weight > kPopVendorCartCap) {
				ShowWarning("Population engine: fake vendor %u cart weight %d exceeds cap %d — trimming.\n",
					sd->id, sd->cart_weight, kPopVendorCartCap);
				// Drop trailing vend slots until within cap (defensive; the per-item
				// gate above should already keep us at/under 8000).
				while (sd->cart_weight > kPopVendorCartCap && vend_count > 0) {
					--vend_count;
					const int drop_idx = *(uint16*)(vend_data + vend_count * 8 + 0) - 2;
					if (drop_idx >= 0 && drop_idx < MAX_CART &&
					    sd->cart.u.items_cart[drop_idx].nameid != 0) {
						struct item drop_it = sd->cart.u.items_cart[drop_idx];
						pc_cart_delitem(sd, drop_idx, drop_it.amount, 0, LOG_TYPE_NONE);
					}
				}
			}

			if (vend_count > 0) {
				sd->state.prevend = 1;
				vending_openvending(*sd, vend_title, vend_data, vend_count, nullptr);
			}
		}
	}

	// AutoCombat shells: hand the shell to the REAL autocombat.cpp loop. This
	// seeds sd->ac from the learned skill tree + built-in per-class defaults and
	// starts SC_AUTOCOMBAT (unbounded). The population-engine AI never touches
	// these shells (sd->state.population_combat stays false).
	if (pe_beh == PopulationBehavior::AutoCombat)
		autocombat_shell_start(sd);

    return sd;
}

// Generate bot name — used as last-resort fallback.
// When population_engine_name_bot_fallback=0 (off) produces a deterministic syllable name from
// a built-in 22×22×22 table (10 648 unique combos) so bots never display as "Bot_<id>".
// When population_engine_name_bot_fallback=1 (on) falls back to the classic "Bot_<id>" format.
// ---------------------------------------------------------------------------
// Casual, player-style vend shop name. Deliberately varied — trader shorthand,
// lazy/afk, money-motivated, funny, loosely item-flavoured, name-owned,
// casual-descriptive — so a townful of fake vendors does NOT read as
// "[Potions]" x100. `sample_item` (0 = none) lets a few names riff on what is
// actually being sold.
static std::string population_vendor_gen_shop_name(const char* owner_name, t_itemid sample_item)
{
	auto pick = [](std::initializer_list<const char*> l) -> const char* {
		const char* arr[20];
		int n = 0;
		for (const char* s : l) if (n < 20) arr[n++] = s;
		return n > 0 ? arr[rnd() % static_cast<uint32_t>(n)] : "";
	};

	// Short lower-case tag from a sold item's display name (first ~16 chars).
	std::string tag;
	if (sample_item != 0) {
		if (std::shared_ptr<item_data> sid = item_db.find(sample_item)) {
			std::string s = !sid->ename.empty() ? sid->ename : sid->name;
			for (char& c : s) {
				c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
				if (c == '_') c = ' ';
			}
			if (s.size() > 16) s.resize(16);
			tag = s;
		}
	}
	const std::string owner = owner_name ? owner_name : "";

	char buf[64];
	const uint32_t style = rnd() % 100u;

	if (style < 15) {                                   // trader shorthand
		if (!tag.empty() && (rnd() & 1))
			snprintf(buf, sizeof(buf), "%s %s", pick({ "s>", "b>", "wts", "s>", "s> cheap", "selling" }), tag.c_str());
		else
			snprintf(buf, sizeof(buf), "%s", pick({ "s> stuff", "wts misc", "b> zeny", "s> everything",
			                                        "s>", "clearance", "quick sale", "s> junk", "wtb pots" }));
	} else if (style < 28) {                            // lazy / afk
		snprintf(buf, sizeof(buf), "%s", pick({ "afk", "brb", "just afk", "cr", "zzz", "...", "asdf",
		                                        "afk vend", "bio lol", "one sec", "away", "afk 10m" }));
	} else if (style < 43) {                            // money-motivated
		snprintf(buf, sizeof(buf), "%s", pick({ "need zeny", "leveling fund", "new char fund", "broke",
		                                        "rent money", "pls buy", "funding my gear", "zeny plz",
		                                        "almost broke", "saving up", "gear fund" }));
	} else if (style < 58) {                            // funny / personal
		snprintf(buf, sizeof(buf), "%s", pick({ "take my junk", "everything must go", "dont judge prices",
		                                        "overpriced sry", "blame the market", "found this stuff",
		                                        "loot dump", "my stash", "<3", "impulse buys", "garage sale",
		                                        "certified fresh" }));
	} else if (style < 76 && !tag.empty()) {            // loosely item-flavoured
		snprintf(buf, sizeof(buf), pick({ "%s and more", "got %s", "%s restock", "mostly %s",
		                                  "%s + misc", "%s here", "fresh %s", "cheap %s" }), tag.c_str());
	} else if (style < 90 && !owner.empty()) {          // name-owned
		snprintf(buf, sizeof(buf), pick({ "%s's shop", "%s's corner", "%s's leftovers", "%s's stash",
		                                  "%s's picks", "%s mart" }), owner.c_str());
	} else {                                            // casual-descriptive
		snprintf(buf, sizeof(buf), "%s", pick({ "random stuff", "misc goods", "odds n ends", "bits and bobs",
		                                        "general goods", "stuff i found", "this and that", "various",
		                                        "assorted", "adventurer supplies", "travel gear" }));
	}
	if (buf[0] == '\0')
		snprintf(buf, sizeof(buf), "%s", "s> stuff");
	return std::string(buf);
}

static std::string generate_bot_name(uint32_t index) {
	extern struct Battle_Config battle_config;
	if (battle_config.population_engine_name_bot_fallback) {
		char name[NAME_LENGTH];
		snprintf(name, sizeof(name), "Bot_%u", index);
		return std::string(name);
	}
	// Fallback-off: deterministic 6-char syllable name, no YAML dependency.
	// 22 × 22 × 22 = 10 648 unique names; wraps only beyond that.
	static const char* const s_start[] = {
		"Ka","Ve","To","Mi","Lu","Ra","Se","No","Di","Fy",
		"Xe","Ja","Hi","Wu","Bl","Cr","Dr","Fr","Gl","Pr","St","Vy"
	};
	static const char* const s_mid[] = {
		"ra","ni","ko","ta","li","de","fe","ga","hu","im",
		"ja","ke","lo","mu","na","ov","pe","ri","so","tu","ul","vi"
	};
	static const char* const s_end[] = {
		"ko","mi","ya","ro","ne","an","el","is","os","ur",
		"ax","by","ce","da","en","fi","gu","he","ix","jo","ky","lo"
	};
	static constexpr size_t N0 = 22, NM = 22, NE = 22;
	const size_t i0 = index % N0;
	const size_t im = (index / N0) % NM;
	const size_t ie = (index / (N0 * NM)) % NE;
	char name[NAME_LENGTH];
	snprintf(name, sizeof(name), "%s%s%s", s_start[i0], s_mid[im], s_end[ie]);
	return std::string(name);
}

static std::string generate_population_pc_name(uint32_t index, const PopulationEngine* cfg)
{
	const std::string profile_key = cfg != nullptr ? cfg->name_profile : std::string();
	const PopulationNameProfile* prof = population_names_db().find_profile_or_default(profile_key);
	const PopulationNameProfile::Strategy strat = population_effective_name_strategy(prof);

	for (int attempt = 0; attempt < 32; ++attempt) {
		std::string base = population_roll_base_name_from_profile(strat, prof, index, static_cast<uint32_t>(attempt));
		if (prof != nullptr && prof->max_len > 0 && static_cast<int>(base.size()) > prof->max_len)
			base.resize(static_cast<size_t>(prof->max_len));

		std::string full;
		if (cfg != nullptr)
			full += cfg->name_prefix;
		full += base;
		if (cfg != nullptr)
			full += cfg->name_suffix;

		if (full.empty())
			full = generate_bot_name(index);

		if (full.size() > static_cast<size_t>(NAME_LENGTH - 1))
			full.resize(NAME_LENGTH - 1);

		std::string lower = full;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(::tolower(c)); });

		if (population_yaml_name_hits_blocklist(lower)) {
			g_population_engine_stats.name_retries++;
			continue;
		}
		// Reject if a real player online shares the name — avoids identity confusion
		// and systems that match by name. This is an online-only check (no char-DB query).
		if (map_nick2sd(full.c_str(), false) != nullptr) {
			g_population_engine_stats.name_retries++;
			continue;
		}
		return full;
	}
	return generate_bot_name(index);
}

// Get random job ID (only jobs with proper sprites - conservative whitelist)
static int16_t get_random_job_id() {
    // Conservative whitelist of jobs that definitely have proper sprites
    // Based on jobmaster.txt and standard RO jobs that are commonly used
    // Excludes jobs that may pass pcdb_checkid() but don't have client-side sprites
#ifdef RENEWAL
    static const int16_t jobs_with_sprites[] = {
        // Basic 1-1 jobs (core jobs)
        JOB_NOVICE, JOB_SWORDMAN, JOB_MAGE, JOB_ARCHER, JOB_ACOLYTE, JOB_MERCHANT, JOB_THIEF,
        // 2-1 jobs (core jobs)
        JOB_KNIGHT, JOB_PRIEST, JOB_WIZARD, JOB_BLACKSMITH, JOB_HUNTER, JOB_ASSASSIN,
        // 2-2 jobs (core jobs)
        JOB_CRUSADER, JOB_MONK, JOB_SAGE, JOB_ROGUE, JOB_ALCHEMIST, JOB_BARD, JOB_DANCER,
        // Special basic jobs (confirmed to have sprites)
        JOB_SUPER_NOVICE, JOB_GUNSLINGER, JOB_NINJA, JOB_TAEKWON,
        // High/Trans 1-1 jobs
        JOB_NOVICE_HIGH, JOB_SWORDMAN_HIGH, JOB_MAGE_HIGH, JOB_ARCHER_HIGH, JOB_ACOLYTE_HIGH, JOB_MERCHANT_HIGH, JOB_THIEF_HIGH,
        // High/Trans 2-1 jobs
        JOB_LORD_KNIGHT, JOB_HIGH_PRIEST, JOB_HIGH_WIZARD, JOB_WHITESMITH, JOB_SNIPER, JOB_ASSASSIN_CROSS,
        // High/Trans 2-2 jobs
        JOB_PALADIN, JOB_CHAMPION, JOB_PROFESSOR, JOB_STALKER, JOB_CREATOR, JOB_CLOWN, JOB_GYPSY,
        // Baby 1-1 jobs
        JOB_BABY, JOB_BABY_SWORDMAN, JOB_BABY_MAGE, JOB_BABY_ARCHER, JOB_BABY_ACOLYTE, JOB_BABY_MERCHANT, JOB_BABY_THIEF,
        // Baby 2-1 jobs
        JOB_BABY_KNIGHT, JOB_BABY_PRIEST, JOB_BABY_WIZARD, JOB_BABY_BLACKSMITH, JOB_BABY_HUNTER, JOB_BABY_ASSASSIN,
        // Baby 2-2 jobs
        JOB_BABY_CRUSADER, JOB_BABY_MONK, JOB_BABY_SAGE, JOB_BABY_ROGUE, JOB_BABY_ALCHEMIST, JOB_BABY_BARD, JOB_BABY_DANCER, JOB_SUPER_BABY,
        // Special expanded jobs (confirmed in jobmaster.txt)
        JOB_STAR_GLADIATOR, JOB_SOUL_LINKER,
        // 3rd jobs 2-1 (confirmed in jobmaster.txt)
        JOB_RUNE_KNIGHT, JOB_WARLOCK, JOB_RANGER, JOB_ARCH_BISHOP, JOB_MECHANIC, JOB_GUILLOTINE_CROSS,
        // 3rd jobs 2-1 trans (confirmed in jobmaster.txt)
        JOB_RUNE_KNIGHT_T, JOB_WARLOCK_T, JOB_RANGER_T, JOB_ARCH_BISHOP_T, JOB_MECHANIC_T, JOB_GUILLOTINE_CROSS_T,
        // 3rd jobs 2-2 (confirmed in jobmaster.txt)
        JOB_ROYAL_GUARD, JOB_SORCERER, JOB_MINSTREL, JOB_WANDERER, JOB_SURA, JOB_GENETIC, JOB_SHADOW_CHASER,
        // 3rd jobs 2-2 trans (confirmed in jobmaster.txt)
        JOB_ROYAL_GUARD_T, JOB_SORCERER_T, JOB_MINSTREL_T, JOB_WANDERER_T, JOB_SURA_T, JOB_GENETIC_T, JOB_SHADOW_CHASER_T,
        // Baby 3rd jobs (conservative - only base versions)
        JOB_BABY_RUNE_KNIGHT, JOB_BABY_WARLOCK, JOB_BABY_RANGER, JOB_BABY_ARCH_BISHOP, JOB_BABY_MECHANIC, JOB_BABY_GUILLOTINE_CROSS,
        JOB_BABY_ROYAL_GUARD, JOB_BABY_SORCERER, JOB_BABY_MINSTREL, JOB_BABY_WANDERER, JOB_BABY_SURA, JOB_BABY_GENETIC, JOB_BABY_SHADOW_CHASER,
        // Super Novice Expanded
        JOB_SUPER_NOVICE_E, JOB_SUPER_BABY_E,
        // Kagerou/Oboro (confirmed in jobmaster.txt)
        JOB_KAGEROU, JOB_OBORO,
        // Rebellion (confirmed in jobmaster.txt)
        JOB_REBELLION,
        // Baby expanded jobs (conservative - only basic ones)
        JOB_BABY_NINJA, JOB_BABY_TAEKWON, JOB_BABY_GUNSLINGER,
        // Star Emperor/Soul Reaper (only base versions - _2 variants may not have sprites)
        JOB_STAR_EMPEROR, JOB_SOUL_REAPER, JOB_BABY_STAR_EMPEROR, JOB_BABY_SOUL_REAPER,
        // 4th jobs (confirmed in jobmaster.txt - only base versions)
        JOB_DRAGON_KNIGHT, JOB_MEISTER, JOB_SHADOW_CROSS, JOB_ARCH_MAGE, JOB_CARDINAL, JOB_WINDHAWK,
        JOB_IMPERIAL_GUARD, JOB_BIOLO, JOB_ABYSS_CHASER, JOB_ELEMENTAL_MASTER, JOB_INQUISITOR, JOB_TROUBADOUR, JOB_TROUVERE,
        JOB_SKY_EMPEROR, JOB_SOUL_ASCETIC, JOB_SHINKIRO, JOB_SHIRANUI, JOB_NIGHT_WATCH, JOB_HYPER_NOVICE
    };
#else
    // Pre-RE: no 3rd/4th/Summoner-era jobs — they are absent from job_db / skill tree and break fake PC spawn.
    static const int16_t jobs_with_sprites[] = {
        JOB_NOVICE, JOB_SWORDMAN, JOB_MAGE, JOB_ARCHER, JOB_ACOLYTE, JOB_MERCHANT, JOB_THIEF,
        JOB_KNIGHT, JOB_PRIEST, JOB_WIZARD, JOB_BLACKSMITH, JOB_HUNTER, JOB_ASSASSIN,
        JOB_CRUSADER, JOB_MONK, JOB_SAGE, JOB_ROGUE, JOB_ALCHEMIST, JOB_BARD, JOB_DANCER,
        JOB_SUPER_NOVICE, JOB_GUNSLINGER, JOB_NINJA, JOB_TAEKWON,
        JOB_NOVICE_HIGH, JOB_SWORDMAN_HIGH, JOB_MAGE_HIGH, JOB_ARCHER_HIGH, JOB_ACOLYTE_HIGH, JOB_MERCHANT_HIGH, JOB_THIEF_HIGH,
        JOB_LORD_KNIGHT, JOB_HIGH_PRIEST, JOB_HIGH_WIZARD, JOB_WHITESMITH, JOB_SNIPER, JOB_ASSASSIN_CROSS,
        JOB_PALADIN, JOB_CHAMPION, JOB_PROFESSOR, JOB_STALKER, JOB_CREATOR, JOB_CLOWN, JOB_GYPSY,
        // NO Baby / Super Baby jobs — the population is Novice -> 1st -> 2nd ->
        // Transcendent plus the standalone expanded classes below.
        JOB_STAR_GLADIATOR, JOB_SOUL_LINKER,
    };
#endif
    
    // Cache filtered list of valid jobs (jobs with view data AND sprites)
    static std::vector<int16_t> valid_jobs;
    static bool initialized = false;
    
    if (!initialized) {
        // Require client view data, server job_db entry, and a valid MAPID (pre-RE rejects renewal-only IDs).
        for (int16_t job_id : jobs_with_sprites) {
            if (!pcdb_checkid(job_id))
                continue;
            if (!job_db.exists(static_cast<uint16_t>(job_id)))
                continue;
            if (pc_jobid2mapid(job_id) == static_cast<uint64_t>(-1))
                continue;
            valid_jobs.push_back(job_id);
        }
        initialized = true;
        
        if (valid_jobs.empty()) {
            ShowError("Population engine: No valid jobs with sprites found! Using JOB_NOVICE as fallback\n");
            valid_jobs.push_back(JOB_NOVICE);
        }
    }
    
    return valid_jobs[rnd() % valid_jobs.size()];
}

// Get a random support-oriented job (ONLY Priest classes as requested)
// Get the required gender for a job (returns 'M' for male-only, 'F' for female-only, '\0' for gender-neutral)
static char get_job_required_sex(uint16_t job_id) {
    // Male-only jobs
    if (job_id == JOB_BARD || job_id == JOB_CLOWN || 
        job_id == JOB_MINSTREL || job_id == JOB_MINSTREL_T ||
        job_id == JOB_BABY_MINSTREL || job_id == JOB_BABY_BARD ||
        job_id == JOB_KAGEROU || job_id == JOB_BABY_KAGEROU ||
        job_id == JOB_TROUBADOUR || job_id == JOB_SHINKIRO) {
        return 'M';
    }
    
    // Female-only jobs
    if (job_id == JOB_DANCER || job_id == JOB_GYPSY ||
        job_id == JOB_WANDERER || job_id == JOB_WANDERER_T ||
        job_id == JOB_BABY_WANDERER || job_id == JOB_BABY_DANCER ||
        job_id == JOB_OBORO || job_id == JOB_BABY_OBORO ||
        job_id == JOB_TROUVERE || job_id == JOB_SHIRANUI) {
        return 'F';
    }
    
    // Gender-neutral jobs
    return '\0';
}

static t_itemid find_ammo_item_by_subtype(e_ammo_type sub) {
    // Prefer lightest ammo with no level requirement so shells of any level can equip it
    // and the weight-capped add-amount leaves enough room for equipping.
    // If all matching ammo has a level gate, fall back to the lightest match.
    t_itemid best = 0;
    int best_w = INT_MAX;
    t_itemid fallback = 0;
    int fallback_w = INT_MAX;
    for (const auto& it : item_db) {
        std::shared_ptr<item_data> id = it.second;
        if (!id || id->nameid == UNKNOWN_ITEM_ID)
            continue;
        if (id->type == IT_AMMO && id->subtype == sub && id->equip && (id->equip & EQP_AMMO)) {
            if (id->elv == 0) {
                if (id->weight < best_w) { best = it.first; best_w = id->weight; }
            } else {
                if (id->weight < fallback_w) { fallback = it.first; fallback_w = id->weight; }
            }
        }
    }
    return best != 0 ? best : fallback;
}

static t_itemid find_arrow_ammo_item() {
    t_itemid id = find_ammo_item_by_subtype(AMMO_ARROW);
    return id != 0 ? id : 1750; // Steel Arrow fallback
}

/// Derive weapon/shield sprites from inventory (status.weapon is weapon_type, not a sprite id).
static void population_engine_sync_vd_weapon_shield(map_session_data* sd) {
    if (sd == nullptr)
        return;
    pc_setinventorydata(*sd);
    sd->update_look(LOOK_WEAPON);
    sd->update_look(LOOK_SHIELD);
    if (sd->equip_index[EQI_AMMO] >= 0 && sd->inventory_data[sd->equip_index[EQI_AMMO]] != nullptr) {
        const item_data* ad = sd->inventory_data[sd->equip_index[EQI_AMMO]];
        if (ad->type == IT_AMMO) {
            const t_itemid look = ad->view_id != 0 ? ad->view_id : ad->nameid;
            sd->vd.look[LOOK_SHIELD] = look;
            sd->status.shield = look;
        }
    }
}

// Authoritative appearance finalizer for a fake-player shell. Called at the end
// of spawn AND after every respawn/relocation so the client always receives a
// valid, consistent sprite state.
//
// ROOT CAUSE of the "broken fake-player sprites": headgear / shield slots were
// being written with the item *nameid* instead of the item's *view id*
// (item_data.look). The client then tried to load a sprite numbered by the
// nameid, which does not exist -> blank / garbled / invisible headgear & shield.
// The real view ids are produced by pc_equipitem()->pc_set_costume_view(); this
// function rebuilds everything from those and never uses a nameid.
static void population_engine_finalize_shell_appearance(map_session_data* sd) {
    if (sd == nullptr)
        return;
    // 1. status.head_*/robe <- VIEW id of each equipped headgear/garment.
    pc_set_costume_view(sd);
    // 2. Rebuild vd base sprite + hair/head/robe from status.* (also range-caps).
    status_set_viewdata(sd, sd->status.class_);
    // 3. Weapon / shield view ids from the equipped weapon & shield.
    pc_setinventorydata(*sd);
    sd->update_look(LOOK_WEAPON);
    sd->update_look(LOOK_SHIELD);
    // 4. Sync headgear / robe view ids into vd.
    sd->vd.look[LOOK_HEAD_TOP]    = sd->status.head_top;
    sd->vd.look[LOOK_HEAD_MID]    = sd->status.head_mid;
    sd->vd.look[LOOK_HEAD_BOTTOM] = sd->status.head_bottom;
    sd->vd.look[LOOK_ROBE]        = sd->status.robe;
    // 5. Hair style / hair colour keep their normal values (range-capped so the
    //    head sprite renders). Body style (LOOK_BODY2) is NOT touched here.
    sd->status.hair       = cap_value(sd->status.hair, MIN_HAIR_STYLE, MAX_HAIR_STYLE);
    sd->status.hair_color = cap_value(sd->status.hair_color, MIN_HAIR_COLOR, MAX_HAIR_COLOR);
    sd->vd.look[LOOK_HAIR]       = sd->status.hair;
    sd->vd.look[LOOK_HAIR_COLOR] = sd->status.hair_color;
    // 6. The ONLY forced palette: clothes colour = 0 (equivalent to @ccolor 0).
    sd->status.clothes_color       = 0;
    sd->vd.look[LOOK_CLOTHES_COLOR] = 0;
    sd->vd.sex = sd->status.sex;
    // 7. Fake players never display / maintain a Hat Effect. Clear both the
    //    unit-data list and the SC so no residual state is kept server-side.
    //    Also face the camera like a normal player: unit_dataset() memsets
    //    ud->dir to 0 (DIR_NORTH = facing away), which is why AFK shells look
    //    like they are standing backwards.
    if (unit_data *ud = unit_bl2ud(sd); ud != nullptr) {
        ud->hatEffects.clear();
        ud->dir = DIR_SOUTH;
    }
    sd->head_dir = 0;
    if (sd->sc.getSCE(SC_HAT_EFFECT))
        status_change_end(sd, SC_HAT_EFFECT);
    // LOOK_BASE is set by status_set_viewdata() above (= class_, and legitimately
    // 0 for Novice). It is NOT forced here — the client's own palette/GRF decides
    // rendering.
}

// Helper: Find a valid equippable item by equipment type
static uint16_t find_valid_equip_item(uint32 equip_type) {
    // Iterate through actual item database instead of searching by ID
    // This avoids checking thousands of non-existent items
    for (const auto& it : item_db) {
        t_itemid item_id = it.first;
        std::shared_ptr<item_data> id = it.second;
        
        // Skip dummy items (UNKNOWN_ITEM_ID = 512)
        if (!id || id->nameid == UNKNOWN_ITEM_ID)
            continue;
        
        // Check if item matches the equipment type
        if (id->equip && (id->equip & equip_type)) {
            return (uint16_t)item_id;
        }
    }
    
    return 0; // No valid item found
}

// Get appropriate weapon item ID for a job
// Get base job class (1st class) from any job ID
static uint16_t get_base_job(uint16_t job_id) {
    // Resolve the 1st-class base of ANY job (2nd, transcendent, baby, 3rd, 4th)
    // via its MAPID. The old numeric-range checks were broken: rAthena's job-id
    // enum interleaves 2nd / trans / 3rd values, so e.g. `JOB_KNIGHT .. JOB_ROYAL_GUARD`
    // ([7 .. ~4060]) swallowed every trans 1-1 job — a transcendent Mage
    // (JOB_MAGE_HIGH) resolved to JOB_SWORDMAN and was seeded a melee rotation.
    // MAPID_BASEMASK is the class tree for every job, trans/baby/3rd/4th included.
    const uint64 m = pc_jobid2mapid(job_id);
    if (m == static_cast<uint64>(-1))
        return JOB_NOVICE;
    switch (m & MAPID_BASEMASK) {
        case MAPID_SWORDMAN:   return JOB_SWORDMAN;
        case MAPID_MAGE:       return JOB_MAGE;
        case MAPID_ARCHER:     return JOB_ARCHER;
        case MAPID_ACOLYTE:    return JOB_ACOLYTE;
        case MAPID_MERCHANT:   return JOB_MERCHANT;
        case MAPID_THIEF:      return JOB_THIEF;
        case MAPID_TAEKWON:    return JOB_TAEKWON;
        case MAPID_GUNSLINGER: return JOB_GUNSLINGER;
        case MAPID_NINJA:      return JOB_NINJA;
        default:              return JOB_NOVICE; // Novice / Super Novice / summoner-era / unknown
    }
}

uint16_t population_engine_job_base_class(uint16_t job_id)
{
	return get_base_job(job_id);
}

const PopulationEngine *population_engine_resolve_equipment(uint16_t job_id)
{
	auto equipment = population_engine_find_any(job_id);
	if (equipment)
		return equipment.get();
	const uint16_t base_job = get_base_job(job_id);
	if (base_job != job_id) {
		equipment = population_engine_find_any(base_job);
		if (equipment)
			return equipment.get();
	}
	return nullptr;
}

// Known-safe pre-renewal base weapon per class tree. All ids verified against
// db/pre-re/item_db_equip.yml: low/no EquipLevelMin and equippable by the whole
// tree (so it works for 1st job, 2nd job, trans and baby variants alike).
//   Sword  1101 (Sword, 1hSword)      Rod    1601 (Rod, Staff)
//   Knife  1201 (Knife, Dagger)       Club   1501 (Club, Mace)
//   Bow    1701 (Bow)                 Violin 1901 (Musical, Bard)
//   Rope   1950 (Whip, Dancer)        Six_Shooter 13100 (Revolver, Gunslinger)
static t_itemid population_engine_safe_job_weapon(uint16_t job_id) {
	const uint64 m = pc_jobid2mapid(job_id);
	if (m == static_cast<uint64>(-1))
		return 1201; // Knife - universal safe default
	switch (m & MAPID_BASEMASK) {
		case MAPID_TAEKWON:    return 0;      // Taekwon / Star Gladiator: unarmed by design
		case MAPID_GUNSLINGER: return 13100;  // Six_Shooter
		case MAPID_NINJA:      return 1201;   // Knife (ninja use daggers)
		case MAPID_ARCHER:
			// Bard/Clown -> Musical (Violin, 1901). Dancer/Gypsy -> Whip (Rope, 1950).
			// Explicit by job id so it does not depend on sex resolution. Every
			// other archer-tree job (Archer/Hunter/Sniper/Baby) uses a Bow (1701).
			switch (job_id) {
				case JOB_BARD:   case JOB_CLOWN:  case JOB_BABY_BARD:   return 1901; // Violin
				case JOB_DANCER: case JOB_GYPSY:  case JOB_BABY_DANCER: return 1950; // Rope
				default: break;
			}
			if (m & JOBL_2_2) { // any unexpected 2-2 archer job
				const char rs = get_job_required_sex(job_id);
				return (rs == 'F') ? 1950 : 1901;
			}
			return 1701;                     // Bow
		case MAPID_MAGE:       return 1601;   // Rod
		case MAPID_ACOLYTE:    return 1501;   // Club (mace) - Acolyte/Priest/Monk
		case MAPID_MERCHANT:   return 1201;   // Knife (merchant line: dagger/axe/mace ok)
		case MAPID_THIEF:      return 1201;   // Knife (thief/assassin/rogue)
		case MAPID_SWORDMAN:   return 1101;   // Sword
		case MAPID_NOVICE:     return 1201;   // Knife (Novice / Super Novice)
		default:              return 1201;
	}
}

static uint16_t get_job_weapon(uint16_t job_id) {
    // First, try to load from YAML equipment database (exact job match)
    auto equipment = population_engine_find_any(job_id);
    if (equipment && !equipment->weapon_pool.empty()) {
        uint16_t picked = equipment->weapon_pool.size() == 1
            ? equipment->weapon_pool[0]
            : equipment->weapon_pool[rnd() % equipment->weapon_pool.size()];
        if (picked > 0) {
            struct item_data* id = itemdb_search(picked);
            if (id && id->nameid != UNKNOWN_ITEM_ID)
                return picked;
        }
    }

    // Try base job class if exact match not found
    uint16_t base_job = get_base_job(job_id);
    if (base_job != job_id) {
        equipment = population_engine_find_any(base_job);
        if (equipment && !equipment->weapon_pool.empty()) {
            uint16_t picked = equipment->weapon_pool.size() == 1
                ? equipment->weapon_pool[0]
                : equipment->weapon_pool[rnd() % equipment->weapon_pool.size()];
            if (picked > 0) {
                struct item_data* id = itemdb_search(picked);
                if (id && id->nameid != UNKNOWN_ITEM_ID)
                    return picked;
            }
        }
    }
    
    // Fallback to old logic if YAML doesn't have this job
    uint32 equip_type = 0;
    
    // Swordman line - Swords, Spears
    if (job_id == JOB_SWORDMAN || job_id == JOB_KNIGHT || job_id == JOB_CRUSADER ||
        job_id == JOB_LORD_KNIGHT || job_id == JOB_PALADIN || job_id == JOB_RUNE_KNIGHT ||
        job_id == JOB_ROYAL_GUARD || job_id == JOB_BABY_KNIGHT || job_id == JOB_BABY_CRUSADER ||
        job_id == JOB_BABY_RUNE_KNIGHT || job_id == JOB_BABY_ROYAL_GUARD) {
        equip_type = EQP_HAND_R; // Try 1H or 2H weapons
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        // Fallback to common IDs
        uint16_t weapons[] = {1101, 1102, 1103, 1104, 1105, 1106, 1107, 1108, 1109, 1110,
                              1111, 1112, 1113, 1114, 1115, 1116, 1117, 1118, 1119, 1120,
                              1301, 1302, 1303, 1304, 1305, 1306, 1307, 1308, 1309, 1310};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Mage line - Staves
    if (job_id == JOB_MAGE || job_id == JOB_WIZARD || job_id == JOB_SAGE ||
        job_id == JOB_HIGH_WIZARD || job_id == JOB_PROFESSOR || job_id == JOB_WARLOCK ||
        job_id == JOB_SORCERER || job_id == JOB_BABY_MAGE || job_id == JOB_BABY_WIZARD ||
        job_id == JOB_BABY_SAGE || job_id == JOB_BABY_WARLOCK || job_id == JOB_BABY_SORCERER) {
        equip_type = EQP_HAND_R;
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        uint16_t weapons[] = {1601, 1602, 1603, 1604, 1605, 1606, 1607, 1608, 1609, 1610,
                              1701, 1702, 1703, 1704, 1705, 1706, 1707, 1708, 1709, 1710};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Archer line - Bows
    if (job_id == JOB_ARCHER || job_id == JOB_HUNTER || job_id == JOB_SNIPER ||
        job_id == JOB_RANGER || job_id == JOB_BABY_ARCHER || job_id == JOB_BABY_HUNTER ||
        job_id == JOB_BABY_RANGER) {
        equip_type = EQP_HAND_R;
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        uint16_t weapons[] = {1701, 1702, 1703, 1704, 1705, 1706, 1707, 1708, 1709, 1710,
                              1711, 1712, 1713, 1714, 1715, 1716, 1717, 1718, 1719, 1720};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Acolyte line - Maces
    if (job_id == JOB_ACOLYTE || job_id == JOB_PRIEST || job_id == JOB_MONK ||
        job_id == JOB_HIGH_PRIEST || job_id == JOB_CHAMPION || job_id == JOB_ARCH_BISHOP ||
        job_id == JOB_CARDINAL || job_id == JOB_BABY_ACOLYTE || job_id == JOB_BABY_PRIEST ||
        job_id == JOB_BABY_MONK || job_id == JOB_BABY_ARCH_BISHOP) {
        equip_type = EQP_HAND_R;
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        uint16_t weapons[] = {1501, 1502, 1503, 1504, 1505, 1506, 1507, 1508, 1509, 1510,
                              1511, 1512, 1513, 1514, 1515, 1516, 1517, 1518, 1519, 1520};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Thief line - Daggers, Katars
    if (job_id == JOB_THIEF || job_id == JOB_ASSASSIN || job_id == JOB_ROGUE ||
        job_id == JOB_ASSASSIN_CROSS || job_id == JOB_STALKER || job_id == JOB_GUILLOTINE_CROSS ||
        job_id == JOB_SHADOW_CHASER || job_id == JOB_BABY_THIEF || job_id == JOB_BABY_ASSASSIN ||
        job_id == JOB_BABY_ROGUE || job_id == JOB_BABY_GUILLOTINE_CROSS || job_id == JOB_BABY_SHADOW_CHASER) {
        equip_type = EQP_HAND_R;
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        uint16_t weapons[] = {1201, 1202, 1203, 1204, 1205, 1206, 1207, 1208, 1209, 1210,
                              1251, 1252, 1253, 1254, 1255, 1256, 1257, 1258, 1259, 1260};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Merchant line - Axes, Maces
    if (job_id == JOB_MERCHANT || job_id == JOB_BLACKSMITH || job_id == JOB_ALCHEMIST ||
        job_id == JOB_WHITESMITH || job_id == JOB_CREATOR || job_id == JOB_MECHANIC ||
        job_id == JOB_GENETIC || job_id == JOB_BABY_MERCHANT || job_id == JOB_BABY_BLACKSMITH ||
        job_id == JOB_BABY_ALCHEMIST || job_id == JOB_BABY_MECHANIC || job_id == JOB_BABY_GENETIC) {
        equip_type = EQP_HAND_R;
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
        uint16_t weapons[] = {1401, 1402, 1403, 1404, 1405, 1406, 1407, 1408, 1409, 1410,
                              1421, 1422, 1423, 1424, 1425, 1426, 1427, 1428, 1429, 1430};
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Bard/Dancer line - Musical/Whip
    if (job_id == JOB_BARD || job_id == JOB_DANCER || job_id == JOB_CLOWN || job_id == JOB_GYPSY ||
        job_id == JOB_MINSTREL || job_id == JOB_WANDERER || job_id == JOB_TROUBADOUR || job_id == JOB_TROUVERE ||
        job_id == JOB_BABY_BARD || job_id == JOB_BABY_DANCER || job_id == JOB_BABY_MINSTREL || job_id == JOB_BABY_WANDERER) {
        // Musical (Bard) or Whip (Dancer)
        if (job_id == JOB_BARD || job_id == JOB_CLOWN || job_id == JOB_MINSTREL || job_id == JOB_TROUBADOUR ||
            job_id == JOB_BABY_BARD || job_id == JOB_BABY_MINSTREL) {
            uint16_t weapons[] = {1901, 1902, 1903, 1904, 1905, 1906, 1907, 1908, 1909, 1910}; // Musical
            return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
        } else {
            uint16_t weapons[] = {1801, 1802, 1803, 1804, 1805, 1806, 1807, 1808, 1809, 1810}; // Whip
            return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
        }
    }
    
    // Ninja line - Huuma
    if (job_id == JOB_NINJA || job_id == JOB_KAGEROU || job_id == JOB_OBORO ||
        job_id == JOB_BABY_NINJA || job_id == JOB_BABY_KAGEROU || job_id == JOB_BABY_OBORO) {
        uint16_t weapons[] = {1951, 1952, 1953, 1954, 1955, 1956, 1957, 1958, 1959, 1960}; // Huuma
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Taekwon line - Knuckle
    if (job_id == JOB_TAEKWON || job_id == JOB_STAR_GLADIATOR || job_id == JOB_SOUL_LINKER ||
        job_id == JOB_BABY_TAEKWON || job_id == JOB_BABY_STAR_GLADIATOR || job_id == JOB_BABY_SOUL_LINKER) {
        uint16_t weapons[] = {1851, 1852, 1853, 1854, 1855, 1856, 1857, 1858, 1859, 1860}; // Knuckle
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Gunslinger line - Revolver, Rifle, Gatling, Shotgun
    if (job_id == JOB_GUNSLINGER || job_id == JOB_REBELLION || job_id == JOB_BABY_GUNSLINGER || job_id == JOB_BABY_REBELLION) {
        uint16_t weapons[] = {13101, 13102, 13103, 13104, 13105, 13106, 13107, 13108, 13109, 13110, // Revolver
                              13151, 13152, 13153, 13154, 13155, 13156, 13157, 13158, 13159, 13160, // Rifle
                              13201, 13202, 13203, 13204, 13205, 13206, 13207, 13208, 13209, 13210, // Gatling
                              13251, 13252, 13253, 13254, 13255, 13256, 13257, 13258, 13259, 13260}; // Shotgun
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Sura - Book
    if (job_id == JOB_SURA || job_id == JOB_SURA_T || job_id == JOB_BABY_SURA) {
        uint16_t weapons[] = {1901, 1902, 1903, 1904, 1905, 1906, 1907, 1908, 1909, 1910}; // Book
        return weapons[rnd() % (sizeof(weapons) / sizeof(weapons[0]))];
    }
    
    // Default: No weapon
    return 0;
}

// Get random headgear item ID
static uint16_t get_random_headgear(uint8_t slot) {
    uint32 equip_type = 0;
    if (slot == 0) equip_type = EQP_HEAD_TOP;
    else if (slot == 1) equip_type = EQP_HEAD_MID;
    else if (slot == 2) equip_type = EQP_HEAD_LOW;
    
    // Try to find a valid item first
    if (equip_type != 0) {
        uint16_t found = find_valid_equip_item(equip_type);
        if (found) return found;
    }
    
    // Fallback: Common headgear item IDs (standard RO item IDs)
    // Head Top (slot 0)
    if (slot == 0) {
        uint16_t headgears[] = {5001, 5002, 5003, 5004, 5005, 5006, 5007, 5008, 5009, 5010,
                                5011, 5012, 5013, 5014, 5015, 5016, 5017, 5018, 5019, 5020,
                                5021, 5022, 5023, 5024, 5025, 5026, 5027, 5028, 5029, 5030};
        return headgears[rnd() % (sizeof(headgears) / sizeof(headgears[0]))];
    }
    // Head Mid (slot 1)
    else if (slot == 1) {
        uint16_t headgears[] = {5101, 5102, 5103, 5104, 5105, 5106, 5107, 5108, 5109, 5110,
                                5111, 5112, 5113, 5114, 5115, 5116, 5117, 5118, 5119, 5120};
        return headgears[rnd() % (sizeof(headgears) / sizeof(headgears[0]))];
    }
    // Head Bottom (slot 2)
    else {
        uint16_t headgears[] = {5201, 5202, 5203, 5204, 5205, 5206, 5207, 5208, 5209, 5210,
                                5211, 5212, 5213, 5214, 5215, 5216, 5217, 5218, 5219, 5220};
        return headgears[rnd() % (sizeof(headgears) / sizeof(headgears[0]))];
    }
}

// Get random costume/robe item ID
static uint16_t get_random_costume_robe() {
    // Try to find a valid robe/garment first
    uint16_t found = find_valid_equip_item(EQP_GARMENT);
    if (found) return found;
    
    // Fallback: Common costume/robe item IDs (standard RO item IDs)
    uint16_t robes[] = {25001, 25002, 25003, 25004, 25005, 25006, 25007, 25008, 25009, 25010,
                        25011, 25012, 25013, 25014, 25015, 25016, 25017, 25018, 25019, 25020};
    return robes[rnd() % (sizeof(robes) / sizeof(robes[0]))];
}

bool population_engine_start(const PopulationEngineConfig& config, PopulationEngineStats& stats) {
    int16_t map_id = config.map_id;
    if (g_population_engine_running) {
        ShowWarning("Population engine: Already running, stop it first\n");
        return false;
    }
    struct map_data* mapdata = map_getmapdata(map_id);
    if (!mapdata) {
        ShowError("Population engine: Invalid map_id %d\n", map_id);
        return false;
    }
    {
        std::vector<map_session_data*> prev_shells = std::move(g_population_engine_pcs);
        g_population_engine_count = 0;
        g_next_population_engine_index.store(1);
        population_engine_path_clear_all();
        g_population_engine_stats = PopulationEngineStats();
        g_current_config = config;
        for (auto* prev_sd : prev_shells)
            population_engine_shell_release(prev_sd);
    }

    mapdata = map_getmapdata(map_id);
    if (!mapdata) {
        ShowError("Population engine: Map %d no longer valid after shell cleanup.\n", map_id);
        return false;
    }

    std::vector<map_session_data*> new_shells;
    uint32_t local_errors = 0;

    // Check global population shell limit
    extern struct Battle_Config battle_config;
    size_t current_fake_count = g_population_engine_count.load();
    uint32_t max_allowed = battle_config.population_engine_max_count;
    uint32_t num_to_spawn = config.num_units;
    
    // Adjust num_to_spawn if it would exceed the global limit
    if (current_fake_count + num_to_spawn > max_allowed) {
        num_to_spawn = (current_fake_count < static_cast<size_t>(max_allowed)) ? static_cast<uint32_t>(static_cast<size_t>(max_allowed) - current_fake_count) : 0;
        if (num_to_spawn < config.num_units) {
            ShowWarning("Population engine: Requested %u players but only %u can be spawned (global limit: %u, current: %zu)\n",
                config.num_units, num_to_spawn, max_allowed, current_fake_count);
        }
    }
    
    ShowStatus("Population engine: Spawning %u population PCs on map %d.\n", num_to_spawn, map_id);
    
    for (uint32_t i = 0; i < num_to_spawn; i++) {
        // Check global limit before each spawn (may have changed during loop)
        if (g_population_engine_count.load() >= max_allowed) {
            break; // Reached global limit
        }
        // Find spawn position
        int x = config.spawn_x;
        int y = config.spawn_y;
        
        if (config.spread_units || x <= 0 || y <= 0) {
            // Find a valid walkable position on map
            int16_t search_x = 0, search_y = 0;
            bool found_valid = false;
            
            if (mapdata->xs > 0 && mapdata->ys > 0) {
                // Try multiple times to find a valid walkable cell
                for (int attempts = 0; attempts < 50; attempts++) {
                    // Try map_search_freecell first (finds walkable cells)
                    if (map_search_freecell(nullptr, map_id, &search_x, &search_y, 
                        std::min(20, (int)(mapdata->xs/2)), std::min(20, (int)(mapdata->ys/2)), 1)) {
                        // Double-check it's walkable
                        if (map_getcell(map_id, search_x, search_y, CELL_CHKPASS)) {
                            x = search_x;
                            y = search_y;
                            found_valid = true;
                            break;
                        }
                    }
                    
                    // Fallback: try random position and validate
                    search_x = 50 + (rnd() % std::max(1, static_cast<int>(mapdata->xs - 100)));
                    search_y = 50 + (rnd() % std::max(1, static_cast<int>(mapdata->ys - 100)));
                    search_x = static_cast<int16_t>(std::max(0, std::min(static_cast<int>(search_x), static_cast<int>(mapdata->xs - 1))));
                    search_y = static_cast<int16_t>(std::max(0, std::min(static_cast<int>(search_y), static_cast<int>(mapdata->ys - 1))));
                    
                    if (map_getcell(map_id, search_x, search_y, CELL_CHKPASS)) {
                        x = search_x;
                        y = search_y;
                        found_valid = true;
                        break;
                    }
                }
                
                if (!found_valid) {
                    // Last resort: use a safe default position
                    x = std::max(50, std::min(100, (int)(mapdata->xs / 2)));
                    y = std::max(50, std::min(100, (int)(mapdata->ys / 2)));
                }
            } else {
                // Fallback for maps without size info
                x = 100 + (i % 20) * 5;
                y = 100 + (i / 20) * 5;
            }
        } else {
            // Validate provided coordinates - ensure they're walkable
            x = std::max(0, std::min(x, (int)(mapdata->xs - 1)));
            y = std::max(0, std::min(y, (int)(mapdata->ys - 1)));
            
            // If not walkable, try to find nearby walkable cell
            if (!map_getcell(map_id, x, y, CELL_CHKPASS)) {
                int16_t search_x = x, search_y = y;
                if (!map_search_freecell(nullptr, map_id, &search_x, &search_y, 5, 5, 1)) {
                    // If still can't find, use default
                    x = std::max(50, std::min(100, (int)(mapdata->xs / 2)));
                    y = std::max(50, std::min(100, (int)(mapdata->ys / 2)));
                } else {
                    x = search_x;
                    y = search_y;
                }
            }
        }
        
        // Random appearance - get a job with valid view data
        uint16_t job_id = get_random_job_id();
        
        // Double-check the job has valid view data before spawning
        if (!pcdb_checkid(job_id)) {
            ShowWarning("Population engine: Job %d has no view data, skipping population shell %u", job_id, i);
            g_population_engine_stats.errors++;
            continue;
        }
        
        // Use battle_config limits for hair/cloth customization (from client.conf); spawn may override from YAML
        uint8_t hair_style = MAX_HAIR_STYLE; // Fixed to max (42)
        uint16_t hair_color = rnd() % 131; // 0-600
        // Try to load equipment from YAML first (exact job match)
        PopulationDbSource pop_src = PopulationDbSource::Main;
        auto equipment = population_engine_find_any(job_id, &pop_src);

        // If not found, try base job class
        if (!equipment) {
            uint16_t base_job = get_base_job(job_id);
            if (base_job != job_id) {
                equipment = population_engine_find_any(base_job, &pop_src);
            }
        }

        // Gender: job-locked classes first; else optional YAML Sex on equipment row
        char sex;
        char required_sex = get_job_required_sex(job_id);
        if (required_sex != '\0') {
            sex = required_sex;
        } else if (equipment && equipment->sex_override >= 0) {
            sex = equipment->sex_override ? 'M' : 'F';
        } else {
            sex = (rnd() % 2) ? 'M' : 'F';
        }
        
        uint16_t weapon, shield, head_top, head_mid, head_bottom, garment;
        struct script_code* init_script = nullptr;
        bool skip_arrow = false;

        auto pick_pool = [](const std::vector<uint16_t>& p) -> uint16_t {
            if (p.empty()) return 0;
            return p.size() == 1 ? p[0] : p[rnd() % p.size()];
        };

        if (equipment) {
            // Use YAML configuration
            weapon      = pick_pool(equipment->weapon_pool);
            shield      = pick_pool(equipment->shield_pool);
            head_top    = pick_pool(equipment->head_top_pool);
            head_mid    = pick_pool(equipment->head_mid_pool);
            head_bottom = pick_pool(equipment->head_bottom_pool);
            garment     = pick_pool(equipment->garment_pool);
            init_script = equipment->script;
            skip_arrow  = equipment->skip_arrow;
        } else {
            // Fallback to old random logic
            weapon = get_job_weapon(job_id);  // Job-specific weapon
            // Try to find a valid shield first
            shield = find_valid_equip_item(EQP_SHIELD);
            if (shield == 0) {
                // Fallback: 50% chance for shield (basic shield ID 2101)
                shield = (rnd() % 2 == 0) ? 2101 : 0;
            }
            head_top = (rnd() % 3 == 0) ? get_random_headgear(0) : 0; // 33% chance for head top
            head_mid = (rnd() % 3 == 0) ? get_random_headgear(1) : 0; // 33% chance for head mid
            head_bottom = (rnd() % 3 == 0) ? get_random_headgear(2) : 0; // 33% chance for head bottom
            garment = (rnd() % 2 == 0) ? get_random_costume_robe() : 0; // 50% chance for random garment / robe look
        }
        
        uint32_t option = 0;
        // cloth_color 0-698 is a fallback before equipment YAML ClothesColor overrides it.
        uint16_t cloth_color = static_cast<uint16_t>(rnd() % 699); // 0-698 fallback

        // Collision-safe allocation from the 5 M ID pool.
        uint32_t spawn_index = population_engine_allocate_index();
        if (spawn_index == 0) {
            local_errors++;
            continue; // pool fully exhausted
        }
        const PopulationEngine* pop_cfg = equipment ? equipment.get() : nullptr;
        map_session_data* sd = population_engine_spawn_shell(map_id, x, y, spawn_index, job_id, sex, hair_style,
            hair_color, weapon, shield, head_top, head_mid, head_bottom, option, cloth_color, garment,
            init_script, skip_arrow, pop_cfg, /*map_category=*/0, pop_src);
        
        if (sd) {
            new_shells.push_back(sd);
            g_population_engine_count++;
        } else {
            local_errors++;
        }
    }

    for (auto* sd : new_shells) {
        g_population_engine_pcs.push_back(sd);
        g_population_engine_stats.total_created++;
        g_population_engine_stats.active_units++;
    }
    g_population_engine_stats.errors += local_errors;
    g_population_engine_stats.unit_ids.clear();
    for (auto* sd : g_population_engine_pcs) {
        if (sd) g_population_engine_stats.unit_ids.push_back(sd->id);
    }
    stats = g_population_engine_stats;
    g_population_engine_running = true;
    ShowStatus("Population engine: Created %u population PCs.\n", g_population_engine_stats.total_created);
    return true;
}

void population_engine_stop() {
    // Cancel all timers first so no stale timer fires after running=false is set
    // or after a subsequent start() sets running=true again.
    // Timer operations are main-thread-only and must not be done under the mutex.
    if (g_autosummon_timer != INVALID_TIMER) {
        const TimerData* td = get_timer(g_autosummon_timer);
        if (td && td->func == population_engine_autosummon_timer)
            delete_timer(g_autosummon_timer, population_engine_autosummon_timer);
        g_autosummon_timer = INVALID_TIMER;
    }
    if (g_pop_chat_timer != INVALID_TIMER) {
        const TimerData* td = get_timer(g_pop_chat_timer);
        if (td && td->func == population_engine_chat_timer)
            delete_timer(g_pop_chat_timer, population_engine_chat_timer);
        g_pop_chat_timer = INVALID_TIMER;
    }
    if (g_population_combat_global_timer != INVALID_TIMER) {
        const TimerData* tdc = get_timer(g_population_combat_global_timer);
        if (tdc && tdc->func == population_engine_global_combat_timer)
            delete_timer(g_population_combat_global_timer, population_engine_global_combat_timer);
        g_population_combat_global_timer = INVALID_TIMER;
    }
    // Automatic population targets + their manager timer (declared in the header;
    // defined later in this TU — the call is fine, only the definition order differs).
    population_engine_auto_clear_all();
    population_engine_path_stop_wander_timer();

    if (!g_population_engine_running && g_population_engine_pcs.empty())
        return;
    ShowStatus("Population engine: Stopping and cleaning up %zu population shells.\n", g_population_engine_pcs.size());
    std::vector<map_session_data*> to_release = std::move(g_population_engine_pcs);
    g_population_engine_count = 0;
    g_next_population_engine_index.store(1);
    g_population_engine_stats = PopulationEngineStats();
    g_population_engine_running = false;
    g_pop_chat_next_tick.clear();
    population_engine_path_clear_all();
    g_chat_cursor = 0;
    for (map_session_data* sd : to_release)
        population_engine_shell_release(sd);
    population_engine_write_count_sql(0);
    ShowStatus("Population engine: Stopped.\n");
}

// Get statistics
PopulationEngineStats population_engine_get_stats() {
    return g_population_engine_stats;
}

bool population_engine_is_running() {
    return g_population_engine_running;
}

size_t population_engine_get_count() {
	return g_population_engine_count.load();
}

// ============================================================
// Manual GM control API
// ============================================================

/// Pick the autosummon "map category" (1 town / 2 field / 3 dungeon) for a map so
/// the resolved behaviour override matches the map type on a manual @populate add.
static uint8_t population_manual_map_category(int16_t map_id)
{
	struct map_data *md = map_getmapdata(map_id);
	if (md && md->getMapFlag(MF_TOWN))
		return 1;
	// No reliable "is dungeon" flag; treat everything non-town as field so merchant
	// jobs use field_behavior (Combat) rather than accidentally becoming vendors.
	return 2;
}

/// A last-resort spawn map when a rolled job has no configured maps at all.
/// Prefers "prontera"; otherwise the first valid Towns: entry in the spawn DB.
/// Single source of truth for "may fake players be placed on this map at all?".
/// Used by every population path (manual @populate <n> <map>, allmap, and the
/// auto manager) so there is only one filter implementation.
///
/// Hard-excludes, using rAthena's own map flags / instance system:
///   - unknown / not-yet-loaded maps
///   - instance maps        (name contains '@', or instance_id / instance_src_map set)
///   - PvP maps             (MF_PVP)
///   - GvG / WoE maps       (MF_GVG*, MF_GVG_TE*)
///   - Battleground maps    (MF_BATTLEGROUND)
///   - the jail             (sec_pri*)
///
/// NOTE: MF_RESTRICTED is deliberately NOT used as an exclusion — many servers
/// (this one included) put every town in a restricted map-zone for item/skill
/// rules, which has nothing to do with population suitability.
/// Job/map compatibility and the db/population_spawn.yml allow-list are applied
/// separately by the callers — this function only answers "is the map itself a
/// safe place to host population".
static bool population_map_is_valid(int16_t map_id, const char **reason)
{
	auto deny = [&](const char *r) -> bool { if (reason) *reason = r; return false; };
	if (map_id < 0)
		return deny("no such map");
	struct map_data *md = map_getmapdata(map_id);
	if (md == nullptr || md->cell == nullptr || md->name[0] == '\0')
		return deny("map is not loaded on this map-server");
	if (strchr(md->name, '@') != nullptr)
		return deny("instance map");
	if (md->instance_id != 0 || md->instance_src_map != 0)
		return deny("instance map");
	if (md->getMapFlag(MF_PVP))
		return deny("PvP map");
	if (md->getMapFlag(MF_GVG) || md->getMapFlag(MF_GVG_CASTLE) || md->getMapFlag(MF_GVG_DUNGEON)
	    || md->getMapFlag(MF_GVG_TE) || md->getMapFlag(MF_GVG_TE_CASTLE))
		return deny("GvG/WoE map");
	if (md->getMapFlag(MF_BATTLEGROUND))
		return deny("Battleground map");
	if (strncmp(md->name, "sec_pri", 7) == 0)
		return deny("jail");
	if (reason)
		*reason = nullptr;
	return true;
}

/// Public wrapper: is `map_name` allowed to host fake players? Fills *reason with
/// a short human-readable cause when it is not.
bool population_engine_map_allowed(const char *map_name, const char **reason)
{
	if (reason)
		*reason = "no such map";
	if (map_name == nullptr || map_name[0] == '\0')
		return false;
	return population_map_is_valid(map_mapname2mapid(map_name), reason);
}

/// Count how many loaded maps are eligible vs excluded for population.
void population_engine_map_policy_counts(size_t *eligible, size_t *excluded)
{
	size_t ok = 0, bad = 0;
	for (int i = 0; i < map_num; ++i) {
		if (population_map_is_valid(static_cast<int16_t>(i), nullptr))
			++ok;
		else
			++bad;
	}
	if (eligible) *eligible = ok;
	if (excluded) *excluded = bad;
}

static int16_t population_safe_default_map()
{
	const int16 pron = map_mapname2mapid("prontera");
	if (pron >= 0 && population_map_is_valid(pron, nullptr))
		return pron;
	for (auto it = population_spawn_db().begin(); it != population_spawn_db().end(); ++it) {
		if (!it->second)
			continue;
		for (const std::string &m : it->second->towns) {
			const int16 t = map_mapname2mapid(m.c_str());
			if (t >= 0)
				return t;
		}
	}
	return -1;
}

/// Resolve the map a shell of `job_id` should spawn on. db/population_spawn.yml is
/// the job->maps rule set: `job_id` resolves (with trans/baby base-job fallback)
/// to a Population Engine profile, whose Towns/Fields/Dungeons lists are the maps
/// that job may use. See population_engine_manual_add() header for the full rule.
/// Sets *out_redirected when the shell had to be moved off `requested_map_id`
/// because that map is not configured for the job's profile.
/// Returns a valid loaded map id, or -1 if nothing usable exists.
// ===========================================================================
// Monster-difficulty-aware job selection
// ===========================================================================
// A map's fake-player job tier is derived from the ACTUAL monsters that spawn
// there (map_data.moblist[] -> spawn_data -> mob_db level), not from the map
// name. Works for official + custom maps/monsters and any future additions.
//
//   tier -1 : no monsters (town / safe zone) -> no job restriction
//   tier  0 : trivial   (metric <= 15) -> Novice / 1st job
//   tier  1 : early      (<= 35)        -> 1st job (+ Novice)
//   tier  2 : mid        (<= 70)        -> 2nd job (+ 1st job)
//   tier  3 : high       (> 70)         -> 2nd / Transcendent
//
// The metric blends the weighted-average monster level (main population) with
// the map's highest non-boss monster level so mixed-level maps don't collapse
// to a single monster's value.
int population_map_difficulty_tier(int16_t m)
{
	if (m < 0)
		return -1;
	struct map_data *md = map_getmapdata(m);
	if (md == nullptr)
		return -1;
	// Towns keep their existing behaviour (social / idle / vending / novices) —
	// a stray scripted mob spawn in a town must not turn it into a "dungeon".
	if (md->getMapFlag(MF_TOWN))
		return -1;

	long   lv_sum   = 0;
	int    lv_weight = 0;
	int    max_lv   = 0;
	int    kinds    = 0;
	for (int i = 0; i < MAX_MOB_LIST_PER_MAP; ++i) {
		const struct spawn_data *s = md->moblist[i];
		if (s == nullptr || s->id <= 0)
			continue;
		std::shared_ptr<s_mob_db> mob = mob_db.find(static_cast<uint32>(s->id));
		int lv = (s->level > 0) ? static_cast<int>(s->level) : (mob ? mob->lv : 0);
		if (lv <= 0)
			continue;
		++kinds;
		const int w = std::max<int>(1, static_cast<int>(s->num));
		const bool is_boss = s->state.boss ||
			(mob && mob->get_bosstype() == BOSSTYPE_MVP);
		if (!is_boss) {
			lv_sum    += static_cast<long>(lv) * w;
			lv_weight += w;
		}
		if (lv > max_lv)
			max_lv = lv;
	}
	if (kinds == 0)
		return -1; // no monsters here

	int metric;
	if (lv_weight == 0)
		metric = max_lv;                       // boss-only map
	else {
		const int avg = static_cast<int>(lv_sum / lv_weight);
		metric = (avg * 3 + max_lv) / 4;
	}
	if (metric <= 12) return 0;
	if (metric <= 30) return 1;
	if (metric <= 60) return 2;
	return 3;
}

// Job progression tier from the job's MAPID (no invented ids — pure rAthena):
//   0 Novice / Super Novice | 1 first job (incl. expanded 1-1) | 2 second job |
//   3 transcendent
int population_job_tier(uint16_t job_id)
{
	const uint64 mp = pc_jobid2mapid(job_id);
	if (mp == static_cast<uint64>(-1))
		return 1;
	if (mp & JOBL_UPPER)
		return 3;
	if (mp & (JOBL_2_1 | JOBL_2_2))
		return 2;
	if ((mp & MAPID_BASEMASK) == MAPID_NOVICE)
		return 0;
	return 1;
}

// True if `job_id` is an appropriate farmer for `map_id`'s monster difficulty.
// Existing job/map restrictions (population_resolve_job_spawn_map / job DB) are
// applied separately and still win — this only trims obvious tier mismatches.
// Priest/Bard/Gypsy support is added post-spawn by
// population_engine_form_fake_parties_on_map(), independent of this gate.
bool population_engine_job_fits_map(uint16_t job_id, int16_t map_id)
{
	const int mt = population_map_difficulty_tier(map_id);
	if (mt < 0)
		return true; // town / no monsters -> unrestricted
	const int jt = population_job_tier(job_id);
	// Allow the map's tier plus one tier of "progression overlap" above it
	// (an over-levelled player farming a softer map), never more.
	if (jt > mt + 1)
		return false;
	if (mt >= 2 && jt == 0)
		return false;                 // no Novices in real dungeons
	if (mt >= 3 && jt <= 1)
		return false;                 // high dungeons: 2nd / trans only
	return true;
}

static int16_t population_resolve_job_spawn_map(uint16_t job_id, int32_t requested_map_id, bool *out_redirected,
                                                uint8_t *out_category = nullptr)
{
	if (out_redirected)
		*out_redirected = false;
	if (out_category)
		*out_category = 0; // caller falls back to a flag-based guess

	// Candidate maps come from db/population_spawn.yml but are still passed through
	// population_map_is_valid() so an instance/PvP/GvG map that slipped into the
	// YAML can never be selected.
	auto collect = [](const std::vector<std::string> &names, std::vector<int16_t> &out) {
		for (const std::string &m : names) {
			const int16 mid = map_mapname2mapid(m.c_str());
			if (mid >= 0 && population_map_is_valid(mid, nullptr)
			    && std::find(out.begin(), out.end(), mid) == out.end())
				out.push_back(mid);
		}
	};

	std::vector<int16_t> towns, fields, dungeons;
	const PopulationEngine *eq = population_engine_resolve_equipment(job_id);
	if (eq != nullptr && !eq->source_profile_name.empty()) {
		auto se = population_spawn_db().find(eq->source_profile_name);
		if (se) {
			collect(se->towns,    towns);
			collect(se->fields,   fields);
			collect(se->dungeons, dungeons);
		}
	}

	// 1. Requested map explicitly configured for this job's profile -> honour it.
	if (requested_map_id >= 0) {
		const int16_t rq = static_cast<int16_t>(requested_map_id);
		auto in = [&](const std::vector<int16_t> &v) { return std::find(v.begin(), v.end(), rq) != v.end(); };
		if (in(towns))    { if (out_category) *out_category = 1; return rq; }
		if (in(fields))   { if (out_category) *out_category = 2; return rq; }
		if (in(dungeons)) { if (out_category) *out_category = 3; return rq; }
	}

	// 2. Not configured for the job (or no request) -> pick from the job's own
	//    lists. For an "allmap"/no-preference spawn (requested_map_id < 0) the
	//    category is chosen by WEIGHTED RANDOM so the world fills up evenly
	//    (towns + fields + dungeons), not "everyone in Prontera". A specific
	//    requested map that just wasn't valid for this job falls back to the
	//    safest category (town > field > dungeon).
	extern struct Battle_Config battle_config;
	const std::vector<int16_t> *pool = nullptr;
	if (requested_map_id < 0) {
		int w_field = std::clamp(battle_config.population_engine_allmap_field_pct, 0, 100);
		int w_dun   = std::clamp(battle_config.population_engine_allmap_dungeon_pct, 0, 100);
		if (w_field + w_dun > 100) { const int s = w_field + w_dun; w_field = w_field * 100 / s; w_dun = w_dun * 100 / s; }
		const int w_town = 100 - w_field - w_dun;
		// zero out weights for empty categories, then roll.
		const int tw = towns.empty()    ? 0 : std::max(1, w_town);
		const int fw = fields.empty()   ? 0 : std::max(1, w_field);
		const int dw = dungeons.empty() ? 0 : std::max(1, w_dun);
		const int total = tw + fw + dw;
		if (total > 0) {
			int roll = static_cast<int>(rnd() % static_cast<uint32_t>(total));
			if (roll < tw)            pool = &towns;
			else if (roll < tw + fw)  pool = &fields;
			else                      pool = &dungeons;
		}
	}
	if (pool == nullptr) {
		if      (!towns.empty())    pool = &towns;
		else if (!fields.empty())   pool = &fields;
		else if (!dungeons.empty()) pool = &dungeons;
	}
	if (pool != nullptr) {
		if (requested_map_id >= 0 && out_redirected)
			*out_redirected = true;
		if (out_category)
			*out_category = (pool == &towns) ? 1 : (pool == &fields ? 2 : 3);
		return (*pool)[rnd() % pool->size()];
	}

	// 3. No job-specific maps at all -> requested map if given, else a safe default.
	if (requested_map_id >= 0)
		return static_cast<int16_t>(requested_map_id);
	return population_safe_default_map();
}

// Group partyless support shells (Priest tree, or Bard/Dancer tree) on a map
// into REAL map-server-local parties of 2-5: one support "leader" + nearby
// farmers (falling back to another support shell if too few farmers). Idempotent
// — already-partied shells are skipped, so it is safe to call after every batch.
static void population_engine_form_fake_parties_on_map(int16_t map_id)
{
	if (map_id < 0)
		return;
	std::vector<map_session_data*> support, farmers;
	for (map_session_data* sd : g_population_engine_pcs) {
		if (sd == nullptr || sd->m != map_id) continue;
		if (!sd->state.active || sd->prev == nullptr || pc_isdead(sd)) continue;
		if (sd->status.party_id != 0) continue;
		const uint64 cm   = static_cast<uint64>(sd->class_);
		const uint64 base = cm & MAPID_BASEMASK;
		const bool is_priest     = (base == MAPID_ACOLYTE && !(cm & JOBL_2_2)); // Aco/Priest/HighPriest
		const bool is_barddancer = (base == MAPID_ARCHER  &&  (cm & JOBL_2_2)); // Bard/Clown/Dancer/Gypsy
		if (is_priest || is_barddancer)
			support.push_back(sd);
		else
			farmers.push_back(sd);
	}
	size_t fi = 0;
	for (map_session_data* lead : support) {
		if (lead->status.party_id != 0)
			continue;
		map_session_data* members[MAX_PARTY] = {};
		int cnt = 0;
		members[cnt++] = lead;
		const int want = 2 + static_cast<int>(rnd() % 4); // 2..5
		while (cnt < want && fi < farmers.size()) {
			map_session_data* c = farmers[fi++];
			if (c != nullptr && c->status.party_id == 0)
				members[cnt++] = c;
		}
		for (size_t si = 0; si < support.size() && cnt < want; ++si) {
			map_session_data* s2 = support[si];
			if (s2 != lead && s2->status.party_id == 0)
				members[cnt++] = s2;
		}
		if (cnt >= 2 && party_create_fake(members, cnt) != 0) {
			// The leader (slot 0) uses the existing AutoSupport system to support
			// its party members. Seed its sd->ac config from its learned skills.
			autocombat_seed_fake_leader(members[0]);
		}
	}
}

int population_engine_manual_add(int32_t map_id, uint32_t count, int *out_redirected)
{
	if (out_redirected)
		*out_redirected = 0;
	if (count == 0)
		return 0;
	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);
	const size_t before = g_population_engine_count.load();
	if (before >= max_global) {
		ShowWarning("Population engine: @populate add refused — already at population_engine_max_count (%zu).\n", max_global);
		return 0;
	}

	// If a specific map was requested it must be a real loaded map AND an
	// eligible population map (not an instance/PvP/GvG/BG/jail/restricted map).
	// The command handler normally reports this to the GM first; this is the
	// backstop for any other caller.
	if (map_id >= 0) {
		const char *why = nullptr;
		if (!population_map_is_valid(static_cast<int16_t>(map_id), &why)) {
			ShowError("Population engine: @populate — map id %d not available for population (%s).\n",
				map_id, why ? why : "restricted");
			return 0;
		}
	}

	int redirected = 0;
	std::set<int16_t> touched_maps;
	for (uint32_t i = 0; i < count; ++i) {
		if (g_population_engine_count.load() >= max_global)
			break;
		// Roll a job, resolve its map, and keep re-rolling (bounded) until the
		// job's progression tier fits that map's ACTUAL monster difficulty. Same
		// gate is used by automatic population. Falls through after the cap so a
		// spawn is never skipped outright.
		uint16_t job = 0;
		int16_t  target = -1;
		bool     redir = false;
		uint8_t  cat = 0;
		bool     fits = false;
		for (int tries = 0; tries < 16; ++tries) {
			job = static_cast<uint16_t>(get_random_job_id());
			if (!pcdb_checkid(job))
				continue;
			redir = false; cat = 0;
			target = population_resolve_job_spawn_map(job, map_id, &redir, &cat);
			if (target < 0)
				continue;
			if (population_engine_job_fits_map(job, target)) { fits = true; break; }
		}
		if (!pcdb_checkid(job))
			continue;
		if (target < 0) {
			ShowWarning("Population engine: @populate add — no usable spawn map for job %u; shell skipped.\n", job);
			continue;
		}
		(void)fits;
		if (redir)
			++redirected;
		if (cat == 0)
			cat = population_manual_map_category(target);
		// One shell of this exact job on the resolved map. autosummon_fill_map
		// resolves gear/behaviour and finds a walkable cell.
		autosummon_fill_map(target, 1, job, nullptr,
			cat, /*bypass_existing_check=*/true);
		touched_maps.insert(target);
	}

	// Form Priest / Bard / Gypsy support parties on every map this batch spawned on.
	for (int16_t m : touched_maps)
		population_engine_form_fake_parties_on_map(m);

	if (out_redirected)
		*out_redirected = redirected;
	const size_t after = g_population_engine_count.load();
	if (after > before)
		g_population_engine_running = true; // keep @populate status / stop coherent
	return static_cast<int>(after - before);
}

// ---------------------------------------------------------------------------
// @populate vending <amount> <map|allmap>
// ---------------------------------------------------------------------------
// Creates Merchant-class fake players that immediately open a REAL rAthena vend
// (same vendor path used by Vendor-personality town shells: MC_VENDING + cart +
// vending_openvending()). Stock comes from db/population_vendors.yml (VendorKey)
// when configured, else the built-in default consumable list. Fake-player
// account protection means no permanent `vendings` SQL rows are written.
static const uint16_t kVendorJobs[] = { JOB_MERCHANT, JOB_MERCHANT, JOB_MERCHANT,
                                        JOB_BLACKSMITH, JOB_ALCHEMIST };

static bool population_spawn_one_vendor(int16_t map_id, uint32_t key_rotor)
{
	struct map_data *md = map_getmapdata(map_id);
	if (md == nullptr || md->cell == nullptr || md->xs <= 0 || md->ys <= 0)
		return false;

	const uint16_t job = kVendorJobs[rnd() % (sizeof(kVendorJobs) / sizeof(kVendorJobs[0]))];
	if (!pcdb_checkid(job))
		return false;

	// Profile for gear / name: prefer a vendor profile (db/population_vendor_pop.yml),
	// then the generic job profile, else nullptr (built-in defaults still work).
	PopulationDbSource src = PopulationDbSource::Vendor;
	std::shared_ptr<PopulationEngine> cfg = population_vendor_pop_db().find(job);
	if (!cfg) {
		cfg = population_engine_find_any(job, &src);
		if (!cfg) {
			const uint16_t bj = get_base_job(job);
			if (bj != job) cfg = population_engine_find_any(bj, &src);
		}
	}
	const PopulationEngine *pop_cfg = cfg ? cfg.get() : nullptr;

	// Stock key: rotate through the VendorKeys defined in db/population_vendors.yml
	// so a crowd of vendors sells a spread of shops. Empty -> built-in defaults.
	static std::vector<std::string> s_vkeys;
	static size_t s_vkey_epoch = SIZE_MAX;
	if (s_vkey_epoch != population_vendor_db().entry_count()) {
		s_vkeys = population_vendor_db().keys();
		s_vkey_epoch = population_vendor_db().entry_count();
	}
	std::string vkey;
	if (!s_vkeys.empty())
		vkey = s_vkeys[key_rotor % s_vkeys.size()];

	// Find a valid standing cell.
	int16 x = 0, y = 0;
	const int16_t mx = static_cast<int16_t>(std::min<int>(50, std::max<int>(2, md->xs / 5)));
	const int16_t my = static_cast<int16_t>(std::min<int>(50, std::max<int>(2, md->ys / 5)));
	for (int attempt = 0; attempt < 60 && (x == 0 && y == 0); ++attempt) {
		int16 sx = static_cast<int16>(mx + (rnd() % std::max<int>(1, md->xs - 2 * mx)));
		int16 sy = static_cast<int16>(my + (rnd() % std::max<int>(1, md->ys - 2 * my)));
		if (population_cell_is_good_spawn(map_id, sx, sy)) { x = sx; y = sy; }
	}
	if (x == 0 && y == 0) {
		int16 sx = static_cast<int16>(md->xs / 2), sy = static_cast<int16>(md->ys / 2);
		if (map_search_freecell(nullptr, map_id, &sx, &sy,
		        std::min<int>(30, md->xs / 2), std::min<int>(30, md->ys / 2), 1)
		    && population_cell_is_good_spawn(map_id, sx, sy)) { x = sx; y = sy; }
	}
	if (x == 0 && y == 0)
		return false;

	const uint32_t index = population_engine_allocate_index();
	if (index == 0)
		return false;

	char sex = get_job_required_sex(job);
	if (sex == '\0') sex = (rnd() % 2) ? SEX_MALE : SEX_FEMALE;

	uint16_t weapon = 0, shield = 0, head_top = 0, head_mid = 0, head_bottom = 0, garment = 0;
	struct script_code *init_script = nullptr;
	if (pop_cfg) {
		auto pick = [](const std::vector<uint16_t>& p) -> uint16_t {
			return p.empty() ? 0 : (p.size() == 1 ? p[0] : p[rnd() % p.size()]);
		};
		weapon      = pick(pop_cfg->weapon_pool);
		shield      = pick(pop_cfg->shield_pool);
		head_top    = pick(pop_cfg->head_top_pool);
		head_mid    = pick(pop_cfg->head_mid_pool);
		head_bottom = pick(pop_cfg->head_bottom_pool);
		garment     = pick(pop_cfg->garment_pool);
		init_script = pop_cfg->script;
	} else {
		weapon = get_job_weapon(job);
	}

	map_session_data *sd = population_engine_spawn_shell(
		map_id, x, y, index, job, sex, MAX_HAIR_STYLE,
		static_cast<uint16_t>(rnd() % 131), weapon, shield, head_top, head_mid, head_bottom,
		0 /*option*/, 0 /*cloth_color*/, garment, init_script, /*skip_arrow=*/true,
		pop_cfg, /*map_category=*/1, src,
		PopulationBehavior::Vendor, vkey.empty() ? nullptr : vkey.c_str());
	if (sd == nullptr)
		return false;

	g_population_engine_pcs.push_back(sd);
	g_population_engine_count++;
	g_population_engine_stats.total_created++;
	g_population_engine_stats.active_units++;
	sd->pop.source = 0; // MANUAL (removable by @populate remove / removeall)
	return true;
}

int population_engine_manual_add_vendors(int32_t map_id, uint32_t count, int *out_redirected)
{
	if (out_redirected)
		*out_redirected = 0;
	if (count == 0)
		return 0;
	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);
	const size_t before = g_population_engine_count.load();

	// Build the target map list.
	std::vector<int16_t> targets;
	if (map_id >= 0) {
		if (!population_map_is_valid(static_cast<int16_t>(map_id), nullptr))
			return 0;
		targets.push_back(static_cast<int16_t>(map_id));
	} else {
		// allmap: every eligible TOWN map (per the existing population map rules).
		for (int i = 0; i < map_num; ++i) {
			struct map_data *md = map_getmapdata(i);
			if (md == nullptr || md->cell == nullptr)
				continue;
			if (!md->getMapFlag(MF_TOWN))
				continue;
			if (population_map_is_valid(static_cast<int16_t>(i), nullptr))
				targets.push_back(static_cast<int16_t>(i));
		}
	}
	if (targets.empty())
		return 0;

	int made = 0;
	uint32_t rotor = static_cast<uint32_t>(rnd());
	for (uint32_t i = 0; i < count; ++i) {
		if (g_population_engine_count.load() >= max_global)
			break;
		const int16_t m = targets[i % targets.size()];
		if (population_spawn_one_vendor(m, rotor++))
			++made;
	}

	if (made > 0)
		g_population_engine_running = true;
	return static_cast<int>(g_population_engine_count.load() - before);
}

int population_engine_manual_fill_from_spawndb()
{
	const size_t before = g_population_engine_count.load();
	g_population_engine_running = true;
	g_autosummon_force_once = true;
	population_engine_autosummon_timer(INVALID_TIMER, gettick(), 0, 0);
	return static_cast<int>(g_population_engine_count.load() - before);
}

// ---------------------------------------------------------------------------
// @populate AutoCombat <Job> <qty> <map>   — fake players on the REAL loop
// ---------------------------------------------------------------------------
// Spawns population shells whose per-tick brain is autocombat.cpp's
// autocombat_main() (via SC_AUTOCOMBAT), NOT the population-engine AI. Purpose:
// stress-test the actual AutoCombat workload (target search / A* / movement /
// battle / skills / autoloot / teleport) at large scale. Mortal + auto-respawn.
// No hard quantity cap here — bounded only by population_engine_max_count.

static void population_manual_release_one(map_session_data *sd); // defined just below

static bool population_spawn_one_autocombat(int16_t map_id, uint16_t job)
{
	struct map_data *md = map_getmapdata(map_id);
	if (md == nullptr || md->cell == nullptr || md->xs <= 0 || md->ys <= 0)
		return false;
	if (!pcdb_checkid(job))
		return false;

	// Gear / name profile: the job's own profile if it has one, else its base
	// job's, else nullptr (built-in defaults still produce a usable shell).
	PopulationDbSource src = PopulationDbSource::Main;
	std::shared_ptr<PopulationEngine> cfg = population_engine_find_any(job, &src);
	if (!cfg) {
		const uint16_t bj = get_base_job(job);
		if (bj != job) cfg = population_engine_find_any(bj, &src);
	}
	const PopulationEngine *pop_cfg = cfg ? cfg.get() : nullptr;

	// Natural spread: independent random walkable cell per shell.
	int16 x = 0, y = 0;
	for (int attempt = 0; attempt < 80 && (x == 0 && y == 0); ++attempt) {
		int16 sx = static_cast<int16>(2 + (rnd() % std::max<int>(1, md->xs - 4)));
		int16 sy = static_cast<int16>(2 + (rnd() % std::max<int>(1, md->ys - 4)));
		if (population_cell_is_good_spawn(map_id, sx, sy)) { x = sx; y = sy; }
	}
	if (x == 0 && y == 0) {
		int16 sx = static_cast<int16>(md->xs / 2), sy = static_cast<int16>(md->ys / 2);
		if (map_search_freecell(nullptr, map_id, &sx, &sy,
		        std::min<int>(40, md->xs / 2), std::min<int>(40, md->ys / 2), 1)
		    && population_cell_is_good_spawn(map_id, sx, sy)) { x = sx; y = sy; }
	}
	if (x == 0 && y == 0)
		return false;

	const uint32_t index = population_engine_allocate_index();
	if (index == 0)
		return false;

	char sex = get_job_required_sex(job);
	if (sex == '\0') sex = (rnd() % 2) ? SEX_MALE : SEX_FEMALE;

	uint16_t weapon = 0, shield = 0, head_top = 0, head_mid = 0, head_bottom = 0, garment = 0;
	struct script_code *init_script = nullptr;
	if (pop_cfg) {
		auto pick = [](const std::vector<uint16_t>& p) -> uint16_t {
			return p.empty() ? 0 : (p.size() == 1 ? p[0] : p[rnd() % p.size()]);
		};
		weapon      = pick(pop_cfg->weapon_pool);
		shield      = pick(pop_cfg->shield_pool);
		head_top    = pick(pop_cfg->head_top_pool);
		head_mid    = pick(pop_cfg->head_mid_pool);
		head_bottom = pick(pop_cfg->head_bottom_pool);
		garment     = pick(pop_cfg->garment_pool);
		init_script = pop_cfg->script;
	} else {
		weapon = get_job_weapon(job);
	}

	const uint8_t cat = population_manual_map_category(map_id);

	map_session_data *sd = population_engine_spawn_shell(
		map_id, x, y, index, job, sex, MAX_HAIR_STYLE,
		static_cast<uint16_t>(rnd() % 131), weapon, shield, head_top, head_mid, head_bottom,
		0 /*option*/, 0 /*cloth_color*/, garment, init_script, /*skip_arrow=*/false,
		pop_cfg, cat, src,
		PopulationBehavior::AutoCombat, nullptr);
	if (sd == nullptr)
		return false;

	g_population_engine_pcs.push_back(sd);
	g_population_engine_count++;
	g_population_engine_stats.total_created++;
	g_population_engine_stats.active_units++;
	sd->pop.source = 0;                   // MANUAL
	sd->pop.flags |= PSF::Mortal;          // take damage, die, auto-respawn
	// spawn_shell already called autocombat_shell_start() for AutoCombat behaviour.
	return true;
}

int population_engine_manual_add_autocombat(uint16_t job_id, int32_t map_id, uint32_t count, int *out_redirected)
{
	if (out_redirected)
		*out_redirected = 0;
	if (count == 0 || !pcdb_checkid(job_id))
		return 0;
	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);
	const size_t before = g_population_engine_count.load();
	if (before >= max_global) {
		ShowWarning("Population engine: @populate AutoCombat refused — already at population_engine_max_count (%zu).\n", max_global);
		return 0;
	}

	if (map_id < 0 || !population_map_is_valid(static_cast<int16_t>(map_id), nullptr))
		return 0;

	// If the requested map is not appropriate for the job's progression tier,
	// redirect to a job-appropriate map (same policy as @populate add).
	int16_t target = static_cast<int16_t>(map_id);
	bool redir = false;
	if (!population_engine_job_fits_map(job_id, target)) {
		bool r2 = false; uint8_t c2 = 0;
		const int16_t alt = population_resolve_job_spawn_map(job_id, -1, &r2, &c2);
		if (alt >= 0) { target = alt; redir = true; }
	}

	int made = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (g_population_engine_count.load() >= max_global)
			break;
		if (population_spawn_one_autocombat(target, job_id))
			++made;
	}

	if (out_redirected)
		*out_redirected = redir ? made : 0;
	if (made > 0)
		g_population_engine_running = true;
	return made;
}

/// Release every AutoCombat shell (behavior == AutoCombat). Never touches real
/// players, manual/auto/vending/arena shells. Returns the number removed.
int population_engine_autocombat_remove_all()
{
	int removed = 0;
	for (size_t i = g_population_engine_pcs.size(); i-- > 0; ) {
		map_session_data *sd = g_population_engine_pcs[i];
		if (!sd || static_cast<PopulationBehavior>(sd->pop.behavior) != PopulationBehavior::AutoCombat)
			continue;
		g_population_engine_pcs.erase(g_population_engine_pcs.begin() + i);
		population_manual_release_one(sd);
		++removed;
	}
	if (g_population_engine_pcs.empty() && !population_engine_auto_active())
		g_population_engine_running = false;
	return removed;
}

/// Release AutoCombat shells of one job on one map (job_id 0 = any job,
/// map_id < 0 = any map). Returns the number removed.
int population_engine_autocombat_remove(uint16_t job_id, int32_t map_id)
{
	int removed = 0;
	for (size_t i = g_population_engine_pcs.size(); i-- > 0; ) {
		map_session_data *sd = g_population_engine_pcs[i];
		if (!sd || static_cast<PopulationBehavior>(sd->pop.behavior) != PopulationBehavior::AutoCombat)
			continue;
		if (job_id != 0 && sd->status.class_ != job_id)
			continue;
		if (map_id >= 0 && sd->m != map_id)
			continue;
		g_population_engine_pcs.erase(g_population_engine_pcs.begin() + i);
		population_manual_release_one(sd);
		++removed;
	}
	if (g_population_engine_pcs.empty() && !population_engine_auto_active())
		g_population_engine_running = false;
	return removed;
}

size_t population_engine_autocombat_list(std::vector<PopulationAutoCombatGroupRow> &out)
{
	out.clear();
	for (const map_session_data *sd : g_population_engine_pcs) {
		if (!sd || static_cast<PopulationBehavior>(sd->pop.behavior) != PopulationBehavior::AutoCombat)
			continue;
		const int16_t m = static_cast<int16_t>(sd->m);
		PopulationAutoCombatGroupRow *row = nullptr;
		for (auto &r : out)
			if (r.job_id == sd->status.class_ && r.map_id == m) { row = &r; break; }
		if (row == nullptr) {
			PopulationAutoCombatGroupRow nr{};
			nr.job_id = sd->status.class_;
			nr.map_id = m;
			safestrncpy(nr.map_name, map_mapid2mapname(m) ? map_mapid2mapname(m) : "?", sizeof(nr.map_name));
			out.push_back(nr);
			row = &out.back();
		}
		row->alive++;
		if (pc_isdead(sd))
			row->dead++;
	}
	return out.size();
}

/// Shared teardown for the manual remove commands: pop `sd` from the registry,
/// fix the counters (population_engine_shell_release does not touch them), release.
static void population_manual_release_one(map_session_data *sd)
{
	if (g_population_engine_count.load() > 0)
		g_population_engine_count--;
	if (g_population_engine_stats.active_units > 0)
		g_population_engine_stats.active_units--;
	population_engine_shell_release(sd);
}

int population_engine_manual_remove(uint32_t count)
{
	// Remove MANUAL shells first (newest first), then AUTO shells only if the
	// GM asked for more than the manual pool holds. Removing AUTO shells this way
	// is fine: the auto manager simply tops that map back up on its next tick.
	int removed = 0;
	for (int pass = 0; pass < 2 && count > 0; ++pass) {
		const uint8_t want_src = (pass == 0) ? 0 : 1;
		for (size_t i = g_population_engine_pcs.size(); i-- > 0 && count > 0; ) {
			map_session_data *sd = g_population_engine_pcs[i];
			if (!sd || sd->pop.source != want_src)
				continue;
			g_population_engine_pcs.erase(g_population_engine_pcs.begin() + i);
			population_manual_release_one(sd);
			++removed;
			--count;
		}
	}
	if (g_population_engine_pcs.empty() && !population_engine_auto_active())
		g_population_engine_running = false;
	return removed;
}

int population_engine_manual_remove_all()
{
	// "Remove all" also stops automatic replenishment — otherwise the auto
	// manager would immediately recreate everything on its next tick.
	population_engine_auto_clear_all();
	std::vector<map_session_data*> to_release = std::move(g_population_engine_pcs);
	g_population_engine_pcs.clear();
	int removed = 0;
	for (map_session_data *sd : to_release) {
		population_manual_release_one(sd);
		++removed;
	}
	g_population_engine_count.store(0);
	g_population_engine_stats.active_units = 0;
	g_population_engine_running = false;
	population_engine_write_count_sql(0);
	return removed;
}

bool population_engine_manual_remove_named(const char *name)
{
	if (!name || !name[0])
		return false;
	for (auto it = g_population_engine_pcs.begin(); it != g_population_engine_pcs.end(); ++it) {
		map_session_data *sd = *it;
		if (!sd)
			continue;
		if (strcmpi(sd->status.name, name) != 0)
			continue;
		g_population_engine_pcs.erase(it);
		population_manual_release_one(sd);
		if (g_population_engine_pcs.empty() && !population_engine_auto_active())
			g_population_engine_running = false;
		return true;
	}
	return false;
}

// --- @recall / @recallall support -------------------------------------------
// Fake players are not in pc_db / nick_db (kept out on purpose so they don't
// pollute every map_foreachpc consumer), so map_nick2sd() and mapit_getallusers()
// can't see them. These give the recall commands a scoped way to include them.

map_session_data *population_engine_find_by_name(const char *name)
{
	if (!name || !name[0])
		return nullptr;
	for (map_session_data *sd : g_population_engine_pcs) {
		if (sd != nullptr && strcmpi(sd->status.name, name) == 0)
			return sd;
	}
	return nullptr;
}

// Warp every live fake player to (map_index,x,y). Skips shells already there,
// dead shells, and (unless the GM may warp anywhere) shells on MF_NOWARP maps.
// Recall is a MOVE, never a despawn: identity / AI / party / tracking are kept.
int population_engine_recall_all(uint16_t map_index, int16_t x, int16_t y, bool ignore_nowarp)
{
	const int16_t dst_m = map_mapindex2mapid(map_index);
	int moved = 0;
	// Copy the pointer list first: pc_setpos may re-enter engine paths.
	std::vector<map_session_data*> list(g_population_engine_pcs.begin(), g_population_engine_pcs.end());
	for (map_session_data *sd : list) {
		if (sd == nullptr || sd->prev == nullptr || !sd->state.active)
			continue;
		if (pc_isdead(sd))
			continue;
		if (sd->m == dst_m && sd->x == x && sd->y == y)
			continue;
		if (!ignore_nowarp && sd->m >= 0 && map_getmapflag(sd->m, MF_NOWARP))
			continue;
		if (pc_setpos(sd, map_index, x, y, CLR_RESPAWN) == SETPOS_OK)
			++moved;
	}
	return moved;
}

int population_engine_manual_remove_on_map(const char *map_name, uint32_t count)
{
	if (!map_name || !map_name[0])
		return -1;
	const int16_t mid = map_mapname2mapid(map_name);
	if (mid < 0)
		return -1;
	const bool all = (count == 0);
	int removed = 0;
	// Manual shells on the map first, then auto shells on the map.
	for (int pass = 0; pass < 2; ++pass) {
		const uint8_t want_src = (pass == 0) ? 0 : 1;
		for (size_t i = g_population_engine_pcs.size(); i-- > 0; ) {
			if (!all && count == 0)
				break;
			map_session_data *sd = g_population_engine_pcs[i];
			if (!sd || sd->m != mid || sd->pop.source != want_src)
				continue;
			g_population_engine_pcs.erase(g_population_engine_pcs.begin() + i);
			population_manual_release_one(sd);
			++removed;
			if (!all)
				--count;
		}
	}
	if (g_population_engine_pcs.empty() && !population_engine_auto_active())
		g_population_engine_running = false;
	return removed;
}

size_t population_engine_snapshot(std::vector<PopulationShellSnapshot> &out,
                                  const char *map_name_filter)
{
	out.clear();
	const bool filter = (map_name_filter != nullptr && map_name_filter[0] != '\0');
	int16_t filter_id = -1;
	if (filter)
		filter_id = map_mapname2mapid(map_name_filter);
	for (map_session_data *sd : g_population_engine_pcs) {
		if (!sd || !sd->state.active || sd->prev == nullptr)
			continue;
		if (map_id2bl(sd->id) != sd)
			continue;
		if (filter && sd->m != filter_id)
			continue;
		PopulationShellSnapshot row{};
		safestrncpy(row.name, sd->status.name, sizeof(row.name));
		const char *mn = map_mapid2mapname(sd->m);
		safestrncpy(row.map_name, (mn && mn[0]) ? mn : "?", sizeof(row.map_name));
		row.x          = sd->x;
		row.y          = sd->y;
		row.job_id     = static_cast<uint16_t>(sd->status.class_);
		row.account_id = sd->status.account_id;
		row.dead       = (pc_isdead(sd) != 0);
		row.is_auto    = (sd->pop.source == 1);
		row.vending    = (sd->state.vending != 0);
		out.push_back(row);
	}
	return out.size();
}

// ============================================================
// Automatic population manager  (@populate auto ...)
// ============================================================
// Manual fake players (pop.source == 0) are created once by @populate and never
// replaced. Automatic targets keep a map (or "allmap") topped up to a count of
// AUTO fake players (pop.source == 1). Targets are in memory only, so a restart
// starts with none (auto OFF) unless a GM re-issues @populate auto.

static constexpr int16_t POP_AUTO_ALLMAP = -2;

struct PopAutoTarget {
	int16_t  map_id;   ///< POP_AUTO_ALLMAP = distribute across every job-appropriate map.
	uint32_t target;
};
static std::vector<PopAutoTarget> g_pop_auto_targets;
static int32 g_pop_auto_timer = INVALID_TIMER;

TIMER_FUNC(population_engine_auto_manager_timer);

size_t population_engine_manual_count()
{
	size_t n = 0;
	for (map_session_data *sd : g_population_engine_pcs)
		if (sd && sd->pop.source == 0) ++n;
	return n;
}

size_t population_engine_auto_count()
{
	size_t n = 0;
	for (map_session_data *sd : g_population_engine_pcs)
		if (sd && sd->pop.source == 1) ++n;
	return n;
}

size_t population_engine_vendor_count()
{
	size_t n = 0;
	for (map_session_data *sd : g_population_engine_pcs)
		if (sd && sd->state.vending) ++n;
	return n;
}

/// Count live AUTO shells that belong to the target keyed by `key`
/// (POP_AUTO_ALLMAP = every AUTO shell; otherwise AUTO shells on that map).
static size_t population_auto_count_for(int16_t key)
{
	size_t n = 0;
	for (map_session_data *sd : g_population_engine_pcs) {
		if (!sd || sd->pop.source != 1) continue;
		if (key == POP_AUTO_ALLMAP || sd->m == key) ++n;
	}
	return n;
}

/// Spawn up to `count` AUTO-owned shells. `requested_map` >= 0 targets one map
/// (only jobs whose profile lists it are used, so the map count actually
/// converges); < 0 = each job on its own profile map. Returns shells created.
static int population_auto_spawn(int16_t requested_map, uint32_t count)
{
	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);
	const bool strict_map = (requested_map >= 0);
	int made = 0;
	for (uint32_t i = 0; i < count; ++i) {
		if (g_population_engine_count.load() >= max_global)
			break;
		uint16_t job = 0;
		int16_t target = -1;
		uint8_t cat = 0;
		bool ok = false;
		bool fits = false;
		for (int tries = 0; tries < 16 && !ok; ++tries) {
			job = static_cast<uint16_t>(get_random_job_id());
			if (!pcdb_checkid(job))
				continue;
			bool redir = false;
			const int32 req = strict_map ? requested_map : -1;
			target = population_resolve_job_spawn_map(job, req, &redir, &cat);
			if (target < 0)
				continue;
			if (strict_map && target != requested_map)
				continue; // this job does not belong on the requested map — re-roll
			// Monster-difficulty-aware tier gate (same as manual population).
			// After ~10 tries, accept whatever fits the map so a shell is still made.
			if (!population_engine_job_fits_map(job, target)) {
				if (tries < 10)
					continue;
			} else {
				fits = true;
			}
			ok = true;
		}
		if (!ok)
			continue;
		(void)fits;
		if (cat == 0)
			cat = population_manual_map_category(target);
		const size_t pc_before = g_population_engine_count.load();
		autosummon_fill_map(target, 1, job, nullptr, cat, /*bypass_existing_check=*/true);
		if (g_population_engine_count.load() > pc_before && !g_population_engine_pcs.empty()) {
			g_population_engine_pcs.back()->pop.source = 1; // tag as AUTO
			++made;
		}
	}
	return made;
}

TIMER_FUNC(population_engine_auto_manager_timer)
{
	if (g_pop_auto_targets.empty())
		return 0;
	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);
	for (const PopAutoTarget &t : g_pop_auto_targets) {
		if (g_population_engine_count.load() >= max_global)
			break;
		const size_t cur = population_auto_count_for(t.map_id);
		if (cur >= t.target)
			continue;
		size_t deficit = t.target - cur;
		if (deficit > 50)      // never burst-spawn in a single tick
			deficit = 50;
		population_auto_spawn(t.map_id, static_cast<uint32_t>(deficit));
	}
	// Form Priest / Bard / Gypsy support parties on every map that currently has a
	// partyless support shell (covers both per-map and allmap auto targets).
	{
		std::set<int16_t> maps;
		for (map_session_data *sd : g_population_engine_pcs) {
			if (sd == nullptr || sd->m < 0 || sd->status.party_id != 0) continue;
			const uint64 cm = static_cast<uint64>(sd->class_);
			const uint64 base = cm & MAPID_BASEMASK;
			if ((base == MAPID_ACOLYTE && !(cm & JOBL_2_2)) ||
			    (base == MAPID_ARCHER  &&  (cm & JOBL_2_2)))
				maps.insert(sd->m);
		}
		for (int16_t m : maps)
			population_engine_form_fake_parties_on_map(m);
	}
	if (g_population_engine_count.load() > 0)
		g_population_engine_running = true;
	return 0;
}

static void population_auto_timer_arm()
{
	if (g_pop_auto_timer != INVALID_TIMER)
		return;
	static bool s_func_registered = false;
	if (!s_func_registered) {
		add_timer_func_list(population_engine_auto_manager_timer, "population_engine_auto_manager_timer");
		s_func_registered = true;
	}
	g_pop_auto_timer = add_timer_interval(gettick() + 100, population_engine_auto_manager_timer, 0, 0, 10000);
}

static void population_auto_timer_disarm()
{
	if (g_pop_auto_timer == INVALID_TIMER)
		return;
	const TimerData *td = get_timer(g_pop_auto_timer);
	if (td && td->func == population_engine_auto_manager_timer)
		delete_timer(g_pop_auto_timer, population_engine_auto_manager_timer);
	g_pop_auto_timer = INVALID_TIMER;
}

bool population_engine_auto_active()
{
	return !g_pop_auto_targets.empty();
}

int population_engine_auto_set(int32_t map_id, uint32_t target)
{
	const int16_t key = (map_id < 0) ? POP_AUTO_ALLMAP : static_cast<int16_t>(map_id);
	if (key >= 0 && !population_map_is_valid(key, nullptr))
		return -1;
	bool found = false;
	for (PopAutoTarget &t : g_pop_auto_targets) {
		if (t.map_id == key) { t.target = target; found = true; break; }
	}
	if (!found)
		g_pop_auto_targets.push_back(PopAutoTarget{ key, target });
	population_auto_timer_arm();
	g_population_engine_running = true;
	// Immediate first top-up so the GM sees the result now, not in 10s.
	population_engine_auto_manager_timer(INVALID_TIMER, gettick(), 0, 0);
	return static_cast<int>(population_auto_count_for(key));
}

bool population_engine_auto_clear(int32_t map_id)
{
	const int16_t key = (map_id < 0) ? POP_AUTO_ALLMAP : static_cast<int16_t>(map_id);
	bool removed = false;
	for (auto it = g_pop_auto_targets.begin(); it != g_pop_auto_targets.end(); ++it) {
		if (it->map_id == key) {
			g_pop_auto_targets.erase(it);
			removed = true;
			break;
		}
	}
	if (g_pop_auto_targets.empty())
		population_auto_timer_disarm();
	return removed;
}

void population_engine_auto_clear_all()
{
	g_pop_auto_targets.clear();
	population_auto_timer_disarm();
}

size_t population_engine_auto_status(std::vector<PopulationAutoStatusRow> &out)
{
	out.clear();
	for (const PopAutoTarget &t : g_pop_auto_targets) {
		PopulationAutoStatusRow row{};
		row.map_id  = t.map_id;
		row.target  = t.target;
		row.current = static_cast<uint32_t>(population_auto_count_for(t.map_id));
		if (t.map_id == POP_AUTO_ALLMAP) {
			safestrncpy(row.map_name, "allmap", sizeof(row.map_name));
		} else {
			const char *mn = map_mapid2mapname(t.map_id);
			safestrncpy(row.map_name, (mn && mn[0]) ? mn : "?", sizeof(row.map_name));
		}
		out.push_back(row);
	}
	return out.size();
}

/// Returns true if `id` belongs to a population shell (checks account_id range).
bool population_engine_is_population_pc(int32_t id) {
	block_list *bl = map_id2bl(id);
	if (bl && bl->type == BL_PC) {
		map_session_data *sd = BL_CAST(BL_PC, bl);
		if (sd && IS_POPULATION_ENGINE_ACCOUNT_ID(sd->status.account_id))
			return true;
	}
	return false;
}

bool population_engine_shell_is_mortal(const map_session_data *sd) {
	return sd != nullptr && (sd->pop.flags & PSF::Mortal) != 0;
}

void population_engine_on_shell_damaged(map_session_data *sd, struct block_list *src) {
	if (!sd || !IS_POPULATION_ENGINE_ACCOUNT_ID(sd->status.account_id))
		return;
	sd->pop.last_attacked_tick = gettick();
	sd->pop.last_attacker_id   = (src != nullptr) ? static_cast<uint32_t>(src->id) : 0u;
	// last_damage_received is set by the caller (pc_damage) before calling here.
	// Capture the skill_id from the attacker's unit_data (if any) for SkillUsed condition.
	// unit_data::skill_id holds the skill currently being/just executed by the attacker;
	// it is still set at the point pc_damage is called from skill_attack/skill_castend.
	if (src != nullptr) {
		const unit_data *src_ud = unit_bl2ud(src);
		if (src_ud != nullptr && src_ud->skill_id != 0) {
			sd->pop.last_skill_used_on_me      = src_ud->skill_id;
			sd->pop.last_skill_used_on_me_tick = gettick();
		}
	}
	// Immediately attempt reactive buff casts (fog wall, hide, etc.) mirroring
	// mob_skill_db closedattacked / longrangeattacked event-driven semantics.
	// SelfTargeted / MeleeAttacked / RangeAttacked conditions are satisfied right
	// now (last_attacked_tick is fresh), so don't wait for the next poll tick.
	population_engine_shell_reactive_cast(sd);
}

void population_engine_combat_shell_stop(map_session_data *sd)
{
	if (!sd || !sd->state.population_combat)
		return;
	population_engine_combat_shell_teardown(sd);
}

void population_engine_on_whisper_to_population_pc(map_session_data* from_sd, map_session_data* bot_sd, const char* message)
{
	if (!from_sd || !bot_sd || !message)
		return;
	if (population_engine_is_population_pc(from_sd->id))
		return;
	if (!population_engine_is_population_pc(bot_sd->id))
		return;

	// Party request: if the player whispers a party-related keyword the shell accepts or
	// creates a party and invites the player back (mirrors autocombat accept_party_request).
	const bool has_msg = message[0] != '\0';
	if (has_msg) {
		const char* kw_party[]  = { "party", "pt", "join", "invite" };
		bool is_party_request = false;
		for (const char* kw : kw_party) {
			if (stristr(message, kw) != nullptr) {
				is_party_request = true;
				break;
			}
		}
		if (is_party_request) {
			// Case A: the player is already in a party and invites the shell → accept immediately.
			if (bot_sd->party_invite > 0 && bot_sd->party_invite_account == from_sd->status.account_id) {
				party_add_member(bot_sd->party_invite, *bot_sd);
				// Acknowledge via a whisper reply.
				const std::vector<std::string>* accept_pool = population_chat_db().lines_for_category("party_invite_accept");
				if (accept_pool && !accept_pool->empty()) {
					const std::string& line = (*accept_pool)[rnd() % accept_pool->size()];
					char buf[CHAT_SIZE_MAX];
					population_engine_format_chat_line(bot_sd, line.c_str(), buf, sizeof(buf));
					clif_wis_message(from_sd, bot_sd->status.name, buf, strlen(buf) + 1, pc_get_group_level(bot_sd));
				}
				return;
			}
			// Case B: shell has a party → invite the player.
			if (bot_sd->status.party_id > 0 && bot_sd->status.party_id < 0x70000000) {
				// Only real (char-server) parties can be extended.
				party_invite(*bot_sd, from_sd);
				return;
			}
			// Case C: nobody has a party → bot creates one and invites immediately after creation.
			// Creating a real party requires char-server round-trip; instead enable accept_party_request
			// so that when the player sends a formal invite the bot auto-accepts.
			bot_sd->pop.accept_party_request = true;
			// Hint to the player via whisper.
			const std::vector<std::string>* pool = population_chat_db().lines_for_category("party_invite_accept");
			if (pool && !pool->empty()) {
				const std::string& line = (*pool)[rnd() % pool->size()];
				char buf[CHAT_SIZE_MAX];
				population_engine_format_chat_line(bot_sd, line.c_str(), buf, sizeof(buf));
				clif_wis_message(from_sd, bot_sd->status.name, buf, strlen(buf) + 1, pc_get_group_level(bot_sd));
			}
			return;
		}
	}

	population_engine_deliver_chat_reply_locked(bot_sd, from_sd);
}

void population_engine_on_global_chat_mention(map_session_data* from_sd, const char* message)
{
	if (!from_sd || !message || !message[0])
		return;
	if (population_engine_is_population_pc(from_sd->id))
		return;

	extern struct Battle_Config battle_config;
	if (!battle_config.population_engine_chat_enable || !battle_config.population_engine_chat_reply_enable)
		return;

	for (map_session_data* bot : g_population_engine_pcs) {
		if (!bot || !bot->state.active || bot->prev == nullptr)
			continue;
		if (map_id2bl(bot->id) != bot)
			continue;
		if (bot->m != from_sd->m)
			continue;
		if (bot->id == from_sd->id)
			continue;
		if (!bot->status.name[0])
			continue;
		if (stristr(message, bot->status.name) == nullptr)
			continue;
		population_engine_deliver_chat_reply_locked(bot, nullptr);
	}
}

void do_final_population_engine() {
	// Stop first so shells are released while all DB shared_ptrs are still valid.
	// Clearing the DBs before stop() would leave teardown code with null lookups.
	population_engine_stop();
	population_engine_db().clear();
	population_pvp_db().clear();
	population_vendor_pop_db().clear();
	population_shared_db().clear();
	population_chat_db().clear();
	population_spawn_db().clear();
	population_names_db().clear();
	population_skill_db().clear();
}

// ============================================================
// Arena PvP API
// ============================================================

/// Spawn `shell_count` population shells on `map_name` configured for PvP.
/// Shells target real (non-shell) players so a player can observe and engage the AI.
/// spawn_x/y set the spawn-center (0 = random spread).
/// map_search_freecell guarantees walkability for all spawn positions.
/// Shells share fake party ID 0x71000000|map_id so they don't fight each other.
/// Returns the number of shells actually spawned (0 on error).
int population_engine_arena_start(const char* map_name, int shell_count,
                                   int spawn_x, int spawn_y,
                                   uint16_t job_override, int team_id)
{
	if (!map_name || shell_count <= 0 || shell_count > 25)
		return 0;
	const int16 mid = map_mapname2mapid(map_name);
	if (mid < 0) {
		ShowWarning("population_engine_arena_start: unknown map '%s'.\n", map_name);
		return 0;
	}

	// NOTE: We intentionally do NOT call population_engine_arena_stop() here.
	// Scripts (see npc/custom/population/arena.txt) call start() in a loop —
	// once per slot — so they can present a per-slot job menu. Stopping every
	// call would wipe all prior spawns and leave only the last shell standing
	// (which is what produced "5v5 ended up 1v1"). Callers that want a clean
	// arena should call population_arena_stop("<map>") explicitly first; the
	// arena script already does this once before its spawn loop.

	extern struct Battle_Config battle_config;
	const size_t max_global = static_cast<size_t>(battle_config.population_engine_max_count);

	// Job composition selection priority:
	//   1. Explicit job_override (caller passed a single JOB_ id) -> all shells = that job.
	//   2. YAML ArenaJobPool (if non-empty) -> sample with replacement.
	//   3. Built-in fallback compositions keyed on shell_count.
	std::vector<uint16_t> job_pool;
	// Arena composition lives in db/population_pvp.yml. Fall back to the main DB
	// only if the PvP DB has no ArenaJobPool defined (transitional setups).
	const std::vector<uint16_t>& yaml_pool = !population_pvp_db().arena_job_pool().empty()
		? population_pvp_db().arena_job_pool()
		: population_engine_db().arena_job_pool();
	if (job_override != 0) {
		// Validate job_override resolves to a known equipment profile (or its base job).
		// We still spawn even if no profile matches; the spawn path falls back to
		// get_job_weapon() and an empty equipment set (same behaviour as before).
		job_pool.assign(static_cast<size_t>(shell_count), job_override);
	} else if (!yaml_pool.empty()) {
		// Fill shell_count slots by sampling randomly (with replacement) from the YAML pool.
		// This ensures variety — no two consecutive summons will necessarily be the same job.
		job_pool.reserve(static_cast<size_t>(shell_count));
		for (int i = 0; i < shell_count; ++i)
			job_pool.push_back(yaml_pool[rnd() % yaml_pool.size()]);
	} else {
		// Built-in fixed compositions: tank/healer/dps presets for common counts.
		static const uint16_t s_attackers[3] = {
			static_cast<uint16_t>(JOB_ASSASSIN_CROSS),
			static_cast<uint16_t>(JOB_SNIPER),
			static_cast<uint16_t>(JOB_LORD_KNIGHT),
		};
		if (shell_count == 5) {
			job_pool = {
				static_cast<uint16_t>(JOB_PALADIN),
				static_cast<uint16_t>(JOB_HIGH_PRIEST),
				static_cast<uint16_t>(JOB_ASSASSIN_CROSS),
				static_cast<uint16_t>(JOB_SNIPER),
				static_cast<uint16_t>(JOB_LORD_KNIGHT),
			};
		} else if (shell_count == 3) {
			job_pool = {
				static_cast<uint16_t>(JOB_PALADIN),
				static_cast<uint16_t>(JOB_HIGH_PRIEST),
				s_attackers[rnd() % 3],
			};
		} else {
			// 1 or other counts: random attacker; vary the choice so repeated calls differ.
			for (int i = 0; i < shell_count; ++i)
				job_pool.push_back(s_attackers[rnd() % 3]);
		}
	}

	struct map_data* mapdata = map_getmapdata(mid);
	if (!mapdata || !mapdata->cell)
		return 0;

	// Helper: find a walkable cell near (cx,cy) using map_search_freecell.
	// If cx/cy are 0 or invalid, falls back to a random passable cell.
	auto find_spawn_cell = [&](int cx, int cy, int &out_x, int &out_y) -> bool {
		const int radius = 10;
		if (cx > 0 && cy > 0
		    && cx < mapdata->xs && cy < mapdata->ys) {
			// map_search_freecell: when src=nullptr the initial *x/*y is the center.
			int16_t sx = static_cast<int16_t>(cx);
			int16_t sy = static_cast<int16_t>(cy);
			if (map_search_freecell(nullptr, mid, &sx, &sy,
			    static_cast<int16_t>(radius), static_cast<int16_t>(radius), 1)) {
				out_x = sx;
				out_y = sy;
				return true;
			}
		}
		// Fallback: random passable cell anywhere on the map.
		for (int attempt = 0; attempt < 20; ++attempt) {
			int16_t sx = static_cast<int16_t>(std::max(0, std::min(
				static_cast<int>(50 + rnd() % std::max(1, static_cast<int>(mapdata->xs - 100))),
				static_cast<int>(mapdata->xs - 1))));
			int16_t sy = static_cast<int16_t>(std::max(0, std::min(
				static_cast<int>(50 + rnd() % std::max(1, static_cast<int>(mapdata->ys - 100))),
				static_cast<int>(mapdata->ys - 1))));
			if (map_getcell(mid, sx, sy, CELL_CHKPASS)) {
				out_x = sx;
				out_y = sy;
				return true;
			}
		}
		// Last-resort: map_search_freecell across half the map.
		{
			int16_t sx = 0, sy = 0;
			if (map_search_freecell(nullptr, mid, &sx, &sy,
			    static_cast<int16_t>(std::min(20, static_cast<int>(mapdata->xs / 2))),
			    static_cast<int16_t>(std::min(20, static_cast<int>(mapdata->ys / 2))), 1)) {
				out_x = sx;
				out_y = sy;
				return true;
			}
		}
		return false;
	};

	int spawned = 0;
	// Arena shells bypass the PvE autosummon population cap (max_global) — arenas spawn
	// at most 25 shells and must work even when the PvE engine is at capacity. We still
	// honour an absolute hard ceiling to protect the index pool.
	const size_t arena_hard_cap = max_global + static_cast<size_t>(shell_count) + 8u;
	(void)arena_hard_cap; // reserved for future bookkeeping
	for (int i = 0; i < shell_count; ++i) {
		int x = 0, y = 0;
		// Jitter the spawn center per-iteration so multiple shells don't stack on the
		// same cell (map_search_freecell with stack=1 can return the same cell for
		// consecutive identical centers). Spread ~3 cells in a small ring.
		const int jx = spawn_x + ((i % 3) - 1) * 3;
		const int jy = spawn_y + (((i / 3) % 3) - 1) * 3;
		if (!find_spawn_cell(jx, jy, x, y))
			continue;

		// Pick the job for this shell from the arena composition.
		uint16_t job_id = job_pool[static_cast<size_t>(i)];
		PopulationDbSource pop_src = PopulationDbSource::Pvp;
		auto equipment = population_pvp_db().find(job_id);
		if (!equipment) {
			const uint16_t base_job = get_base_job(job_id);
			if (base_job != job_id) equipment = population_pvp_db().find(base_job);
		}
		if (!equipment) {
			// Backward-compatibility fallback: arena composition might still live in
			// db/population_engine.yml during transition. Tag the shell with the DB
			// that actually owned the entry so runtime lookups stay consistent.
			equipment = population_engine_find_any(job_id, &pop_src);
			if (!equipment) {
				const uint16_t base_job = get_base_job(job_id);
				if (base_job != job_id) equipment = population_engine_find_any(base_job, &pop_src);
			}
		}

		char sex;
		const char req_sex = get_job_required_sex(job_id);
		if (req_sex != '\0') sex = req_sex;
		else if (equipment && equipment->sex_override >= 0) sex = equipment->sex_override ? 'M' : 'F';
		else sex = (rnd() % 2) ? 'M' : 'F';

		const uint8_t  hair_style  = MAX_HAIR_STYLE;
		const uint16_t hair_color  = static_cast<uint16_t>(rnd() % 131);
		const uint16_t cloth_color = static_cast<uint16_t>(rnd() % 699);

		auto pick_pool = [](const std::vector<uint16_t>& p) -> uint16_t {
			if (p.empty()) return 0;
			return p.size() == 1 ? p[0] : p[rnd() % p.size()];
		};

		uint16_t weapon = 0, shield = 0, head_top = 0, head_mid = 0, head_bottom = 0, garment = 0;
		struct script_code* init_script = nullptr;
		bool skip_arrow = false;
		if (equipment) {
			weapon      = pick_pool(equipment->weapon_pool);
			shield      = pick_pool(equipment->shield_pool);
			head_top    = pick_pool(equipment->head_top_pool);
			head_mid    = pick_pool(equipment->head_mid_pool);
			head_bottom = pick_pool(equipment->head_bottom_pool);
			garment     = pick_pool(equipment->garment_pool);
			init_script = equipment->script;
			skip_arrow  = equipment->skip_arrow;
		} else {
			weapon = get_job_weapon(job_id);
		}

		uint32_t index = population_engine_allocate_index();
		if (index == 0)
			continue; // pool fully exhausted

		const PopulationEngine* pop_cfg = equipment ? equipment.get() : nullptr;
		// Arena shells are always mortal — force the Mortal flag.
		const uint32_t arena_flags = (pop_cfg ? pop_cfg->flags : 0u) | PSF::Mortal | PSF::CombatActive;

		map_session_data* sd = population_engine_spawn_shell(
			mid, x, y, index, job_id, sex, hair_style,
			hair_color, weapon, shield, head_top, head_mid, head_bottom,
			0, cloth_color, garment, init_script, skip_arrow, pop_cfg, 3 /*dungeon category*/, pop_src);

		if (sd) {
			// Override flags to ensure mortal and set arena team marker.
			sd->pop.flags = arena_flags;
			sd->pop.arena_team = static_cast<int8_t>(team_id);

			// Do NOT assign a synthetic party_id (see the note in
			// population_engine_spawn_shell's caller). Arena friend/foe relations
			// are resolved by population_engine_arena_relation() via pop.arena_team,
			// not by party_id.
			sd->status.party_id = 0;

			g_population_engine_pcs.push_back(sd);
			g_population_engine_count++;
			g_population_engine_stats.total_created++;
			g_population_engine_stats.active_units++;
			++spawned;
		}
	}
	return spawned;
}

/// Remove all arena shells from `map_name` and release them.
void population_engine_arena_stop(const char* map_name)
{
	if (!map_name)
		return;
	const int16 mid = map_mapname2mapid(map_name);
	if (mid < 0)
		return;

	// Partition arena shells out of g_population_engine_pcs FIRST so that the
	// global vector never holds freed pointers. The combat/chat timers iterate
	// g_population_engine_pcs between ticks; leaving freed entries there causes
	// UB when the next sweep reads sd->id from deallocated memory.
	std::vector<map_session_data*> to_release;
	auto new_end = std::remove_if(g_population_engine_pcs.begin(), g_population_engine_pcs.end(),
		[mid, &to_release](map_session_data *sd) {
			if (!sd || sd->pop.arena_team == 0 || sd->m != mid)
				return false;
			to_release.push_back(sd);
			return true;
		});
	const size_t removed = static_cast<size_t>(g_population_engine_pcs.end() - new_end);
	g_population_engine_pcs.erase(new_end, g_population_engine_pcs.end());
	if (g_population_engine_count.load() >= removed)
		g_population_engine_count -= removed;
	else
		g_population_engine_count = 0;

	for (map_session_data *sd : to_release)
		population_engine_shell_release(sd);
}


// -----------------------------------------------------------------------------
// Public accessor: arena PvP job pool (auto-derived from db/population_pvp.yml)
// Used by the buildin script command population_arena_jobs() so NPCs can build
// dynamic Arena Manager menus instead of hard-coding job lists.
// -----------------------------------------------------------------------------
std::vector<uint16_t> population_engine_arena_job_pool()
{
    return population_pvp_db().arena_job_pool();
}

// -----------------------------------------------------------------------------
// Arena PvP — battle target relation hook
// -----------------------------------------------------------------------------
// Bridges the arena_team tag (set by population_engine_arena_start) into
// rAthena's friend-vs-foe machinery. Without this hook, ally shells (team 2)
// have no party/guild/bg link to the real player and `battle_check_target`
// classifies them as neutral — autosupport then refuses to buff/heal the
// player and AoE skills land friend-fire.
//
// Convention used by the arena (see population_arena_start docs):
//   team 1 = enemy shells     (hostile to player + team 2 shells)
//   team 2 = allied shells    (friendly to player + each other)
//   real player on the arena map is implicitly treated as team 2.
//
// Returns +1 (allies), -1 (enemies), 0 (no opinion — let normal logic run).
// Same-map check guarantees we don't leak relations across maps.
// -----------------------------------------------------------------------------
int population_engine_arena_relation(const block_list *s_bl, const block_list *t_bl)
{
    if (!s_bl || !t_bl)
        return 0;
    if (s_bl == t_bl)
        return 0;
    if (s_bl->m != t_bl->m)
        return 0;
    if (s_bl->type != BL_PC || t_bl->type != BL_PC)
        return 0;

    const map_session_data *s_sd = reinterpret_cast<const map_session_data*>(s_bl);
    const map_session_data *t_sd = reinterpret_cast<const map_session_data*>(t_bl);

    const int s_team = s_sd->pop.arena_team;
    const int t_team = t_sd->pop.arena_team;

    // Effective team: real players on a map that has any arena shell are
    // implicitly team 2 (the player's side). For a relation to apply, at
    // least one side must actually be a tagged arena shell — otherwise we
    // have two real players and the engine has no opinion.
    const bool s_is_shell = s_team > 0;
    const bool t_is_shell = t_team > 0;
    if (!s_is_shell && !t_is_shell)
        return 0;

    const int s_eff = s_is_shell ? s_team : 2;
    const int t_eff = t_is_shell ? t_team : 2;

    return (s_eff == t_eff) ? 1 : -1;
}

// PC-only convenience used by the population shell ally scan callbacks so a
// team-2 ally shell will recognise the real player on the same map as a valid
// heal/buff target. Same logic as population_engine_arena_relation but bool.
bool population_engine_arena_is_ally(const map_session_data *a, const map_session_data *b)
{
    if (!a || !b || a == b)
        return false;
    return population_engine_arena_relation(
        reinterpret_cast<const block_list*>(a),
        reinterpret_cast<const block_list*>(b)) > 0;
}
