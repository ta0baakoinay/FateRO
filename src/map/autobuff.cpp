#include "autobuff.hpp"
#include "battle.hpp"
#include "party.hpp"
#include "pc.hpp"
#include "status.hpp"
#include "unit.hpp"

#include <vector>
#include <map>

#include <common/nullpo.hpp>
#include <common/socket.hpp>
#include <common/showmsg.hpp>
#include <common/utils.hpp>

using namespace rathena;

int ab_restore_state_timer(int tid, int64 tick, int id, intptr_t data) {
    map_session_data* sd = map_id2sd(id);
    if (!sd) {
        ShowWarning("ab_restore_state_timer: Player not found\n");
        return 0;
    }

    bool is_portal_warp = (data >= 100);
    int retry_count = is_portal_warp ? (data - 100) : static_cast<int>(data);

    // Always try to read the duration from registry
    int saved_duration = static_cast<int>(pc_readaccountreg(sd, add_str("#ab_duration")));

    // For new connections, validate registry is working
    if (!is_portal_warp && retry_count < 5) {
        // Test if registry is accessible by checking a known value
        int test_val = static_cast<int>(pc_readaccountreg(sd, add_str("#CASHPOINTS")));

        // If we can't read registry properly and duration should exist, retry
        if (saved_duration <= 0 && sd->ab.duration_ > 0) {
            ShowInfo("ab_restore_state_timer: Registry not ready for %s, retrying (%d/5)\n",
                     sd->status.name, retry_count + 1);
            add_timer(gettick() + 3000, ab_restore_state_timer, sd->id, retry_count + 1);
            return 0;
        }
    }

    // Use saved duration from registry or fallback to stored value
    int duration_to_restore = (saved_duration > 0) ? saved_duration : sd->ab.duration_;

    if (duration_to_restore > 0) {
        ShowInfo("ab_restore_state_timer: Restoring AutoBuff for %s with duration %d (source: %s)\n",
                 sd->status.name, duration_to_restore,
                 (saved_duration > 0) ? "registry" : "memory");

        // Start the AutoBuff status
        if (status_change_start(sd, sd, SC_AUTOBUFF, 10000, 0, 0, 0, 0,
                               duration_to_restore, SCSTART_NOAVOID)) {

            // Clear the registry value after successful restoration
            if (saved_duration > 0) {
                pc_setaccountreg(sd, add_str("#ab_duration"), 0);
            }
            sd->ab.duration_ = 0;

            ShowInfo("ab_restore_state_timer: Successfully restored AutoBuff for %s\n", sd->status.name);
        } else {
            ShowError("ab_restore_state_timer: Failed to start AutoBuff status for %s\n", sd->status.name);
        }
   } else {
        ShowInfo("ab_restore_state_timer: No AutoBuff duration to restore for %s\n", sd->status.name);
    }

    return 0;
}

// --- Auto-follow tuning (runtime constants, no config churn) --------------
// Re-issue the chase path only when the target has moved more than this many
// cells from the last point we chased toward.
#define AB_REPATH_THRESHOLD 3
// While the follower is stationary and repathing keeps failing, don't hammer
// path_search() every processFollow tick (which can run as often as every
// 250ms, feature.autobuff_timer) - only retry at this cadence.
#define AB_REPATH_RETRY_MS 1000
// How long the follower may make zero progress toward the target (blocked
// path, brief out-of-view, latency, ...) before the config-gated teleport
// rescue kicks in. Deliberately well above the "1-2s hiccup" range so normal
// pathfinding gets a real chance to catch up before we ever consider a warp.
#define AB_UNREACHABLE_GRACE_MS 6000

// Teleport-to-target policy gate. battle_config.feature_autobuff_teleportiflost:
//   0 = no        : never teleport to the followed player
//   1 = samemap   : teleport only within the same map
//   2 = anywhere  : teleport across maps too
// This never bypasses instance ownership or map-flag teleport/warp rules.
static bool ab_follow_teleport_ok(map_session_data* sd, map_session_data* target, bool cross_map) {
	const int mode = battle_config.feature_autobuff_teleportiflost;
	if (mode <= 0)
		return false;
	if (cross_map && mode < 2)
		return false;

	struct map_data* smap = map_getmapdata(sd->m);
	struct map_data* tmap = map_getmapdata(target->m);
	if (smap == nullptr || tmap == nullptr)
		return false;

	// Instance safety: only allow when the destination is a normal map, or the
	// exact same instance the follower already belongs to. Never hop between
	// two different instances or from a normal map into an instance.
	if (smap->instance_id != tmap->instance_id)
		return false;

	if (smap->getMapFlag(MF_NOTELEPORT) || smap->getMapFlag(MF_NORETURN))
		return false;
	if (tmap->getMapFlag(MF_NOTELEPORT) || tmap->getMapFlag(MF_NOWARP)
		|| tmap->getMapFlag(MF_NOWARPTO) || tmap->getMapFlag(MF_PVP_NOPARTY)
		|| tmap->getMapFlag(MF_GVG) || tmap->getMapFlag(MF_GVG_CASTLE)
		|| tmap->getMapFlag(MF_BATTLEGROUND))
		return false;
	return true;
}

static bool ab_follow_do_teleport(map_session_data* sd, map_session_data* target) {
	if (pc_setpos(sd, target->mapindex, target->x, target->y, CLR_TELEPORT) != SETPOS_OK)
		return false;
	pc_delinvincibletimer(sd);
	if (sd->state.autotrade)
		clif_parse_LoadEndAck(sd->fd, sd);
	sd->ab.follow_unreachable_since = 0;
	sd->ab.follow_last_x = target->x;
	sd->ab.follow_last_y = target->y;
	return true;
}

// Follow (movement) logic. Deliberately lightweight per tick:
//   * no per-tick path_search()  * no per-tick unit_stop_walking()
//   * squared-distance range test (no sqrt)  * no heap allocation
// A chase is (re)issued only on: first acquisition, target moved > threshold,
// follower not walking, or the walk target drifted. unit_walktobl() itself only
// re-paths when the target actually changes cell, so the steady state is nearly
// free even with thousands of followers.
void processFollow(bool skip, map_session_data* sd, party_data* p) {
	if (skip || sd->state.ab_stay || sd->state.ab_stop || pc_issit(sd) || pc_isdead(sd)
		|| sd->ab.following_player == 0)
		return;

	map_session_data* target = map_charid2sd(sd->ab.following_player);
	if (target == nullptr) {
		ab_partymessage(sd, "FollowPlayerOffline",
			(char*)"Autobuff : Player to follow is offline, I don't follow anyone !", 600);
		sd->state.ab_stay = true;
		sd->ab.follow_last_x = sd->ab.follow_last_y = -1;
		sd->ab.follow_unreachable_since = 0;
		return;
	}

	bool in_party = false;
	for (int i = 0; i < MAX_PARTY; ++i) {
		if (p->party.member[i].char_id == target->status.char_id) { in_party = true; break; }
	}
	if (!in_party) {
		ab_partymessage(sd, "FollowPlayerOffline",
			(char*)"Autobuff : Player to follow is not in the party anymore !", 600);
		return;
	}

	// Different map -> config-gated, instance-safe teleport only.
	if (target->m != sd->m) {
		if (ab_follow_teleport_ok(sd, target, true))
			ab_follow_do_teleport(sd, target);
		else
			ab_partymessage(sd, "TooFarFromFollow",
				(char*)"Autobuff : too far away - the followed player is on a different map!", 600);
		return;
	}

	const int range = (sd->ab.dist_to_leader < 1) ? 1 : (int)sd->ab.dist_to_leader;
	const int dx = target->x - sd->x;
	const int dy = target->y - sd->y;
	if (dx * dx + dy * dy <= range * range) {
		sd->ab.follow_unreachable_since = 0; // in support range - nothing to do
		return;
	}

	const t_tick now = gettick();
	const int tdx = target->x - sd->ab.follow_last_x;
	const int tdy = target->y - sd->ab.follow_last_y;
	const bool need_repath =
		   sd->ab.follow_last_x < 0                                   // no cached chase
		|| (tdx * tdx + tdy * tdy) > (AB_REPATH_THRESHOLD * AB_REPATH_THRESHOLD) // target moved
		|| sd->ud.walktimer == INVALID_TIMER                          // not walking
		|| sd->ud.target_to != target->id;                           // walking elsewhere

	// While actively walking, re-issue as soon as the target drifts (keeps the
	// chase responsive). While stalled, only retry at AB_REPATH_RETRY_MS - a
	// blocked/failed path_search() is expensive and won't usually resolve
	// itself within the same tick, so hammering it every 250-500ms just burns
	// CPU across every following character.
	const bool retry_due = DIFF_TICK(now, sd->ab.follow_repath_tick) >= AB_REPATH_RETRY_MS;
	if (need_repath && (sd->ud.walktimer != INVALID_TIMER || sd->ab.follow_last_x < 0 || retry_due)) {
		sd->ab.follow_repath_tick = now;
		if (unit_walktobl(sd, target, range, 0)) {
			sd->ab.follow_last_x = target->x;
			sd->ab.follow_last_y = target->y;
			sd->ab.follow_unreachable_since = 0;
		}
	}

	// Stuck recovery: only once we've made zero progress for a real amount of
	// time, not just for a tick or two - a brief obstacle, latency spike, or
	// momentary loss of the target should resolve itself via the normal
	// pathfinding above well before this fires.
	if (sd->ud.walktimer == INVALID_TIMER) {
		if (sd->ab.follow_unreachable_since == 0)
			sd->ab.follow_unreachable_since = now;

		if (DIFF_TICK(now, sd->ab.follow_unreachable_since) >= AB_UNREACHABLE_GRACE_MS) {
			sd->ab.follow_unreachable_since = now; // start a fresh grace window either way
			bool rescued = false;
			if (ab_follow_teleport_ok(sd, target, false)) {
				if (pc_checkskill(sd, AL_TELEPORT) > 0 && ab_canuseskill(sd, AL_TELEPORT, 1)
					&& unit_skilluse_id(sd, sd->id, AL_TELEPORT, 1)) {
					skill_consume_requirement(sd, AL_TELEPORT, 1, 2);
					rescued = true;
				} else {
					int fw = pc_search_inventory(sd, 601);
					if (fw < 0)
						fw = pc_search_inventory(sd, 12887);
					if (fw >= 0 && pc_useitem(sd, fw))
						rescued = true;
				}
				if (!rescued)
					rescued = ab_follow_do_teleport(sd, target);
			}
			if (!rescued)
				ab_partymessage(sd, "CompletelyStuck",
					(char*)"AutoBuff: cannot reach the player - path blocked and teleport not allowed", 1500);
		}
	} else {
		sd->ab.follow_unreachable_since = 0; // making progress - cancel any pending rescue
	}
}

// Private message command processing
int processPrivateCommands(map_session_data* sd, enum sc_type type,
	map_session_data* leader_sd, party_data* p) {
	if (sd->ab.order_msg.empty() || sd->ab.allow_pm_cmd == AB_PM_DISABLEPM)
		return -1;

	auto& front = sd->ab.order_msg.front();
	auto [text, sender] = front;
	if (sender == sd) {
		std::string msg = "Autobuff : you can't send message to yourself";
		ab_partymessage(sd, "Cmd", msg.data(), 5);
		sd->ab.order_msg.pop_front();
		return 0;
	}

	std::istringstream iss(text);
	std::vector<std::string> words{ std::istream_iterator<std::string>{iss}, {} };
	std::ostringstream err;

	switch (sd->ab.allow_pm_cmd) {
	case AB_PM_ALLOWPMFROMLEADER:
		if (!leader_sd || sender->status.char_id != leader_sd->status.char_id)
			err << "Autobuff : Only the leader of the party can command me!";
		break;
	case AB_PM_ALLOWPM: {
		bool in_party = std::any_of(std::begin(p->party.member), std::end(p->party.member),
			[&](auto& m) { return m.char_id == sender->status.char_id; });
		if (!in_party)
			err << "Autobuff : Only party members can command me!";
	}   break;
	case AB_PM_NAMEDPLAYER: {
		bool in_party = std::any_of(std::begin(p->party.member), std::end(p->party.member),
			[&](auto& m) { return m.char_id == sender->status.char_id; });
		bool named_ok = std::any_of(sd->ab.allow_pm_char_id.begin(), sd->ab.allow_pm_char_id.end(),
			[&](int id) { return id == sender->status.char_id; });
		if (!in_party || !named_ok)
			err << "Autobuff : Only specific members of the party can command me!";
	}   break;
	default: break;
	}

	bool should_pop = true;

	if (err.tellp()) {
		ab_partymessage(sd, "Cmd", err.str().data(), 5);
	}
	else {
		ab_commandHandler handler;
		if (auto it = handler.command_map.find(words[0]); it != handler.command_map.end()) {
			int cmd_result = it->second(sd, sender, words);
			if (cmd_result == 2)
				should_pop = false;
		}
		else {
			std::string msg = "Autobuff : Unknown command";
			ab_partymessage(sd, "Cmd", msg.data(), 5);
		}
	}

	if (should_pop && !sd->ab.order_msg.empty())
		sd->ab.order_msg.pop_front();

	return 0;
}

// Resurrection logic with full GvG exclusions and gem checks
void processResurrection(map_session_data* sd, t_tick last_tick, bool& skip) {
	if (skip || !sd->ab.autobuff_resurection || pc_checkskill(sd, ALL_RESURRECTION) <= 0)
		return;
	// Exclude all GvG zones
	if (map_getmapflag(sd->m, MF_GVG) || map_getmapflag(sd->m, MF_GVG_TE)
		|| map_getmapflag(sd->m, MF_GVG_CASTLE) || map_getmapflag(sd->m, MF_GVG_TE_CASTLE)
		|| map_getmapflag(sd->m, MF_BATTLEGROUND))
		return;

	int lvl = pc_checkskill(sd, ALL_RESURRECTION);
	int gem_idx = pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE);
	int gems = gem_idx >= 0 ? sd->inventory.u.items_inventory[gem_idx].amount : 0;
	if (gems < 8) {
		std::string msg = "I'm low on Blue Gemstones!";
		ab_partymessage(sd, "LowGemstone", msg.data(), 100);
	}

	if (!ab_canuseskill(sd, ALL_RESURRECTION, lvl))
		return;

	int dead_id = 0;
	map_foreachinarea(ab_targetresu, sd->m,
		sd->x - AREA_SIZE, sd->y - AREA_SIZE,
		sd->x + AREA_SIZE, sd->y + AREA_SIZE,
		BL_PC, &dead_id, sd->id);
	if (dead_id <= 0)
		return;

	auto* target = map_id2sd(dead_id);
	if (!target || gems <= 0) {
		std::string msg = "I'm out of Blue Gemstones!";
		ab_partymessage(sd, "OutGemstone", msg.data(), 100);
		return;
	}

	auto skill = skill_db.find(ALL_RESURRECTION);
	int ab_range = (skill->inf & INF_SELF_SKILL) ? 10
		: skill_get_range(ALL_RESURRECTION, lvl);
	ab_range = (ab_range == 0) ? 2 : abs(ab_range);

	if (!check_distance(target->x - sd->x, target->y - sd->y, ab_range)
		|| !path_search_long(nullptr, sd->m, sd->x, sd->y,
			target->x, target->y, CELL_CHKWALL)) {
		if (!unit_walktobl(sd, target, ab_range, 1))
			return;
		skip = true;
		return;
	}

	if (unit_skilluse_id(sd, dead_id, ALL_RESURRECTION, lvl)) {
		skip = true;
		skill_consume_requirement(sd, ALL_RESURRECTION, lvl, 2);
		sd->ab.skill_cd = std::max(sd->ab.skill_cd,
			last_tick + battle_config.feature_autobuff_bskill_delay
			+ skill_get_cast(ALL_RESURRECTION, lvl));
	}
}

//Brain
int ab_status(map_session_data* sd, enum sc_type type) {
	auto handle_error = [&](std::string msg) {
		status_change_end(sd, type);
		ab_partymessage(sd, "Error", msg.data(), 1);
		return 0;
		};

	auto* p = sd->status.party_id ? party_search(sd->status.party_id) : nullptr;
    if (!p) {
        status_change_end(sd, type);
        clif_displaymessage(sd->fd, "Autobuff : OFF - You can't enable it when you're not in a party");
        return 0;
    }

	if (battle_config.feature_autobuff_duration_type) {
		if (sd->ab.duration_ <= 0) {
			std::string msg = "Automessage - You don't have timer left on autoattack system!";
			ab_partymessage(sd, "TimerOut", msg.data(), 5);
			return -1;
		}

		sd->ab.duration_ = sd->ab.duration_ - battle_config.feature_autobuff_timer;
		pc_setaccountreg(sd, add_str("#ab_duration"), sd->ab.duration_);
	}

	bool skip = pc_cant_act(sd) || pc_isdead(sd);

	// Leader & online count
	map_session_data* leader_sd = nullptr;
	int player_pos = -1, leader_pos = -1, nb_online = 0;
	for (int i = 0; i < MAX_PARTY; ++i) {
		auto& m = p->party.member[i];
		if (!m.char_id) continue;
		auto* msd = map_charid2sd(m.char_id);
		if (!msd) continue;
		if (msd->status.char_id != sd->status.char_id) ++nb_online;
		else player_pos = i;
		if (party_isleader(msd)) {
			leader_sd = msd;
			leader_pos = i;
		}
	}
	if (!nb_online && !sd->ab.autobuff_disable_alone)
		return handle_error("Autobuff : OFF - You're the only online player in the party");

	// Class check
	int base = sd->class_ & MAPID_BASEMASK;
	int upper = sd->class_ & MAPID_UPPERMASK;
	if (base != MAPID_ACOLYTE && upper != MAPID_CRUSADER
		&& upper != MAPID_BARDDANCER && upper != MAPID_ALCHEMIST)
		return handle_error("Autobuff : OFF - Only an acolyte, crusader, bard, dancer and alchemist base job can use autobuff");

	if (sd->regen.state.overweight) {
		std::string msg = "Autobuff : I'm overweight !";
		ab_partymessage(sd, "Overweight", msg.data(), 300);
	}

	// PM disabled purge + message
	if (!sd->ab.order_msg.empty() && sd->ab.allow_pm_cmd == AB_PM_DISABLEPM) {
		std::string msg = "Autobuff : Private message command is disabled";
		ab_partymessage(sd, "Cmd", msg.data(), 300);
		sd->ab.order_msg.clear();
	}

	// Private commands
	processPrivateCommands(sd, type, leader_sd, p);

	//check if logout from command
	if (!sd) return 0;

	// Skip if he received stop order or is sit or dead
	if (sd->state.ab_stop || pc_issit(sd))
		skip = true;

	// Resurrection
	t_tick last_tick = gettick();
	processResurrection(sd, last_tick, skip);

	// Movement (follow)
	processFollow(skip, sd, p);

	/* Auto buff skills */
	if (sd->ab.priorize_buff) {
		auto_buff_action(skip, sd, leader_sd, p, leader_pos);
		auto_heal_action(skip, sd, leader_sd, p, leader_pos);
	}
	else {
		auto_heal_action(skip, sd, leader_sd, p, leader_pos);
		auto_buff_action(skip, sd, leader_sd, p, leader_pos);
	}

	/* Auto buff items */
	if (!skip && !sd->ab.autobuff_buffitems.empty()) {
		for (auto& itAutobuffitem : sd->ab.autobuff_buffitems) {
			if (!sd->sc.getSCE(itAutobuffitem.status)) {
				int i_ = pc_search_inventory(sd, itAutobuffitem.item_id);
				if (i_ >= 0)
					pc_useitem(sd, i_);
			}
		}
	}

	// Auto buff potions
	if (!skip && sd->ab.state_autobuff_potions && !sd->ab.autobuff_potions.empty()) {
		for (auto& potion : sd->ab.autobuff_potions) {
			// Check if potion is active
			if (!potion.is_active) {
				continue;
			}

			int current_hp_percent = (sd->battle_status.hp * 100) / sd->battle_status.max_hp;
			int current_sp_percent = (sd->battle_status.sp * 100) / sd->battle_status.max_sp;

			// Find item in inventory
			int inventory_index = pc_search_inventory(sd, potion.item_id);
			if (inventory_index < 0) {
				continue;
			}

			bool should_use_potion = false;

			// Check HP condition (only if HP threshold is set)
			if (potion.min_hp > 0 && current_hp_percent < potion.min_hp) {
				should_use_potion = true;
			}

			// Check SP condition (only if SP threshold is set)
			if (potion.min_sp > 0 && current_sp_percent < potion.min_sp) {
				should_use_potion = true;
			}

			if (should_use_potion) {
				if (pc_useitem(sd, inventory_index)) {
					return 1; // Exit early after using one potion
				}
			}
		}
	}

	return 1;
}

void auto_heal_action(bool& skip, map_session_data* sd, map_session_data* leader_sd, struct party_data* p, int leader_pos) {
	std::shared_ptr<s_skill_db> skill;
	t_tick last_tick = gettick();
	time_t last_time = time(NULL);

	if (skip || sd->ab.autobuff_heal.empty() || last_tick < sd->ab.skill_cd)
		return;

	for (auto& heal : sd->ab.autobuff_heal) {
		if (skip) break;

		if (!heal.is_active || last_tick < heal.last_use || !ab_canuseskill(sd, heal.skill_id, heal.skill_lv) || pc_checkskill(sd, heal.skill_id) <= 0)
			continue;

		skill = skill_db.find(heal.skill_id);
		bool heal_used = false;
		bool can_walk = true;

		const bool is_self_skill = skill->inf & INF_SELF_SKILL;
		const bool is_potionpitcher5 = (heal.skill_id == AM_POTIONPITCHER && heal.skill_lv == 5);
		int ab_skill_range = skill_get_range(heal.skill_id, heal.skill_lv);
		ab_skill_range = (ab_skill_range == 0) ? 2 : std::abs(ab_skill_range);
		if (is_self_skill) ab_skill_range = 10;

		auto should_heal = [&](const map_session_data* target) -> bool {
			if (!target || target->m != sd->m || pc_isdead(target))
				return false;
			int current = is_potionpitcher5 ? target->battle_status.sp : target->battle_status.hp;
			int maximum = is_potionpitcher5 ? target->battle_status.max_sp : target->battle_status.max_hp;
			return get_percentage(current, maximum) < heal.min_hp;
		};

		auto use_heal_skill = [&](map_session_data* target) -> bool {
			if (heal.skill_id == PR_SANCTUARY) {
				int gem_idx = pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE);
				int gems = gem_idx >= 0 ? sd->inventory.u.items_inventory[gem_idx].amount : 0;
				if (gems <= 0) {
					std::string msg = "I'm out of Blue Gemstones!";
					ab_partymessage(sd, "OutGemstone", msg.data(), 100);
					return false;
				}
				else if (gems < 8) {
					std::string msg = "I'm low on Blue Gemstones!";
					ab_partymessage(sd, "LowGemstone", msg.data(), 100);
				}
			}
			if (heal.skill_id == CR_SLIMPITCHER || heal.skill_id == PR_SANCTUARY)
				return unit_skilluse_pos(sd, target->x, target->y, heal.skill_id, heal.skill_lv);
			else
				return unit_skilluse_id(sd, is_self_skill ? sd->id : target->id, heal.skill_id, heal.skill_lv);
		};

		auto try_heal_target = [&](map_session_data* target) -> bool {
			if (!check_distance_bl(sd, target, ab_skill_range) ||
				!path_search_long(nullptr, sd->m, sd->x, sd->y, target->x, target->y, CELL_CHKWALL)) {
				can_walk = unit_walktobl(sd, target, ab_skill_range, 1);
				if (!can_walk) return false;
				skip = true;
				return false;
			}
			heal_used = use_heal_skill(target);
			return heal_used;
		};

		switch (heal.priorize_heal) {
		case AB_SELF:
			if (should_heal(sd)) {
				heal_used = use_heal_skill(sd);
			}
			break;

		case AB_LEADER:
			if (leader_sd && leader_sd->m == sd->m && !pc_isdead(leader_sd)) {
				if (should_heal(leader_sd))
					try_heal_target(leader_sd);
			}
			break;

		case AB_NAMEDPLAYER:
			for (const auto& char_id : heal.priorize_heal_char_id) {
				map_session_data* target_sd = nullptr;

				for (int i = 0; i < MAX_PARTY; i++) {
					if (p->party.member[i].char_id == char_id) {
						target_sd = map_charid2sd(char_id);
						break;
					}
				}
				if (!target_sd || !should_heal(target_sd)) continue;
				if (try_heal_target(target_sd)) break;
			}
			break;

		case AB_ALL:
		case AB_EVERYONEEXCEPTSELF:
			for (int i = 0; i < MAX_PARTY; i++) {
				map_session_data* target_sd = map_charid2sd(p->party.member[i].char_id);
				if (!target_sd || (heal.priorize_heal == AB_EVERYONEEXCEPTSELF && target_sd == sd)) continue;
				if (!should_heal(target_sd)) continue;
				if (try_heal_target(target_sd)) break;
			}
			break;
		}

		if (heal_used) {
			skip = true;
			sd->idletime = last_time;
			skill_consume_requirement(sd, heal.skill_id, heal.skill_lv, 2);
			heal.last_use = last_tick + skill_get_cast(heal.skill_id, heal.skill_lv) + skill_get_delay(heal.skill_id, heal.skill_lv);
			sd->ab.skill_cd = std::max(sd->ab.skill_cd, last_tick + battle_config.feature_autobuff_bskill_delay + skill_get_cast(heal.skill_id, heal.skill_lv));
		}
	}
}

void auto_buff_action(bool& skip, map_session_data* sd, map_session_data* leader_sd, struct party_data* p, int leader_pos) {
	std::shared_ptr<s_skill_db> skill;
	t_tick last_tick = gettick();
	bool encore = false, song = false;

#ifndef RENEWAL
	if (sd->sc.getSCE(SC_DANCING)) {
		unit_skilluse_id(sd, sd->id, BD_ADAPTATION, 1);
	}
#endif

	if (skip || sd->ab.autobuff_buffskills.empty() || last_tick < sd->ab.skill_cd)
		return;

	for (auto& buff : sd->ab.autobuff_buffskills) {
		if (skip) break;

		if (!buff.is_active || last_tick < buff.last_use || !ab_canuseskill(sd, buff.skill_id, buff.skill_lv) || pc_checkskill(sd, buff.skill_id) <= 0)
			continue;

		skill = skill_db.find(buff.skill_id);
		int ab_skill_range = (skill->inf & INF_SELF_SKILL) ? 10 : skill_get_range(buff.skill_id, buff.skill_lv);
		ab_skill_range = ab_skill_range == 0 ? 2 : std::abs(ab_skill_range);

		bool buff_used = false;
		bool can_walk = true;

		if (skill->inf2[INF2_ISSONG] || skill->inf2[INF2_ISENSEMBLE]) {
			song = true;
			encore = (sd->skill_id_dance == buff.skill_id && sd->skill_lv_dance == buff.skill_lv);
		}

		auto needs_buff = [&](map_session_data* target) -> bool {
			if (!target || pc_isdead(target) || target->m != sd->m)
				return false;
			if (buff.skill_id == CR_FULLPROTECTION) {
				return !target->sc.getSCE(SC_CP_WEAPON) &&
					!target->sc.getSCE(SC_CP_SHIELD) &&
					!target->sc.getSCE(SC_CP_ARMOR) &&
					!target->sc.getSCE(SC_CP_HELM);
			}
			return !target->sc.getSCE(skill_get_sc(buff.skill_id));
			};

		auto valid_target = [&](map_session_data* target) -> bool {
			if (!target) return false;
			if (target->m != sd->m || pc_isdead(target) ) return false;
			if (target == sd && skill->inf2[INF2_NOTARGETSELF]) return false;
			return true;
			};

		auto use_buff_on_target = [&](map_session_data* target) -> bool {
			if (!check_distance(sd->x - target->x, sd->y - target->y, ab_skill_range) ||
				!path_search_long(nullptr, sd->m, sd->x, sd->y, target->x, target->y, CELL_CHKWALL)) {
				can_walk = unit_walktobl(sd, target, ab_skill_range, 1);
				if (!can_walk) {
					skip = true;
					return false;
				}
			}
			buff_used = unit_skilluse_id(
				sd,
				(skill->inf & INF_SELF_SKILL) ? sd->id : target->id,
				encore ? BD_ENCORE : buff.skill_id,
				encore ? 1 : buff.skill_lv
			);
			return buff_used;
			};

		switch (buff.priorize_buff) {
		case AB_SELF:
			if (needs_buff(sd)) {
				buff_used = unit_skilluse_id(sd, sd->id, encore ? BD_ENCORE : buff.skill_id, encore ? 1 : buff.skill_lv);
			}
			break;

		case AB_LEADER:
			if (leader_sd && valid_target(leader_sd) && needs_buff(leader_sd)) {
				use_buff_on_target(leader_sd);
			}
			break;

		case AB_NAMEDPLAYER:
			for (auto char_id : buff.priorize_buff_char_id) {
				map_session_data* target_sd = map_charid2sd(char_id);
				if (!valid_target(target_sd) || !needs_buff(target_sd)) continue;
				if (use_buff_on_target(target_sd)) break;
			}
			break;

		case AB_ALL:
		case AB_EVERYONEEXCEPTSELF:
			for (int i = 0; i < MAX_PARTY; ++i) {
				map_session_data* target_sd = map_charid2sd(p->party.member[i].char_id);
				if (!valid_target(target_sd)) continue;
				if (buff.priorize_buff == AB_EVERYONEEXCEPTSELF && target_sd == sd) continue;
				if (needs_buff(target_sd) && use_buff_on_target(target_sd)) break;
			}
			break;
		}

		if (buff_used) {
			skip = true;
			buff.last_use = last_tick + skill_get_cast(buff.skill_id, buff.skill_lv) + skill_get_delay(buff.skill_id, buff.skill_lv);
			sd->ab.skill_cd = std::max(
				sd->ab.skill_cd,
				last_tick + battle_config.feature_autobuff_bskill_delay + skill_get_cast(buff.skill_id, buff.skill_lv)
			);
		}
	}
}


void ab_token_respawn(block_list* target, int flag) {
	if (target->type == BL_PC && flag) {
		map_session_data* ab_psd = (TBL_PC*)target;
		if (ab_psd && ab_psd->sc.getSCE(SC_AUTOBUFF)) {
			if (ab_psd->ab.autobuff_token_siegfried)
				pc_revive_item(ab_psd);
			else if (ab_psd->ab.return_to_savepoint)
				pc_respawn(ab_psd, CLR_OUTSIGHT);
		}
	}
	return;
}

bool ab_walk(struct block_list* bl, short x, short y, unsigned char flag) {
	struct unit_data* ud = NULL;
	ud = unit_bl2ud(bl);

	if (ud == NULL)
		return 0;

	// start the new walk if we're more  than 3 cells of the destination
	if ((abs(x - ud->to_x) > 2) || (abs(y - ud->to_y) > 2) || (ud->walktimer == INVALID_TIMER)) {
		return unit_walktoxy(bl, x, y, flag);
	}

	return 0;
}

bool ab_partymessage(map_session_data* sd, std::string key, char* message, int delay) {
	if (!sd)
		return false;

	auto& party_msg = sd->ab.party_msg;

	if (party_msg.empty()) {
		// Ajouter directement le message et le dÔö£┬«lai si le vecteur est vide
		party_msg.emplace_back(key, gettick() + delay * 1000);
	}
	else {
		// Rechercher si le message pour cette clÔö£┬« a dÔö£┬«jÔö£├í Ôö£┬«tÔö£┬« envoyÔö£┬«
		auto it = std::find_if(party_msg.begin(), party_msg.end(),
			[&key](const std::pair<std::string, t_tick>& msg) { return msg.first == key; });

		if (it != party_msg.end()) {
			// VÔö£┬«rifier le dÔö£┬«lai avant d'envoyer un nouveau message
			if (gettick() < it->second) {
				return false;  // Ne pas envoyer si le dÔö£┬«lai n'est pas encore atteint
			}
			// Mettre Ôö£├í jour le dÔö£┬«lai du message
			it->second = gettick() + delay * 1000;
		}
		else {
			// Ajouter une nouvelle entrÔö£┬«e avec le dÔö£┬«lai du message
			party_msg.emplace_back(key, gettick() + delay * 1000);
		}
	}

	party_send_message(sd, message, (int)strlen(message) + 1);

	return true;
}

bool ab_canuseskill(map_session_data* sd, uint16 skill_id, uint16 skill_lv) {
    int inf = skill_get_inf(skill_id);
	t_tick tick = gettick();

	if (pc_checkskill(sd, skill_id) < skill_lv)
		return false;

	if (skill_get_sp(skill_id, skill_lv) > sd->battle_status.sp)
		return false;

	if (battle_config.idletime_option & IDLE_USESKILLTOID)
		sd->idletime = tick;

	if ((pc_cant_act2(sd) || sd->chatID) && skill_id != RK_REFRESH && !(skill_id == SR_GENTLETOUCH_CURE &&
		(sd->sc.opt1 == OPT1_STONE || sd->sc.opt1 == OPT1_FREEZE || sd->sc.opt1 == OPT1_STUN)) &&
		sd->state.storage_flag && !(inf & INF_SELF_SKILL))
		return false;

	if (pc_issit(sd))
		return false;

	if (skill_isNotOk(skill_id, *sd))
		return false;

	if (!skill_check_condition_castbegin(*sd, skill_id, skill_lv) || !skill_check_condition_castend(*sd, skill_id, skill_lv))
		return false;

	if (sd->ud.skilltimer != INVALID_TIMER) {
		if (skill_id != SA_CASTCANCEL && skill_id != SO_SPELLFIST)
			return false;
	}
	else if (DIFF_TICK(tick, sd->ud.canact_tick) < 0) {
		if (sd->skillitem != skill_id)
			return false;
	}

	if (sd->sc.option & OPTION_COSTUME)
		return false;

	if (sd->sc.getSCE(SC_BASILICA) && (skill_id != HP_BASILICA || sd->sc.getSCE(SC_BASILICA)->val4 != sd->id))
		return false; // On basilica only caster can use Basilica again to stop it.

	if (sd->menuskill_id) {
		if (sd->menuskill_id == SA_TAMINGMONSTER) {
			clif_menuskill_clear(sd); //Cancel pet capture.
		}
		else if (sd->menuskill_id != SA_AUTOSPELL)
			return false; //Can't use skills while a menu is open.
	}

	skill_lv = min(pc_checkskill(sd, skill_id), skill_lv); //never trust client

	pc_delinvincibletimer(sd);

	return true;
}

int ab_targetresu(struct block_list* bl, va_list ap) {
	nullpo_ret(bl);
	int* tbl_dead_id = va_arg(ap, int*);
	map_session_data* party_sd = nullptr;

	// source id
	int src_id = va_arg(ap, int);
	map_session_data* sd = map_id2sd(src_id);
	nullpo_ret(sd);

	//target id
	map_session_data* tsd = map_id2sd(bl->id);
	nullpo_ret(tsd);

	int i = 0;

	// Check if the target is dead
	if (pc_isdead(tsd)) {
		// Check if the target is in the party
		struct party_data* p = party_search(sd->status.party_id);

		for (int i = 0; i < MAX_PARTY; i++) {
			if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
				continue;

			party_sd = map_charid2sd(p->party.member[i].char_id);

			if (party_sd == nullptr)
				continue;

			if (party_sd == tsd)
				break;
		}

		if (i < MAX_PARTY) {
			*tbl_dead_id = bl->id;
			return 1;
		}
	}

	return 0;
}

void ab_save(map_session_data* sd) {

	// Insert ab_common_config
	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"INSERT INTO `ab_common_config` (`char_id`,`following_player`,`dist_to_leader`,`autobuff_resurection`,`skill_cd`,`allow_pm_cmd`,`autobuff_potions`,`return_to_savepoint`,`autobuff_token_siegfried`,`autobuff_disable_alone`,`priorize_buff`) "
		"VALUES (%u, %u, %" PRIu16 ", %d, %" PRId64 ", %d, %d, %d, %d, %d, %d) "
		"ON DUPLICATE KEY UPDATE `following_player` = %u, `dist_to_leader` = %" PRIu16 ", `autobuff_resurection` = %d, `skill_cd` = %" PRId64 ", `allow_pm_cmd` = %d, `autobuff_potions` = %d, `return_to_savepoint` = %d, `autobuff_token_siegfried` = %d, `autobuff_disable_alone` = %d, `priorize_buff` = %d",
		sd->status.char_id,
		sd->ab.following_player,
		sd->ab.dist_to_leader,
		sd->ab.autobuff_resurection,
		sd->ab.skill_cd,
		sd->ab.allow_pm_cmd,
		sd->ab.state_autobuff_potions,
		sd->ab.return_to_savepoint,
		sd->ab.autobuff_token_siegfried,
		sd->ab.autobuff_disable_alone,
		sd->ab.priorize_buff,
		sd->ab.following_player,
		sd->ab.dist_to_leader,
		sd->ab.autobuff_resurection,
		sd->ab.skill_cd,
		sd->ab.allow_pm_cmd,
		sd->ab.state_autobuff_potions,
		sd->ab.return_to_savepoint,
		sd->ab.autobuff_token_siegfried,
		sd->ab.autobuff_disable_alone,
		sd->ab.priorize_buff)
		) {
		Sql_ShowDebug(mmysql_handle);
	}

	// Clean ab_items
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `ab_items` WHERE `char_id` = %u", sd->status.char_id)) {
		Sql_ShowDebug(mmysql_handle);
	}

	// Insert ab_items - 0 - autobuffitems
	if (!sd->ab.autobuff_buffitems.empty()) {
		for (auto& itAutobuffitem : sd->ab.autobuff_buffitems) {
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT INTO `ab_items` (`char_id`, `type`, `item_id`, `status`) "
				"VALUES (%u, 0, %" PRIu16 ", %d)",
				sd->status.char_id, itAutobuffitem.item_id, itAutobuffitem.status))
			{
				Sql_ShowDebug(mmysql_handle);
			}
		}
	}

	// Insert ab_items - 1 - autopotions
	if (!sd->ab.autobuff_potions.empty()) {
		for (auto& itAutopotion : sd->ab.autobuff_potions) {
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT INTO `ab_items` (`char_id`,`type`,`item_id`,`min_hp`,`min_sp`) VALUES (%u, 1, %u, %" PRIu16 ", %" PRIu16 ")",
				sd->status.char_id, itAutopotion.item_id, itAutopotion.min_hp, itAutopotion.min_sp)
				) {
				Sql_ShowDebug(mmysql_handle);
			}
		}
	}

	// Clean ab_skills_char_ids
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `ab_skills_char_ids` WHERE `ab_skills_id` IN (SELECT `ab_skills_id` FROM `ab_skills` WHERE `char_id` = %u)", sd->status.char_id)) {
		Sql_ShowDebug(mmysql_handle);
	}

	// Clean ab_skills
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `ab_skills` WHERE `char_id` = %u", sd->status.char_id)) {
		Sql_ShowDebug(mmysql_handle);
	}

	// Insert autobuff_heal
	if (!sd->ab.autobuff_heal.empty()) {
		for (auto& itAutoheal : sd->ab.autobuff_heal) {
			uint64 ab_skills_id = 0;
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT INTO `ab_skills` (`char_id`,`type`,`skill_id`,`skill_lv`,`min_hp`,`last_use`,`priorize_type`) VALUES (%u, 0, %" PRIu16 ", %" PRIu16 ", %" PRIu16 ", %" PRId64 ", %d)",
				sd->status.char_id, itAutoheal.skill_id, itAutoheal.skill_lv, itAutoheal.min_hp, itAutoheal.last_use, itAutoheal.priorize_heal)
				) {
				Sql_ShowDebug(mmysql_handle);
			}

			ab_skills_id = Sql_LastInsertId(mmysql_handle);

			if (ab_skills_id > 0) {
				for (auto& char_id : itAutoheal.priorize_heal_char_id) {
					if (SQL_ERROR == Sql_Query(mmysql_handle,
						"INSERT INTO `ab_skills_char_ids` (`ab_skills_id`,`ab_char_id`) VALUES (%" PRIu64 ", %u)",
						ab_skills_id, char_id)
						) {
						Sql_ShowDebug(mmysql_handle);
					}
				}
			}
		}
	}

	// Insert autobuff_buffskills
	if (!sd->ab.autobuff_buffskills.empty()) {
		for (auto& itAutobuffskills : sd->ab.autobuff_buffskills) {
			uint64 ab_skills_id = 0;
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT INTO `ab_skills` (`char_id`,`type`,`skill_id`,`skill_lv`,`last_use`,`priorize_type`) VALUES (%u, 1, %" PRIu16 ", %" PRIu16 ", %" PRId64 ", %d)",
				sd->status.char_id, itAutobuffskills.skill_id, itAutobuffskills.skill_lv, itAutobuffskills.last_use, itAutobuffskills.priorize_buff)
				) {
				Sql_ShowDebug(mmysql_handle);
			}

			ab_skills_id = Sql_LastInsertId(mmysql_handle);

			if (ab_skills_id > 0) {
				for (auto& char_id : itAutobuffskills.priorize_buff_char_id) {
					if (SQL_ERROR == Sql_Query(mmysql_handle,
						"INSERT INTO `ab_skills_char_ids` (`ab_skills_id`,`ab_char_id`) VALUES (%" PRIu64 ", %u)",
						ab_skills_id, char_id)
						) {
						Sql_ShowDebug(mmysql_handle);
					}
				}
			}
		}
	}

	// Handle authorized pm player
	// Clean ab_pm_char_ids
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `ab_pm_char_ids` WHERE `char_id` = %u", sd->status.char_id)) {
		Sql_ShowDebug(mmysql_handle);
	}

	// Insert ab_pm_char_ids
	if (!sd->ab.allow_pm_char_id.empty()) {
		for (auto& pmCharId : sd->ab.allow_pm_char_id) {
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT INTO `ab_pm_char_ids` (`char_id`,`ab_char_id`) VALUES (%u, %u)",
				sd->status.char_id, pmCharId)
				) {
				Sql_ShowDebug(mmysql_handle);
			}
		}
	}
		// ADD THIS CODE RIGHT HERE, BEFORE THE FINAL }
// Save AutoBuff duration with validation
if (sd->state.autobuff && sd->sc.getSCE(SC_AUTOBUFF)) {
    struct status_change_entry* sce = sd->sc.getSCE(SC_AUTOBUFF);
    if (sce && sce->timer != INVALID_TIMER) {
        int remaining_time = DIFF_TICK(get_timer(sce->timer)->tick, gettick());

        // Save if there's meaningful time left (more than 5 seconds)
        if (remaining_time > 5000) {
            sd->ab.duration_ = remaining_time;
            ShowInfo("ab_save: Saved AutoBuff duration %d for %s\n", sd->ab.duration_, sd->status.name);
        } else {
            // Don't save very short durations
            sd->ab.duration_ = 0;
            pc_setaccountreg(sd, add_str("#ab_duration"), 0);
			ShowInfo("ab_save: Cleared short AutoBuff duration for %s\n", sd->status.name);
        }
    }
} else {
    // Clear duration if not active
    sd->ab.duration_ = 0;
    pc_setaccountreg(sd, add_str("#ab_duration"), 0);
}
}

void ab_load(map_session_data* sd) {
	int type;
	struct party_data* p = nullptr;

	if (!sd) {
		ShowError("ab_load: sd is null\n");
		return;
	}

		ShowInfo("ab_load: Loading AutoBuff config for player %s (char_id: %u, connect_new: %d)\n",
		         sd->status.name, sd->status.char_id, sd->state.connect_new);
	if (sd->sc.getSCE(SC_AUTOBUFF))
		status_change_end(sd, SC_AUTOBUFF);

	// Check if the player is in a party
	if (sd->status.party_id)
		p = party_search(sd->status.party_id);

	// ab_common_config
		if (Sql_Query(mmysql_handle,
		    "SELECT `following_player`,`dist_to_leader`,`autobuff_resurection`,`skill_cd`,`allow_pm_cmd`,`autobuff_potions`,`return_to_savepoint`,`autobuff_token_siegfried`,`autobuff_disable_alone`,`priorize_buff` "
		    "FROM `ab_common_config` "
		    "WHERE `char_id` = %u",
		    sd->status.char_id) != SQL_SUCCESS) {
		    Sql_ShowDebug(mmysql_handle);
		    ShowError("ab_load: Failed to load common config for char_id %u\n", sd->status.char_id);
		    return;
		}

	if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			char* data;
			Sql_GetData(mmysql_handle, 0, &data, NULL); sd->ab.following_player = static_cast<uint32>(atoi(data));
			Sql_GetData(mmysql_handle, 1, &data, NULL); sd->ab.dist_to_leader = static_cast<uint16>(atoi(data));
			Sql_GetData(mmysql_handle, 2, &data, NULL); sd->ab.autobuff_resurection = static_cast<bool>(atoi(data));
			Sql_GetData(mmysql_handle, 3, &data, NULL); sd->ab.skill_cd = 0; // Reset cooldown after restart
			Sql_GetData(mmysql_handle, 4, &data, NULL); sd->ab.allow_pm_cmd = static_cast<ab_pmcmdconfig>(atoi(data)); // Cast vers enum
			Sql_GetData(mmysql_handle, 5, &data, NULL); sd->ab.state_autobuff_potions = static_cast<bool>(atoi(data));
			Sql_GetData(mmysql_handle, 6, &data, NULL); sd->ab.return_to_savepoint = static_cast<bool>(atoi(data));
			Sql_GetData(mmysql_handle, 7, &data, NULL); sd->ab.autobuff_token_siegfried = static_cast<bool>(atoi(data));
			Sql_GetData(mmysql_handle, 8, &data, NULL); sd->ab.autobuff_disable_alone = static_cast<bool>(atoi(data));
			Sql_GetData(mmysql_handle, 9, &data, NULL); sd->ab.priorize_buff = static_cast<bool>(atoi(data));

        // Add this line:
        sd->ab.duration_ = 0; // or load from database if it should be persistent
		}
	}
	else {
		sd->ab.following_player = 0;
		sd->ab.dist_to_leader = 2;
		sd->ab.autobuff_resurection = true;
		sd->ab.skill_cd = gettick();
		sd->ab.allow_pm_cmd = AB_PM_ALLOWPM;
		sd->ab.state_autobuff_potions = true;
		sd->ab.return_to_savepoint = false;
		sd->ab.autobuff_token_siegfried = false;
		sd->ab.autobuff_disable_alone = true;
		sd->ab.priorize_buff = false;
		sd->ab.duration_ = 0;
	}
	Sql_FreeResult(mmysql_handle);

	// ab_items
	if (Sql_Query(mmysql_handle,
		"SELECT `type`,`item_id`,`min_hp`,`min_sp`,`status` "
		"FROM `ab_items` "
		"WHERE `char_id` = %u",
		sd->status.char_id) != SQL_SUCCESS) {
		Sql_ShowDebug(mmysql_handle);
		return;
	}

if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			char* data;
			Sql_GetData(mmysql_handle, 0, &data, NULL); type = atoi(data);

			switch (type) {
			case 0: {
				// Autobuff item - declare variable inside braces
				s_autobuff_buffitems autobuffitem;
				autobuffitem.is_active = true;  // Now this line works
				Sql_GetData(mmysql_handle, 1, &data, NULL); autobuffitem.item_id = static_cast<uint32>(atoi(data));
				Sql_GetData(mmysql_handle, 4, &data, NULL); autobuffitem.status = atoi(data);
				sd->ab.autobuff_buffitems.push_back(autobuffitem);
				break;
			}
			case 1: {
				// Autopotion - declare variable inside braces
				s_autobuff_potions autopotion;
				autopotion.is_active = true;  // Now this line works
				Sql_GetData(mmysql_handle, 1, &data, NULL); autopotion.item_id = static_cast<uint32>(atoi(data));
				Sql_GetData(mmysql_handle, 2, &data, NULL); autopotion.min_hp = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 3, &data, NULL); autopotion.min_sp = static_cast<uint16>(atoi(data));
				sd->ab.autobuff_potions.push_back(autopotion);
				break;
			}
			}
		}
	}
	Sql_FreeResult(mmysql_handle);

	// ab_skills
	if (Sql_Query(mmysql_handle,
		"SELECT `type`,`skill_id`,`skill_lv`,`min_hp`,`last_use`,`priorize_type`,`ab_skills_id` "
		"FROM `ab_skills` "
		"WHERE `char_id` = %u",
		sd->status.char_id) != SQL_SUCCESS) {
		Sql_ShowDebug(mmysql_handle);
		ShowError("ab_load: Failed to load skills for char_id %u\n", sd->status.char_id);
		return;
	}

	if (Sql_NumRows(mmysql_handle) > 0) {
		ShowInfo("ab_load: Loading %llu skills for %s\n", Sql_NumRows(mmysql_handle), sd->status.name);
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			uint64 ab_skills_id = 0;
			char* data;


			Sql_GetData(mmysql_handle, 0, &data, NULL); type = atoi(data);

			switch (type) {
			case 0: {
				// Heal skill
				s_autobuff_heal autoheal;
				autoheal.is_active = true;  // CRITICAL: Mark as active
				Sql_GetData(mmysql_handle, 1, &data, NULL); autoheal.skill_id = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 2, &data, NULL); autoheal.skill_lv = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 3, &data, NULL); autoheal.min_hp = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 4, &data, NULL); autoheal.last_use = 0; // Reset to allow immediate use
				Sql_GetData(mmysql_handle, 5, &data, NULL); autoheal.priorize_heal = static_cast<ab_targettype>(atoi(data));
				Sql_GetData(mmysql_handle, 6, &data, NULL); ab_skills_id = static_cast<uint64>(atoll(data));
				// Load members
				if (Sql_Query(qsmysql_handle,
					"SELECT `ab_char_id` FROM `ab_skills_char_ids` WHERE `ab_skills_id` = %" PRIu64, ab_skills_id) == SQL_SUCCESS) {
					while (SQL_SUCCESS == Sql_NextRow(qsmysql_handle)) {
						Sql_GetData(qsmysql_handle, 0, &data, NULL);
						uint32 priorize_heal_char_id = static_cast<uint32>(atoi(data));
						// Simply store all character IDs - party validation happens at runtime
						autoheal.priorize_heal_char_id.insert(priorize_heal_char_id);
					}
				}
				Sql_FreeResult(qsmysql_handle);
				sd->ab.autobuff_heal.push_back(autoheal);
				break;
			}
			case 1: {
				// Buff skill
				s_autobuff_buffskills autobuffskill;
				autobuffskill.is_active = true;  // CRITICAL: Mark as active
				Sql_GetData(mmysql_handle, 1, &data, NULL); autobuffskill.skill_id = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 2, &data, NULL); autobuffskill.skill_lv = static_cast<uint16>(atoi(data));
				Sql_GetData(mmysql_handle, 4, &data, NULL); autobuffskill.last_use = 0; // Reset to allow immediate use
				Sql_GetData(mmysql_handle, 5, &data, NULL); autobuffskill.priorize_buff = static_cast<ab_targettype>(atoi(data));
				Sql_GetData(mmysql_handle, 6, &data, NULL); ab_skills_id = static_cast<uint64>(atoll(data));

				ShowInfo("ab_load: Loading buff skill %u (lv %u) for %s\n",
						 autobuffskill.skill_id, autobuffskill.skill_lv, sd->status.name);

				// Load character priorities
				if (Sql_Query(qsmysql_handle,
					"SELECT `ab_char_id` FROM `ab_skills_char_ids` WHERE `ab_skills_id` = %" PRIu64, ab_skills_id) == SQL_SUCCESS) {
					while (SQL_SUCCESS == Sql_NextRow(qsmysql_handle)) {
						Sql_GetData(qsmysql_handle, 0, &data, NULL);
						uint32 priorize_buff_char_id = static_cast<uint32>(atoi(data));
						// Simply store all character IDs - party validation happens at runtime
						autobuffskill.priorize_buff_char_id.insert(priorize_buff_char_id);
					}
				}
				Sql_FreeResult(qsmysql_handle);
				sd->ab.autobuff_buffskills.push_back(autobuffskill);
				break;
			}
			} // End of switch
		} // End of while loop
	} else {
	}
	Sql_FreeResult(mmysql_handle);

	// ab_pm_char_ids
	if (Sql_Query(mmysql_handle,
		"SELECT `ab_char_id` FROM `ab_pm_char_ids` WHERE `char_id` = %u",
		sd->status.char_id) != SQL_SUCCESS) {
		Sql_ShowDebug(mmysql_handle);
		return;
	}

	if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			char* data;
			Sql_GetData(mmysql_handle, 0, &data, NULL);
			sd->ab.allow_pm_char_id.insert(static_cast<uint32>(atoi(data)));
		}
	}
	Sql_FreeResult(mmysql_handle);

// Improved state restoration logic for server restarts
	bool is_new_connection = (sd->state.connect_new != 0);

	// For server restarts, always use delayed restoration
	if (is_new_connection) {
		// New connection or server restart - use timer with longer delay
		add_timer(gettick() + 8000, ab_restore_state_timer, sd->id, 0);
	} else {
		// Existing connection (portal warp) - try immediate read first
		sd->ab.duration_ = static_cast<int>(pc_readaccountreg(sd, add_str("#ab_duration")));

		if (sd->ab.duration_ > 0) {
			// Portal warp with saved duration - restore quickly
			add_timer(gettick() + 1000, ab_restore_state_timer, sd->id, 100);
		}
	}
	return;
}

// Backup restoration function called later in the login process
int ab_delayed_restore_timer(int tid, int64 tick, int id, intptr_t data) {
    map_session_data* sd = map_id2sd(id);
    if (!sd) {
        return 0;
    }

    // Only restore if AutoBuff isn't already active
    if (!sd->state.autobuff && !sd->sc.getSCE(SC_AUTOBUFF)) {
        int saved_duration = static_cast<int>(pc_readaccountreg(sd, add_str("#ab_duration")));

        if (saved_duration > 0) {
            ShowInfo("ab_delayed_restore_timer: Late restoration for %s with duration %d\n",
                     sd->status.name, saved_duration);

            if (status_change_start(sd, sd, SC_AUTOBUFF, 10000, 0, 0, 0, 0,
                                   saved_duration, SCSTART_NOAVOID)) {
                pc_setaccountreg(sd, add_str("#ab_duration"), 0);
                ShowInfo("ab_delayed_restore_timer: Successfully restored AutoBuff for %s\n", sd->status.name);
            }
        }
    }

    return 0;
}

ab_commandHandler::ab_commandHandler() {
	command_map = {
		{"clear", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_clear_command(sd, ssd, cmd); }},
		{"stop", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_stop_command(sd, ssd, cmd); }},
		{"start", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_start_command(sd, ssd, cmd); }},
		{"stay", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_stay_command(sd, ssd, cmd); }},
		{"move", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_move_command(sd, ssd, cmd); }},
		{"sit", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_sit_command(sd, ssd, cmd); }},
		{"stand", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_stand_command(sd, ssd, cmd); }},
		{"logout", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_logout_command(sd, ssd, cmd); }},
		{"go", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_go_command(sd, ssd, cmd); }},
		{"heal", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"bless", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"agi", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"ress", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"kyrie", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"pneuma", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"safety", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"impo", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"suffra", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_buff_command(sd, ssd, cmd); }},
		{"follow", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_follow_command(sd, ssd, cmd); }},
		{"warp", [this](map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) { return ab_warp_command(sd, ssd, cmd); }},
	};
}

// Autobuff command msg functions
bool ab_commandHandler::ab_clear_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	sd->ab.order_msg.clear();
	return true;
}

bool ab_commandHandler::ab_stop_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	sd->state.ab_stop = true;
	return true;
}

bool ab_commandHandler::ab_start_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	sd->state.ab_stop = false;
	return true;
}

bool ab_commandHandler::ab_stay_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	sd->state.ab_stay = true;
	return true;
}

bool ab_commandHandler::ab_move_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	sd->state.ab_stay = false;
	return true;
}

bool ab_commandHandler::ab_stand_command(map_session_data *sd, map_session_data *ssd, std::vector<std::string> cmd) {
	sd->state.ab_stay = false;
	return true;
}

bool ab_commandHandler::ab_sit_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	if (!pc_issit(sd)) {
		pc_setsit(sd);
		skill_sit(sd, true);
		clif_standing(*sd);
	}
	return true;
}

bool ab_commandHandler::ab_logout_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	if (session_isActive(sd->fd))
		clif_parse_QuitGame(sd->fd, sd);
	else
		map_quit(sd);
	return true;
}

bool ab_commandHandler::ab_go_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	int dest_x, dest_y;
	bool walk_result = false;

	//Not enough args
	if (cmd.size() != 3) { //todoab tell player right command arg
		std::string msg = "Autobuff : send the following private message : go <x> <y>";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
		return false;
	}

	try {
		dest_x = std::stoi(cmd[1]);
	}
	catch (const std::invalid_argument&) {
		std::string msg = "Autobuff : send the following private message : go <x> <y>";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
		return false;
	}
	catch (const std::out_of_range&) {
		std::string msg = "Autobuff : send the following private message : go <x> <y>";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
		return false;
	}

	try {
		dest_y = std::stoi(cmd[2]);
	}
	catch (const std::invalid_argument&) {
		std::string msg = "Autobuff : send the following private message : go <x> <y>";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
		return false;
	}
	catch (const std::out_of_range&) {
		std::string msg = "Autobuff : send the following private message : go <x> <y>";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
		return false;
	}

	if ((abs(dest_x - sd->x) > 2) || (abs(dest_y - sd->y) > 2)) {
		walk_result = ab_walk(sd, dest_x, dest_y, 8);
	}
	if (!walk_result) {
		if ((abs(sd->x - dest_x) > 6) || (abs(sd->y - dest_y) > 6)
			|| !(path_search_long(NULL, sd->m, sd->x, sd->y, dest_x, dest_y, CELL_CHKNOPASS))
			) {
			struct walkpath_data wpd1;
			if (path_search(&wpd1, sd->m, sd->x, sd->y, dest_x, dest_y, 0, CELL_CHKNOPASS))
				walk_result = ab_walk(sd, sd->x + dirx[wpd1.path[0]], sd->y + diry[wpd1.path[0]], 8);
		}
	}

	if (!walk_result) {
		std::string msg = "Autobuff : I can't go to the coordinates that were sent";
		ab_partymessage(sd, "goCmd", msg.data(), 5);
	}

	//Stay at the indicated location until move cmd
	if (walk_result)
		sd->state.ab_stay = true;

	return walk_result;
}

int ab_commandHandler::ab_buff_command(map_session_data *sd, map_session_data *ssd, std::vector<std::string> cmd) {
	uint16 skill_id, skill_lv;
	bool buff_used = false;

	map_session_data* pl_sd;

	//Not enough args
	if (cmd.size() != 2) {
		std::string msg = "Autobuff : send the following private message : bless, agi, kyrie, heal, pneuma, safety <me,you,player name>";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	// Check if cmd target is right
	if (cmd[1] != "me" && cmd[1] != "you") {
		if ((pl_sd = map_nick2sd(cmd[1].c_str(), true)) == nullptr)
		{
			std::string msg = "Autobuff : the player isn't online or doesn't exist";
			ab_partymessage(sd, "buffCmd", msg.data(), 5);
			return false;
		}
		else { // Check if on the same map
			if (pl_sd->m != sd->m) {
				std::string msg = "Autobuff : I can't buff, the player is not on the same map";
				ab_partymessage(sd, "buffCmd", msg.data(), 5);
				return false;
			}
		}
	}
	else if (cmd[1] == "me" && ssd->m != sd->m) { // Check if on the same map
		std::string msg = "Autobuff : I can't buff, we're not on the same map";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (gettick() < sd->ab.skill_cd) {
		std::string msg = "Autobuff : I can't buff because of skill delay";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	//check if the buff have the skill
	if (cmd[0] == "bless") {
		skill_id = AL_BLESSING;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "agi") {
		skill_id = AL_INCAGI;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "kyrie") {
		skill_id = PR_KYRIE;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "heal") {
		skill_id = AL_HEAL;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "ress") {
		skill_id = ALL_RESURRECTION;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "safety") {
		skill_id = MG_SAFETYWALL;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "pneuma") {
		skill_id = AL_PNEUMA;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "impo") {
		skill_id = PR_IMPOSITIO;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else if (cmd[0] == "suffra") {
		skill_id = PR_SUFFRAGIUM;
		skill_lv = pc_checkskill(sd, skill_id);
	}
	else {
		std::string msg = "Autobuff : unknown skill, you can only use bless, agi, kyrie, heal";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (skill_lv <= 0) {
		std::string msg = "Autobuff : I don't have that skill";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (!ab_canuseskill(sd, skill_id, skill_lv)) {
		std::string msg = "Autobuff : I can't use a skill right now";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return 2;
	}

	if (skill_id == ALL_RESURRECTION) {
		if (cmd[1] == "you") {
			std::string msg = "Autobuff : I can't ressurect myself";
			ab_partymessage(sd, "buffCmd", msg.data(), 5);
			return false;
		}
		else if (cmd[1] == "me" && !status_isdead(*ssd)) {
			std::string msg = "Autobuff : I can't ressurect, you aren't dead";
			ab_partymessage(sd, "buffCmd", msg.data(), 5);
			return false;
		}
		else if (pl_sd && !status_isdead(*pl_sd)) {
			std::string msg = "Autobuff : I can't ressurect, player isn't't dead";
			ab_partymessage(sd, "buffCmd", msg.data(), 5);
			return false;
		}
	}

	if ((skill_id == AL_PNEUMA || skill_id == MG_SAFETYWALL) && (sd->sc.getSCE(SC_PNEUMA) || sd->sc.getSCE(SC_SAFETYWALL))) {
		std::string msg = "Autobuff : I can't use pneuma or safety wall if you are already under a pneuma or on a safety wall";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (cmd[1] == "you") {
		buff_used = unit_skilluse_id(sd, sd->id, skill_id, skill_lv);
	}
#ifdef RENEWAL
	else if (cmd[1] == "me") {
		if (skill_id == PR_SUFFRAGIUM || skill_id == PR_IMPOSITIO)
			buff_used = unit_skilluse_id(sd, sd->id, skill_id, skill_lv);
		else
			buff_used = unit_skilluse_id(sd, ssd->id, skill_id, skill_lv);
	}
#else
	else if (cmd[1] == "me") {
		buff_used = unit_skilluse_id(sd, ssd->id, skill_id, skill_lv);
	}
#endif
	else {
		buff_used = unit_skilluse_id(sd, pl_sd->id, skill_id, skill_lv);
	}

	if (!buff_used) {
		std::string msg = "Autobuff : I can't buff for some reasons";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	sd->idletime = time(NULL);

	// Skill consumption
	skill_consume_requirement(sd, skill_id, skill_lv, 2);

	// General skill delay with min buff skill delay
	if (battle_config.feature_autobuff_bskill_delay
		&& skill_get_delay(skill_id, skill_lv) < battle_config.feature_autobuff_bskill_delay
		&& sd->ab.skill_cd < (gettick() + battle_config.feature_autobuff_bskill_delay)) {
		sd->ab.skill_cd = gettick() + battle_config.feature_autobuff_bskill_delay + skill_get_cast(skill_id, skill_lv);
	}

	return buff_used;
}

bool ab_commandHandler::ab_warp_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	bool buff_used = false;
	uint16 skill_id, skill_lv;
	int16 x, y;
	int warp_slot, i_warp_slot;
	std::shared_ptr<s_skill_unit_group> group;

	//Not enough args
	if (cmd.size() != 2) {
		std::string msg = "Autobuff : send the following private message : warp <warp number>";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (gettick() < sd->ab.skill_cd) {
		std::string msg = "Autobuff : I can't buff because of skill delay";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	skill_id = AL_WARP;
	skill_lv = pc_checkskill(sd, skill_id);

	if (skill_lv <= 0) {
		std::string msg = "Autobuff : I don't have that skill";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	int gemstone_index = pc_search_inventory(sd, ITEMID_BLUE_GEMSTONE);
	int gemstone_amount = (gemstone_index >= 0) ? sd->inventory.u.items_inventory[gemstone_index].amount : 0;

	if (gemstone_amount == 0) {
		std::string out_gemstone_msg = "I'm out of Blue Gemstones!";
		ab_partymessage(sd, "OutGemstone", out_gemstone_msg.data(), 600);
		return false;
	}


	i_warp_slot = std::stoi(cmd[1]);
	warp_slot = i_warp_slot - 1;

	// Check if cmd target is right
	if (i_warp_slot < 0 || warp_slot > skill_lv + 1) {
		std::string msg = "Autobuff : You need to send a correct warp number that exist on your char";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (!ab_canuseskill(sd, skill_id, skill_lv)) {
		std::string msg = "Autobuff : I can't use a skill right now";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	if (i_warp_slot && strcmp(sd->status.memo_point[warp_slot].map, "") == 0) {
		std::string msg = "Autobuff : You don't have a memo on this warp slot yet";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;
	}

	int range = (int)sqrt((float)1) + 2; // calculation of an odd number (+ 4 area around)
	if (!map_search_freecell(sd, 0, &x, &y, range, range, 0)) {
		std::string msg = "Autobuff : Skill failed";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;// failed
	}

	buff_used = unit_skilluse_pos(sd, x, y, skill_id, skill_lv);

	if (!buff_used) {
		std::string msg = "Autobuff : Skill failed";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;// failed
	}

	sd->menuskill_id = skill_id;
	sd->menuskill_val = (x << 16) | sd->ud.skilly; //Store warp position here.

	//It is possible to use teleport with the storage window open bugreport:8027
	if (pc_cant_act(sd) && !sd->state.storage_flag && skill_id != AL_TELEPORT) {
		clif_menuskill_clear(sd);
		std::string msg = "Autobuff : Skill failed";
		ab_partymessage(sd, "buffCmd", msg.data(), 5);
		return false;// failed
	}

	if (!i_warp_slot)
		buff_used = skill_castend_map(sd, skill_id, sd->status.save_point.map);
	else
		buff_used = skill_castend_map(sd, skill_id, sd->status.memo_point[warp_slot].map);

	sd->idletime = time(NULL);

	// Reset values
	sd->skillitem = sd->skillitemlv = sd->skillitem_keep_requirement = 0;

	// General skill delay with min buff skill delay
	if (battle_config.feature_autobuff_bskill_delay
		&& skill_get_delay(skill_id, skill_lv) < battle_config.feature_autobuff_bskill_delay
		&& sd->ab.skill_cd < (gettick() + battle_config.feature_autobuff_bskill_delay)) {
		sd->ab.skill_cd = gettick() + battle_config.feature_autobuff_bskill_delay + skill_get_cast(skill_id, skill_lv);
	}

	return buff_used;
}

bool ab_commandHandler::ab_follow_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd) {
	map_session_data* pl_sd;
	map_session_data* party_sd = nullptr;
	struct party_data* p = party_search(sd->status.party_id);
	int player_pos = 0;

	//Not enough args
	if (cmd.size() != 2) { //todoab tell player right command arg
		std::string msg = "Autobuff : send the following private message : follow <me,player name>";
		ab_partymessage(sd, "followCmd", msg.data(), 5);
		return false;
	}

	// Check if cmd target is right
	if (cmd[1] == "you") {
		std::string msg = "Autobuff : I can't follow myself";
		ab_partymessage(sd, "followCmd", msg.data(), 5);
		return false;
	}
	else if (cmd[1] == "me") {
		for (player_pos = 0; player_pos < MAX_PARTY; player_pos++) {
			if (p->party.member[player_pos].account_id == 0 || p->party.member[player_pos].char_id == 0)
				continue;

			party_sd = map_charid2sd(p->party.member[player_pos].char_id);

			if (party_sd == nullptr)
				continue;

			if (party_sd == ssd)
				break;
		}

		if (player_pos >= MAX_PARTY) {
			std::string msg = "Autobuff : I can't follow a player that is not in my party";
			ab_partymessage(sd, "followCmd", msg.data(), 5);
			return false;
		}

		sd->ab.following_player = ssd->status.char_id;
	}
	else {
		if ((pl_sd = map_nick2sd(cmd[1].c_str(), true)) == nullptr)
		{
			std::string msg = "Autobuff : the player isn't online or doesn't exist";
			ab_partymessage(sd, "followCmd", msg.data(), 5);
			return false;
		}

		for (player_pos = 0; player_pos < MAX_PARTY; player_pos++) {
			if (p->party.member[player_pos].account_id == 0 || p->party.member[player_pos].char_id == 0)
				continue;

			party_sd = map_charid2sd(p->party.member[player_pos].char_id);

			if (party_sd == nullptr)
				continue;

			if (party_sd == pl_sd)
				break;
		}

		if (player_pos >= MAX_PARTY) {
			std::string msg = "Autobuff : I can't follow a player that is not in my party";
			ab_partymessage(sd, "followCmd", msg.data(), 5);
			return false;
		}

		sd->ab.following_player = pl_sd->status.char_id;
	}

	sd->state.ab_stay = false;
	return true;
}

// 0 = init, 1 = start, 2 = stop
bool ab_changestate_autobuff(map_session_data *sd, int flag) {
    map_data* mapdata;
    map_session_data* pl_sd;
    struct s_mapiterator* iter;
    int ip_limitation = 0, fateshield_limitation = 0;

    switch (flag) {
    case 1:
        if (battle_config.feature_autobuff_iplimit) {
            iter = mapit_getallusers();
            for (pl_sd = (TBL_PC*)mapit_first(iter); mapit_exists(iter); pl_sd = (TBL_PC*)mapit_next(iter)) {
                if (pl_sd->id != sd->id &&
                    session[sd->fd]->client_addr == pl_sd->ab.client_addr &&
                    pl_sd->state.autobuff)
                    ip_limitation++;

                if (ip_limitation >= battle_config.feature_autobuff_iplimit) {
                    std::string msg = "There is already an account using autobuff";
                    ab_partymessage(sd, "FlagOff", msg.data(), 5);
                    mapit_free(iter);
                    return false;
                }
            }
            mapit_free(iter);
        }

        if (battle_config.feature_autobuff_fateshieldlimit) {
            // (your fateshield checks here)
        }

        mapdata = map_getmapdata(sd->m);

        if (!battle_config.feature_autobuff_allow_town && mapdata->getMapFlag(MF_TOWN))
            return false;

        if (!battle_config.feature_autobuff_allow_pvp && mapdata->getMapFlag(MF_PVP))
            return false;

        if (!battle_config.feature_autobuff_allow_gvg && mapdata_flag_gvg2(mapdata))
            return false;

        if (!battle_config.feature_autobuff_allow_bg && mapdata->getMapFlag(MF_BATTLEGROUND))
            return false;

        if (battle_config.feature_autobuff_hateffect) {
            // your hateffect logic
        }

        if (battle_config.feature_autobuff_prefixname) {
            // your prefixname logic
        }

        sd->state.autobuff = true;
        break;

    case 0:
        // turn off autobuff
        sd->state.autobuff = false;
        break;

    case 2:
        if (battle_config.feature_autobuff_hateffect) {
            // remove hateffect logic
        }
        sd->state.autobuff = false;
        break;
    }

    return true;
}