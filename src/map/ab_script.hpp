#ifndef AB_SCRIPT_H
#define AB_SCRIPT_H

#include "party.hpp"
#include "pc.hpp"

#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <iostream>
#include <sstream>

#include <common/db.hpp>
#include <common/database.hpp>
#include <common/utilities.hpp>
#include <common/utils.hpp>

//Get string script
void handle_autobuff_heal(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p);
void handle_autobuff_potions(std::ostringstream& os_buf, int index, script_state* st, TBL_PC* sd);
void handle_autobuff_buff(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p);
void handle_autobuff_follow(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p);
void handle_autobuff_items(std::ostringstream& os_buf, int index, script_state* st, TBL_PC* sd);
void handle_autobuff_pm(std::ostringstream& os_buf, script_state* st, TBL_PC* sd, struct party_data* p);
void handle_resurrection(std::ostringstream& os_buf, TBL_PC* sd);
void handle_potions(std::ostringstream& os_buf, TBL_PC* sd);
void handle_token_of_siegfried(std::ostringstream& os_buf, TBL_PC* sd);
void handle_return_to_savepoint(std::ostringstream& os_buf, TBL_PC* sd);
void handle_party_config(std::ostringstream& os_buf, TBL_PC* sd);
void handle_priorize_buff(std::ostringstream& os_buf, TBL_PC* sd);
void handle_potion_pitcher(std::ostringstream& os_buf, int index, int extra_index, int extra_index2, script_state* st, TBL_PC* sd, struct party_data* p);

//Start the ab
bool handleAutobuff_fromitem(TBL_PC* sd, t_itemid item_id, t_tick max_duration);
bool handleAutobuff_start(TBL_PC* sd, t_tick duration_seconds);

//Set script
void handleAutoHeal(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p);
void handleAutoPotion(const std::vector<std::string>& result, TBL_PC* sd);
void handleFollowPlayer(const std::vector<std::string>& result, TBL_PC* sd);
void handleAutoBuffSkills(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p);
void handleDistanceToLeader(const std::vector<std::string>& result, TBL_PC* sd);
void handleAutoBuffItems(const std::vector<std::string>& result, TBL_PC* sd);
void handleAutoResurrection(const std::vector<std::string>& result, TBL_PC* sd);
void handlePMConfiguration(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p);
void handleAutoBuffPotionState(const std::vector<std::string>& result, TBL_PC* sd);
void handleReturnToSavepoint(const std::vector<std::string>& result, TBL_PC* sd);
void handleResetAutoBuffConfig(TBL_PC* sd);
void handleTokenOfSiegfried(const std::vector<std::string>& result, TBL_PC* sd);
void handleDisableWhenAlone(const std::vector<std::string>& result, TBL_PC* sd);
void handlePriorizeBuff(const std::vector<std::string>& result, TBL_PC* sd);
int handleGetautobuffint(TBL_PC* sd, const int value);
void handleAutoPotionPitcher(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p);

#endif // AB_SCRIPT_H