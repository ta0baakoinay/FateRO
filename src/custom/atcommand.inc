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
