//===== rAthena Custom Source ==========================================
//= Auto Combat
//===== By: ============================================================
//= jsn (discord: jasonch35)
//===== Current Version: ===============================================
//= 20241211
//===== Description: ===================================================
//= Inspired by Ragnarok:EL
//======================================================================
#include <common/nullpo.hpp>
#include <common/random.hpp>

#include "autocombat.hpp"
#include "population_engine/core/pe_perf.hpp"
#include "battle.hpp"
#include "log.hpp"
#include "map.hpp"
#include "party.hpp"
#include "chat.hpp"
#include "pc.hpp"
#include "population_engine.hpp"
#include "storage.hpp"
#include "unit.hpp"

#include <float.h>
#include <math.h>
#include <cstdlib>
#include <set>
#include <utility>

// ADD THIS LINE HERE:
std::vector<s_autocombat_skill_delay> autocombat_custom_delays;

void autocombat_pc_login(map_session_data *sd)
{
	nullpo_retv(sd);
	int64 tick = gettick();
	int char_id = sd->status.char_id;
	int type;
	char *data;
	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `disable_normal_atk`, `retaliate`, `element_switch`, `disable_tp_skill`, `disable_flywing`, "
		"`tp_when_mvp`, `no_mob_delay`, `emergency_hp`, `sit_min_hp`, `sit_min_sp`, `loot_item_config`, `end_status_config`, "
		"`buff_items_mask`, `heal_lv_0`, `heal_min_0`, `heal_lv_1`, `heal_min_1`, `heal_lv_2`, `heal_min_2` "
		"FROM `autocombat_main` "
		"WHERE `char_id` = %d", char_id))
	{
		Sql_ShowDebug(mmysql_handle);
		return;
	}

	if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			Sql_GetData(mmysql_handle, 0, &data, nullptr); sd->ac.disable_normal_atk = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 1, &data, nullptr); sd->ac.retaliate = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 2, &data, nullptr); sd->ac.element_switch = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 3, &data, nullptr); sd->ac.teleport.disable_tp_skill = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 4, &data, nullptr); sd->ac.teleport.disable_flywing = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 5, &data, nullptr); sd->ac.teleport.tp_when_mvp = (bool)atoi(data);
			Sql_GetData(mmysql_handle, 6, &data, nullptr); sd->ac.teleport.no_mob_delay = (uint32)atoi(data);
			Sql_GetData(mmysql_handle, 7, &data, nullptr); sd->ac.teleport.emergency_hp = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 8, &data, nullptr); sd->ac.sit_min_hp = max((uint16)atoi(data), 5);
			Sql_GetData(mmysql_handle, 9, &data, nullptr); sd->ac.sit_min_sp = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 10, &data, nullptr); sd->ac.loot_item_config = (enum loot_item_config)atoi(data);
			Sql_GetData(mmysql_handle, 11, &data, nullptr); sd->ac.end_status_config = (uint8)atoi(data);
			Sql_GetData(mmysql_handle, 12, &data, nullptr); sd->ac.buffitems = (uint32)atoi(data);
			Sql_GetData(mmysql_handle, 13, &data, nullptr); sd->ac.healskills[0].skill_lv = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 14, &data, nullptr); sd->ac.healskills[0].min_hp_sp = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 15, &data, nullptr); sd->ac.healskills[1].skill_lv = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 16, &data, nullptr); sd->ac.healskills[1].min_hp_sp = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 17, &data, nullptr); sd->ac.healskills[2].skill_lv = (uint16)atoi(data);
			Sql_GetData(mmysql_handle, 18, &data, nullptr); sd->ac.healskills[2].min_hp_sp = (uint16)atoi(data);
		}
	}

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `type`, `item_id`, `min_hp_sp` "
		"FROM `autocombat_items` "
		"WHERE `char_id` = %d", char_id))
	{
		Sql_ShowDebug(mmysql_handle);
		return;
	}

	if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			Sql_GetData(mmysql_handle, 0, &data, nullptr); type = atoi(data);
			switch(type) {
			case 0: {
				s_hp_potions hp_potions;
				Sql_GetData(mmysql_handle, 1, &data, nullptr); hp_potions.item_id = atoi(data);
				Sql_GetData(mmysql_handle, 2, &data, nullptr); hp_potions.min_hp = (uint16)atoi(data);
				sd->ac.hp_potions.push_back(hp_potions);
				}
				break;
			case 1: {
				s_sp_potions sp_potions;
				Sql_GetData(mmysql_handle, 1, &data, nullptr); sp_potions.item_id = atoi(data);
				Sql_GetData(mmysql_handle, 2, &data, nullptr); sp_potions.min_sp = (uint16)atoi(data);
				sd->ac.sp_potions.push_back(sp_potions);
				}
				break;
			default:
				if ((enum loot_item_config)type == sd->ac.loot_item_config + 1) {
					Sql_GetData(mmysql_handle, 1, &data, NULL);
					if (atoi(data) > 0)
						sd->ac.loot_item_selected.push_back(atoi(data));
				}
				break;
			}
		}
	}

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `type`, `skill_id`, `skill_lv` "
		"FROM `autocombat_skills` "
		"WHERE `char_id` = %d", char_id))
	{
		Sql_ShowDebug(mmysql_handle);
		return;
	}

	if (Sql_NumRows(mmysql_handle) > 0) {
		while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
			Sql_GetData(mmysql_handle, 0, &data, nullptr); type = atoi(data);
			switch(type) {
			case 0: {
				struct s_buff_skills buffskills;
				Sql_GetData(mmysql_handle, 1, &data, nullptr); buffskills.skill_id = (uint16)atoi(data);
				Sql_GetData(mmysql_handle, 2, &data, nullptr); buffskills.skill_lv = (uint16)atoi(data);
				sd->ac.buffskills.push_back(buffskills);
				}
				break;
			case 1: {
				struct s_attack_skills attackskills;
				Sql_GetData(mmysql_handle, 1, &data, nullptr); attackskills.skill_id = (uint16)atoi(data);
				Sql_GetData(mmysql_handle, 2, &data, nullptr); attackskills.skill_lv = (uint16)atoi(data);
				sd->ac.attackskills.push_back(attackskills);
				}
				break;
			}
		}
	}

	Sql_FreeResult(mmysql_handle);

	sd->ac.last_hit = tick;
	sd->ac.last_teleport = tick;
	sd->ac.last_move = 0;
	sd->ac.attack_target_id = 0;
	sd->ac.target_id = 0;
}

static void ac_pc_save(map_session_data *sd)
{
	nullpo_retv(sd);
	StringBuf buf;
	StringBuf_Init(&buf);
	int char_id = sd->status.char_id;
	int i, n = 0;
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `autocombat_items` WHERE `char_id` = %d AND (type = 0 OR type = 1)", char_id))
		Sql_ShowDebug(mmysql_handle);
	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `autocombat_skills` WHERE `char_id` = %d", char_id))
		Sql_ShowDebug(mmysql_handle);

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"INSERT INTO `autocombat_main` (`char_id`, `disable_normal_atk`, `retaliate`, `element_switch`, `disable_tp_skill`, "
		"`disable_flywing`, `tp_when_mvp`, `no_mob_delay`, `emergency_hp`, `sit_min_hp`, `sit_min_sp`, `loot_item_config`, `end_status_config`, "
		"`buff_items_mask`, `heal_lv_0`, `heal_min_0`, `heal_lv_1`, `heal_min_1`, `heal_lv_2`, `heal_min_2`) "
		"VALUES (%d, %d, %d, %d, %d, %d, %d, %u, %u, %u, %u, %d, %u, %u, %u, %u, %u, %u, %u, %u) "
		"ON DUPLICATE KEY UPDATE "
		"`disable_normal_atk` = VALUES(`disable_normal_atk`), `retaliate` = VALUES(`retaliate`), `element_switch` = VALUES(`element_switch`), "
		"`disable_tp_skill` = VALUES(`disable_tp_skill`), `disable_flywing` = VALUES(`disable_flywing`), `tp_when_mvp` = VALUES(`tp_when_mvp`), "
		"`no_mob_delay` = VALUES(`no_mob_delay`), `emergency_hp` = VALUES(`emergency_hp`), `sit_min_hp` = VALUES(`sit_min_hp`), "
		"`sit_min_sp` = VALUES(`sit_min_sp`), `loot_item_config` = VALUES(`loot_item_config`), `end_status_config` = VALUES(`end_status_config`), `buff_items_mask` = VALUES(`buff_items_mask`), "
		"`heal_lv_0` = VALUES(`heal_lv_0`), `heal_min_0` = VALUES(`heal_min_0`), `heal_lv_1` = VALUES(`heal_lv_1`), `heal_min_1` = VALUES(`heal_min_1`), "
		"`heal_lv_2` = VALUES(`heal_lv_2`), `heal_min_2` = VALUES(`heal_min_2`)",
		// Insert values
		char_id,
		sd->ac.disable_normal_atk, sd->ac.retaliate, sd->ac.element_switch,
		sd->ac.teleport.disable_tp_skill, sd->ac.teleport.disable_flywing, sd->ac.teleport.tp_when_mvp,
		sd->ac.teleport.no_mob_delay, sd->ac.teleport.emergency_hp,
		sd->ac.sit_min_hp, sd->ac.sit_min_sp, sd->ac.loot_item_config, sd->ac.end_status_config, sd->ac.buffitems,
		sd->ac.healskills[0].skill_lv, sd->ac.healskills[0].min_hp_sp,
		sd->ac.healskills[1].skill_lv, sd->ac.healskills[1].min_hp_sp,
		sd->ac.healskills[2].skill_lv, sd->ac.healskills[2].min_hp_sp
	))
	{
		Sql_ShowDebug(mmysql_handle);
	}

	StringBuf_Printf(&buf, "INSERT INTO `autocombat_items` (`char_id`, `type`, `item_id`, `min_hp_sp`) VALUES ");

	for (i = 0; i < sd->ac.hp_potions.size(); i++) {
		if (n > 0) StringBuf_AppendStr(&buf, ",");
		StringBuf_Printf(&buf, "(%d, 0, %d, %d)", char_id, sd->ac.hp_potions[i].item_id, sd->ac.hp_potions[i].min_hp);
		n++;
    }

	for (i = 0; i < sd->ac.sp_potions.size(); i++) {
		if (n > 0) StringBuf_AppendStr(&buf, ",");
		StringBuf_Printf(&buf, "(%d, 1, %d, %d)", char_id, sd->ac.sp_potions[i].item_id, sd->ac.sp_potions[i].min_sp);
		n++;
    }

	if (n > 0) {
		if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
			Sql_ShowDebug(mmysql_handle);
	}

	n = 0;
	StringBuf_Clear(&buf);
	StringBuf_Printf(&buf, "INSERT INTO `autocombat_skills` (`char_id`, `type`, `skill_id`, `skill_lv`) VALUES ");

	for (i = 0; i < sd->ac.buffskills.size(); i++) {
		if (n > 0) StringBuf_AppendStr(&buf, ",");
		StringBuf_Printf(&buf, "(%d, 0, %d, %d)", char_id, sd->ac.buffskills[i].skill_id, sd->ac.buffskills[i].skill_lv);
		n++;
    }

	for (i = 0; i < sd->ac.attackskills.size(); i++) {
		if (n > 0) StringBuf_AppendStr(&buf, ",");
		StringBuf_Printf(&buf, "(%d, 1, %d, %d)", char_id, sd->ac.attackskills[i].skill_id, sd->ac.attackskills[i].skill_lv);
		n++;
    }

	if (n > 0) {
		if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
			Sql_ShowDebug(mmysql_handle);
	}

// 	StringBuf_Destroy(&buf);
}

static int ac_generate_destination(int16 m, int16 *x, int16 *y)
{
    struct map_data *mapd = map_getmapdata(m);
	int i = 0;
	do {
		*x = rnd() % (mapd->xs - 2) + 1;
		*y = rnd() % (mapd->ys - 2) + 1;
	} while ((map_getcell(m, *x, *y, CELL_CHKNOPASS) || map_getcell(m, *x, *y, CELL_CHKNPC)) && (i++) < 1000);
	return i;
}

// Roam-destination picker used by autocombat_main's "no target" walk.
//
// Real players roam the WHOLE map (unchanged: falls through to
// ac_generate_destination).
//
// Population-engine AutoCombat shells are AFK farmers: they must stay in the
// pocket of the map where the GM placed them so that (a) when the mobs they
// were farming respawn, or the GM spawns fresh mobs, the shell is still within
// its 14-cell target scan and re-engages within one tick, and (b) A* paths stay
// short (cheap for thousands of shells). Without this a shell that clears its
// area random-walks to the far side of the map and never comes back — which
// looks exactly like "AutoCombat stopped after the monsters died".
#define AUTOCOMBAT_SHELL_ROAM_RADIUS 16
static void ac_generate_roam_destination(map_session_data *sd, int16 *x, int16 *y)
{
	nullpo_retv(sd);
	if (population_engine_is_population_pc(sd->id) && sd->pop.spawn_map_id == sd->m) {
		struct map_data *mapd = map_getmapdata(sd->m);
		if (mapd != nullptr) {
			const int16 cx = sd->pop.spawn_x;
			const int16 cy = sd->pop.spawn_y;
			const int R = AUTOCOMBAT_SHELL_ROAM_RADIUS;
			for (int tries = 0; tries < 30; tries++) {
				int tx = cx + (int)(rnd() % (2 * R + 1)) - R;
				int ty = cy + (int)(rnd() % (2 * R + 1)) - R;
				if (tx < 1) tx = 1;
				else if (tx > mapd->xs - 2) tx = mapd->xs - 2;
				if (ty < 1) ty = 1;
				else if (ty > mapd->ys - 2) ty = mapd->ys - 2;
				if (!map_getcell(sd->m, tx, ty, CELL_CHKNOPASS) &&
				    !map_getcell(sd->m, tx, ty, CELL_CHKNPC)) {
					*x = tx;
					*y = ty;
					return;
				}
			}
			// Anchor area unusable this time — keep the shell where it is
			// rather than flinging it across the map.
			*x = sd->x;
			*y = sd->y;
			return;
		}
	}
	ac_generate_destination(sd->m, x, y);
}

static int ac_randomwarp(map_session_data *sd)
{
	nullpo_ret(sd);
	int i = 0;
	int16 x, y, m = sd->m;

	i = ac_generate_destination(m, &x, &y);

	if (i < 1000)
		return pc_setpos(sd, map_id2index(m), x, y, CLR_TELEPORT);

	return 0;
}

static bool ac_teleport(map_session_data *sd, bool forced)
{
	int i;
	bool teleported = false;
	bool useitem = false;

    if (!sd->sc.getSCE(SC_AUTOCOMBAT) || sd->state.autotrade || map_getmapflag(sd->m, MF_NOTELEPORT)) {
        return teleported;
    }

    // Population-engine AutoCombat shells never teleport: pc_setpos on a
    // socket-less shell needs the engine's own re-add sequence, which this path
    // does not run. They walk everywhere instead (heavier pathfinding load —
    // which is the point of the stress test). ac_walk failure just retries.
    if (population_engine_is_population_pc(sd->id))
        return teleported;

	// Check if user has teleport capabilities before attempting forced teleport
	if (forced) {
		bool has_teleport_capability = false;

		// Check if teleport skill is available and not disabled
		if (!sd->ac.teleport.disable_tp_skill && pc_checkskill(sd, AL_TELEPORT) > 0 && sd->status.sp > 20) {
			has_teleport_capability = true;
		}

		// Check if fly wing is available and not disabled
		if (!sd->ac.teleport.disable_flywing && !has_teleport_capability) {
			i = pc_search_inventory(sd, 12887); // infinite fly wing
			if (i < 0) {
				i = pc_search_inventory(sd, 12323); // novice fly wing
			}
			if (i < 0) {
				i = pc_search_inventory(sd, 601); // fly wing
			}
			if (i >= 0) {
				has_teleport_capability = true;
			}
		}

		// If no teleport capability, don't force teleport
		if (!has_teleport_capability) {
			return false;
		}
	}

// Check if user has teleport capabilities before attempting forced teleport
	if (forced) {
		bool has_teleport_capability = false;

		// Check if teleport skill is available and not disabled
		if (!sd->ac.teleport.disable_tp_skill && pc_checkskill(sd, AL_TELEPORT) > 0 && sd->status.sp > 20) {
			has_teleport_capability = true;
		}

		// Check if fly wing is available and not disabled
		if (!sd->ac.teleport.disable_flywing && !has_teleport_capability) {
			i = pc_search_inventory(sd, 12887); // infinite fly wing
			if (i < 0) {
				i = pc_search_inventory(sd, 12323); // novice fly wing
			}
			if (i < 0) {
				i = pc_search_inventory(sd, 601); // fly wing
			}
			if (i >= 0) {
				has_teleport_capability = true;
			}
		}

		// If no teleport capability, don't force teleport
		if (!has_teleport_capability) {
			return false;
		}
	}

	if (!sd->ac.teleport.disable_tp_skill && pc_checkskill(sd, AL_TELEPORT) > 0 && sd->status.sp > 20) {
		skill_consume_requirement(sd, AL_TELEPORT, 1, 3);
		ac_randomwarp(sd);
		teleported = true;
	}

	if (!sd->ac.teleport.disable_flywing && teleported == false) {
		i = pc_search_inventory(sd, 12887); // infinite fly wing

		if (i < 0) {
			i = pc_search_inventory(sd, 12323); // novice fly wing
			if (i >= 0)
				useitem = true;
		}

		if (i < 0) {
			i = pc_search_inventory(sd, 601); // fly wing
			if (i >= 0)
				useitem = true;
		}

        if (i >= 0) {
            if (useitem == true)
                pc_delitem(sd, i, 1, 0, 0, LOG_TYPE_CONSUME);
            ac_randomwarp(sd);
            teleported = true;
        }
	}

	if (teleported == true) {
		sd->ac.last_teleport = gettick();
		// Population shells have no client socket (fd <= 0); the map-move is
		// finalised by pc_setpos itself. Only a real player needs the manual
		// load-end handshake here.
		if (!population_engine_is_population_pc(sd->id))
			clif_parse_LoadEndAck(sd->fd, sd);
	}

	return teleported;
}

/**
 * @param flag 1 - 90% weight, 2 - warped, 3 - ran out of ammunition
 */
void ac_abort(map_session_data *sd, uint8 flag)
{
	nullpo_retv(sd);

	// Population-engine AutoCombat shells never actually abort: an abort would
	// permanently stop the loop (and, worse, GM-kick / warp the fake player).
	// Recover from the abort cause instead and keep autocombat_main running.
	if (population_engine_is_population_pc(sd->id)) {
		autocombat_shell_recover(sd, flag);
		return;
	}

	struct status_change_entry *sce = sd->sc.getSCE(SC_AUTOCOMBAT);
	if (sce) {
		sce->val4 = 0;
	}


	switch(flag) {
	case 1:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You are 90% weight, Auto Combat ended.", false, SELF);
		clif_refresh(sd); // Only case 1 has refresh
		break;
	case 2:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You were warped. Auto Combat ended.", false, SELF);
		break;
	case 3:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You have ran out of ammunition. Auto Combat ended.", false, SELF);
		break;
	case 4:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You joined a chatroom. Auto Combat ended.", false, SELF);
		break;
	}


	if (sd->ac.end_status_config == 1) {
		pc_setpos(sd, mapindex_name2id(sd->status.save_point.map), sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
	} else if (sd->ac.end_status_config == 2) {
		clif_GM_kick(nullptr, sd);
	}
}

void autocombat_status_start(map_session_data *sd, int64 tick)
{
	nullpo_retv(sd);
	int i;

	sd->ac.mapindex = sd->m;
	sd->ac.last_teleport = tick;
	sd->ac.last_hit = tick;
	sd->ac.last_move = 0;

	// Initialize performance optimization variables
	sd->ac.last_target_search = tick;
	sd->ac.last_buff_check = tick;
	sd->ac.last_potion_check = tick;
	sd->ac.idle_ticks = 0;

	// Initialize performance optimization variables
	sd->ac.last_target_search = tick;
	sd->ac.last_buff_check = tick;
	sd->ac.last_potion_check = tick;
	sd->ac.idle_ticks = 0;


    for (auto it = sd->ac.attackskills.begin(); it != sd->ac.attackskills.end();) {
        if (!pc_checkskill(sd, it->skill_id)) {
            it = sd->ac.attackskills.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = sd->ac.buffskills.begin(); it != sd->ac.buffskills.end();) {
        if (!pc_checkskill(sd, it->skill_id)) {
            it = sd->ac.buffskills.erase(it);
        } else {
            ++it;
        }
    }

	for (i = 0; i < ARRAYLENGTH(heal_skill_id); i++) {
		if (!pc_checkskill(sd, heal_skill_id[i])) {
			sd->ac.healskills[i].skill_lv = 0;
			sd->ac.healskills[i].min_hp_sp = 0;
		}
	}

    while (!sd->ac.walk_xy.empty())
        sd->ac.walk_xy.pop();
	ac_generate_roam_destination(sd, &sd->ac.destination.first, &sd->ac.destination.second);
	// Fake players have no char_id row — never touch the autocombat_* SQL tables.
	if (!population_engine_is_population_pc(sd->id))
		ac_pc_save(sd);
}

/**
 * @param flag 1 - 90% weight, 2 - warped, 3 - ran out of ammunition
 */
void ac_abort_flag(struct map_session_data *sd, uint8 flag)
{ // abort Auto Combat when 90 weight, or changed map for some reason
	status_change_end(sd, SC_AUTOCOMBAT);
	switch(flag) {
	case 1:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You are 90%% weight, Auto Combat ended.", false, SELF);
		break;
	case 2:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You were warped. Auto Combat ended.", false, SELF);
		break;
	case 3:
		clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "You have ran out of ammunition. Auto Combat ended.", false, SELF);
		break;
	}
	if (sd->ac.end_status_config == 1) { // warp to save point
		pc_setpos(sd, mapindex_name2id(sd->status.save_point.map), sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
	} else if (sd->ac.end_status_config == 2) { // logout
		clif_GM_kick(nullptr, sd);
	}
}

#if AUTOCOMBAT_DURATION_CONFIG != 0
static void ac_abort_expired(struct map_session_data *sd)
{ // abort Auto Combat when duration ended. separated function for macro
	status_change_end(sd, SC_AUTOCOMBAT);
	clif_messagecolor(sd, color_table[COLOR_LIGHT_GREEN], "Auto Combat has expired.", false, SELF);
	if (sd->ac.end_status_config == 1) { // warp to save point
		pc_setpos(sd, mapindex_name2id(sd->status.save_point.map), sd->status.save_point.x, sd->status.save_point.y, CLR_TELEPORT);
	} else if (sd->ac.end_status_config == 2) { // logout
		clif_GM_kick(nullptr, sd);
	}
}
#endif

void autocombat_status_end(map_session_data *sd)
{

	nullpo_retv(sd);


#if AUTOCOMBAT_DURATION_CONFIG == 0
	return;
#else

	if (!sd->sc.getSCE(SC_AUTOCOMBAT)) {
		return;
	}
	}


#if AUTOCOMBAT_DURATION_CONFIG > 3
	int i = pc_search_inventory(sd, AUTOCOMBAT_DURATION_CONFIG);
	if (sd->inventory.u.items_inventory[i].nameid > 0 && sd->inventory.u.items_inventory[i].amount > 0 && sd->inventory.u.items_inventory[i].expire_time > 1) {
		if (sd->inventory.u.items_inventory[i].expire_time - (int)time(NULL) <= 2) {
			ac_abort_expired(sd);
			return;
		}
	}
	if (i == -1)
#elif AUTOCOMBAT_DURATION_CONFIG == 2
	if (pc_readglobalreg(sd, add_str("#AC_DURATION")) >= sd->ac.max_duration)
#elif AUTOCOMBAT_DURATION_CONFIG == 1
	if (pc_readglobalreg(sd, add_str("AC_DURATION")) >= sd->ac.max_duration)
#endif
		ac_abort_expired(sd);
#endif
}

void autocombat_mob_damage(struct block_list *src)
{ // expiremental - unused
	if (src->type != BL_PC) return;
	map_session_data *sd = (map_session_data *)src;
	nullpo_retv(sd);
}

static void ac_heal_potions(map_session_data *sd)
{
	nullpo_retv(sd);
	int i;

	// ---- Population-engine AutoCombat shells: unlimited, inventory-free -----
	// "Use an HP/SP potion at <=80%". Implemented at the AI level: a direct
	// restore, no item lookup, no pc_useitem, no inventory stacks to hold in
	// RAM. Throttled to ~1.4/s so we don't repaint HP/SP bars every 250ms tick.
	if (population_engine_is_population_pc(sd->id)) {
		const int64 now = gettick();
		if (DIFF_TICK(now, sd->ac.last_potion_check) < 700)
			return;
		bool acted = false;
		if (sd->battle_status.max_hp > 0 &&
		    (sd->battle_status.hp * 100 / sd->battle_status.max_hp) <= 80) {
			status_percent_heal(sd, 40, 0);   // ~one White Potion, scaled to max HP
			acted = true;
		}
		if (sd->battle_status.max_sp > 0 &&
		    (sd->battle_status.sp * 100 / sd->battle_status.max_sp) <= 80) {
			status_percent_heal(sd, 0, 40);
			acted = true;
		}
		if (acted)
			sd->ac.last_potion_check = now;
		return;
	}

    for (auto it = sd->ac.hp_potions.begin(); it != sd->ac.hp_potions.end(); ++it) {
        if (it->min_hp > 0 && ((sd->battle_status.hp * 100 / sd->battle_status.max_hp) < it->min_hp)) {
            if ((i = pc_search_inventory(sd, it->item_id)) >= 0) {
                pc_useitem(sd, i);
                return;
            }
		}
    }

    for (auto it = sd->ac.sp_potions.begin(); it != sd->ac.sp_potions.end(); ++it) {
        if (it->min_sp > 0 && ((sd->battle_status.sp * 100 / sd->battle_status.max_sp) < it->min_sp)) {
            if ((i = pc_search_inventory(sd, it->item_id)) >= 0) {
                pc_useitem(sd, i);
                return;
            }
		}
    }
}

#if AUTOCOMBAT_LOOTING_CONFIG == 1
static bool ac_can_loot_item(map_session_data *sd, struct block_list *bl)
{
	struct map_session_data *first_sd, *second_sd, *third_sd;
	struct flooritem_data *fitem = BL_CAST(BL_ITEM, bl);
	struct party_data *p = nullptr;
	int64 tick = gettick();

	nullpo_retr(false, sd);
	if (fitem == NULL)
		return false;

	if (sd->status.party_id)
		p = party_search(sd->status.party_id);

	if (path_search(nullptr, sd->m, sd->x, sd->y, bl->x, bl->y, 1, CELL_CHKNOREACH)) {
        if (fitem->first_get_charid > 0 && fitem->first_get_charid != sd->status.char_id) {
            first_sd = map_charid2sd(fitem->first_get_charid);
            if (DIFF_TICK(tick, fitem->first_get_tick) < 0) {
                if (!(p && p->party.item&1 && first_sd && first_sd->status.party_id == sd->status.party_id))
                    return false;
            } else if (fitem->second_get_charid > 0 && fitem->second_get_charid != sd->status.char_id) {
                second_sd = map_charid2sd(fitem->second_get_charid);
                if (DIFF_TICK(tick, fitem->second_get_tick) < 0) {
                    if (!(p && p->party.item&1 &&
                        ((first_sd && first_sd->status.party_id == sd->status.party_id) ||
                        (second_sd && second_sd->status.party_id == sd->status.party_id))
                    ))
                        return false;
                } else if (fitem->third_get_charid > 0 && fitem->third_get_charid != sd->status.char_id) {
                    third_sd = map_charid2sd(fitem->third_get_charid);
                    if (DIFF_TICK(tick, fitem->third_get_tick) < 0) {
                        if(!(p && p->party.item&1 &&
                            ((first_sd && first_sd->status.party_id == sd->status.party_id) ||
                            (second_sd && second_sd->status.party_id == sd->status.party_id) ||
                            (third_sd && third_sd->status.party_id == sd->status.party_id))
                        ))
                            return false;
                    }
                }
            }
        }
		if (sd->ac.loot_item_config >= AC_LOOT_GROUP_1)
			return std::find(sd->ac.loot_item_selected.begin(), sd->ac.loot_item_selected.end(), fitem->item.nameid) != sd->ac.loot_item_selected.end();
		return true;
	}
	return false;
}

static int ac_look_for_items(struct block_list *bl, va_list ap)
{
	map_session_data *sd = va_arg(ap, map_session_data *);
	if (sd == nullptr || bl == nullptr || sd->ac.loot_item_id) return 1;
	sd->ac.loot_item_id = ac_can_loot_item(sd, bl) ? bl->id : 0;
	return 1;
}

static bool ac_loot_items(map_session_data *sd)
{
	nullpo_retr(false, sd);
	struct block_list *bl = map_id2bl(sd->ac.loot_item_id);

	if (sd->ac.loot_item_id && !ac_can_loot_item(sd, bl))
		sd->ac.loot_item_id = 0;

	for (int range = 1; range <= AUTOCOMBAT_TARGETRANGE; range += 2) {
		map_foreachinrange(ac_look_for_items, sd, range, BL_ITEM, sd);
		if (sd->ac.loot_item_id) break;
	}

	if (sd->ac.loot_item_id) {
		TBL_ITEM *fi = (TBL_ITEM *)map_id2bl(sd->ac.loot_item_id);
		if (fi != nullptr) {
			if (pc_issit(sd)) {
				pc_setstand(sd, false);
				skill_sit(sd, 0);
				clif_standing(*sd);
			}
			if (!check_distance_bl(sd, fi, 1))
				unit_walktobl(sd, fi, 1, 1);
			else
				pc_takeitem(sd, fi);
			return true;
		}
	}
	return false;
}
#endif

bool autocombat_autoloot(map_session_data *sd, t_itemid nameid)
{
#if AUTOCOMBAT_LOOTING_CONFIG == 2 || AUTOCOMBAT_LOOTING_CONFIG == 3
	nullpo_retr(false, sd);
	if (!sd->sc.getSCE(SC_AUTOCOMBAT))
		return false;
	if (sd->ac.loot_item_config == AC_LOOT_NONE)
		return false;
	if (sd->ac.loot_item_config == AC_LOOT_ALL)
		return true;

	return std::find(sd->ac.loot_item_selected.begin(), sd->ac.loot_item_selected.end(), nameid) != sd->ac.loot_item_selected.end();
#else
	return false;
#endif
}

void autocombat_autoloot_storage(map_session_data *sd, int16 index, int amount, unsigned int weight)
{
#if AUTOCOMBAT_LOOTING_CONFIG != 3
	return;
#else
	nullpo_retv(sd);
	if (!sd->sc.getSCE(SC_AUTOCOMBAT))
		return;

	switch(sd->inventory_data[index]->type) {
	case IT_HEALING:
	case IT_UNKNOWN:
	case IT_USABLE:
	case IT_ETC:
	case IT_UNKNOWN2:
	case IT_AMMO:
	case IT_DELAYCONSUME:
	case IT_SHADOWGEAR:
		sd->ac.loot_score++;
		break;
	case IT_CASH:
	case IT_ARMOR:
	case IT_WEAPON:
	case IT_CARD:
	case IT_PETEGG:
	case IT_PETARMOR:
		sd->ac.loot_score += 100;
		break;
	}
	sd->state.storage_flag = 1;
	storage_storageadd(sd, &sd->storage, index, amount);
	if (sd->ac.loot_score >= 100) {
		sd->ac.loot_score = 0;
		storage_storageclose(sd);
	}
#endif
}

static bool ac_check_combo(uint16 skill_id, int current_combo_id)
{
	if (!skill_is_combo(skill_id) || !current_combo_id)
		return false;

	switch(skill_id) {
	case MO_CHAINCOMBO:
		if (current_combo_id == MO_TRIPLEATTACK)
			return true;
		break;
	case MO_COMBOFINISH:
		if (current_combo_id == MO_CHAINCOMBO)
			return true;
		break;
	case CH_TIGERFIST:
		if (current_combo_id == MO_COMBOFINISH)
			return true;
		break;
	case CH_CHAINCRUSH:
		switch(current_combo_id) {
		case MO_COMBOFINISH:
		case CH_TIGERFIST:
			return true;
			break;
		}
		break;
	case MO_EXTREMITYFIST:
		switch(current_combo_id) {
		case MO_COMBOFINISH:
		case CH_TIGERFIST:
		case CH_CHAINCRUSH:
			return true;
			break;
		}
		break;
	}

	return false;
}

static bool ac_skillnotok(map_session_data *sd, struct block_list *target, uint16 skill_id, uint16 skill_lv)
{
	nullpo_retr(false, sd);
	struct status_data *tstatus;
	int inf = skill_get_inf(skill_id);
	skill_lv = min(pc_checkskill(sd, skill_id), skill_lv);

	if (skill_lv <= 0)
		return false;

	switch(skill_id) { // TK stance handled in main
	case TK_COUNTER:
	case TK_DOWNKICK:
	case TK_STORMKICK:
	case TK_TURNKICK:
		return false;
	default: break;
	}

	struct s_skill_condition require = skill_get_requirement(sd, skill_id, skill_lv);

	if (require.sp > sd->battle_status.sp)
		return false;

	if (require.hp > sd->battle_status.hp)
		return false;

	if (require.weapon && !pc_check_weapontype(sd, require.weapon))
		return false;

	if (pc_cant_act2(sd) && skill_id != RK_REFRESH && !(skill_id == SR_GENTLETOUCH_CURE &&
		(sd->sc.opt1 == OPT1_STONE || sd->sc.opt1 == OPT1_FREEZE || sd->sc.opt1 == OPT1_STUN)) &&
		sd->state.storage_flag && !(inf&INF_SELF_SKILL)
	)
		return false;

	if (pc_issit(sd))
		return false;

	if (skill_isNotOk(skill_id, *sd))
		return false;

	if (sd->ud.skilltimer != INVALID_TIMER) {
		if (skill_id != SA_CASTCANCEL && skill_id != SO_SPELLFIST)
			return false;
	}

	if (sd->sc.option&OPTION_COSTUME)
		return false;

	if (sd->sc.getSCE(SC_BASILICA) && (skill_id != HP_BASILICA || sd->sc.getSCE(SC_BASILICA)->val4 != sd->id))
		return false;

	if (sd->menuskill_id) {
		if (sd->menuskill_id == SA_TAMINGMONSTER){
			clif_menuskill_clear(sd);
		} else if (sd->menuskill_id != SA_AUTOSPELL)
			return false;
	}

	if (skill_is_combo(skill_id)) {
		if (skill_id == MO_EXTREMITYFIST) { // lets search if asura is used as combo
			struct s_attack_skills attackskills;
			bool using_combo = false;
            for (const auto& attackskills : sd->ac.attackskills) {
                if (attackskills.skill_id == MO_COMBOFINISH ||
                    attackskills.skill_id == CH_TIGERFIST ||
                    attackskills.skill_id == CH_CHAINCRUSH
				) {
                    using_combo = true;
                    break;
                }
            }
			if (!using_combo) {
				if (sd->spiritball > 4) {
					return true;
				} else return false;
			}
		}
		if (!sd->sc.getSCE(SC_COMBO) || !ac_check_combo(skill_id, sd->sc.getSCE(SC_COMBO)->val1))
			return false;
	}

	if (skill_id == AL_HEAL && target != nullptr) {
		tstatus = status_get_status_data(*target);
		if (!battle_check_undead(tstatus->race, tstatus->def_ele))
			return false;
	}

	return true;
}

// Cheap, allocation-free half of target validation: everything EXCEPT the
// walkable-path check. Alive, not hide/cloak, within range, and (when the
// shell/player has a monster-selection filter and no locked target yet) on
// the allowed mob_id list. Split out so the range sweep can reject the bulk
// of candidates without ever touching path_search().
static bool ac_candidate_valid(map_session_data *sd, struct block_list *bl)
{
	struct mob_data *md = BL_CAST(BL_MOB, bl);
	if (md == nullptr || status_isdead(*bl))
		return false;
	if (distance_xy(sd->x, sd->y, bl->x, bl->y) >= AUTOCOMBAT_TARGETRANGE)
		return false;
	if (md->sc.option & (OPTION_HIDE|OPTION_CLOAK))
		return false;
	if (!sd->ac.target_id && !sd->ac.mob_id.empty())
		return std::find(sd->ac.mob_id.begin(), sd->ac.mob_id.end(), md->mob_id) != sd->ac.mob_id.end();
	return true;
}

// Expensive half: does a walkable path to the candidate exist? Unchanged
// semantics vs the old ac_check_target (path_search flag&1 = easy path, then
// A* fallback). Now only ever run on the few nearest candidates, not on
// every monster in the sweep.
static bool ac_target_reachable(map_session_data *sd, struct block_list *bl)
{
	return path_search(nullptr, sd->m, sd->x, sd->y, bl->x, bl->y, 1, CELL_CHKNOPASS) != 0;
}

static bool ac_check_target(map_session_data *sd, unsigned int id)
{
	struct block_list *bl = map_id2bl(id);
	nullpo_retr(false, sd);
	if (bl == nullptr || bl->type != BL_MOB)
		return false;
	if (!ac_candidate_valid(sd, bl))
		return false;
	return ac_target_reachable(sd, bl);
}

// Nearest-first candidate list gathered by the range sweep. Small fixed size:
// we only ever need enough to survive a couple of "closest is unreachable"
// misses before falling back to roam, and each extra slot is one more
// potential path_search below.
#define AC_MAX_CANDIDATES 4
struct s_ac_candidates {
	int   id[AC_MAX_CANDIDATES];
	int   d2[AC_MAX_CANDIDATES];
	int   n;
};

// map_foreachinrange callback: collect up to AC_MAX_CANDIDATES nearest valid
// monsters by squared distance, sorted ascending. NO path_search here — that
// was the per-mob A* fan-out that made thousands of shells expensive. The
// walkable-path check is done once, on the best candidate(s), by the caller.
static int ac_look_for_targets(struct block_list *bl, va_list ap)
{
	s_ac_candidates *c = va_arg(ap, s_ac_candidates *);
	int src_id         = va_arg(ap, int);
	struct block_list *src = map_id2bl(src_id);
	map_session_data *sd = map_id2sd(src_id);
	struct mob_data *md = BL_CAST(BL_MOB, bl);

	nullpo_retr(0, src);
	nullpo_retr(0, bl);
	nullpo_retr(0, sd);

	if (battle_config.ksprotection && mob_ksprotected(src, bl))
		return 0;

	if (md != nullptr && md->db->mexp > 0 && sd->ac.teleport.tp_when_mvp)
		ac_teleport(sd, false);

	if (!ac_candidate_valid(sd, bl))
		return 0;

	int dx = bl->x - src->x;
	int dy = bl->y - src->y;
	int dist2 = dx * dx + dy * dy;

	// Insertion sort into the fixed nearest-N buffer (N is tiny).
	int pos = c->n < AC_MAX_CANDIDATES ? c->n : AC_MAX_CANDIDATES - 1;
	if (c->n == AC_MAX_CANDIDATES && dist2 >= c->d2[pos])
		return 1; // farther than everything we already kept
	while (pos > 0 && c->d2[pos - 1] > dist2) {
		c->id[pos] = c->id[pos - 1];
		c->d2[pos] = c->d2[pos - 1];
		pos--;
	}
	c->id[pos] = bl->id;
	c->d2[pos] = dist2;
	if (c->n < AC_MAX_CANDIDATES)
		c->n++;
	return 1;
}

static void ac_check_target_alive(map_session_data *sd, bool allow_search = true)
{
	nullpo_retv(sd);
	int64 current_tick = gettick();
	if (ac_check_target(sd, sd->ac.attack_target_id)) {
		sd->ac.target_id = sd->ac.attack_target_id;
		return;
	} else {
		sd->ac.attack_target_id = 0;
	}

	if (!ac_check_target(sd, sd->ac.target_id)) {
		sd->ac.target_id = 0;

		if (!allow_search)
			return; // this tick already ran acquisition once; only revalidating

		// Shells reacquire almost instantly (150 ms) so there is no visible
		// idle gap when a target dies; real players stay at 300 ms. The old
		// value for shells was 0 (a full range sweep EVERY 250 ms tick) —
		// 150 ms is below one AI tick and below human perception but stops
		// the every-tick sweep from thousands of shells at once.
		const int retarget_ms = population_engine_is_population_pc(sd->id) ? 150 : 300;
		if (DIFF_TICK(current_tick, sd->ac.last_target_search) > retarget_ms) {
			s_ac_candidates cand;
			cand.n = 0;

			// One circular spatial sweep — same primitive, same range — but it
			// now only gathers the nearest few candidates (no path check).
			{
				PE_PERF_SCOPE("ac.sweep");
				map_foreachinrange(ac_look_for_targets, sd, AUTOCOMBAT_TARGETRANGE, BL_MOB, &cand, sd->id);
			}

			// Authoritative "nearest valid REACHABLE mob" selection, preserved:
			// walk the candidates closest-first and pick the first one that has
			// a walkable path. At most AC_MAX_CANDIDATES path_search calls
			// instead of one per monster in range.
			sd->ac.target_id = 0;
			for (int i = 0; i < cand.n; i++) {
				struct block_list *cbl = map_id2bl(cand.id[i]);
				if (cbl != nullptr && ac_target_reachable(sd, cbl)) {
					sd->ac.target_id = cand.id[i];
					break;
				}
			}
			sd->ac.last_target_search = current_tick;
		}
	}
}

void autocombat_pc_damage(map_session_data *sd, struct block_list *src, bool was_sitting)
{
	nullpo_retv(sd);
	struct mob_data *md = (TBL_MOB *)src;

	if (!sd->sc.getSCE(SC_AUTOCOMBAT)) return;



	if (md != nullptr && md->get_bosstype() == BOSSTYPE_MVP && sd->ac.teleport.tp_when_mvp) {
		ac_teleport(sd, false);
		return;
	}
	if ((!sd->ac.teleport.disable_tp_skill || !sd->ac.teleport.disable_flywing) && sd->ac.teleport.emergency_hp > 0 && (sd->battle_status.hp * 100 / sd->battle_status.max_hp) < sd->ac.teleport.emergency_hp) {
		ac_teleport(sd, false);
		return;
	}

	if (was_sitting) {
		if (sd->ac.retaliate) {
			sd->ac.attack_target_id = src->id;
			sd->ac.target_id = src->id;
		} else {
			ac_teleport(sd, false);
		}
	}

	if (sd->ac.target_id) return;

	if (sd->ac.retaliate && !sd->ac.target_id && src->type == BL_MOB && path_search(nullptr, sd->m, sd->x, sd->y, src->x, src->y, 1, CELL_CHKNOREACH)) {
		sd->ac.attack_target_id = src->id;
		sd->ac.target_id = src->id;
	}

	sd->ac.last_hit = gettick();
}
// Function to get additional global delay for a specific skill
static int ac_get_additional_skill_delay(uint16 skill_id) {
	for (const auto& custom_delay : autocombat_custom_delays) {
		if (custom_delay.skill_id == skill_id) {
			return custom_delay.delay;
		}
	}
	return 0; // Return 0 if no additional delay needed
}

static bool ac_set_cooldown(map_session_data *sd, int64 tick, uint16 skill_id, uint16 skill_lv)
{
	nullpo_retr(false, sd);
	bool skip = false;
	int cooldown = pc_get_skillcooldown(sd, skill_id, skill_lv);
	int delay = skill_delayfix(sd, skill_id, skill_lv);
	int casttime = skill_castfix(sd, skill_id, skill_lv);
	casttime = skill_castfix_sc(sd, casttime, skill_get_castnodex(skill_id)); // TODO: Renewal Casttime
	if (cooldown) {
		skill_blockpc_start(*sd, skill_id, cooldown);
		delay += battle_config.autocombat_skill_delay; // add configurable delay on skills that dont rely on aspd
	} else {
		delay += sd->battle_status.adelay;
		skip = true;
	}
	sd->ac.skill_cd = tick + delay + casttime;
	return skip;
}
// we duplicate this function because rA devs dont want to include to the header
bool autocombat_can_use_item(map_session_data *sd, struct item_data *item)
{
	nullpo_retr(false, sd);
	nullpo_retr(false, item);
	uint64 job = 1ULL << ( sd->class_ & MAPID_BASEMASK );
	size_t index;
	if ((sd->class_ & JOBL_2_1) != 0) {
		index = 1;
	} else if ((sd->class_ & JOBL_2_2) != 0) {
		index = 2;
	} else {
		index = 0;
	}
	return (item->class_base[index] & job) != 0;
}

static bool ac_is_grid_valid(int16 m, int row, int col)
{
    // row is x axis, column is y axis
    struct map_data *mapd = map_getmapdata(m);
    return (row >= 0) && (row <= mapd->xs) && (col >= 0) && (col < mapd->ys);
}

static bool ac_is_grid_walkable(int16 m, int row, int col)
{
    if (map_getcell(m, row, col, CELL_CHKNPC)) return false;

	return map_getcell(m, row, col, CELL_CHKPASS) ? true : false;
}

static bool ac_is_destination(int row, int col, Pair dest)
{
    return (row == dest.first && col == dest.second) ? true : false;
}

static float ac_calc_hvalue(int row, int col, Pair dest)
{
    return ((float)sqrt((row - dest.first) * (row - dest.first) + (col - dest.second) * (col - dest.second)));
}

static bool ac_astar_search(map_session_data *sd, int src_x, int src_y, Pair dest)
{
	int16 m = sd->m;
    struct map_data *mapd = map_getmapdata(m);

    if (ac_is_grid_valid(m, dest.first, dest.second) == false ||
		ac_is_grid_walkable(m, dest.first, dest.second) == false ||
		ac_is_destination(src_x, src_y, dest) == true)
	{
		return false;
	}

	int16 row = mapd->xs;
	int16 col = mapd->ys;

	bool closed_list[row][col];
    memset(closed_list, false, sizeof(closed_list));

    aCell cell_details[row][col];

    int i, j;
	int idx;

    for (i = 0; i < row; i++) {
        for (j = 0; j < col; j++) {
            cell_details[i][j].f = FLT_MAX;
            cell_details[i][j].g = FLT_MAX;
            cell_details[i][j].h = FLT_MAX;
            cell_details[i][j].parent_i = -1;
            cell_details[i][j].parent_j = -1;
        }
    }

    i = src_x, j = src_y;
    cell_details[i][j].f = 0.0;
    cell_details[i][j].g = 0.0;
    cell_details[i][j].h = 0.0;
    cell_details[i][j].parent_i = i;
    cell_details[i][j].parent_j = j;

    std::set<pPair> open_list;

    open_list.insert(std::make_pair(0.0, std::make_pair(i, j)));

#define AC_CALC_SUCCESSOR(node_i, node_j, score) \
	{ \
		if (ac_is_grid_valid(m, (node_i), (node_j)) == true) { \
            if (ac_is_destination((node_i), (node_j), dest) == true) { \
                cell_details[(node_i)][(node_j)].parent_i = i; \
                cell_details[(node_i)][(node_j)].parent_j = j; \
				dest_row = dest.first; \
				dest_col = dest.second; \
				while (!(cell_details[dest_row][dest_col].parent_i == dest_row && cell_details[dest_row][dest_col].parent_j == dest_col)) { \
					sd->ac.walk_xy.push(std::make_pair(dest_row, dest_col)); \
					int temp_row = cell_details[dest_row][dest_col].parent_i; \
					int temp_col = cell_details[dest_row][dest_col].parent_j; \
					dest_row = temp_row; \
					dest_col = temp_col; \
				} \
				sd->ac.walk_xy.push(std::make_pair(dest_row, dest_col)); \
                return true; \
			} \
            if (closed_list[(node_i)][(node_j)] == false && ac_is_grid_walkable(m, (node_i), (node_j)) == true) { \
                gNew = cell_details[i][j].g + (score); \
                hNew = ac_calc_hvalue((node_i), (node_j), dest); \
                fNew = gNew + hNew; \
                if (cell_details[(node_i)][(node_j)].f == FLT_MAX || cell_details[(node_i)][(node_j)].f > fNew) { \
                    open_list.insert(std::make_pair(fNew, std::make_pair((node_i), (node_j)))); \
                    cell_details[(node_i)][(node_j)].f = fNew; \
                    cell_details[(node_i)][(node_j)].g = gNew; \
                    cell_details[(node_i)][(node_j)].h = hNew; \
                    cell_details[(node_i)][(node_j)].parent_i = i; \
                    cell_details[(node_i)][(node_j)].parent_j = j; \
                } \
            } \
        } \
	}

    while (!open_list.empty()) {
        pPair p = *open_list.begin();
		open_list.erase(open_list.begin());
        i = p.second.first;
        j = p.second.second;
		closed_list[i][j] = true;
        float gNew, hNew, fNew;
		int dest_row, dest_col;
		AC_CALC_SUCCESSOR(i - 1, j, 1.0); // North
		AC_CALC_SUCCESSOR(i + 1, j, 1.0); // South
		AC_CALC_SUCCESSOR(i, j + 1, 1.0); // East
		AC_CALC_SUCCESSOR(i, j - 1, 1.0); // West
		AC_CALC_SUCCESSOR(i - 1, j + 1, 1.414); // North-East
		AC_CALC_SUCCESSOR(i - 1, j - 1, 1.414); // North-West
		AC_CALC_SUCCESSOR(i + 1, j + 1, 1.414); // South-East
		AC_CALC_SUCCESSOR(i + 1, j - 1, 1.414); // South-West
    }

#undef AC_CALC_SUCCESSOR
    return false;
}

static bool ac_walk(map_session_data *sd, int64 tick, int teleport_tick, int walk_tick)
{
	nullpo_retr(false, sd);
	struct block_list *bl = sd;
	nullpo_retr(false, bl);

// Check for no-mob teleport - this should happen when:
	// 1. User has teleport capability (skill or fly wing)
	// 2. No mob delay is configured and time has passed
	// 3. No current target
	// 4. Not recently hit by monster
	int hit_tick = DIFF_TICK(tick, sd->ac.last_hit);

	if (sd->ac.teleport.no_mob_delay > 0 &&
		teleport_tick > sd->ac.teleport.no_mob_delay &&
		!sd->ac.target_id &&
		hit_tick > sd->ac.teleport.no_mob_delay) {

		// Check if player has teleport capability
		bool can_teleport = false;

		// Check teleport skill first
		if (!sd->ac.teleport.disable_tp_skill && pc_checkskill(sd, AL_TELEPORT) > 0 && sd->status.sp > 20) {
			can_teleport = true;
		}

		// Check fly wings if teleport skill not available
		if (!can_teleport && !sd->ac.teleport.disable_flywing) {
			int i = pc_search_inventory(sd, 12887); // infinite fly wing
			if (i < 0) {
				i = pc_search_inventory(sd, 12323); // novice fly wing
			}
			if (i < 0) {
				i = pc_search_inventory(sd, 601); // fly wing
			}
			if (i >= 0) {
				can_teleport = true;
			}
		}

		// Attempt teleport if capability exists
		if (can_teleport) {
			if (ac_teleport(sd, false))
				return true;
		}
	}
	bool retry_walk = false;
	if (bl->x == sd->ac.last_x && bl->y == sd->ac.last_y) {
		retry_walk = true;
		sd->ac.walk_retries++;
	}

	if (ac_is_destination(bl->x, bl->y, sd->ac.destination) || sd->ac.walk_retries > 2) {
		ac_generate_roam_destination(sd, &sd->ac.destination.first, &sd->ac.destination.second);
		sd->ac.walk_retries = 0;
	}

	if (retry_walk || walk_tick > 2000) {
        while (!sd->ac.walk_xy.empty())
            sd->ac.walk_xy.pop();
		int searched = 0;
		do {
			if (searched > 0)
				ac_generate_roam_destination(sd, &sd->ac.destination.first, &sd->ac.destination.second);
			searched++;
			if (searched > AUTOCOMBAT_MAX_ASTAR_SEARCH) {
				// Before giving up completely, try one more teleport attempt if player has capability
				bool can_teleport = false;

				// Check teleport skill
				if (!sd->ac.teleport.disable_tp_skill && pc_checkskill(sd, AL_TELEPORT) > 0 && sd->status.sp > 20) {
					can_teleport = true;
				}

				// Check fly wings
				if (!can_teleport && !sd->ac.teleport.disable_flywing) {
					int i = pc_search_inventory(sd, 12887); // infinite fly wing
					if (i < 0) i = pc_search_inventory(sd, 12323); // novice fly wing
					if (i < 0) i = pc_search_inventory(sd, 601); // fly wing
					if (i >= 0) can_teleport = true;
				}

				// Only attempt teleport if player has the capability
				if (can_teleport) {
					return ac_teleport(sd, false);
				}
				return false;
			}
		} while(ac_astar_search(sd, bl->x, bl->y, sd->ac.destination) == false);
	}
    int number_of_pops = 8;
	if (sd->ac.walk_xy.size() && distance_xy(bl->x, bl->y, sd->ac.walk_xy.top().first, sd->ac.walk_xy.top().second) < 8) {
        while (number_of_pops > 0 && sd->ac.walk_xy.size() > 1) {
            sd->ac.walk_xy.pop();
            --number_of_pops;
        }
	}
	if (sd->ac.walk_xy.size())
		unit_walktoxy(bl, sd->ac.walk_xy.top().first, sd->ac.walk_xy.top().second, 0);
	sd->ac.last_x = bl->x;
	sd->ac.last_y = bl->y;
	sd->ac.last_move = tick;
	return true;
}

static bool ac_battle_check_arrows(map_session_data *sd)
{
	short index = sd->equip_index[EQI_AMMO];
	if (sd->inventory_data[index]) {
		switch (sd->status.weapon) {
		case W_BOW:
			if (sd->inventory_data[index]->subtype != AMMO_ARROW) {
				return false;
			}
			break;
		case W_REVOLVER:
		case W_RIFLE:
		case W_GATLING:
		case W_SHOTGUN:
			if (sd->inventory_data[index]->subtype != AMMO_BULLET) {
				return false;
			}
			break;
		case W_GRENADE:
			if (sd->inventory_data[index]->subtype != AMMO_GRENADE) {
				return false;
			}
			break;
		}
	}
	return true;
}

// map_foreachinrange callback: count REAL players (not population shells) in view.
static int ac_count_real_pc(struct block_list *bl, va_list ap)
{
	int *n = va_arg(ap, int *);
	if (bl != nullptr && bl->type == BL_PC && !population_engine_is_population_pc(bl->id))
		(*n)++;
	return 0;
}

// Cached, throttled "is a real player watching this shell?" query. The BL_PC
// block list is tiny (real players only), and this runs at most once per
// second per shell, so it is cheap even at very large shell counts.
static int autocombat_watchers(map_session_data *sd, int64 tick)
{
	if (sd->ac.last_watch_scan != 0 && DIFF_TICK(tick, sd->ac.last_watch_scan) < 1000)
		return sd->ac.watchers;
	int n = 0;
	map_foreachinrange(ac_count_real_pc, sd, AREA_SIZE + 4, BL_PC, &n);
	sd->ac.watchers = (int16)n;
	sd->ac.last_watch_scan = tick;
	return n;
}

// Adaptive cadence gate. SC_AUTOCOMBAT still fires every AUTOCOMBAT_DEFAULTNEXTTICK
// (250 ms) so the duration / val4 accounting is completely untouched, but the
// expensive AI body only runs every Nth call, chosen from the shell's actual
// situation:
//   ACTIVE COMBAT (has target, or hit < 2 s ago)      -> 250 ms  (every tick)
//   WATCHED, SEARCHING / MOVING                        -> 500 ms
//   WATCHED, IDLE (regen / sit, no mobs)              -> 750 ms
//   UNWATCHED + IDLE (no real player in view)         -> 1500 ms
//   real (non-shell) players stay snappy (250/500)
// A per-entity phase offset (id-derived) spreads thousands of shells across
// the window instead of all firing on the same 250 ms boundary.
// Returns true and stamps last_ai_run when the full body should run this call.
static bool autocombat_should_run_now(map_session_data *sd, int64 tick)
{
	const bool has_target = (sd->ac.target_id != 0 || sd->ac.attack_target_id != 0);
	const int  hit_tick   = DIFF_TICK(tick, sd->ac.last_hit);

	int cadence_ms;
	if (has_target || hit_tick < 2000) {
		cadence_ms = AUTOCOMBAT_DEFAULTNEXTTICK;            // ACTIVE
	} else if (!population_engine_is_population_pc(sd->id)) {
		cadence_ms = (sd->ac.idle_ticks > 10) ? 500 : AUTOCOMBAT_DEFAULTNEXTTICK;
	} else if (autocombat_watchers(sd, tick) == 0) {
		cadence_ms = 1500;                                  // UNWATCHED IDLE
	} else if (sd->ac.idle_ticks > 10) {
		cadence_ms = 750;                                   // WATCHED IDLE
	} else {
		cadence_ms = 500;                                   // WATCHED SEARCHING/MOVING
	}

	if (cadence_ms <= AUTOCOMBAT_DEFAULTNEXTTICK) {
		sd->ac.last_ai_run = tick;
		return true;
	}

	if (sd->ac.last_ai_run == 0) // first run: deterministic stagger across the window
		sd->ac.last_ai_run = tick - (int64)(sd->id % cadence_ms);

	if (DIFF_TICK(tick, sd->ac.last_ai_run) + (AUTOCOMBAT_DEFAULTNEXTTICK / 2) < cadence_ms)
		return false;

	sd->ac.last_ai_run = tick;
	return true;
}

void autocombat_main(map_session_data *sd, int64 tick)
{
	PE_PERF_SCOPE("ac.tick"); // every SC_AUTOCOMBAT invocation (throttled + full)

	struct block_list *bl = sd;

	nullpo_retv(bl);
	nullpo_retv(sd);

	// Check if autocombat status is still active
	if (!sd->sc.getSCE(SC_AUTOCOMBAT)) {
		return; // Status ended, stop processing
	}

	// Population-engine AutoCombat shell that is dead / warping / not on map:
	// the respawn timer re-seeds and re-arms SC_AUTOCOMBAT after revive.
	if (population_engine_is_population_pc(sd->id) && (pc_isdead(sd) || sd->prev == nullptr))
		return;

	// Adaptive scheduling: skip the heavy body on "off" ticks. The SC keeps
	// firing at 250 ms; only the full simulation is throttled. Damage / target
	// acquisition pull the shell straight back to full speed via the state
	// check inside autocombat_should_run_now (has_target / recent last_hit).
	if (!autocombat_should_run_now(sd, tick))
		return;

	PE_PERF_SCOPE("ac.main"); // full AI body only (calls here vs ac.tick = throttle ratio)

	int teleport_tick = DIFF_TICK(tick, sd->ac.last_teleport);
	int walk_tick = DIFF_TICK(tick, sd->ac.last_move);
	int hit_tick = DIFF_TICK(tick, sd->ac.last_hit);
	bool skip = false;
	bool teleported = false;
	// Population-engine AutoCombat shells are fake stress dummies: their carry
	// weight is irrelevant and must never end the loop. (A real player still
	// aborts at 90% weight as before.)
	const bool is_pop_shell = population_engine_is_population_pc(sd->id);
	bool overweight = !is_pop_shell && (sd->weight * 100 >= sd->max_weight * 90);
	// Track idle state for performance optimization
	if (!sd->ac.target_id && !sd->ac.attack_target_id && hit_tick > 5000) {
		sd->ac.idle_ticks++;
	} else {
		sd->ac.idle_ticks = 0;
	}
	bool is_idle = (sd->ac.idle_ticks > 10); // Idle for 5+ seconds
	struct block_list *target = nullptr;
	struct unit_data *ud = unit_bl2ud(bl);
	struct status_data *st = status_get_status_data(*bl);
	int i, j;

	// Check for chatroom - abort if player is in a chatroom
	if (sd->chatID) {
	    ac_abort(sd, 4);
	    return;
	}

	// Check for overweight - abort if 90% or more weight
	if (overweight) {
	    ac_abort(sd, 1);
	    return;
	}

	if (bl->m != sd->ac.mapindex) {
	    ac_abort(sd, 2);
	    return;
	}

	if (!sd->ac.disable_normal_atk && sd->ac.attackskills.empty() && sd->state.arrow_atk && ac_battle_check_arrows(sd) == false) {
	    ac_abort(sd, 3);
	    return;
}

	//====== DURATION ===========================================
#if AUTOCOMBAT_DURATION_CONFIG >= 1 && AUTOCOMBAT_DURATION_CONFIG <= 3
	sd->ac.duration += AUTOCOMBAT_DEFAULTNEXTTICK;
	if (sd->ac.duration >= 1000) {
		sd->ac.duration = 0;
#if AUTOCOMBAT_DURATION_CONFIG == 1
		pc_setglobalreg(sd, add_str("AC_DURATION"), pc_readglobalreg(sd, add_str("AC_DURATION")) + 1);
#elif AUTOCOMBAT_DURATION_CONFIG == 2
		pc_setglobalreg(sd, add_str("#AC_DURATION"), pc_readglobalreg(sd, add_str("#AC_DURATION")) + 1);
#endif
	}
#endif

	sd->idletime = tick;
	if (pc_issit(sd)) { // skip everything when resting
		clif_sitting(*bl);
		sd->ac.last_teleport = tick;
		sd->ac.last_hit = tick;
		sd->ac.last_move = tick;
		skip = true;
	}

	//====== TK KICKS ============================================
	if (sd->sc.getSCE(SC_COMBO)) {
		unit_stop_attack(bl);
		uint16 skill_id = sd->sc.getSCE(SC_COMBO)->val1;
		uint16 skill_lv = pc_checkskill(sd, skill_id);
		switch(sd->sc.getSCE(SC_COMBO)->val1) {
		case TK_STORMKICK:
		case TK_DOWNKICK:
		case TK_TURNKICK:
		case TK_COUNTER:
			if (unit_skilluse_id(bl, sd->ac.target_id, skill_id, skill_lv)) {
				skill_consume_requirement(sd, skill_id, skill_lv, 3);
				ac_set_cooldown(sd, tick, skill_id, skill_lv);
			}
			return;
		default:
			break;
		}
	}

	//====== HEAL SKILLS =========================================
	if (hit_tick > 2000) {
		if (tick >= sd->ac.skill_cd) {
			uint32 current_hpsp = sd->battle_status.hp;
			uint32 max_hpsp = sd->battle_status.max_hp;
			uint16 skill_lv;
			for (i = 0; i < ARRAYLENGTH(heal_skill_id); i++) {
				if ((skill_lv = sd->ac.healskills[i].skill_lv) <= 0) continue;
				if (i == 2) {
					current_hpsp = sd->battle_status.sp;
					max_hpsp = sd->battle_status.max_sp;
				}
				if (ac_skillnotok(sd, nullptr, heal_skill_id[i], skill_lv) && ((current_hpsp * 100 / max_hpsp < sd->ac.healskills[i].min_hp_sp))) {
					if (unit_skilluse_id(bl, bl->id, heal_skill_id[i], skill_lv)) {
						skill_consume_requirement(sd, heal_skill_id[i], skill_lv, 2);
						skip = ac_set_cooldown(sd, tick, heal_skill_id[i], skill_lv);
						break;
					}
				}
			}
		}
	}

	//====== HEAL POTIONS =========================================
	ac_heal_potions(sd);

	//====== ITEM LOOT ===========================================
#if AUTOCOMBAT_LOOTING_CONFIG == 1
	if (sd->ac.loot_item_config > AC_LOOT_NONE && ac_loot_items(sd)) return;
#endif

	//====== SIT REGEN =========================================
	bool overweight_sit = !is_pop_shell && (sd->weight * 100 >= sd->max_weight * 50);
	if (!pc_issit(sd)
		&& ((sd->ac.sit_min_hp > 0 && ((sd->battle_status.hp * 100 / sd->battle_status.max_hp) < sd->ac.sit_min_hp))
		|| (sd->ac.sit_min_sp > 0 && ((sd->battle_status.sp * 100 / sd->battle_status.max_sp) < sd->ac.sit_min_sp)))
		&& hit_tick > 2000
		&& !overweight_sit && !sd->ac.attack_target_id
		) {
			skip = true;
			pc_setsit(sd);
			skill_sit(sd, 1);
			clif_sitting(*bl);
	} else if (pc_issit(sd)
		&& ((sd->battle_status.hp * 100 / sd->battle_status.max_hp) >= AUTOCOMBAT_SIT_MAX_HPSP)
		&& ((sd->battle_status.sp * 100 / sd->battle_status.max_sp) >= AUTOCOMBAT_SIT_MAX_HPSP)
		) {
			pc_setstand(sd, false);
			skill_sit(sd, 0);
			clif_standing(*bl);
	} else if (pc_issit(sd) && overweight_sit) {
		pc_setstand(sd, false);
		skill_sit(sd, 0);
		clif_standing(*bl);
	}

//====== BUFF SKILLS =========================================
	if (!skip && !map_getmapflag(bl->m, MF_NOSKILL) && !sd->ac.buffskills.empty() && hit_tick > 2000 && tick >= sd->ac.skill_cd && DIFF_TICK(tick, sd->ac.last_buff_check) > 3000) {
		// Check buffs only every 3 seconds to reduce CPU usage
		for (auto &buffskills : sd->ac.buffskills) {
			if (buffskills.skill_id == SM_ENDURE && sd->special_state.no_walk_delay)
				continue;
			if (sd->sc.getSCE(skill_get_sc(buffskills.skill_id)))
				continue;
			if (ac_skillnotok(sd, nullptr, buffskills.skill_id, buffskills.skill_lv)) {
				if (buffskills.skill_id == CH_SOULCOLLECT && sd->spiritball > 0)
					continue;
				if (buffskills.skill_id == MO_CALLSPIRITS && sd->spiritball >= 5)
					continue;
				if (buffskills.skill_id == SA_AUTOSPELL) {
					sd->menuskill_val = pc_checkskill(sd, SA_AUTOSPELL);
					switch(buffskills.skill_lv) {
					case 1:
						skill_autospell(sd, MG_NAPALMBEAT);
						break;
					case 2:
					case 3:
					case 4:
						switch(rnd() % 3) {
						case 0:
							skill_autospell(sd, MG_FIREBOLT);
							break;
						case 1:
							skill_autospell(sd, MG_COLDBOLT);
							break;
						case 2:
							skill_autospell(sd, MG_LIGHTNINGBOLT);
							break;
						}
						break;
					case 5:
					case 6:
					case 7:
						skill_autospell(sd, MG_SOULSTRIKE);
						break;
					case 8:
					case 9:
						skill_autospell(sd, MG_FIREBALL);
						break;
					case 10:
						skill_autospell(sd, MG_FROSTDIVER);
						break;
					}
					sd->menuskill_id = 0;
					sd->menuskill_val = 0;
					continue;
				}
				if (unit_skilluse_id(sd, sd->id, buffskills.skill_id, buffskills.skill_lv)) {
					skill_consume_requirement(sd, buffskills.skill_id, buffskills.skill_lv, 3);
					ac_set_cooldown(sd, tick, buffskills.skill_id, buffskills.skill_lv);
					return;
				}
			}
		}
		sd->ac.last_buff_check = tick;
	}

	//====== BUFF ITEMS =========================================
	for (i = 0; i < ARRAYLENGTH(buff_items); i++) {
		if (sd->ac.buffitems & (1 << i) && !sd->sc.getSCE(buff_items[i].status)) {
			j = pc_search_inventory(sd, buff_items[i].item_id);
			if (j >= 0) {
				pc_delitem(sd, j, 1, 0, 0, LOG_TYPE_CONSUME);
				clif_specialeffect(bl, buff_items[i].effect, AREA);
				status_change_start(bl, bl, buff_items[i].status, 10000, buff_items[i].val1, buff_items[i].val2, buff_items[i].val3, buff_items[i].val4, buff_items[i].tick, SCSTART_NONE);
				break;
			}
		}
	}

	ac_check_target_alive(sd);

	if (sd->ac.target_id) {
		target = map_id2bl(sd->ac.target_id);
		if (target != nullptr && target->type == BL_MOB) {
			if (ud != nullptr)
				ud->target = sd->ac.target_id;
		}
	}

	//====== MELEE & ATTACK SKILLS =========================================
	if (!skip && sd->ac.target_id && target != nullptr) {
		sd->ac.last_teleport = tick;
		if (sd->ac.target_id != sd->ac.attack_target_id)
			sd->ac.attack_target_id = sd->ac.target_id;
		if (!map_getmapflag(bl->m, MF_NOSKILL) && !sd->ac.attackskills.empty() && tick >= sd->ac.skill_cd) {
			int total_skills = sd->ac.attackskills.size();
		int index = rnd()%total_skills;
			while(total_skills > 0) {
				index++;
				total_skills--;
				if (index >= sd->ac.attackskills.size())
					index = 0;
			auto &attackskills = sd->ac.attackskills[index];
				if (ac_skillnotok(sd, target, attackskills.skill_id, attackskills.skill_lv) ) {
					// Use the caster-resolved range: skill_get_range() returns the DB
					// base range, which is 0 / negative ("use weapon range") for
					// AC_DOUBLE, AC_SHOWER, SN_SHARPSHOOTING and every other bow / gun
					// attack skill. The old code collapsed that to 2 (melee), so a
					// ranged AutoCombat user — real Sniper or a fake AutoCombat shell —
					// would endlessly walk toward the monster trying to reach point
					// blank and never actually shoot. skill_get_range2() resolves the
					// real effective range for THIS caster (weapon range + Vulture's
					// Eye / Snake Eye, etc.).
					int ac_skill_range = skill_get_range2(bl, attackskills.skill_id, attackskills.skill_lv, true);
					if (ac_skill_range <= 0)
						ac_skill_range = (attackskills.skill_id == CR_GRANDCROSS)
							? 2 : max((int)status_get_range(bl), 2);
					if (!battle_check_range(bl, target, ac_skill_range)) {
						if (unit_walktobl(bl, target, ac_skill_range, 2))
							return;
						skip = true;
						continue;
					}
					if (skill_is_combo(attackskills.skill_id) && sd->sc.getSCE(SC_COMBO)) {
						if (!unit_skilluse_id(bl, bl->id, attackskills.skill_id, attackskills.skill_lv))
							continue;
						return;
					}
					unit_stop_attack(bl);
					if (attackskills.skill_id == AL_HEAL || skill_get_inf(attackskills.skill_id) & INF_ATTACK_SKILL) {
						if (!unit_skilluse_id(bl, target->id, attackskills.skill_id, attackskills.skill_lv))
							continue;
					} else if (skill_get_inf(attackskills.skill_id) & INF_GROUND_SKILL) {
						if (!unit_skilluse_pos(bl, target->x, target->y, attackskills.skill_id, attackskills.skill_lv))
							continue;
					} else if (skill_get_inf(attackskills.skill_id) & INF_SELF_SKILL) {
						if (!unit_skilluse_id(bl, bl->id, attackskills.skill_id, attackskills.skill_lv))
							continue;
					}
					skill_consume_requirement(sd, attackskills.skill_id, attackskills.skill_lv, 3);
					if (ac_set_cooldown(sd, tick, attackskills.skill_id, attackskills.skill_lv))
						return;
					break;
				}
			}
		}

		if (!sd->ac.disable_normal_atk && !pc_issit(sd) && !skip) {
			if (distance_bl(bl, target) > st->rhw.range + 2)
				unit_walktobl(bl, target, st->rhw.range, 2);
			else {
				unit_attack(bl, sd->ac.target_id, 1);
				skip = true;
			}
		}
	}

	if (sd->ac.target_id)
		ac_check_target_alive(sd);

	//====== WALK =========================================
	if (!skip && !pc_issit(sd) && !sd->ac.target_id && !teleported) {
		if (ac_walk(sd, tick, teleport_tick, walk_tick) == false)
			ac_teleport(sd, true); // whole system error - redo
	}
}

// ===========================================================================
// Fake-player party-leader AutoSupport
// ===========================================================================
// Reuses the AutoSupport CONFIG (sd->ac.healskills / sd->ac.buffskills) and the
// existing validation / cooldown / SP core (ac_skillnotok, ac_set_cooldown,
// skill_consume_requirement, skill_get_range2). It is a small, self-contained
// support routine — it does NOT touch autocombat_main() and is NEVER called for
// a real player, so normal @autocombat / @settings behaviour is unchanged.
//
// A fake party leader supports party MEMBERS (not just itself): heal the
// lowest-HP% member below the configured threshold within range; then keep the
// configured buffs up on nearby members who are missing them. Timing is paced
// (ac.skill_cd + a ~1.2s decision interval + a random post-action lag + the 3s
// buff re-check) so it reads like a real support player, not a per-tick bot.

// Seed a freshly-assigned fake party leader's sd->ac support config from the
// skills it actually knows (no separate config system; SQL config is untouched).
void autocombat_seed_fake_leader(map_session_data *sd)
{
	if (sd == nullptr)
		return;
	const uint16 heal_lv = pc_checkskill(sd, AL_HEAL);
	sd->ac.healskills[0].skill_lv  = heal_lv;           // slot 0 = AL_HEAL
	sd->ac.healskills[0].min_hp_sp = (heal_lv > 0) ? 70 : 0; // support members below 70% HP
	sd->ac.healskills[1].skill_lv  = 0;
	sd->ac.healskills[2].skill_lv  = 0;

	sd->ac.buffskills.clear();
	static const uint16 kSeedBuffs[] = {
		AL_BLESSING, AL_INCAGI, PR_KYRIE, PR_GLORIA, PR_MAGNIFICAT, PR_IMPOSITIO
	};
	for (uint16 skid : kSeedBuffs) {
		const uint16 lv = pc_checkskill(sd, skid);
		if (lv > 0) {
			s_buff_skills b;
			b.skill_id = skid;
			b.skill_lv = lv;
			sd->ac.buffskills.push_back(b);
		}
	}
	sd->ac.last_buff_check = 0;
	sd->ac.skill_cd        = 0;
}

// ===========================================================================
// Population-engine AutoCombat shells
// ===========================================================================

// Ensure `sd` holds at least `amount` of item `nameid` (identified). Cheap:
// only adds when the current stack is short. No logging (fake player).
static void ac_shell_ensure_item(map_session_data *sd, t_itemid nameid, int amount)
{
	if (nameid == 0 || amount <= 0)
		return;
	int have = 0;
	int idx = pc_search_inventory(sd, nameid);
	if (idx >= 0)
		have = sd->inventory.u.items_inventory[idx].amount;
	if (have >= amount)
		return;
	struct item it = {};
	it.nameid = nameid;
	it.identify = 1;
	it.amount = amount - have;
	pc_additem(sd, &it, it.amount, LOG_TYPE_NONE);
}

void autocombat_shell_start(map_session_data *sd)
{
	if (sd == nullptr || !population_engine_is_population_pc(sd->id))
		return;

	// ---- reset any prior config -------------------------------------------
	sd->ac = s_auto_combat();

	// ---- offensive rotation: learned skills ∩ the AutoCombat attack table -
	for (uint16 skid : attack_skills) {
		if (sd->ac.attackskills.size() >= 8)
			break;
		const uint16 lv = pc_checkskill(sd, skid);
		if (lv > 0) {
			s_attack_skills a;
			a.skill_id = skid;
			a.skill_lv = lv;
			sd->ac.attackskills.push_back(a);
		}
	}
	// ---- self-buffs: learned skills ∩ the AutoCombat buff table ----------
	for (uint16 skid : buff_skills) {
		if (sd->ac.buffskills.size() >= 8)
			break;
		const uint16 lv = pc_checkskill(sd, skid);
		if (lv > 0) {
			s_buff_skills b;
			b.skill_id = skid;
			b.skill_lv = lv;
			sd->ac.buffskills.push_back(b);
		}
	}
	// ---- heal skills ----------------------------------------------------
	for (int i = 0; i < (int)ARRAYLENGTH(heal_skill_id); i++) {
		const uint16 lv = pc_checkskill(sd, heal_skill_id[i]);
		sd->ac.healskills[i].skill_lv  = lv;
		sd->ac.healskills[i].min_hp_sp = (lv > 0) ? ((i == 2) ? 40 : 50) : 0;
	}

	// ---- consumables --------------------------------------------------
	// HP/SP potions are NOT stocked as inventory items: ac_heal_potions() has a
	// dedicated shell fast-path that restores HP/SP directly at <=80% with no
	// item lookup and no stacks held in RAM (see that function). Leaving
	// hp_potions / sp_potions empty keeps the real-player potion loop a no-op
	// for shells.
	// Ranged shells still need real ammo so autocombat_main does not abort on
	// "out of ammunition" (that check reads the equipped ammo slot).
	ac_shell_ensure_item(sd, 1750, 30000); // Arrow
	ac_shell_ensure_item(sd, 1550, 30000); // Bullet

	// ---- behaviour flags ------------------------------------------------
	sd->ac.disable_normal_atk = false;
	sd->ac.retaliate          = true;
	sd->ac.element_switch      = false;
	// NO looting for shells. With AC_LOOT_ALL, autocombat_main's loot pass
	// (ac_loot_items) returns true every tick as long as any drop is on the
	// ground within 14 cells — which, right after a kill on a real hunting map,
	// is always — so autocombat_main returns before it ever reaches the
	// re-target / attack code and the shell freezes on the corpse. Drops are
	// cosmetic for a stress test; skip them so combat is continuous.
	sd->ac.loot_item_config    = AC_LOOT_NONE;
	sd->ac.end_status_config   = 0;                  // never warp-to-save / logout
	// Never sit to regen: the inventory-free potion fast-path in ac_heal_potions
	// keeps HP/SP topped up, and sitting is pure idle time (defeats "always
	// actively attack").
	sd->ac.sit_min_hp          = 0;
	sd->ac.sit_min_sp          = 0;
	// Teleport is disabled for shells (see ac_teleport): they walk everywhere.
	sd->ac.teleport.disable_tp_skill = true;
	sd->ac.teleport.disable_flywing  = true;
	sd->ac.teleport.tp_when_mvp       = false;
	sd->ac.teleport.emergency_hp      = 0;
	sd->ac.teleport.no_mob_delay      = 0;
	sd->ac.mob_id.clear();                           // attack any monster

	// NOTE: do NOT inflate sd->max_weight here — sd->max_weight * 90 in
	// autocombat_main's overweight check overflows int32 for huge values and
	// makes the check fire every tick. The overweight abort is instead disabled
	// for population shells directly in autocombat_main (is_pop_shell).

	// Start the REAL loop. val1 = 1 -> unbounded (val4 = INT_MAX), same as the
	// @settings NPC's "infinite duration" path.
	sc_start(sd, sd, SC_AUTOCOMBAT, 100, 1, 1);
}

void autocombat_shell_stop(map_session_data *sd)
{
	if (sd == nullptr)
		return;
	if (sd->sc.getSCE(SC_AUTOCOMBAT))
		status_change_end(sd, SC_AUTOCOMBAT);
}

void autocombat_shell_recover(map_session_data *sd, uint8 flag)
{
	if (sd == nullptr)
		return;
	switch (flag) {
	case 1: // overweight — cannot actually happen for a shell any more (the
	        // overweight check is disabled for population shells in
	        // autocombat_main); nothing to do.
		break;
	case 2: // "warped" — the shell's own teleport or a respawn moved it; resync
		sd->ac.mapindex = sd->m;
		break;
	default:
		break;
	}
	// Keep the loop alive: SC_AUTOCOMBAT / val4 were never touched (we intercept
	// before ac_abort's body). Just clear the transient blockers.
	sd->ac.walk_retries = 0;
	sd->ac.last_hit = gettick();
}

void autocombat_support_party(map_session_data *leader, int64 tick)
{
	if (leader == nullptr || leader->prev == nullptr)
		return;
	// Fake-player gate: reuse the existing population-engine identification.
	if (!population_engine_is_population_pc(leader->id))
		return;
	if (!party_id_is_fake(leader->status.party_id))
		return;

	party_data *p = party_search(leader->status.party_id);
	if (p == nullptr || p->data[0].sd != leader)   // must still be THIS party's leader
		return;

	// Pace the decisions — never every tick.
	if (tick < leader->ac.skill_cd)
		return;
	if (DIFF_TICK(tick, leader->ac.last_buff_check) < 1200 && leader->ac.last_buff_check != 0)
		return;
	if (pc_isdead(leader) || pc_issit(leader) || pc_cant_act(leader))
		return;
	if (leader->ud.skilltimer != INVALID_TIMER)
		return;

	// Valid support candidates (leader included). Handles dead / disconnected /
	// different-map / inactive members without dereferencing bad pointers.
	map_session_data *cand[MAX_PARTY];
	int nc = 0;
	for (int i = 0; i < MAX_PARTY; i++) {
		map_session_data *m = p->data[i].sd;
		if (m == nullptr || m->prev == nullptr)
			continue;
		if (m->m != leader->m)                      // different map -> skip
			continue;
		if (!m->state.active || pc_isdead(m))
			continue;
		cand[nc++] = m;
	}
	if (nc == 0)
		return;

	bool acted = false;

	// ---- HEAL: the lowest-HP% member below threshold, within AL_HEAL range ----
	{
		const uint16 heal_lv = leader->ac.healskills[0].skill_lv;
		const uint16 thr     = leader->ac.healskills[0].min_hp_sp;
		if (heal_lv > 0 && thr > 0 && pc_checkskill(leader, AL_HEAL) > 0 &&
		    ac_skillnotok(leader, nullptr, AL_HEAL, heal_lv)) {          // nullptr target => live-heal check, not the undead path
			const int range = skill_get_range2(leader, AL_HEAL, heal_lv, true);
			map_session_data *best = nullptr;
			int best_pct = thr;
			for (int i = 0; i < nc; i++) {
				map_session_data *m = cand[i];
				if (m->battle_status.max_hp <= 0)
					continue;
				const int pct = (int)((int64)m->battle_status.hp * 100 / m->battle_status.max_hp);
				if (pct >= thr)
					continue;                                          // healthy enough
				if (m != leader && !check_distance_bl(leader, m, range))
					continue;                                          // out of range -> just not chosen
				if (pct < best_pct) { best_pct = pct; best = m; }
			}
			if (best != nullptr && unit_skilluse_id(leader, best->id, AL_HEAL, heal_lv)) {
				skill_consume_requirement(leader, AL_HEAL, heal_lv, 2);
				ac_set_cooldown(leader, tick, AL_HEAL, heal_lv);
				acted = true;
			}
		}
	}

	// ---- BUFF: keep configured buffs up on a nearby member who is missing one ----
	if (!acted && !leader->ac.buffskills.empty() &&
	    DIFF_TICK(tick, leader->ac.last_buff_check) > 3000) {
		for (auto &bs : leader->ac.buffskills) {
			const uint16 lv = min(pc_checkskill(leader, bs.skill_id), bs.skill_lv);
			if (lv <= 0)
				continue;
			if (!ac_skillnotok(leader, nullptr, bs.skill_id, lv))
				continue;
			const sc_type sc = skill_get_sc(bs.skill_id);
			const int range  = skill_get_range2(leader, bs.skill_id, lv, true);
			map_session_data *tgt = nullptr;
			for (int i = 0; i < nc; i++) {
				map_session_data *m = cand[i];
				if (m == leader)
					continue;
				if (sc != SC_NONE && m->sc.getSCE(sc))                  // already buffed -> skip (no rebuff spam)
					continue;
				if (!check_distance_bl(leader, m, range))
					continue;
				tgt = m;
				break;
			}
			if (tgt == nullptr && (sc == SC_NONE || !leader->sc.getSCE(sc)))
				tgt = leader;                                           // fall back to self if the leader lacks it
			if (tgt != nullptr && unit_skilluse_id(leader, tgt->id, bs.skill_id, lv)) {
				skill_consume_requirement(leader, bs.skill_id, lv, 3);
				ac_set_cooldown(leader, tick, bs.skill_id, lv);
				acted = true;
				break;
			}
		}
		leader->ac.last_buff_check = tick;
	}

	// Natural "reaction lag" before the next support decision (matches how
	// ac_set_cooldown assigns sd->ac.skill_cd = tick + delay).
	if (acted) {
		const int64 next_cd = tick + 700 + (int64)(rnd() % 900);
		if (next_cd > leader->ac.skill_cd)
			leader->ac.skill_cd = (int)next_cd;
	}
}

void do_init_autocombat(void)
{
	if (SQL_ERROR == Sql_QueryStr(mmysql_handle,
		"CREATE TABLE IF NOT EXISTS `autocombat_main` ("
		"`char_id` INT UNSIGNED NOT NULL,"
		"`disable_normal_atk` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`retaliate` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`element_switch` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`disable_tp_skill` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`disable_flywing` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`tp_when_mvp` TINYINT(1) UNSIGNED NOT NULL DEFAULT '0',"
		"`no_mob_delay` INT UNSIGNED NOT NULL DEFAULT '0',"
		"`emergency_hp` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`sit_min_hp` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`sit_min_sp` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`loot_item_config` TINYINT(1) NOT NULL DEFAULT '0',"
		"`end_status_config` TINYINT(1) NOT NULL DEFAULT '0',"
		"`buff_items_mask` INT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_lv_0` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_min_0` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_lv_1` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_min_1` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_lv_2` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"`heal_min_2` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"PRIMARY KEY (`char_id`)"
		")"
	))
	{
		Sql_ShowDebug(mmysql_handle);
	}

	if (SQL_ERROR == Sql_QueryStr(mmysql_handle,
		"CREATE TABLE IF NOT EXISTS `autocombat_items` ("
		"`char_id` INT UNSIGNED NOT NULL,"
		"`type` TINYINT(1) NOT NULL,"
		"`item_id` INT UNSIGNED NOT NULL,"
		"`min_hp_sp` INT UNSIGNED NOT NULL DEFAULT '0',"
		"PRIMARY KEY (`char_id`, `type`, `item_id`)"
		")"
	))
	{
		Sql_ShowDebug(mmysql_handle);
	}

	if (SQL_ERROR == Sql_QueryStr(mmysql_handle,
		"CREATE TABLE IF NOT EXISTS `autocombat_skills` ("
		"`char_id` INT UNSIGNED NOT NULL,"
		"`type` TINYINT(1) NOT NULL,"
		"`skill_id` INT NOT NULL,"
		"`skill_lv` TINYINT UNSIGNED NOT NULL DEFAULT '0',"
		"PRIMARY KEY (`char_id`, `type`, `skill_id`)"
		")"
	))
	{
		Sql_ShowDebug(mmysql_handle);
	}
}