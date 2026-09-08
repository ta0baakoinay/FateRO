// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "skill_vending.hpp"

#include <common/strlib.hpp> // safesnprintf

#include "map/clif.hpp"
#include "map/intif.hpp"
#include "map/itemdb.hpp"
#include "map/map.hpp"
#include "map/pc.hpp"

SkillVending::SkillVending() : SkillImpl(MC_VENDING) {
}

void SkillVending::castendNoDamageId(block_list *src, block_list *target, uint16 skill_lv, t_tick tick, int32& flag) const {
	map_session_data *sd = BL_CAST(BL_PC, src);
	if (sd == nullptr)
		return;

	// Prevent vending of GMs with unnecessary Level to trade/drop. [Skotlex]
	if (!pc_can_give_items(sd)) {
		clif_skill_fail(*sd, MC_VENDING);
		return;
	}

	sd->state.prevend = 1;
	sd->state.workinprogress = WIP_DISABLE_ALL;
	sd->vend_skill_lv = skill_lv;
	sd->vend_lvl = skill_lv;   // Extended Vending [Lilith / Easycore]
	sd->vend_loot = 0;         // Extended Vending [Lilith / Easycore]

	// Extended Vending system [Lilith / Easycore] --------------------------
	// If more than one currency is available, show the currency picker first.
	// The pick (clif_parse_SelectArrow -> skill_vending) then resolves the
	// currency and opens the shop-setup UI (saving the cart first if needed).
	if (battle_config.extended_vending) {
		int32 opts = 0;
		if (battle_config.item_zeny && item_db.exists(battle_config.item_zeny))
			opts++;
		if (battle_config.item_cash && item_db.exists(battle_config.item_cash))
			opts++;
		for (const auto &it : itemdb_vending) {
			if (item_db.exists(it.first) &&
				it.first != (t_itemid)battle_config.item_zeny &&
				it.first != (t_itemid)battle_config.item_cash)
				opts++;
		}

		if (opts > 1) {
			sd->state.pending_vending_ui = false;
			clif_vend(*sd, sd->vend_lvl);
			return;
		}

		if (opts == 1) {
			t_itemid only = 0;
			if (battle_config.item_zeny && item_db.exists(battle_config.item_zeny))
				only = battle_config.item_zeny;
			else if (battle_config.item_cash && item_db.exists(battle_config.item_cash))
				only = battle_config.item_cash;
			else {
				for (const auto &it : itemdb_vending) {
					if (item_db.exists(it.first)) { only = it.first; break; }
				}
			}
			sd->vend_loot = only;

			char output[CHAT_SIZE_MAX];
			safesnprintf(output, sizeof(output), msg_txt(sd, 1906), itemdb_ename(sd->vend_loot)); // "Current Currency: %s"
			clif_messagecolor(sd, color_table[COLOR_CYAN], output, false, SELF);
		}
		// opts == 0 -> plain zeny (vend_loot stays 0)
	}
	// --------------------------------------------------------------------

	int32 i = 0;
	ARR_FIND(0, MAX_CART, i, sd->cart.u.items_cart[i].nameid && sd->cart.u.items_cart[i].id == 0);
	if (i < MAX_CART) {
		// Save the cart before opening the vending UI (opened by the intif callback)
		sd->state.pending_vending_ui = true;
		intif_storage_save(sd, &sd->cart);
	} else {
		// Instantly open the vending UI
		sd->state.pending_vending_ui = false;
		clif_openvendingreq(*sd, 2 + skill_lv);
	}
}
