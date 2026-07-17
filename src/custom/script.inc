// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Script Commands
 * Place the body of custom script commands in this file.
 * Format:
 *	BUILDIN_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//BUILDIN_FUNC(example)
//{
//	ShowInfo("buildin_example: It works!\n");
//	script_pushint(st,1);
//	return 0;
//}




BUILDIN_FUNC(getautocombatv) { // getautocombatv(<type>);
	map_session_data *sd;
	if (!script_rid2sd(sd)) {
		ShowError("buildin_getautocombatv: No player attached!\n");
		return SCRIPT_CMD_FAILURE;
	}

	struct item_data *item;
	enum ac_type type = (enum ac_type)script_getnum(st, 2);
	int i, num = 0;
	switch(type) {
	case AC_HPPOTION:
	case AC_SPPOTION:
		for (i = 0; i < MAX_INVENTORY; i++) {
			if (sd->inventory.u.items_inventory[i].nameid > 0 && sd->inventory_data[i]->type == IT_HEALING && sd->inventory.u.items_inventory[i].amount > 0) {
				pc_setreg(sd, reference_uid(add_str("@ACINFO_ID"), num), sd->inventory.u.items_inventory[i].nameid);
				num++;
			}
		}
		break;
    case AC_BUFFITEM:
		for (i = 0; i < ARRAYLENGTH(buff_items); i++) {
			for (int j = 0; j < MAX_INVENTORY; j++) {
				item = sd->inventory_data[j];
				if (item != nullptr && autocombat_can_use_item(sd, item) && sd->inventory.u.items_inventory[j].nameid == buff_items[i].item_id && sd->inventory.u.items_inventory[j].amount > 0) {
					pc_setreg(sd, reference_uid(add_str("@ACINFO_ID"), num), buff_items[i].item_id);
					num++;
				}
			}
		}
		break;
	case AC_HEALSKILL:
		num = ARRAYLENGTH(heal_skill_id);
		for (i = 0; i < num; i++) {
			pc_setreg(sd, reference_uid(add_str("@ACINFO_ID"), i), heal_skill_id[i]);
			pc_setreg(sd, reference_uid(add_str("@ACINFO_VAL"), i), sd->ac.healskills[i].skill_lv);
			pc_setreg(sd, reference_uid(add_str("@ACINFO_VAL2"), i), sd->ac.healskills[i].min_hp_sp);
		}
		break;
	case AC_BUFFSKILL:
		for (i = 0; i < ARRAYLENGTH(buff_skills); i++) {
			if (pc_checkskill(sd, buff_skills[i]) > 0) {
				pc_setreg(sd, reference_uid(add_str("@ACINFO_ID"), num), buff_skills[i]);
				pc_setregstr(sd, reference_uid(add_str("@ACINFO_NAME$"), num), skill_get_desc(buff_skills[i]));
				num++;
			}
		}
		break;
    case AC_ATTACKSKILL:
		for (i = 0; i < ARRAYLENGTH(attack_skills); i++) {
			if (pc_checkskill(sd, attack_skills[i]) > 0) {
				pc_setreg(sd, reference_uid(add_str("@ACINFO_ID"), num), attack_skills[i]);
				pc_setregstr(sd, reference_uid(add_str("@ACINFO_NAME$"), num), skill_get_desc(attack_skills[i]));
				num++;
			}
		}
		break;
	case AC_LOOTITEM:
		num = sd->ac.loot_item_config;
		break;
	case AC_ENDCONDITION:
		switch(sd->ac.end_status_config) {
		case 0: script_pushstrcopy(st, "Do Nothing"); return SCRIPT_CMD_SUCCESS;
		case 1: script_pushstrcopy(st, "Warp to SavePoint"); return SCRIPT_CMD_SUCCESS;
		case 2: script_pushstrcopy(st, "Logout"); return SCRIPT_CMD_SUCCESS;
		default: break;
		}
		break;
	case AC_SIT:
	case AC_MOBS:
    case AC_NORMALATK:
    case AC_TELEPORT:
    case AC_RETALIATE:
		break;
	default:
		ShowError("buildin_getautocombatv: wrong type!\n");
		return SCRIPT_CMD_FAILURE;
	}

	script_pushint(st, num);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getautocombat) { // getautocombat(<type>, <index>);
	map_session_data *sd;
	if (!script_rid2sd(sd)) {
		ShowError("buildin_getautocombat: No player attached!\n");
		return SCRIPT_CMD_FAILURE;
	}

	enum ac_type type = (enum ac_type)script_getnum(st, 2);
	int index = script_hasdata(st, 3) ? script_getnum(st, 3) : 0;
	int i, num = 0;
	switch(type) {
	case AC_HEALSKILL:
		num = sd->ac.healskills[index].skill_lv;
		break;
	case AC_HPPOTION:
		ARR_FIND(0, sd->ac.hp_potions.size(), i, sd->ac.hp_potions[i].item_id == index);
		num = i < sd->ac.hp_potions.size() ? sd->ac.hp_potions[i].min_hp : 0;
		break;
	case AC_SPPOTION:
		ARR_FIND(0, sd->ac.sp_potions.size(), i, sd->ac.sp_potions[i].item_id == index);
		num = i < sd->ac.sp_potions.size() ? sd->ac.sp_potions[i].min_sp : 0;
		break;
	case AC_BUFFITEM:
		ARR_FIND(0, ARRAYLENGTH(buff_items), i, buff_items[i].item_id == index);
		if (i < ARRAYLENGTH(buff_items))
			num = sd->ac.buffitems & (1 << i);
		break;
    case AC_SIT:
		num = index ? sd->ac.sit_min_sp : sd->ac.sit_min_hp;
		break;
	case AC_BUFFSKILL:
		ARR_FIND(0, sd->ac.buffskills.size(), i, sd->ac.buffskills[i].skill_id == index);
		num = i < sd->ac.buffskills.size() ? sd->ac.buffskills[i].skill_lv : 0;
		break;
	case AC_ATTACKSKILL:
		ARR_FIND(0, sd->ac.attackskills.size(), i, sd->ac.attackskills[i].skill_id == index);
		num = i < sd->ac.attackskills.size() ? sd->ac.attackskills[i].skill_lv : 0;
		break;
	case AC_LOOTITEM:
		num = std::find(sd->ac.loot_item_selected.begin(), sd->ac.loot_item_selected.end(), index) != sd->ac.loot_item_selected.end();
		break;
	case AC_MOBS:
		num = std::find(sd->ac.mob_id.begin(), sd->ac.mob_id.end(), index) != sd->ac.mob_id.end();
		break;
    case AC_NORMALATK:
		num = sd->ac.disable_normal_atk;
		break;
    case AC_TELEPORT:
		switch(index) {
		case 0:
			num = !sd->ac.teleport.disable_tp_skill;
			break;
		case 1:
			num = !sd->ac.teleport.disable_flywing;
			break;
		case 2:
			num = sd->ac.teleport.tp_when_mvp;
			break;
		case 3:
			num = sd->ac.teleport.emergency_hp;
			break;
		case 4:
			num = sd->ac.teleport.no_mob_delay / 1000;
			break;
		}
		break;
	case AC_RETALIATE:
		num = !sd->ac.retaliate;
		break;
	case AC_ENDCONDITION:
		break;
	default:
		ShowError("buildin_getautocombat: wrong type!\n");
		return SCRIPT_CMD_FAILURE;
	}

	script_pushint(st, num);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(setautocombat) {
	struct map_session_data *sd;

	if (!script_rid2sd(sd)) {
		ShowError("buildin_setautocombat: No player attached!\n");
		return SCRIPT_CMD_FAILURE;
	}

	enum ac_type type = (enum ac_type)script_getnum(st, 2);
	int param_count = script_lastdata(st);
	int i = 0;

	switch(type) {
	//========== setautocombat(AC_HEAL, <heal_index>, <skill_lv, <min_hp>);
	case AC_HEALSKILL:
		if (param_count == 5) {
			int heal_index = script_getnum(st, 3);
			sd->ac.healskills[heal_index].skill_lv = script_getnum(st, 4);
			sd->ac.healskills[heal_index].min_hp_sp = script_getnum(st, 5);
		} else {
			ShowError("buildin_setautocombat: AC_HEALSKILL wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_HPPOTION, <item_id>, <min_hp>);
	case AC_HPPOTION:
		if (param_count == 4) {
			int item_id = script_getnum(st, 3);
			uint16 min_hp = script_getnum(st, 4);
			auto it = std::find_if(sd->ac.hp_potions.begin(), sd->ac.hp_potions.end(), [item_id](const s_hp_potions& potion) { return potion.item_id == item_id; });
			if (it != sd->ac.hp_potions.end()) {
				sd->ac.hp_potions.erase(it);
				break;
			}
			s_hp_potions new_struct = {item_id, min_hp};
			sd->ac.hp_potions.push_back(new_struct);
		} else {
			ShowError("buildin_setautocombat: AC_HPPOTION wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_SPPOTION, <item_id>, <min_sp>);
	case AC_SPPOTION:
		if (param_count == 4) {
			int item_id = script_getnum(st, 3);
			uint16 min_sp = script_getnum(st, 4);
			auto it = std::find_if(sd->ac.sp_potions.begin(), sd->ac.sp_potions.end(), [item_id](const s_sp_potions& potion) { return potion.item_id == item_id; });
			if (it != sd->ac.sp_potions.end()) {
				sd->ac.sp_potions.erase(it);
				break;
			}
			s_sp_potions new_struct = {item_id, min_sp};
			sd->ac.sp_potions.push_back(new_struct);
		} else {
			ShowError("buildin_setautocombat: AC_SPPOTION wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_SIT, <0 = hp, 1 = sp>, <percent>);
	case AC_SIT:
		if (param_count == 4) {
			if (!script_getnum(st, 3)) // hp
				sd->ac.sit_min_hp = script_getnum(st, 4);
			else // sp
				sd->ac.sit_min_sp = script_getnum(st, 4);
		} else {
			ShowError("buildin_setautocombat: AC_SIT wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_BUFFSKILL, <skill_id>, <skill_lv>);
	case AC_BUFFSKILL:
		if (param_count == 4) {
			uint16 skill_id = script_getnum(st, 3);
			uint16 skill_lv = script_getnum(st, 4);
			auto it = std::find_if(sd->ac.buffskills.begin(), sd->ac.buffskills.end(), [skill_id](const s_buff_skills& buffskills) { return buffskills.skill_id == skill_id; });
			if (it != sd->ac.buffskills.end()) {
				sd->ac.buffskills.erase(it);
				break;
			}
			s_buff_skills new_struct = {skill_id, skill_lv};
			sd->ac.buffskills.push_back(new_struct);
		} else {
			ShowError("buildin_setautocombat: AC_BUFFSKILL wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_ATTACKSKILL, <skill_id>, <skill_lv>);
	case AC_ATTACKSKILL:
		if (param_count == 4) {
			uint16 skill_id = script_getnum(st, 3);
			uint16 skill_lv = script_getnum(st, 4);
			auto it = std::find_if(sd->ac.attackskills.begin(), sd->ac.attackskills.end(), [skill_id](const s_attack_skills& attackskills) { return attackskills.skill_id == skill_id; });
			if (it != sd->ac.attackskills.end()) {
				sd->ac.attackskills.erase(it);
				break;
			}
			s_attack_skills new_struct = {skill_id, skill_lv};
			sd->ac.attackskills.push_back(new_struct);
		} else {
			ShowError("buildin_setautocombat: AC_ATTACKSKILL wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_BUFFITEM, <item_id>);
	case AC_BUFFITEM:
		if (param_count == 3) {
			ARR_FIND(0, ARRAYLENGTH(buff_items), i, buff_items[i].item_id == script_getnum(st, 3));
			if (i < ARRAYLENGTH(buff_items))
				sd->ac.buffitems ^= (1 << i);
		} else {
			ShowError("buildin_setautocombat: AC_BUFFITEM wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_LOOTITEM, <item_id, or -1 to none, or 0 to all items>);
	case AC_LOOTITEM:
		if (param_count == 3) {
			int item_id = script_getnum(st, 3);
			if (item_id < AC_LOOT_GROUP_MAX) {
				sd->ac.loot_item_selected.clear();
				sd->ac.loot_item_config = (enum loot_item_config)item_id;
				break;
			}
			auto it = std::find_if(sd->ac.loot_item_selected.begin(), sd->ac.loot_item_selected.end(), [item_id](const int& ele) { return ele == item_id; });
			if (it != sd->ac.loot_item_selected.end()) {
				sd->ac.loot_item_selected.erase(it);
				break;
			}
			sd->ac.loot_item_selected.push_back(item_id);
		} else {
			ShowError("buildin_setautocombat: AC_LOOTITEM wrong syntax\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_MOBS, <mob_id or 0 to reset all>);
	case AC_MOBS:
		if (param_count == 3) {
			uint32 mob_id = script_getnum(st, 3);
			if (mob_id == 0) {
				sd->ac.mob_id.clear();
				return SCRIPT_CMD_SUCCESS;
			}
			auto it = std::find_if(sd->ac.mob_id.begin(), sd->ac.mob_id.end(), [mob_id](const int& ele) { return ele == mob_id; });
			if (it != sd->ac.mob_id.end()) {
				sd->ac.mob_id.erase(it);
				break;
			}
			sd->ac.mob_id.push_back(mob_id);
		} else {
			ShowError("buildin_setautocombat: AC_MOBS wrong syntax\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_NORMALATK);
	case AC_NORMALATK:
		if (param_count == 2)
			sd->ac.disable_normal_atk = !sd->ac.disable_normal_atk;
		else {
			ShowError("buildin_setautocombat: AC_NORMALATK wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_TELEPORT, <info>, <value>);
	case AC_TELEPORT:
		if (param_count == 4) {
			switch(script_getnum(st, 3)) {
			case 0:
				sd->ac.teleport.disable_tp_skill = !sd->ac.teleport.disable_tp_skill;
				break;
			case 1:
				sd->ac.teleport.disable_flywing = !sd->ac.teleport.disable_flywing;
				break;
			case 2:
				sd->ac.teleport.tp_when_mvp = !sd->ac.teleport.tp_when_mvp;
				break;
			case 3:
				sd->ac.teleport.emergency_hp = script_getnum(st, 4);
				break;
			case 4:
				sd->ac.teleport.no_mob_delay = script_getnum(st, 4) * 1000;
				break;
			}
		} else {
			ShowError("buildin_setautocombat: auto teleport wrong syntax!\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_RETALIATE);
	case AC_RETALIATE:
		if (param_count == 2) {
			sd->ac.retaliate = !sd->ac.retaliate;
		} else {
			ShowError("buildin_setautocombat: AC_RETALIATE wrong syntax\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	//========== setautocombat(AC_ENDCONDITION);
	case AC_ENDCONDITION:
		if (param_count == 2) {
			sd->ac.end_status_config++;
			if (sd->ac.end_status_config > 2)
				sd->ac.end_status_config = 0;
		} else {
			ShowError("buildin_setautocombat: AC_ENDCONDITION wrong syntax\n");
			return SCRIPT_CMD_FAILURE;
		}
		break;
	default:
		ShowError("buildin_setautocombat: wrong type!\n");
		return SCRIPT_CMD_FAILURE;
	}
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getavailablemobs) {
	map_session_data *sd;
	if (!script_rid2sd(sd))
		return SCRIPT_CMD_FAILURE;

	TBL_MOB *md;
	int64 value;
	struct script_data* data = script_getdata(st, 2);
	const char *name = nullptr;
	int32 id = 0;
	int size = 0;

	if (!data_isreference(data)) {
		ShowError("buildin_getavailablemobs: not a variable\n");
		script_reportdata(data);
		st->state = END;
		return SCRIPT_CMD_FAILURE;
	}

	name = reference_getname(data);
	id = reference_getid(data);

	struct s_mapiterator *iter = mapit_geteachmob();
	for (md = (TBL_MOB *)mapit_first(iter); mapit_exists(iter); md = (TBL_MOB *)mapit_next(iter)) {
		if (md == nullptr) continue;
		bool found = false;
		if (sd->m == md->m) {
			for (int i = 0; i < size; i++) {
				int64 value = get_val2_num(st, reference_uid(id, i), script_getref(st, 2));
				if (md->db->id == value) {
					found = true;
					break;
				}
			}
			if (!found) {
				set_reg_num(st, sd, reference_uid(id, size), name, md->db->id, script_getref(st, 2));
				size++;
			}
		}
	}
	mapit_free(iter);

	script_pushint(st, size);
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(getautocombatduration) { // getautocombatduration(<max_duration>, <max_duration_vip>);
#if AUTOCOMBAT_DURATION_CONFIG == 0
	script_pushint(st, -1);
	return SCRIPT_CMD_SUCCESS;
#else
	map_session_data *sd;
	if (!script_rid2sd(sd)) {
		ShowError("buildin_getautocombatduration: No player attached!\n");
		return SCRIPT_CMD_FAILURE;
	}
	int remaining_duration = 0;
#if AUTOCOMBAT_DURATION_CONFIG > 3
	int i = pc_search_inventory(sd, AUTOCOMBAT_DURATION_CONFIG);
	if (sd->inventory.u.items_inventory[i].nameid > 0 && sd->inventory.u.items_inventory[i].amount > 0 && sd->inventory.u.items_inventory[i].expire_time > 1)
		remaining_duration = sd->inventory.u.items_inventory[i].expire_time - (int)time(NULL);
#else
	int max_duration_vip = script_getnum(st, 3) * 60;
	int max_duration_final = (max_duration_vip > 0 && pc_isvip(sd)) ? max_duration_vip : script_getnum(st, 2) * 60;
#if AUTOCOMBAT_DURATION_CONFIG == 2
	remaining_duration = max_duration_final - pc_readglobalreg(sd, add_str("#AC_DURATION"));
#elif AUTOCOMBAT_DURATION_CONFIG == 1
	remaining_duration = max_duration_final - pc_readglobalreg(sd, add_str("AC_DURATION"));
#endif
	sd->ac.max_duration = max_duration_final - 2; // 2 seconds offset
#endif
	script_pushint(st, max(0, remaining_duration));
	return SCRIPT_CMD_SUCCESS;
#endif
}

BUILDIN_FUNC(getskillname) {
	uint16 skill_id = script_getnum(st, 2);
	if (!skill_get_index(script_getnum(st, 2)))
		script_pushconststr(st, "Unknown Skill");
	else
		script_pushstrcopy(st, skill_get_desc(skill_id));

	return SCRIPT_CMD_SUCCESS;
}
