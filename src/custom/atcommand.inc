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
 