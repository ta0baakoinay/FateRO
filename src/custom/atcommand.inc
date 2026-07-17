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
