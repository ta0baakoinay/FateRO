// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Atcommands
 * Place the body of custom atcommands in this file.
 * Format:
 *	ACMD_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//ACMD_FUNC(newcommand)
//{
//	clif_displaymessage(fd, "It works!");
//	clif_specialeffect(&sd->bl, EF_HEARTCASTING, AREA);
//	return 0;
//}

/**
 * @autosupport allowteleport <no|samemap|anywhere>
 * Server-wide runtime control of the Auto Support (AutoBuff) teleport-to-target
 * policy. Maps onto battle_config.feature_autobuff_teleportiflost (0/1/2) and is
 * picked up immediately by every Auto Support follower - it is NOT per-character.
 * Standard rAthena permission system: available to any group that has this
 * command (the admin / level-99 group already does via all_commands).
 */
ACMD_FUNC(autosupport)
{
	nullpo_retr(-1, sd);

	char sub[32], mode[32];
	sub[0] = mode[0] = '\0';

	if (!message || !*message || sscanf(message, "%31s %31s", sub, mode) < 1 || strcmpi(sub, "allowteleport") != 0) {
		clif_displaymessage(fd, "Usage: @autosupport allowteleport <no|samemap|anywhere>");
		clif_displaymessage(fd, "  no       - Auto Support never teleports to the followed player");
		clif_displaymessage(fd, "  samemap  - teleport only when both are on the same map");
		clif_displaymessage(fd, "  anywhere - teleport across maps too");
		clif_displaymessage(fd, "  (instance / NoTeleport / NoWarp / GvG / BG restrictions still apply)");
		return -1;
	}

	if (mode[0] == '\0') {
		const char* cur = battle_config.feature_autobuff_teleportiflost >= 2 ? "anywhere" :
		                  battle_config.feature_autobuff_teleportiflost == 1 ? "samemap" : "no";
		char buf[96];
		safesnprintf(buf, sizeof(buf), "Auto Support allowteleport is currently: %s", cur);
		clif_displaymessage(fd, buf);
		return 0;
	}

	int32 val;
	if (strcmpi(mode, "no") == 0 || strcmpi(mode, "off") == 0)              val = 0;
	else if (strcmpi(mode, "samemap") == 0 || strcmpi(mode, "same_map") == 0) val = 1;
	else if (strcmpi(mode, "anywhere") == 0 || strcmpi(mode, "any") == 0)   val = 2;
	else {
		clif_displaymessage(fd, "Invalid mode. Use: no | samemap | anywhere");
		return -1;
	}

	battle_config.feature_autobuff_teleportiflost = val;

	char buf[128];
	safesnprintf(buf, sizeof(buf), "Auto Support allowteleport set to '%s' (server-wide, runtime).", mode);
	clif_displaymessage(fd, buf);
	return 0;
}


ACMD_FUNC(battleinfo)
{
    int state = 0;

    if (message && *message) {
        state = atoi(message);
        if (state < 0 || state > 15) {
			clif_displaymessage(fd, "Battle Information States:");
			clif_displaymessage(fd, " 0 - Off");
			clif_displaymessage(fd, " 1 - Kill/Death");
			clif_displaymessage(fd, " 2 - Dispell");
			clif_displaymessage(fd, " 4 - Provoke");
			clif_displaymessage(fd, " 8 - Mindbreaker");
			clif_displaymessage(fd, "15 - All (Kill/Death + Dispell + Provoke + Mindbreaker)");
			clif_displaymessage(fd, "Example: To enable Kill/Death and Dispell, use @battleinfo 3 (1 + 2).");
            return -1;
        }
    } else {
			clif_displaymessage(fd, "Battle Information States:");
			clif_displaymessage(fd, " 0 - Off");
			clif_displaymessage(fd, " 1 - Kill/Death");
			clif_displaymessage(fd, " 2 - Dispell");
			clif_displaymessage(fd, " 4 - Provoke");
			clif_displaymessage(fd, " 8 - Mindbreaker");
			clif_displaymessage(fd, "15 - All (Kill/Death + Dispell + Provoke + Mindbreaker)");
			clif_displaymessage(fd, "Example: To enable Kill/Death and Dispell, use @battleinfo 3 (1 + 2).");
            return -1;
	}

    if (state == 0) {
        clif_displaymessage(fd, "- Battle Information Display OFF -");
        sd->state.battleinfo = 0;
    } else {
        sd->state.battleinfo = state;
        char output[256] = "- Battle Information Display ON - States: ";
        int isFirst = 1;
        if (state & 1) {  // Kill/Death
            if (!isFirst) strcat(output, " | ");
            strcat(output, "Kill/Death");
            isFirst = 0;
        }
        if (state & 2) {  // Dispell
            if (!isFirst) strcat(output, " | ");
            strcat(output, "Dispell");
            isFirst = 0;
        }
        if (state & 4) {  // Provoke
            if (!isFirst) strcat(output, " | ");
            strcat(output, "Provoke");
            isFirst = 0;
        }
        if (state & 8) {  // Mindbreaker
            if (!isFirst) strcat(output, " | ");
            strcat(output, "Mindbreaker");
        }
        clif_displaymessage(fd, output);
    }

    return 0;
}
/*==========================================
 @showbuff - show buff in party windows
 Upgraded by Shakto
------------------------------------------*/
ACMD_FUNC(spb) {
		struct party_data* p = NULL;
		nullpo_retr(-1, sd);

		if( !sd->status.party_id ) {
			clif_displaymessage(fd, "You're not in a party."); // You're not in a party.
			return -1;
		}

        p = party_search(sd->status.party_id);

		if( sd->state.spb ) {
			sd->state.spb = 0;
			clif_displaymessage(fd, "Displaying party member's buffs disabled."); // Displaying party member's buffs disabled.
		} else {
			sd->state.spb = 1;
			clif_displaymessage(fd, "Displaying party member's buffs enabled."); // Displaying party member's buffs enabled.
		}

		clif_party_info(*p,sd);
		return 0;
}


ACMD_FUNC(reloadequipmentskin) {  
    if (equipment_skin_db.reload())  
        clif_displaymessage(fd, "Equipment skin database has been reloaded.");  
    else  
        clif_displaymessage(fd, "Failed to reload equipment skin database.");  
    return 0;
}


void atcommand_battlestats_sub(int fd, struct map_session_data *sd) {
	status_data *status = status_get_status_data(*sd);
	char output[CHAT_SIZE_MAX];

#define battlestats_msg(__msg__) (__msg__); clif_displaymessage(fd, output)

	battlestats_msg(sprintf(output, "Battle Status: %s", sd->status.name));

	clif_displaymessage(fd, "|--- Basic Info ---|");
	battlestats_msg(sprintf(output, "   -> ATK: %d + %d", pc_leftside_atk(sd), pc_rightside_atk(sd)));
	battlestats_msg(sprintf(output, "   -> MATK: %d + %d", pc_leftside_matk(sd), pc_rightside_matk(sd)));
	battlestats_msg(sprintf(output, "   -> HIT: %d", status->hit));
	battlestats_msg(sprintf(output, "   -> CRIT: %d", status->cri / 10));
	battlestats_msg(sprintf(output, "   -> DEF: %d + %d", pc_leftside_def(sd), pc_rightside_def(sd)));
	battlestats_msg(sprintf(output, "   -> MDEF: %d + %d", pc_leftside_mdef(sd), pc_rightside_mdef(sd)));
	battlestats_msg(sprintf(output, "   -> FLEE: %d + %d", status->flee, status->flee2 / 10));
	battlestats_msg(sprintf(output, "   -> ASPD: %d", (2000 - status->amotion) / 10));

	clif_displaymessage(fd, "|--- Bonus Physical Damages ---|");
	if (sd->right_weapon.addsize[SZ_MEDIUM] + sd->left_weapon.addsize[SZ_MEDIUM]) {
		battlestats_msg(sprintf(output, "   -> Medium Size: %d%%", sd->right_weapon.addsize[SZ_MEDIUM] + sd->left_weapon.addsize[SZ_MEDIUM]));
	}
	if (sd->right_weapon.addsize[SZ_ALL] + sd->left_weapon.addsize[SZ_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Sizes: %d%%", sd->right_weapon.addsize[SZ_ALL] + sd->left_weapon.addsize[SZ_ALL]));
	}
	if (sd->right_weapon.addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM] + sd->left_weapon.addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM]) {
		battlestats_msg(sprintf(output, "   -> Player: %d%%", sd->right_weapon.addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM] + sd->left_weapon.addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM]));
	}
	if (sd->right_weapon.addrace[RC_ALL] + sd->left_weapon.addrace[RC_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Races: %d%%", sd->right_weapon.addrace[RC_ALL] + sd->left_weapon.addrace[RC_ALL]));
	}
	if (sd->right_weapon.addclass[CLASS_NORMAL] + sd->left_weapon.addclass[CLASS_NORMAL]) {
		battlestats_msg(sprintf(output, "   -> Normal Class: %d%%", sd->right_weapon.addclass[CLASS_NORMAL] + sd->left_weapon.addclass[CLASS_NORMAL]));
	}
	if (sd->right_weapon.addclass[CLASS_ALL] + sd->left_weapon.addclass[CLASS_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Classes: %d%%", sd->right_weapon.addclass[CLASS_ALL] + sd->left_weapon.addclass[CLASS_ALL]));
	}

	clif_displaymessage(fd, "|--- Bonus Magic Damages ---|");
	if (sd->indexed_bonus.magic_addsize[SZ_MEDIUM]) {
		battlestats_msg(sprintf(output, "   -> Medium Size: %d%%", sd->indexed_bonus.magic_addsize[SZ_MEDIUM]));
	}
	if (sd->indexed_bonus.magic_addsize[SZ_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Sizes: %d%%", sd->indexed_bonus.magic_addsize[SZ_ALL]));
	}
	if (sd->indexed_bonus.magic_addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM]) {
		battlestats_msg(sprintf(output, "   -> Player: %d%%", sd->indexed_bonus.magic_addrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM]));
	}
	if (sd->indexed_bonus.magic_addrace[RC_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Races: %d%%", sd->indexed_bonus.magic_addrace[RC_ALL]));
	}
	if (sd->indexed_bonus.magic_addclass[CLASS_NORMAL]) {
		battlestats_msg(sprintf(output, "   -> Normal Class: %d%%", sd->indexed_bonus.magic_addclass[CLASS_NORMAL]));
	}
	if (sd->indexed_bonus.magic_addclass[CLASS_ALL]) {
		battlestats_msg(sprintf(output, "   -> All Classes: %d%%", sd->indexed_bonus.magic_addclass[CLASS_ALL]));
	}

clif_displaymessage(fd, "|--- Elemental Resistance ---|");
struct {
    const char *format;
    int element;
} ele_resist_info[] = {
    { "   -> Neutral: %d%%", ELE_NEUTRAL },
    { "   -> Water: %d%%", ELE_WATER },
    { "   -> Earth: %d%%", ELE_EARTH },
    { "   -> Fire: %d%%", ELE_FIRE },
    { "   -> Wind: %d%%", ELE_WIND },
    { "   -> Poison: %d%%", ELE_POISON },
    { "   -> Holy: %d%%", ELE_HOLY },
    { "   -> Dark: %d%%", ELE_DARK },
    { "   -> Ghost: %d%%", ELE_GHOST },
    { "   -> Undead: %d%%", ELE_UNDEAD },
};

for (int i = 0; i < ARRAYLENGTH(ele_resist_info); i++) {
    // Calculate total resistance using the same formula as the working @resist command
    int total_resist = (sd->indexed_bonus.subele[ele_resist_info[i].element] + 
                       sd->indexed_bonus.subele_script[ele_resist_info[i].element] + 
                       sd->indexed_bonus.subele[ELE_ALL] + 
                       sd->indexed_bonus.subele_script[ELE_ALL]);
    
    if (total_resist == 0)
        continue;
        
    battlestats_msg(sprintf(output, ele_resist_info[i].format, total_resist));
}

	clif_displaymessage(fd, "|--- Race Resistance ---|");
	struct {
		const char *format;
		int value;
	} race_resist_info[] = {
		{ "   -> Formless: %d%%", sd->indexed_bonus.subrace[RC_FORMLESS] },
		{ "   -> Undead: %d%%", sd->indexed_bonus.subrace[RC_UNDEAD] },
		{ "   -> Beast: %d%%", sd->indexed_bonus.subrace[RC_BRUTE] },
		{ "   -> Plant: %d%%", sd->indexed_bonus.subrace[RC_PLANT] },
		{ "   -> Insect: %d%%", sd->indexed_bonus.subrace[RC_INSECT] },
		{ "   -> Fish: %d%%", sd->indexed_bonus.subrace[RC_FISH] },
		{ "   -> Demon: %d%%", sd->indexed_bonus.subrace[RC_DEMON] },
		{ "   -> DemiHuman: %d%%", sd->indexed_bonus.subrace[RC_DEMIHUMAN] },
		{ "   -> Angel: %d%%", sd->indexed_bonus.subrace[RC_ANGEL] },
		{ "   -> Dragon: %d%%", sd->indexed_bonus.subrace[RC_DRAGON] },
		{ "   -> Player: %d%%", sd->indexed_bonus.subrace[RC_PLAYER_HUMAN || RC_PLAYER_DORAM] },
		{ "   -> ALL: %d%%", sd->indexed_bonus.subrace[RC_ALL] },
		{ "   -> Normal Class: %d%%", sd->indexed_bonus.subclass[CLASS_NORMAL] },
	};
	for (int i = 0; i < ARRAYLENGTH(race_resist_info); i++) {
		if (race_resist_info[i].value == 0)
			continue;
		battlestats_msg(sprintf(output, race_resist_info[i].format, race_resist_info[i].value));
	}

#undef battlestats_msg
}

ACMD_FUNC(battlestats) {
	nullpo_retr(-1, sd);
	atcommand_battlestats_sub(fd, sd);
	return 0;
}

ACMD_FUNC(enemystats) {
	struct map_session_data *pl_sd;

	nullpo_retr(-1, sd);

	memset(atcmd_player_name, '\0', sizeof atcmd_player_name);

	if (!message || !*message || sscanf(message, "%23[^\n]", atcmd_player_name) < 1) {
		clif_displaymessage(fd, "Please enter a player name (usage: @enemystats <char name>)");
		return -1;
	}

	pl_sd = map_nick2sd(atcmd_player_name, true);
	if (pl_sd == NULL ||
		strncmp(pl_sd->status.name, atcmd_player_name, NAME_LENGTH) != 0 ||
		(pc_has_permission(pl_sd, PC_PERM_HIDE_SESSION) && pc_get_group_level(pl_sd) > pc_get_group_level(sd))
		) {
		clif_displaymessage(fd, msg_txt(sd, 3)); // Character not found.
		return -1;
	}

	atcommand_battlestats_sub(fd, pl_sd);
	return 0;
}

/* @populate — population engine (runtime PC shells; alias @fakeplayers) */
/**
 * @reloadpopenginedb — Reload db/population_names.yml, db/population_engine.yml, db/population_chat.yml (does not despawn existing fake PCs).
 */
ACMD_FUNC(reloadpopenginedb)
{
	nullpo_retr(-1, sd);

	uint32_t n = 0;
	if (!population_engine_reload_equipment(&n)) {
		clif_displaymessage(fd, "Population engine equipment reload failed (see map-server console).");
		return -1;
	}

	char buf[128];
	snprintf(buf, sizeof(buf), "Population engine equipment reloaded (%u job profiles).", static_cast<unsigned>(n));
	clif_displaymessage(fd, buf);
	return 0;
}

ACMD_FUNC(populate)
{
	nullpo_retr(-1, sd);

	char cmd[50], param[256];

	if (!message || !*message) {
		clif_displaymessage(fd, "Fake Player (Population Engine) GM control:");
		clif_displaymessage(fd, "  MANUAL (one-time, never auto-replenished):");
		clif_displaymessage(fd, "    @populate <n> <map>       - create n fake players around <map> (job-aware)");
		clif_displaymessage(fd, "    @populate <n> allmap      - create n across all ELIGIBLE population maps");
		clif_displaymessage(fd, "    @populate add <n> [map]   - same, but ADDITIVE (does not clear existing)");
		clif_displaymessage(fd, "    @populate vending <n> <map|allmap> - n Merchant fake players that open REAL vends");
		clif_displaymessage(fd, "  AUTOMATIC (maintained until stopped):");
		clif_displaymessage(fd, "    @populate auto <n> <map>  - keep n auto fake players on <map>");
		clif_displaymessage(fd, "    @populate auto <n> allmap - keep n auto fake players across eligible maps");
		clif_displaymessage(fd, "    @populate auto stop [map] - stop auto (one map, or all); players stay");
		clif_displaymessage(fd, "  AUTOCOMBAT (fake players on the REAL @settings AutoCombat loop, for stress testing):");
		clif_displaymessage(fd, "    @populate AutoCombat <Job> <qty> <map>   - e.g. @populate AutoCombat Knight 100 prontera");
		clif_displaymessage(fd, "    @populate AutoCombat list | clear [<Job> <map>]");
		clif_displaymessage(fd, "  REMOVE (actually deletes fake players):");
		clif_displaymessage(fd, "    @populate remove <n> | <name> | <map> [n] | removeall");
		clif_displaymessage(fd, "  INFO:  @populate status | where [map] | list [map] | count [map] | find <name>");
		clif_displaymessage(fd, "  MISC:  @populate fill | stats | stop | timing | timingreset");
		return -1;
	}

	const char *msg_ptr = message;
	while (*msg_ptr == ' ' || *msg_ptr == '\t')
		msg_ptr++;
	cmd[0] = '\0'; param[0] = '\0';
	sscanf(msg_ptr, "%49s %255[^\n]", cmd, param);

	if (strcmpi(cmd, "add") == 0) {
		uint32 qty = 0;
		char map_name[32] = "";
		const int np = sscanf(param, "%u %31s", &qty, map_name);
		if (np < 1 || qty == 0) {
			clif_displaymessage(fd, "Usage: @populate add <n> [map|allmap]");
			return -1;
		}
		if (qty > 5000) {
			clif_displaymessage(fd, "Quantity too large (max 5000).");
			return -1;
		}
		int32 map_id = -1;                       // -1 = allmap / no preference (job picks its own map)
		if (np >= 2 && map_name[0] && strcmpi(map_name, "allmap") != 0) {
			const char *why = nullptr;
			if (!population_engine_map_allowed(map_name, &why)) {
				char m[128];
				safesnprintf(m, sizeof(m), "This map is not available for Fake Player population (%s).", why ? why : "restricted");
				clif_displaymessage(fd, m);
				return -1;
			}
			map_id = map_mapname2mapid(map_name);
		} else if (np < 2) {
			map_id = sd->m;                      // no map arg: the GM's current map
			const char *why = nullptr;
			if (!population_engine_map_allowed(map_getmapdata(sd->m)->name, &why)) {
				clif_displaymessage(fd, "Your current map is not eligible for population; use @populate add <n> allmap.");
				return -1;
			}
		}
		int redir = 0;
		const int made = population_engine_manual_add(map_id, qty, &redir);
		char msg[160];
		safesnprintf(msg, sizeof(msg), "Created %d MANUAL fake player(s). Total now: %zu.", made, population_engine_get_count());
		clif_displaymessage(fd, msg);
		if (redir > 0) {
			safesnprintf(msg, sizeof(msg), "  %d placed on a job-appropriate map (requested map not valid for their job).", redir);
			clif_displaymessage(fd, msg);
		}
		if (made < (int)qty)
			clif_displaymessage(fd, "  (fewer than requested - global cap population_engine_max_count or no walkable cell)");
		return 0;
	} else if (strcmpi(cmd, "vending") == 0) {
		// @populate vending <amount> <map|allmap>
		// -> <amount> Merchant-class fake players that each open a REAL rAthena
		//    vend (existing vendor path: MC_VENDING + cart + vending_openvending).
		uint32 qty = 0;
		char map_name[32] = "";
		const int np = sscanf(param, "%u %31s", &qty, map_name);
		if (np < 2 || qty == 0 || !map_name[0]) {
			clif_displaymessage(fd, "Usage: @populate vending <amount> <map|allmap>");
			clif_displaymessage(fd, "  e.g. @populate vending 100 maintown   /   @populate vending 200 allmap");
			return -1;
		}
		if (qty > 5000) {
			clif_displaymessage(fd, "Quantity too large (max 5000).");
			return -1;
		}
		const bool allmap = (strcmpi(map_name, "allmap") == 0);
		int32 map_id = -1;
		if (!allmap) {
			const char *why = nullptr;
			if (!population_engine_map_allowed(map_name, &why)) {
				char m[128];
				safesnprintf(m, sizeof(m), "This map is not available for Fake Player population (%s).", why ? why : "restricted");
				clif_displaymessage(fd, m);
				return -1;
			}
			map_id = map_mapname2mapid(map_name);
		}
		{
			char m[128];
			safesnprintf(m, sizeof(m), "Creating %u fake vending merchants on %s...",
				qty, allmap ? "eligible town maps" : map_name);
			clif_displaymessage(fd, m);
		}
		// Consistent with the bare "@populate <n> <map>" form: clear existing fake
		// players first (real players are NEVER touched), then create the vendors.
		population_engine_manual_remove_all();
		int vredir = 0;
		const int vmade = population_engine_manual_add_vendors(map_id, qty, &vredir);
		char m[192];
		safesnprintf(m, sizeof(m), "Successfully created %d fake vendors on %s. Total fake players: %zu (vending now: %zu).",
			vmade, allmap ? "eligible town maps" : map_name,
			population_engine_get_count(), population_engine_vendor_count());
		clif_displaymessage(fd, m);
		if (vmade < (int)qty)
			clif_displaymessage(fd, "  (fewer than requested - global cap population_engine_max_count or no valid cell)");
		return 0;
	} else if (strcmpi(cmd, "auto") == 0) {
		char a1[32] = "", a2[32] = "";
		sscanf(param, "%31s %31s", a1, a2);
		if (a1[0] == '\0') {
			// bare "@populate auto" -> show auto status
			std::vector<PopulationAutoStatusRow> ar;
			population_engine_auto_status(ar);
			if (ar.empty()) {
				clif_displaymessage(fd, "Automatic population: OFF (no targets). Use @populate auto <n> <map|allmap>.");
			} else {
				clif_displaymessage(fd, "Automatic population targets:");
				char m[128];
				for (const auto &r : ar) {
					safesnprintf(m, sizeof(m), "  %-16s %u / %u", r.map_name, r.current, r.target);
					clif_displaymessage(fd, m);
				}
			}
			return 0;
		}
		if (strcmpi(a1, "stop") == 0) {
			if (a2[0] == '\0') {
				population_engine_auto_clear_all();
				char m[96];
				safesnprintf(m, sizeof(m), "Auto population disabled (all targets). Fake players kept: %zu.", population_engine_get_count());
				clif_displaymessage(fd, m);
				return 0;
			}
			int32 stop_id = -1; // allmap
			if (strcmpi(a2, "allmap") != 0) {
				stop_id = map_mapname2mapid(a2);
				if (stop_id < 0) { clif_displaymessage(fd, "Invalid map name."); return -1; }
			}
			if (population_engine_auto_clear(stop_id)) {
				char m[128];
				safesnprintf(m, sizeof(m), "Auto population for '%s' disabled. Its fake players are kept. Other auto targets stay active.", a2);
				clif_displaymessage(fd, m);
			} else {
				clif_displaymessage(fd, "No automatic target for that map.");
				return -1;
			}
			return 0;
		}
		// @populate auto <n> <map|allmap>
		uint32 qty = (uint32)strtoul(a1, nullptr, 10);
		if (qty == 0 || qty > 5000) {
			clif_displaymessage(fd, "Usage: @populate auto <n> <map|allmap>   (1..5000)");
			return -1;
		}
		int32 auto_id = -1; // allmap
		if (a2[0] == '\0') {
			clif_displaymessage(fd, "Usage: @populate auto <n> <map|allmap>");
			return -1;
		}
		if (strcmpi(a2, "allmap") != 0) {
			const char *why = nullptr;
			if (!population_engine_map_allowed(a2, &why)) {
				char m[128];
				safesnprintf(m, sizeof(m), "This map is not available for Fake Player population (%s).", why ? why : "restricted");
				clif_displaymessage(fd, m);
				return -1;
			}
			auto_id = map_mapname2mapid(a2);
		}
		const int cur = population_engine_auto_set(auto_id, qty);
		if (cur < 0) {
			clif_displaymessage(fd, "Could not set automatic target for that map.");
			return -1;
		}
		char m[160];
		safesnprintf(m, sizeof(m), "Automatic population ENABLED: target %u on '%s' (now %d). It will be topped up every 10s.",
			qty, (auto_id < 0 ? "allmap" : a2), cur);
		clif_displaymessage(fd, m);
		return 0;
	} else if (strcmpi(cmd, "fill") == 0) {
		const int made = population_engine_manual_fill_from_spawndb();
		char msg[128];
		safesnprintf(msg, sizeof(msg), "Autosummon pass done: %d fake player(s) created. Total now: %zu.", made, population_engine_get_count());
		clif_displaymessage(fd, msg);
		return 0;
	} else if (strcmpi(cmd, "removeall") == 0) {
		const int gone = population_engine_manual_remove_all();
		char msg[96];
		safesnprintf(msg, sizeof(msg), "Removed %d fake player(s). Total now: 0.", gone);
		clif_displaymessage(fd, msg);
		return 0;
	} else if (strcmpi(cmd, "remove") == 0) {
		if (!param[0]) {
			clif_displaymessage(fd, "Usage: @populate remove <n> | <name> | <map> [n]");
			return -1;
		}
		char t1[64] = "", t2[32] = "";
		sscanf(param, "%63s %31s", t1, t2);
		char *endp = nullptr;
		const unsigned long n = strtoul(t1, &endp, 10);
		if (endp != t1 && *endp == '\0' && n > 0) {
			// remove <n>
			const int gone = population_engine_manual_remove((uint32)n);
			char msg[96];
			safesnprintf(msg, sizeof(msg), "Removed %d fake player(s). Total now: %zu.", gone, population_engine_get_count());
			clif_displaymessage(fd, msg);
		} else if (map_mapname2mapid(t1) >= 0) {
			// remove <map> [n]
			const uint32 cnt = t2[0] ? (uint32)strtoul(t2, nullptr, 10) : 0u; // 0 = all on map
			const int gone = population_engine_manual_remove_on_map(t1, cnt);
			if (gone < 0) { clif_displaymessage(fd, "Invalid map name."); return -1; }
			char msg[128];
			safesnprintf(msg, sizeof(msg), "Removed %d fake player(s) from %s. Total now: %zu.", gone, t1, population_engine_get_count());
			clif_displaymessage(fd, msg);
		} else if (population_engine_manual_remove_named(t1)) {
			char msg[96];
			safesnprintf(msg, sizeof(msg), "Removed fake player '%s'. Total now: %zu.", t1, population_engine_get_count());
			clif_displaymessage(fd, msg);
		} else {
			clif_displaymessage(fd, "No fake player / map by that name.");
			return -1;
		}
		return 0;
	} else if (strcmpi(cmd, "count") == 0) {
		std::vector<PopulationShellSnapshot> rows;
		population_engine_snapshot(rows, param[0] ? param : nullptr);
		char msg[128];
		if (param[0])
			safesnprintf(msg, sizeof(msg), "Fake players on %s: %zu (total online: %zu).", param, rows.size(), population_engine_get_count());
		else
			safesnprintf(msg, sizeof(msg), "Fake players online: %zu.", population_engine_get_count());
		clif_displaymessage(fd, msg);
		return 0;
	} else if (strcmpi(cmd, "list") == 0 || strcmpi(cmd, "where") == 0) {
		std::vector<PopulationShellSnapshot> rows;
		population_engine_snapshot(rows, param[0] ? param : nullptr);
		char msg[192];
		safesnprintf(msg, sizeof(msg), "Fake Players: %zu%s%s   (manual %zu / auto %zu)", rows.size(),
			param[0] ? " on " : "", param[0] ? param : "",
			population_engine_manual_count(), population_engine_auto_count());
		clif_displaymessage(fd, msg);
		clif_displaymessage(fd, "Name                 Job          Map          X    Y    Src");
		clif_displaymessage(fd, "----------------------------------------------------------------------");
		size_t shown = 0;
		for (const auto &r : rows) {
			if (shown++ >= 100) {
				clif_displaymessage(fd, "  ... (truncated at 100; use @populate where <map> to narrow)");
				break;
			}
			const char *jn = job_name(r.job_id);
			safesnprintf(msg, sizeof(msg), "%-20s %-12s %-12s %-4d %-4d %s%s%s",
				r.name, (jn && jn[0]) ? jn : "?", r.map_name, r.x, r.y,
				r.is_auto ? "auto" : "manual", r.vending ? " vend" : "", r.dead ? " (dead)" : "");
			clif_displaymessage(fd, msg);
		}
		return 0;
	} else if (strcmpi(cmd, "find") == 0) {
		if (!param[0]) {
			clif_displaymessage(fd, "Usage: @populate find <name>");
			return -1;
		}
		std::vector<PopulationShellSnapshot> rows;
		population_engine_snapshot(rows, nullptr);
		bool found = false;
		char msg[160];
		for (const auto &r : rows) {
			if (strcmpi(r.name, param) != 0)
				continue;
			found = true;
			const char *jn = job_name(r.job_id);
			safesnprintf(msg, sizeof(msg), "%s : map %s (%d,%d) job %s%s",
				r.name, r.map_name, r.x, r.y, (jn && jn[0]) ? jn : "?", r.dead ? " status DEAD" : " status alive");
			clif_displaymessage(fd, msg);
			break;
		}
		if (!found) {
			clif_displaymessage(fd, "No fake player by that name is currently online.");
			return -1;
		}
		return 0;
	} else if (strcmpi(cmd, "stop") == 0) {
		if (!population_engine_is_running()) {
			clif_displaymessage(fd, "Population engine is not running");
			return -1;
		}
		population_engine_stop();
		clif_displaymessage(fd, "Population engine stopped and cleaned up");
	} else if (strcmpi(cmd, "stats") == 0) {
		PopulationEngineStats stats = population_engine_get_stats();
		const size_t live_count = population_engine_get_count();
		char msg[256];
		clif_displaymessage(fd, "=== Population engine stats ===");
		snprintf(msg, sizeof(msg), "  Active / created / errors : %zu / %u / %u",
			live_count, stats.total_created, stats.errors);
		clif_displaymessage(fd, msg);
		snprintf(msg, sizeof(msg), "  Chat lines emitted        : %u", stats.chat_lines_emitted);
		clif_displaymessage(fd, msg);
		snprintf(msg, sizeof(msg), "  Walk failures             : %u", stats.walk_failures);
		clif_displaymessage(fd, msg);
		snprintf(msg, sizeof(msg), "  Name blocklist retries    : %u", stats.name_retries);
		clif_displaymessage(fd, msg);
		// Memory estimate: each live bot holds one map_session_data allocation.
		// sizeof is a compile-time constant so this is safe in a sprintf context.
		const size_t est_kb = (live_count * sizeof(map_session_data) + 512) / 1024;
		snprintf(msg, sizeof(msg), "  Est. heap (map_session_data x %zu) : ~%zu KB",
			live_count, est_kb);
		clif_displaymessage(fd, msg);
		clif_displaymessage(fd, "================================");
	} else if (strcmpi(cmd, "status") == 0) {
		char m[192];
		clif_displaymessage(fd, "=== Population Status ===");
		safesnprintf(m, sizeof(m), "Total fake players: %zu   (manual %zu / auto %zu / vending %zu)",
			population_engine_get_count(), population_engine_manual_count(),
			population_engine_auto_count(), population_engine_vendor_count());
		clif_displaymessage(fd, m);

		std::vector<PopulationAutoStatusRow> ar;
		population_engine_auto_status(ar);
		if (ar.empty()) {
			clif_displaymessage(fd, "Automatic population: OFF");
		} else {
			clif_displaymessage(fd, "Automatic population: ACTIVE");
			for (const auto &r : ar) {
				safesnprintf(m, sizeof(m), "  %-16s %u / %u", r.map_name, r.current, r.target);
				clif_displaymessage(fd, m);
			}
		}

		{
			size_t elig = 0, excl = 0;
			population_engine_map_policy_counts(&elig, &excl);
			safesnprintf(m, sizeof(m), "Population maps: %zu eligible / %zu excluded  (instances, PvP, GvG/WoE, Battleground, jail -> EXCLUDED)",
				elig, excl);
			clif_displaymessage(fd, m);
		}

		// Per-map breakdown of who is where.
		std::vector<PopulationShellSnapshot> rows;
		population_engine_snapshot(rows, nullptr);
		if (!rows.empty()) {
			std::unordered_map<std::string,int> per_map;
			for (const auto &r : rows) per_map[r.map_name]++;
			clif_displaymessage(fd, "By map:");
			int lines = 0;
			for (const auto &kv : per_map) {
				if (lines++ >= 40) { clif_displaymessage(fd, "  ... (more)"); break; }
				safesnprintf(m, sizeof(m), "  %-16s %d", kv.first.c_str(), kv.second);
				clif_displaymessage(fd, m);
			}
		}
		clif_displaymessage(fd, "========================");
	} else if (strcmpi(cmd, "timing") == 0) {
		const WanderTimingSnapshot ts = population_engine_path_get_timing_snapshot();
		char msg[256];
		clif_displaymessage(fd, "=== Population wander tick profile ===");
		snprintf(msg, sizeof(msg), "  Ticks recorded / window      : %" PRIu64 " / %" PRIu64,
			ts.ticks_recorded, ts.window_size);
		clif_displaymessage(fd, msg);
		if (ts.window_size == 0) {
			clif_displaymessage(fd, "  (no samples yet — wait a few seconds)");
		} else {
			snprintf(msg, sizeof(msg), "  Avg total / tick             : %.1f µs   (%.2f ms)",
				ts.avg_total_us, ts.avg_total_us / 1000.0);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "    avg map_search_freecell    : %.1f µs   (%.1f%%)",
				ts.avg_freecell_us,
				ts.avg_total_us > 0 ? 100.0 * ts.avg_freecell_us / ts.avg_total_us : 0.0);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "    avg fallback path_search   : %.1f µs   (%.1f%%)",
				ts.avg_fallback_us,
				ts.avg_total_us > 0 ? 100.0 * ts.avg_fallback_us / ts.avg_total_us : 0.0);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "    avg unit_walktoxy          : %.1f µs   (%.1f%%)",
				ts.avg_unit_walktoxy_us,
				ts.avg_total_us > 0 ? 100.0 * ts.avg_unit_walktoxy_us / ts.avg_total_us : 0.0);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "  Avg shells examined/walked   : %.1f / %.1f",
				ts.avg_examined, ts.avg_processed);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "  Last tick total              : %" PRIu64 " µs  (proc %u, exam %u)",
				ts.last_total_us, ts.last_processed, ts.last_examined);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "    last freecell/fallback/uw  : %" PRIu64 " / %" PRIu64 " / %" PRIu64 " µs",
				ts.last_freecell_us, ts.last_fallback_us, ts.last_unit_walktoxy_us);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "  Worst tick total             : %" PRIu64 " µs  (proc %u)",
				ts.worst_total_us, ts.worst_processed);
			clif_displaymessage(fd, msg);
			snprintf(msg, sizeof(msg), "  Calls (cumulative) fc/fb/uw  : %" PRIu64 " / %" PRIu64 " / %" PRIu64,
				ts.freecell_calls, ts.fallback_calls, ts.unit_walktoxy_calls);
			clif_displaymessage(fd, msg);
		}
		clif_displaymessage(fd, "=======================================");
		// pe_perf generic section snapshot (chat/spawn/etc).
		{
			pe_perf::SectionStat sects[pe_perf::kMaxSections];
			size_t n = 0;
			pe_perf::snapshot(sects, n);
			clif_displaymessage(fd, "=== Population perf sections ===");
			if (n == 0) {
				clif_displaymessage(fd, "  (no sections recorded yet)");
			} else {
				char hdr[256];
				snprintf(hdr, sizeof(hdr), "  %-28s %10s %12s %10s %10s %10s",
					"section", "calls", "total_us", "avg_us", "last_us", "worst_us");
				clif_displaymessage(fd, hdr);
				for (size_t i = 0; i < n; ++i) {
					const auto &s = sects[i];
					const double avg = s.calls > 0 ? double(s.total_us) / double(s.calls) : 0.0;
					char row[256];
					snprintf(row, sizeof(row), "  %-28s %10" PRIu64 " %12" PRIu64 " %10.1f %10" PRIu64 " %10" PRIu64,
						s.name ? s.name : "?", s.calls, s.total_us, avg, s.last_us, s.worst_us);
					clif_displaymessage(fd, row);
				}
			}
			clif_displaymessage(fd, "================================");
		}
		// Global path_search() volume (all systems, not just population).
		{
			uint64 ps_calls = 0, ps_astar = 0;
			path_search_get_stats(&ps_calls, &ps_astar);
			char row[256];
			snprintf(row, sizeof(row), "  path_search calls: %" PRIu64 "   full A* runs: %" PRIu64 " (%.1f%%)",
				ps_calls, ps_astar, ps_calls > 0 ? 100.0 * double(ps_astar) / double(ps_calls) : 0.0);
			clif_displaymessage(fd, "=== path_search volume (since last timingreset) ===");
			clif_displaymessage(fd, row);
			clif_displaymessage(fd, "===================================================");
		}
	} else if (strcmpi(cmd, "timingreset") == 0) {
		population_engine_path_reset_timing();
		pe_perf::reset();
		path_search_reset_stats();
		clif_displaymessage(fd, "Population wander timing reset.");
	} else if (strcmpi(cmd, "AutoCombat") == 0 || strcmpi(cmd, "ac") == 0) {
		// @populate AutoCombat <Job> <qty> <map>   — fake players on the REAL
		// autocombat.cpp loop (SC_AUTOCOMBAT), for VPS stress testing.
		// @populate AutoCombat list | clear [<Job> <map>]
		char sub[32] = "";
		sscanf(param, "%31s", sub);

		if (sub[0] == '\0') {
			clif_displaymessage(fd, "Usage: @populate AutoCombat <Job> <qty> <map>");
			clif_displaymessage(fd, "       @populate AutoCombat list");
			clif_displaymessage(fd, "       @populate AutoCombat clear [<Job> <map>]");
			clif_displaymessage(fd, "  <Job> = single-word job name (Knight, Priest, Hunter, Wizard, ...) or numeric id.");
			return -1;
		}

		if (strcmpi(sub, "list") == 0) {
			std::vector<PopulationAutoCombatGroupRow> rows;
			population_engine_autocombat_list(rows);
			if (rows.empty()) {
				clif_displaymessage(fd, "No AutoCombat fake players.");
				return 0;
			}
			clif_displaymessage(fd, "Job              Map            Alive  Dead");
			clif_displaymessage(fd, "-------------------------------------------------");
			char m[160];
			uint32_t tot = 0;
			for (const auto &r : rows) {
				const char *jn = job_name(r.job_id);
				safesnprintf(m, sizeof(m), "%-16s %-14s %-6u %u", (jn && jn[0]) ? jn : "?", r.map_name, r.alive, r.dead);
				clif_displaymessage(fd, m);
				tot += r.alive;
			}
			safesnprintf(m, sizeof(m), "Total AutoCombat fake players: %u", tot);
			clif_displaymessage(fd, m);
			return 0;
		}

		if (strcmpi(sub, "clear") == 0) {
			char jobtok[32] = "", mapname[32] = "";
			const int np = sscanf(param, "%*s %31s %31s", jobtok, mapname);
			if (np < 1) {
				const int gone = population_engine_autocombat_remove_all();
				char m[96];
				safesnprintf(m, sizeof(m), "Removed %d AutoCombat fake player(s).", gone);
				clif_displaymessage(fd, m);
				return 0;
			}
			// clear <Job> [map]
			int32 job = atoi(jobtok);
			if (job <= 0) {
				for (int i = JOB_NOVICE; i < JOB_MAX_BASIC && job <= 0; i++)
					if (strncmpi(jobtok, job_name(i), 16) == 0) job = i;
				for (int i = JOB_NOVICE_HIGH; i < JOB_MAX && job <= 0; i++)
					if (strncmpi(jobtok, job_name(i), 16) == 0) job = i;
			}
			if (job <= 0 || !pcdb_checkid(job)) {
				clif_displaymessage(fd, "Unknown job.");
				return -1;
			}
			int32 map_id = -1;
			if (mapname[0]) {
				map_id = map_mapname2mapid(mapname);
				if (map_id < 0) { clif_displaymessage(fd, "Invalid map name."); return -1; }
			}
			const int gone = population_engine_autocombat_remove((uint16)job, map_id);
			char m[128];
			safesnprintf(m, sizeof(m), "Removed %d AutoCombat fake player(s) [%s%s%s].",
				gone, job_name(job), mapname[0] ? " @ " : "", mapname[0] ? mapname : "");
			clif_displaymessage(fd, m);
			return 0;
		}

		// @populate AutoCombat <Job> <qty> <map>
		char jobtok[32] = "", mapname[32] = "";
		uint32 qty = 0;
		if (sscanf(param, "%31s %u %31s", jobtok, &qty, mapname) < 3 || qty == 0 || !mapname[0]) {
			clif_displaymessage(fd, "Usage: @populate AutoCombat <Job> <qty> <map>   e.g. @populate AutoCombat Knight 100 prontera");
			return -1;
		}
		int32 job = atoi(jobtok);
		if (job <= 0) {
			for (int i = JOB_NOVICE; i < JOB_MAX_BASIC && job <= 0; i++)
				if (strncmpi(jobtok, job_name(i), 16) == 0) job = i;
			for (int i = JOB_NOVICE_HIGH; i < JOB_MAX && job <= 0; i++)
				if (strncmpi(jobtok, job_name(i), 16) == 0) job = i;
		}
		if (job <= 0 || !pcdb_checkid(job)) {
			clif_displaymessage(fd, "Unknown job. Use a single-word job name (Knight, Priest, Hunter, ...) or a numeric job id.");
			return -1;
		}
		const char *why = nullptr;
		if (!population_engine_map_allowed(mapname, &why)) {
			char m[128];
			safesnprintf(m, sizeof(m), "This map is not available for Fake Player population (%s).", why ? why : "restricted");
			clif_displaymessage(fd, m);
			return -1;
		}
		const int32 map_id = map_mapname2mapid(mapname);
		clif_displaymessage(fd, "Spawning AutoCombat fake players (this may take a moment for large counts)...");
		int redir = 0;
		const int made = population_engine_manual_add_autocombat((uint16)job, map_id, qty, &redir);
		char m[192];
		safesnprintf(m, sizeof(m), "Created %d / %u AutoCombat fake player(s) [%s @ %s]. Total fake players: %zu.",
			made, qty, job_name(job), mapname, population_engine_get_count());
		clif_displaymessage(fd, m);
		if (redir > 0)
			clif_displaymessage(fd, "  (requested map not suitable for that job's tier — group redirected to a job-appropriate map)");
		if (made < (int)qty)
			clif_displaymessage(fd, "  (fewer than requested — global cap battle_config.population_engine_max_count reached, or no walkable cell)");
		return 0;
	} else {
		uint32_t quantity = 0;
		char map_name[32] = "";
		int parsed = sscanf(msg_ptr, "%u %31s", &quantity, map_name);

		if (parsed < 1 || quantity == 0) {
			clif_displaymessage(fd, "Usage: @populate <quantity> <map|allmap>   (see @populate for the full list)");
			return -1;
		}
		if (quantity > 5000) {
			clif_displaymessage(fd, "Quantity too large (max 5000)");
			return -1;
		}
		int32 map_id = -1;   // -1 = allmap (each job on a map from its own profile)
		const bool allmap = (map_name[0] && strcmpi(map_name, "allmap") == 0);
		if (map_name[0] && !allmap) {
			const char *why = nullptr;
			if (!population_engine_map_allowed(map_name, &why)) {
				char m[128];
				safesnprintf(m, sizeof(m), "This map is not available for Fake Player population (%s).", why ? why : "restricted");
				clif_displaymessage(fd, m);
				return -1;
			}
			map_id = map_mapname2mapid(map_name);
		} else if (!map_name[0]) {
			// no map arg: use the GM's current map if it is eligible, else allmap.
			const char *why = nullptr;
			if (population_engine_map_allowed(map_getmapdata(sd->m)->name, &why))
				map_id = sd->m;
		}
		// MANUAL one-time form: clear existing fake players, then create `quantity`
		// fresh ones. Map selection is JOB-AWARE (db/population_spawn.yml): the
		// requested map is used only for jobs whose profile lists it; other jobs
		// go to a map appropriate for their job. Nothing is auto-replenished.
		population_engine_manual_remove_all();
		int redir = 0;
		const int made = population_engine_manual_add(map_id, quantity, &redir);
		char msg[192];
		safesnprintf(msg, sizeof(msg), "MANUAL: created %d fake player(s) (target: %s). Total now: %zu. Not auto-replenished.",
			made, allmap ? "allmap" : (map_name[0] ? map_name : "current map"), population_engine_get_count());
		clif_displaymessage(fd, msg);
		if (redir > 0) {
			safesnprintf(msg, sizeof(msg), "  %d placed on a job-appropriate map (requested map not valid for their job).", redir);
			clif_displaymessage(fd, msg);
		}
		if (made == 0) {
			clif_displaymessage(fd, "Failed to spawn population PCs (see map-server console).");
			return -1;
		}
	}
	return 0;
}
