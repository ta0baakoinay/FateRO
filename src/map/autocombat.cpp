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

#include "autocombat.hpp"
#include "battle.hpp"
#include "log.hpp"
#include "map.hpp"
#include "party.hpp"
#include "chat.hpp"
#include "pc.hpp"
#include "storage.hpp"

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
	ac_generate_destination(sd->m, &sd->ac.destination.first, &sd->ac.destination.second);
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

static bool ac_check_target(map_session_data *sd, unsigned int id)
{
	struct block_list *bl = map_id2bl(id);
	struct mob_data *md = BL_CAST(BL_MOB, bl);
	nullpo_retr(false, sd);

	if (md == nullptr || status_isdead(*bl))
		return false;

	if (path_search(nullptr, sd->m, sd->x, sd->y, bl->x, bl->y, 1, CELL_CHKNOPASS) && distance_xy(sd->x, sd->y, bl->x, bl->y) < AUTOCOMBAT_TARGETRANGE) {
		if (md->sc.option & (OPTION_HIDE|OPTION_CLOAK))
			return false;
		if (!sd->ac.target_id && !sd->ac.mob_id.empty())
			return std::find(sd->ac.mob_id.begin(), sd->ac.mob_id.end(), md->mob_id) != sd->ac.mob_id.end();
		return true;
	}
	return false;
}

static int ac_look_for_targets(struct block_list *bl, va_list ap)
{
	int *target_id = va_arg(ap, int *);
	int src_id = va_arg(ap, int);
	struct block_list *src = map_id2bl(src_id);
	map_session_data *sd = map_id2sd(src->id);
	struct mob_data *md = BL_CAST(BL_MOB, bl);

	nullpo_retr(1, src);
	nullpo_retr(1, bl);

	if (battle_config.ksprotection && mob_ksprotected(src, bl))
		return 1;

	if (md != nullptr && md->db->mexp > 0 && sd != nullptr && sd->ac.teleport.tp_when_mvp)
		ac_teleport(sd, false);

	if (ac_check_target(sd, bl->id) == true)
		*target_id = bl->id;
	else
		*target_id = 0;

	return 1;
}

static void ac_check_target_alive(map_session_data *sd)
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
		int target_id = 0;
		sd->ac.target_id = 0;

		// Only search for new targets every 0.3 second to reduce CPU usage
		if (DIFF_TICK(current_tick, sd->ac.last_target_search) > 300) {
			int target_id = 0;
			sd->ac.target_id = 0;

			// Use progressive range search - start small and expand
			for (int i = 1; i <= AUTOCOMBAT_TARGETRANGE; i += 2) {
				map_foreachinarea(ac_look_for_targets, sd->m, sd->x - i, sd->y - i, sd->x + i, sd->y + i, BL_MOB, &target_id, sd->id);
				if (target_id) {
					sd->ac.target_id = target_id;
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
		ac_generate_destination(bl->m, &sd->ac.destination.first, &sd->ac.destination.second);
		sd->ac.walk_retries = 0;
	}

	if (retry_walk || walk_tick > 2000) {
        while (!sd->ac.walk_xy.empty())
            sd->ac.walk_xy.pop();
		int searched = 0;
		do {
			if (searched > 0)
				ac_generate_destination(bl->m, &sd->ac.destination.first, &sd->ac.destination.second);
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

void autocombat_main(map_session_data *sd, int64 tick)
{
	struct block_list *bl = sd;

	nullpo_retv(bl);
	nullpo_retv(sd);

	// Check if autocombat status is still active
	if (!sd->sc.getSCE(SC_AUTOCOMBAT)) {
		return; // Status ended, stop processing
	}

	int teleport_tick = DIFF_TICK(tick, sd->ac.last_teleport);
	int walk_tick = DIFF_TICK(tick, sd->ac.last_move);
	int hit_tick = DIFF_TICK(tick, sd->ac.last_hit);
	bool skip = false;
	bool teleported = false;
	bool overweight = (sd->weight * 100 >= sd->max_weight * 90);
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
	bool overweight_sit = (sd->weight * 100 >= sd->max_weight * 50);
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
					int ac_skill_range = skill_get_range(attackskills.skill_id, attackskills.skill_lv);
					if (ac_skill_range <= 0)
						ac_skill_range = ac_skill_range * -1;
					if (ac_skill_range == 0 || attackskills.skill_id == CR_GRANDCROSS)
						ac_skill_range = 2;
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