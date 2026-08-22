#include "autobuff.hpp"
#include "ab_script.hpp"
#include "battle.hpp"
#include "pc.hpp"
#include "script.hpp"

#include <common/db.hpp>
#include <common/showmsg.hpp>

#include <cstdlib> // atoi, strtol, strtoll, exit
#include <sstream>
#include <string>
#include <vector>

std::string describe_target_priority(int priority, const s_autobuff_buffskills& autobuff, const party_data* p) {
	switch (priority) {
	case AB_SELF:
		return "Self";
	case AB_LEADER:
		return "Leader";
	case AB_NAMEDPLAYER: {
		std::ostringstream target_stream;
		for (const auto& member : p->party.member) {
			if (std::find(autobuff.priorize_buff_char_id.begin(), autobuff.priorize_buff_char_id.end(), member.char_id) != autobuff.priorize_buff_char_id.end()) {
				target_stream << member.name << " ";
			}
		}
		return target_stream.str();
	}
	case AB_EVERYONEEXCEPTSELF:
		return "Everyone in the party except itself";
	case AB_ALL:
		return "Everyone in the party";
	default:
		return "";
	}
}

std::string format_player_status(int char_id, bool is_online, bool is_selected, const std::string& name) {
	std::ostringstream player_status;
	player_status << (is_selected ? "^008000(ON)^000000 - " : "^FF0000(OFF)^000000 - ");
	player_status << name << (is_online ? " - Online" : " - Offline");
	return player_status.str();
}

void show_player_list(std::ostringstream& os_buf, std::map<int, std::string>& unique_players, const char* player_state, int& j, script_state* st, TBL_PC* sd) {
	bool first_done = false;
	for (const auto& uplayer : unique_players) {
		if (first_done)
			os_buf << ":";
		else
			first_done = true;

		os_buf << uplayer.second;
		set_reg_num(st, sd, reference_uid(add_str(player_state), j++), player_state, uplayer.first, nullptr);
	}
}

bool find_autobuffheal(TBL_PC* sd, int skill_id, s_autobuff_heal& found_buff) {
	auto it = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
		[skill_id](const s_autobuff_heal& v) { return v.skill_id == skill_id; });

	if (it != sd->ab.autobuff_heal.end()) {
		found_buff = *it;
		return true;
	}
	return false;
}

bool find_autobuffpotionpitcher(TBL_PC* sd, int skill_id, int skill_lv, s_autobuff_heal& found_buff) {
	auto it = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
		[skill_id, skill_lv](const s_autobuff_heal& v) { return v.skill_id == skill_id && v.skill_lv == skill_lv; });

	if (it != sd->ab.autobuff_heal.end()) {
		found_buff = *it;
		return true;
	}
	return false;
}

bool find_autobuffbuff(TBL_PC* sd, int skill_id, s_autobuff_buffskills& found_buff) {
	auto it = std::find_if(sd->ab.autobuff_buffskills.begin(), sd->ab.autobuff_buffskills.end(),
		[skill_id](const s_autobuff_buffskills& v) { return v.skill_id == skill_id; });

	if (it != sd->ab.autobuff_buffskills.end()) {
		found_buff = *it;
		return true;
	}
	return false;
}

std::map<int, std::string> get_unique_players(TBL_PC* sd, const s_autobuff_buffskills& itAutobuffskills, const party_data* p, const char* player_buff_state, script_state* st, std::ostringstream& os_buf) {
	std::map<int, std::string> unique_players;
	for (int i = 0; i < MAX_PARTY; ++i) {
		const auto& pmember = p->party.member[i];

		// Empty
		if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
			continue; // empty

		uint32 char_id = pmember.char_id;
		auto it = itAutobuffskills.priorize_buff_char_id.find(char_id);

		std::ostringstream player_status;
		player_status << ((it != itAutobuffskills.priorize_buff_char_id.end()) ? "^008000(ON)^000000 - " : "^FF0000(OFF)^000000 - ");
		player_status << pmember.name;
		player_status << ((!p->party.member[i].online) ? " - Offline" : " - Online");
		os_buf << "\n     " << player_status.str();

		unique_players[char_id] = player_status.str();
		set_reg_num(st, sd, reference_uid(add_str(player_buff_state), char_id), player_buff_state, (it != itAutobuffskills.priorize_buff_char_id.end()), nullptr);
	}
	return unique_players;
}

// Fonction pour rÔö£┬«cupÔö£┬«rer les compÔö£┬«tences uniques
std::map<int, std::string> get_unique_skills(TBL_PC* sd, const party_data* p, const char* skill_menu_state, script_state* st) {
	std::map<int, std::string> unique_skills;
	if (!sd->ab.autobuff_buffskills.empty()) {
		for (const auto& itAutobuffskills : sd->ab.autobuff_buffskills) {
			auto skill = skill_db.find(itAutobuffskills.skill_id);
			if (!skill) continue;

			std::ostringstream menu_temp;
			menu_temp << "^008000(ON)^000000 " << skill->desc << " - Lv " << itAutobuffskills.skill_lv << " - Target > ";
			menu_temp << describe_target_priority(itAutobuffskills.priorize_buff, itAutobuffskills, p);

			unique_skills[itAutobuffskills.skill_id] = menu_temp.str();

			set_reg_num(st, sd, reference_uid(add_str(skill_menu_state), itAutobuffskills.skill_id), skill_menu_state, 1, nullptr);
		}
	}

	// Ajout des autres compÔö£┬«tences
	for (int i = 0; i < MAX_SKILL; ++i) {
		if (sd->status.skill[i].id > 0 && sd->status.skill[i].lv > 0
			&& sd->status.skill[i].id != AL_RUWACH) {
			auto skill = skill_db.find(sd->status.skill[i].id);
			if (!skill) continue;

			e_cast_type type = skill_get_casttype(sd->status.skill[i].id);
			if ((type == CAST_NODAMAGE && skill->inf & (INF_SUPPORT_SKILL | INF_SELF_SKILL) && (skill_get_sc(skill->nameid) > 0) || skill->nameid == CR_FULLPROTECTION)) {

				if (unique_skills.count(skill->nameid) == 0) {
					std::ostringstream skill_temp;
					skill_temp << "^FF0000(OFF)^000000 " << skill->desc << " - Lv " << std::to_string(sd->status.skill[i].lv);
					unique_skills[skill->nameid] = skill_temp.str();

					set_reg_num(st, sd, reference_uid(add_str(skill_menu_state), skill->nameid), skill_menu_state, 0, nullptr);
				}
			}
		}
	}
	return unique_skills;
}

std::string format_skill_heal_status(const s_autobuff_heal& autobuff, std::shared_ptr<s_skill_db> skill) {
	std::ostringstream os;
	os << (autobuff.is_active ? "^008000(ON)^000000 " : "^FF0000(OFF)^000000 ");
	if(autobuff.skill_id == AM_POTIONPITCHER && autobuff.skill_lv == 5)
		os << "[" << skill->desc << "] - Lv " << autobuff.skill_lv << " - SP % " << autobuff.min_hp;
	else
		os << "[" << skill->desc << "] - Lv " << autobuff.skill_lv << " - HP % " << autobuff.min_hp;
	return os.str();
}

std::string format_skill_buff_status(const s_autobuff_buffskills& autobuff, std::shared_ptr<s_skill_db> skill) {
	std::ostringstream os;
	os << (autobuff.is_active ? "^008000(ON)^000000 " : "^FF0000(OFF)^000000 ");
	os << "[" << skill->desc << "]";
	os << (autobuff.is_active ? " - Lv " + std::to_string(autobuff.skill_lv) : "");
	return os.str();
}

void format_skill_list(TBL_PC* sd, std::ostringstream& menu, const std::map<int, std::string>& unique_skills, const char* skill_menu_ids, const char* skill_menu_state, script_state* st) {
	bool first_done = false;
	int j = 0;
	for (const auto& uskill : unique_skills) {
		if (first_done)
			menu << ":";
		else
			first_done = true;

		menu << uskill.second;

		set_reg_num(st, sd, reference_uid(add_str(skill_menu_ids), ++j), skill_menu_ids, uskill.first, nullptr);
	}
}

std::string describe_heal_priority(const s_autobuff_heal& autobuff, TBL_PC* sd, const std::map<int, std::string>& unique_players, int extra_index) {
	std::ostringstream os;
	os << " - Target > ";

	switch (autobuff.priorize_heal) {
	case AB_SELF:
		return " - Self";
	case AB_LEADER:
		return " - Leader";
	case AB_NAMEDPLAYER: {
		for (const auto& player : unique_players) {
			if (!extra_index) {
				os << "\n" << (autobuff.priorize_heal_char_id.find(player.first) != autobuff.priorize_heal_char_id.end() ? "  * " : "     ") << player.second;
			}
			else
				os << " ~" << player.second;
		}
		break;
	}
	case AB_EVERYONEEXCEPTSELF:
		return " - Everyone in the party except itself";
	case AB_ALL:
		return " - Everyone in the party";
	}
	return os.str();
}

void set_skill_state(script_state* st, TBL_PC* sd, const char* state_name, int skill_id, bool is_active) {
	set_reg_num(st, sd, reference_uid(add_str(state_name), skill_id), state_name, is_active ? 1 : 0, nullptr);
}

void handle_potion_pitcher(std::ostringstream& os_buf, int index, int extra_index, int extra_index2, script_state* st, TBL_PC* sd, struct party_data* p) {
	std::shared_ptr<s_skill_db> skill;

	const char* player_heal_list = ".@player_heal_list$";
	const char* player_heal_ids = ".@player_heal_ids";
	const char* player_heal_state = ".@player_heal_state";
	int j = 0;
	std::map<int, std::string> unique_players;
	std::ostringstream menu;

	//Add a lvl to extra_index2
	extra_index2++;

	if (index == 0) {
		skill = skill_db.find(extra_index);
		if (!skill)
			return;

		s_autobuff_heal autobuff;
		if (find_autobuffpotionpitcher(sd, extra_index, extra_index2, autobuff)) {
			os_buf << format_skill_heal_status(autobuff, skill);

			// Gerer la priorisation des soins
			if (autobuff.priorize_heal == AB_NAMEDPLAYER) {
				for (int i = 0; i < MAX_PARTY; ++i) {
					const auto& pmember = p->party.member[i];

					// Empty
					if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
						continue; // empty

					uint32 char_id = pmember.char_id;
					bool is_selected = autobuff.priorize_heal_char_id.find(char_id) != autobuff.priorize_heal_char_id.end();
					unique_players[char_id] = format_player_status(char_id, !p->party.member[i].online, is_selected, pmember.name);

					set_reg_num(st, sd, reference_uid(add_str(player_heal_state), char_id), player_heal_state, is_selected, nullptr);
				}
			}

			os_buf << describe_heal_priority(autobuff, sd, unique_players, 0);
			show_player_list(menu, unique_players, player_heal_ids, j, st, sd);
			set_reg_str(st, sd, reference_uid(add_str(player_heal_list), 0), player_heal_list, menu.str().c_str(), nullptr);
		}
	}
	else if (index > 0) {
		skill = skill_db.find(index);
		if (!skill)
			return;

		s_autobuff_heal autobuff;
		if (find_autobuffpotionpitcher(sd, index, extra_index2, autobuff)) {
			// GÔö£┬«rer la priorisation des soins
			if (autobuff.priorize_heal == AB_NAMEDPLAYER) {
				for (int i = 0; i < MAX_PARTY; ++i) {
					const auto& pmember = p->party.member[i];

					// Empty
					if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
						continue; // empty

					uint32 char_id = pmember.char_id;
					bool is_selected = autobuff.priorize_heal_char_id.find(char_id) != autobuff.priorize_heal_char_id.end();
					if (is_selected)
						unique_players[char_id] = pmember.name;
				}
			}

			os_buf << format_skill_heal_status(autobuff, skill);
			os_buf << describe_heal_priority(autobuff, sd, unique_players, extra_index);
			set_skill_state(st, sd, ".@skill_menu_state", extra_index2, true);
		}
		else {
			os_buf << "^FF0000(OFF)^000000 " << skill->desc << " - Lv " << std::to_string(extra_index2) << " - No target chosen";
			set_skill_state(st, sd, ".@skill_menu_state", extra_index2, false);
		}
	}

	return;
}

void handle_autobuff_heal(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p) {
	std::shared_ptr<s_skill_db> skill;

	const char* player_heal_list = ".@player_heal_list$";
	const char* player_heal_ids = ".@player_heal_ids";
	const char* player_heal_state = ".@player_heal_state";
	int j = 0;
	std::map<int, std::string> unique_players;
	std::ostringstream menu;

	if (index == 0) {
		skill = skill_db.find(extra_index);
		if (!skill)
			return;

		s_autobuff_heal autobuff;
		if (find_autobuffheal(sd, extra_index, autobuff)) {
			os_buf << format_skill_heal_status(autobuff, skill);

			// GÔö£┬«rer la priorisation des soins
			if (autobuff.priorize_heal == AB_NAMEDPLAYER) {
				for (int i = 0; i < MAX_PARTY; ++i) {
					const auto& pmember = p->party.member[i];

					// Empty
					if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
						continue; // empty

					uint32 char_id = pmember.char_id;
					bool is_selected = autobuff.priorize_heal_char_id.find(char_id) != autobuff.priorize_heal_char_id.end();
					unique_players[char_id] = format_player_status(char_id, !p->party.member[i].online, is_selected, pmember.name);

					set_reg_num(st, sd, reference_uid(add_str(player_heal_state), char_id), player_heal_state, is_selected, nullptr);
				}
			}

			os_buf << describe_heal_priority(autobuff, sd, unique_players, 0);
			show_player_list(menu, unique_players, player_heal_ids, j, st, sd);
			set_reg_str(st, sd, reference_uid(add_str(player_heal_list), 0), player_heal_list, menu.str().c_str(), nullptr);
		}
	}
	else if (index > 0) {
		skill = skill_db.find(index);
		if (!skill)
			return;

		s_autobuff_heal autobuff;
		if (find_autobuffheal(sd, index, autobuff)) {
			// GÔö£┬«rer la priorisation des soins
			if (autobuff.priorize_heal == AB_NAMEDPLAYER) {
				for (int i = 0; i < MAX_PARTY; ++i) {
					const auto& pmember = p->party.member[i];

					// Empty
					if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
						continue; // empty

					uint32 char_id = pmember.char_id;
					bool is_selected = autobuff.priorize_heal_char_id.find(char_id) != autobuff.priorize_heal_char_id.end();
					if (is_selected)
						unique_players[char_id] = pmember.name;
				}
			}

			os_buf << format_skill_heal_status(autobuff, skill);
			os_buf << describe_heal_priority(autobuff, sd, unique_players, extra_index);
			set_skill_state(st, sd, ".@skill_menu_state", index, true);
		}
		else {
			os_buf << "^FF0000(OFF)^000000 " << skill->desc << " - Max lv " << std::to_string(pc_checkskill(sd, index)) << " - No target chosen";
			set_skill_state(st, sd, ".@skill_menu_state", index, false);
		}
	}

	return;
}

void handle_autobuff_potions(std::ostringstream& os_buf, int index, script_state* st, TBL_PC* sd) {
	const char* potion_menu_list = ".@potion_menu_list$";
	const char* potion_menu_ids = ".@potion_menu_ids";
	const char* potion_menu_state = ".@potion_menu_state";
	std::ostringstream menu;
	bool first_done = false;

	int j = 0;

	std::map<int, std::string> unique_items;

	// Helper function to build potion item string
	auto buildPotionItemString = [](const std::string& status, const std::string& item_name, short amount = 0, uint16 min_hp = 0, uint16 min_sp = 0) {
		std::ostringstream item_str;
		item_str << status << " " << item_name;
		if (min_hp > 0)
			item_str << " - HP < " << min_hp << "%";
		if (min_sp > 0)
			item_str << " - SP < " << min_sp << "%";
		if (amount > 0)
			item_str << " - x" << amount;
		return item_str.str();
		};

	// Process active autobuff potions
	for (const auto& itAutopotion : sd->ab.autobuff_potions) {
		if (auto item_data = item_db.find(itAutopotion.item_id)) {
			unique_items[itAutopotion.item_id] = buildPotionItemString("^008000(ON)^000000", item_data->ename, 0, itAutopotion.min_hp, itAutopotion.min_sp);
			set_reg_num(st, sd, reference_uid(add_str(potion_menu_state), itAutopotion.item_id), potion_menu_state, 1, nullptr);
		}
	}

	// Process potions in inventory
	for (int i = 0; i < MAX_INVENTORY; ++i) {
		const auto& inv_item = sd->inventory.u.items_inventory[i];
		if (auto item_data = item_db.find(inv_item.nameid)) {
			if (item_data->type == IT_HEALING) { // check if item is type healing for potions
				if (unique_items.count(inv_item.nameid) > 0) {
					unique_items[inv_item.nameid] += " - x" + std::to_string(inv_item.amount); // append the amount
				}
				else {
					unique_items[inv_item.nameid] = buildPotionItemString("^FF0000(OFF)^000000", item_data->name, inv_item.amount);
					set_reg_num(st, sd, reference_uid(add_str(potion_menu_state), inv_item.nameid), potion_menu_state, 0, nullptr);
				}
			}
		}
	}

	for (const auto& uitem : unique_items) {
		if (first_done)
			menu << ":";
		else
			first_done = true;

		menu << uitem.second;
		set_reg_num(st, sd, reference_uid(add_str(potion_menu_ids), ++j), potion_menu_ids, uitem.first, nullptr);
	}

	set_reg_str(st, sd, reference_uid(add_str(potion_menu_list), 0), potion_menu_list, menu.str().c_str(), nullptr);

	return;
}

void handle_autobuff_follow(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p) {
	map_session_data* p_sd;
	int i;

	if (index == 0) {
		const char* follow_distance_value = ".@follow_distance_value";

		set_reg_num(st, sd, reference_uid(add_str(follow_distance_value), 0), follow_distance_value, sd->ab.dist_to_leader, nullptr);

		os_buf << "[Distance] - " << sd->ab.dist_to_leader << " cell(s) \n";

		if (sd->ab.following_player > 0) {
			if ((p_sd = map_charid2sd(sd->ab.following_player)) != nullptr)
				os_buf << "[Following] - " << p_sd->status.name;
			else {
				bool player_found = false;

				for (i = 0; i < MAX_PARTY; i++) {
					const auto& pmember = p->party.member[i];

					// No player
					if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
						continue;

					// Can't itself
					if (pmember.char_id == sd->status.char_id)
						continue;

					uint32 char_id = pmember.char_id;

					if (char_id == sd->ab.following_player) {
						os_buf << "[Following] - " << pmember.name;
						player_found = true;
						break;
					}
				}

				if (!player_found) {
					os_buf << "[Following] - You're not following anyone";
					sd->ab.following_player = 0;
				}

			}
		}
		else {
			os_buf << "[Following] - Nobody";
		}
	}
	else if (index == 1) {
		const char* player_follow_list = ".@player_follow_list$";
		const char* player_follow_ids = ".@player_follow_ids";
		const char* player_follow_state = ".@player_follow_state";
		bool first_done = false;
		std::ostringstream menu;
		int j = 0, nb_online = 0;
		std::map<int, std::string> unique_players;

		for (i = 0; i < MAX_PARTY; i++) {
			const auto& pmember = p->party.member[i];

			// Empty
			if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
				continue; // empty

			// Can't itself
			if (pmember.char_id == sd->status.char_id)
				continue;

			nb_online++;
			uint32 char_id = pmember.char_id;
			bool is_autobuff_followplayer = sd->ab.following_player > 0 && char_id == sd->ab.following_player;

			std::ostringstream player_status;

			if (is_autobuff_followplayer) {
				player_status << "^008000(ON)^000000 - ";
				set_reg_num(st, sd, reference_uid(add_str(player_follow_state), 0), player_follow_state, char_id, nullptr);
			}
			else {
				player_status << "^FF0000(OFF)^000000 - ";
			}

			player_status << pmember.name;
			player_status << ((!p->party.member[i].online) ? " - Offline" : " - Online");

			unique_players[char_id] = player_status.str();
		}

		if (nb_online == 0)
			os_buf << "You're the only player in the party \n";

		for (const auto& uplayer : unique_players) {
			if (first_done)
				menu << ":";
			else
				first_done = true;

			menu << uplayer.second;
			set_reg_num(st, sd, reference_uid(add_str(player_follow_ids), ++j), player_follow_ids, uplayer.first, nullptr);
		}

		set_reg_str(st, sd, reference_uid(add_str(player_follow_list), 0), player_follow_list, menu.str().c_str(), nullptr);
	}
}

void handle_autobuff_buff(std::ostringstream& os_buf, int index, int extra_index, script_state* st, TBL_PC* sd, struct party_data* p) {
	std::shared_ptr<s_skill_db> skill;

	const char* skill_menu_list = ".@skill_menu_list$";
	const char* skill_menu_ids = ".@skill_menu_ids";
	const char* skill_menu_state = ".@skill_menu_state";

	if (index == 0) { // Skill menu
		std::ostringstream menu;
		std::map<int, std::string> unique_skills = get_unique_skills(sd, p, skill_menu_state, st);
		format_skill_list(sd, menu, unique_skills, skill_menu_ids, skill_menu_state, st);
		set_reg_str(st, sd, reference_uid(add_str(skill_menu_list), 0), skill_menu_list, menu.str().c_str(), nullptr);
	}
	else if (index > 0 && extra_index == 1) { // Player list menu
		const char* player_buff_list = ".@player_buff_list$";
		const char* player_buff_ids = ".@player_buff_ids";
		const char* player_buff_state = ".@player_buff_state";

		s_autobuff_buffskills found_buff;
		if (find_autobuffbuff(sd, index, found_buff)) {
			os_buf << format_skill_buff_status(found_buff, skill_db.find(index));

			std::map<int, std::string> unique_players = get_unique_players(sd, found_buff, p, player_buff_state, st, os_buf);
			std::ostringstream menu;
			format_skill_list(sd, menu, unique_players, player_buff_ids, player_buff_state, st);

			set_reg_str(st, sd, reference_uid(add_str(player_buff_list), 0), player_buff_list, menu.str().c_str(), nullptr);
		}
		else {
			os_buf << "^FF0000(OFF)^000000 " << skill_db.find(index)->desc << " - No target chosen\n";
		}
	}
	else {
		os_buf << "^FF0000(" << skill_db.find(index)->nameid << ")^000000 " << skill_db.find(index)->desc;
	}

	return;
}

void handle_autobuff_party(std::ostringstream& os_buf, int party_id, int sd_following_player, script_state* st, TBL_PC* sd, struct party_data* p) {
	if (party_id == 0) {
		os_buf << "Autobuff - You aren't in a party anymore";
	}
	else {
		const char* player_follow_list = ".@player_follow_list$";
		//const char* player_follow_ids = ".@player_follow_ids";
		const char* player_follow_state = ".@player_follow_state";
		std::ostringstream menu;
		int j = 0, nb_online = 0;
		std::map<int, std::string> unique_players;

		for (int i = 0; i < MAX_PARTY; i++) {
			const auto& pmember = p->party.member[i];

			// Empty
			if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
				continue; // empty

			nb_online++;
			uint32 char_id = pmember.char_id;
			bool is_autobuff_followplayer = sd_following_player > 0 && char_id == sd_following_player;
			unique_players[char_id] = format_player_status(char_id, !p->party.member[i].online, is_autobuff_followplayer, pmember.name);
		}

		if (nb_online == 0)
			os_buf << "You're the only player in the party \n";

		show_player_list(menu, unique_players, player_follow_state, j, st, sd);
		set_reg_str(st, sd, reference_uid(add_str(player_follow_list), 0), player_follow_list, menu.str().c_str(), nullptr);
	}
}

void handle_autobuff_items(std::ostringstream& os_buf, int index, script_state* st, TBL_PC* sd) {
	const char* buffitem_menu_state = ".@buffitem_menu_state";
	auto item_data = item_db.find(index);

	if (item_data && item_data->type == IT_USABLE) {
		auto itAutobuffitem = std::find_if(sd->ab.autobuff_buffitems.begin(), sd->ab.autobuff_buffitems.end(),
			[index](const s_autobuff_buffitems& v) { return v.item_id == index; });

		bool is_autobuff_item = (itAutobuffitem != sd->ab.autobuff_buffitems.end());
		os_buf << (is_autobuff_item ? "^008000(ON)^000000 [" : "^FF0000(OFF)^000000 [")
			<< item_data->name.c_str() << "]";
		set_reg_num(st, sd, reference_uid(add_str(buffitem_menu_state), index), buffitem_menu_state, is_autobuff_item, nullptr);
	}
}

void handle_autobuff_pm(std::ostringstream& os_buf, script_state* st, TBL_PC* sd, struct party_data* p) {
	const char* player_pm_list = ".@player_pm_list$";
	const char* player_pm_ids = ".@player_pm_ids";
	const char* player_pm_state = ".@player_pm_state";
	bool first_done = false;
	std::ostringstream menu;
	int j = 0, nb_online = 0;
	std::map<int, std::string> unique_players;

	auto getPmCommandStatus = [](int pm_cmd) -> std::string {
		switch (pm_cmd) {
		case AB_PM_ALLOWPM: return "Anyone in the party can send me commands";
		case AB_PM_ALLOWPMFROMLEADER: return "Only the leader of the party can send me commands";
		case AB_PM_DISABLEPM: return "Nobody can send me commands";
		case AB_PM_NAMEDPLAYER: return "Only party's member can send me commands \n";
		default: return "Unknown PM command status";
		}
		};

	os_buf << getPmCommandStatus(sd->ab.allow_pm_cmd);

	if (sd->ab.allow_pm_cmd == AB_PM_NAMEDPLAYER) {
		for (int i = 0; i < MAX_PARTY; i++) {
			const auto& pmember = p->party.member[i];

			if (p->party.member[i].account_id == 0 || p->party.member[i].char_id == 0)
				continue; // empty

			// Can't itself
			if (pmember.char_id == sd->status.char_id)
				continue;

			nb_online++;
			uint32 char_id = pmember.char_id;

			std::ostringstream player_status;

			if (sd->ab.allow_pm_char_id.empty()) {
				player_status << "^FF0000(OFF)^000000 - ";
				player_status << pmember.name;
				player_status << ((!p->party.member[i].online) ? " - Offline" : " - Online");

				set_reg_num(st, sd, reference_uid(add_str(player_pm_state), char_id), player_pm_state, false, nullptr);
			}
			else {
				auto it = sd->ab.allow_pm_char_id.find(char_id);

				player_status << ((it != sd->ab.allow_pm_char_id.end()) ? "^008000(ON)^000000 - " : "^FF0000(OFF)^000000 - ");
				player_status << pmember.name;
				player_status << ((!p->party.member[i].online) ? " - Offline" : " - Online");

				set_reg_num(st, sd, reference_uid(add_str(player_pm_state), char_id), player_pm_state, (it != sd->ab.allow_pm_char_id.end()), nullptr);
			}
			unique_players[char_id] = player_status.str();
		}

		if (nb_online == 0)
			os_buf << "You're the only player in the party \n";

		for (const auto& uplayer : unique_players) {
			if (first_done)
				menu << ":";
			else
				first_done = true;

			menu << uplayer.second;
			set_reg_num(st, sd, reference_uid(add_str(player_pm_ids), ++j), player_pm_ids, uplayer.first, nullptr);
		}

		set_reg_str(st, sd, reference_uid(add_str(player_pm_list), 0), player_pm_list, menu.str().c_str(), nullptr);
	}
}

void handle_resurrection(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Ressurection] - " << (sd->ab.autobuff_resurection ? "Enabled" : "Disabled");
}

void handle_potions(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Potions] - " << (sd->ab.state_autobuff_potions ? "Enabled" : "Disabled");
}

void handle_token_of_siegfried(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Token of Siegfried] - " << (sd->ab.autobuff_token_siegfried ? "Enabled" : "Disabled");
}

void handle_return_to_savepoint(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Return to save point on death] - " << (sd->ab.return_to_savepoint ? "Enabled" : "Disabled");
}

void handle_priorize_buff(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Priorize buff over heal] - " << (sd->ab.priorize_buff ? "Enabled" : "Disabled");
}

void handle_party_config(std::ostringstream& os_buf, TBL_PC* sd) {
	os_buf << "[Party Config] - Allow the Autobuff when you're the only online member of the party: "
		<< (sd->ab.autobuff_disable_alone ? "Enabled" : "Disabled");
}

bool handleAutobuff_fromitem(TBL_PC* sd, t_itemid item_id, t_tick max_duration) {
	t_tick duration_ = 0;
	std::shared_ptr<item_data> id;

	switch (battle_config.feature_autobuff_duration_type) {
	case 0:
		uint32 item_expire_time;

		id = item_db.find(item_id);

		if (!id)
			return SCRIPT_CMD_FAILURE;

		item_expire_time = pc_getrental_search_inventory(sd, item_id);

		if (item_expire_time == 0) {
			ShowError("autobuff_fromitem: Item not found in the inventory or is not a consumed delay.\n", item_id);
			return SCRIPT_CMD_FAILURE;
		}

		duration_ = DIFF_TICK(item_expire_time, time(NULL));

		if (duration_ >= max_duration)
			duration_ = max_duration * 1000;
		else
			duration_ = duration_ * 1000;
		break;

	case 1:
		if (sd->ab.duration_ <= 0) {
			std::string msg = "Automessage - You don't have timer left on autoattack system!";
			ab_partymessage(sd, "TimerOut", msg.data(), 300);
		}
		else
			duration_ = sd->ab.duration_;
		break;
	}

	if(duration_ > 0)
		status_change_start(sd, sd, SC_AUTOBUFF, 10000, 0, 0, 0, 0, duration_, SCSTART_NOAVOID);

	return SCRIPT_CMD_SUCCESS;
}

bool handleAutobuff_start(TBL_PC* sd, t_tick duration_seconds) {
	t_tick duration_ = duration_seconds * 1000;

	if (duration_ > 0)
		status_change_start(sd, sd, SC_AUTOBUFF, 10000, 0, 0, 0, 0, duration_, SCSTART_NOAVOID);

	return SCRIPT_CMD_SUCCESS;
}

void handleAutoPotionPitcher(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p) {
	if (result.size() == 6) {
		bool is_active = static_cast<bool>(std::stoi(result.at(1)));
		uint16 skill_id = static_cast<uint16>(std::stoi(result.at(2)));
		uint16 skill_lv = static_cast<uint16>(std::stoi(result.at(3)));
		uint16 min_hp = static_cast<uint16>(std::stoi(result.at(4)));
		t_tick last_use = 1;
		ab_targettype priorize_heal = static_cast<ab_targettype>(std::stoi(result[5]));

		auto skill = skill_db.find(skill_id);

		if (!is_active) {
			sd->ab.autobuff_heal.erase(
				std::remove_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
					[skill_id, skill_lv](const s_autobuff_heal& v) {
						return v.skill_id == skill_id && v.skill_lv == skill_lv;
					}),
				sd->ab.autobuff_heal.end());
			return;
		}

		auto itAutoheal = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
			[skill_id, skill_lv](const s_autobuff_heal& v) { return v.skill_id == skill_id && v.skill_lv == skill_lv; });

		if (itAutoheal != sd->ab.autobuff_heal.end()) {
			itAutoheal->is_active = is_active;
			itAutoheal->skill_lv = skill_lv;
			itAutoheal->min_hp = min_hp;
			itAutoheal->priorize_heal = priorize_heal;
			if (priorize_heal != AB_NAMEDPLAYER)
				itAutoheal->priorize_heal_char_id.clear();
		}
		else {
			s_autobuff_heal autoheal{ is_active, skill_id, skill_lv, min_hp, last_use, priorize_heal };
			sd->ab.autobuff_heal.push_back(autoheal);
		}
	}
	else if (result.size() == 5) {
		int skill_id = std::stoi(result[1]);
		bool add_player = std::stoi(result[2]);
		uint32 player_id = static_cast<uint32>(std::stoi(result[3]));
		uint16 skill_lv = static_cast<uint16>(std::stoi(result.at(4)));

		auto itAutoheal = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
			[skill_id, skill_lv](const s_autobuff_heal& v) { return v.skill_id == skill_id && v.skill_lv == skill_lv; });
		if (itAutoheal != sd->ab.autobuff_heal.end()) {
			int player_pos;
			ARR_FIND(0, MAX_PARTY, player_pos, p->party.member[player_pos].char_id == player_id);
			if (player_pos < MAX_PARTY) {
				if (add_player)
					itAutoheal->priorize_heal_char_id.insert(player_id);
				else
					itAutoheal->priorize_heal_char_id.erase(player_id);
			}
		}
	}
}

void handleAutoHeal(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p) {
	if (result.size() == 6) {
		bool is_active = static_cast<bool>(std::stoi(result.at(1)));
		uint16 skill_id = static_cast<uint16>(std::stoi(result.at(2)));
		uint16 skill_lv = static_cast<uint16>(std::stoi(result.at(3)));
		uint16 min_hp = static_cast<uint16>(std::stoi(result.at(4)));
		t_tick last_use = 1;
		ab_targettype priorize_heal = static_cast<ab_targettype>(std::stoi(result[5]));

		auto skill = skill_db.find(skill_id);

		if (!is_active) {
			sd->ab.autobuff_heal.erase(
				std::remove_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
					[skill_id](const s_autobuff_heal& v) {
						return v.skill_id == skill_id;
					}),
				sd->ab.autobuff_heal.end());
			return;
		}

		auto itAutoheal = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
			[skill_id](const s_autobuff_heal& v) { return v.skill_id == skill_id; });

		if (itAutoheal != sd->ab.autobuff_heal.end()) {
			itAutoheal->is_active = is_active;
			itAutoheal->skill_lv = skill_lv;
			itAutoheal->min_hp = min_hp;
			itAutoheal->priorize_heal = priorize_heal;
			if (priorize_heal != AB_NAMEDPLAYER)
				itAutoheal->priorize_heal_char_id.clear();
		}
		else {
			s_autobuff_heal autoheal{ is_active, skill_id, skill_lv, min_hp, last_use, priorize_heal };
			sd->ab.autobuff_heal.push_back(autoheal);
		}
	}
	else if (result.size() == 4) {
		int skill_id = std::stoi(result[1]);
		bool add_player = std::stoi(result[2]);
		uint32 player_id = static_cast<uint32>(std::stoi(result[3]));

		auto itAutoheal = std::find_if(sd->ab.autobuff_heal.begin(), sd->ab.autobuff_heal.end(),
			[skill_id](const s_autobuff_heal& v) { return v.skill_id == skill_id; });
		if (itAutoheal != sd->ab.autobuff_heal.end()) {
			int player_pos;
			ARR_FIND(0, MAX_PARTY, player_pos, p->party.member[player_pos].char_id == player_id);
			if (player_pos < MAX_PARTY) {
				if (add_player)
					itAutoheal->priorize_heal_char_id.insert(player_id);
				else
					itAutoheal->priorize_heal_char_id.erase(player_id);
			}
		}
	}
}

void handleAutoPotion(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 5)
		return; // Invalid input

	bool is_active = static_cast<bool>(std::stoi(result.at(1)));
	t_itemid nameid = std::stoi(result.at(2));
	uint16 min_hp = static_cast<uint16>(std::stoi(result.at(3)));
	uint16 min_sp = static_cast<uint16>(std::stoi(result.at(4)));

	auto item_data = item_db.find(nameid);
	if (!item_data || item_data->type != IT_HEALING)
		return; // Item doesn't exist or isn't a healing item

	if (!is_active || (min_hp == 0 && min_sp == 0)) {
		sd->ab.autobuff_potions.erase(
			std::remove_if(sd->ab.autobuff_potions.begin(), sd->ab.autobuff_potions.end(), [nameid](const s_autobuff_potions& v) {
				return v.item_id == nameid;
				}),
			sd->ab.autobuff_potions.end()
		);
	}
	else {
		auto itAutopotion = std::find_if(sd->ab.autobuff_potions.begin(), sd->ab.autobuff_potions.end(), [nameid](const s_autobuff_potions& v) {
			return v.item_id == nameid;
			});

		if (itAutopotion != sd->ab.autobuff_potions.end()) {
			itAutopotion->is_active = is_active;
			itAutopotion->min_hp = min_hp;
			itAutopotion->min_sp = min_sp;
		}
		else {
			s_autobuff_potions autobuff_potions = { is_active, nameid, min_hp, min_sp };
			sd->ab.autobuff_potions.push_back(autobuff_potions);
		}
	}
}

void handleFollowPlayer(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	uint32 player_charid = std::stoi(result.at(1));
	if (player_charid != sd->status.char_id) {
		sd->ab.following_player = player_charid;
		sd->state.ab_stay = false;
	}
}

void handleAutoBuffSkills(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p) {
	if (result.size() == 5) {
		bool is_active = static_cast<bool>(std::stoi(result.at(1)));
		uint16 skill_id = static_cast<uint16>(std::stoi(result.at(2)));
		uint16 skill_lv = static_cast<uint16>(std::stoi(result.at(3)));
		ab_targettype priorize_buff = static_cast<ab_targettype>(std::stoi(result.at(4)));

		auto skill = skill_db.find(skill_id);
		if (!skill || !(skill->inf & (INF_SUPPORT_SKILL | INF_SELF_SKILL)) || skill_get_sc(skill->nameid) == 0)
			return; // Invalid skill or not a support/self skill

		if (!is_active) {
			sd->ab.autobuff_buffskills.erase(
				std::remove_if(sd->ab.autobuff_buffskills.begin(), sd->ab.autobuff_buffskills.end(), [skill_id](const s_autobuff_buffskills& v) {
					return v.skill_id == skill_id;
					}),
				sd->ab.autobuff_buffskills.end()
			);
		}
		else {
			auto itBuffSkill = std::find_if(sd->ab.autobuff_buffskills.begin(), sd->ab.autobuff_buffskills.end(), [skill_id](const s_autobuff_buffskills& v) {
				return v.skill_id == skill_id;
				});

			if (itBuffSkill != sd->ab.autobuff_buffskills.end()) {
				itBuffSkill->is_active = is_active;
				itBuffSkill->skill_lv = skill_lv;
				itBuffSkill->priorize_buff = priorize_buff;
				if (priorize_buff != AB_NAMEDPLAYER)
					itBuffSkill->priorize_buff_char_id.clear();
			}
			else {
				s_autobuff_buffskills buffskills = { is_active, skill_id, skill_lv, priorize_buff };
				sd->ab.autobuff_buffskills.push_back(buffskills);
			}
		}
	}
	else if (result.size() == 4) {
		int skill_id = std::stoi(result[1]);
		bool add_player = std::stoi(result[2]);
		uint32 player_id = static_cast<uint32>(std::stoi(result[3]));

		auto itBuffSkill = std::find_if(sd->ab.autobuff_buffskills.begin(), sd->ab.autobuff_buffskills.end(),
			[skill_id](const s_autobuff_buffskills& v) { return v.skill_id == skill_id; });
		if (itBuffSkill != sd->ab.autobuff_buffskills.end()) {
			int player_pos;
			ARR_FIND(0, MAX_PARTY, player_pos, p->party.member[player_pos].char_id == player_id);
			if (player_pos < MAX_PARTY) {
				if (add_player)
					itBuffSkill->priorize_buff_char_id.insert(player_id);
				else
					itBuffSkill->priorize_buff_char_id.erase(player_id);
			}
		}
	}
}

void handleDistanceToLeader(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	uint16 dist_to_leader = std::stoi(result.at(1));
	sd->ab.dist_to_leader = dist_to_leader;
}

void handleAutoBuffItems(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 4)
		return; // Invalid input

	bool is_active = static_cast<bool>(std::stoi(result.at(1)));
	t_itemid item_id = std::stoi(result.at(2));
	int item_status = std::stoi(result.at(3));

	if (item_status <= SC_NONE || item_status >= SC_MAX)
		return; // Invalid status

	auto item_data = item_db.find(item_id);
	if (!item_data || item_data->type != IT_USABLE)
		return; // Item doesn't exist or isn't usable

	if (!is_active) {
		sd->ab.autobuff_buffitems.erase(
			std::remove_if(sd->ab.autobuff_buffitems.begin(), sd->ab.autobuff_buffitems.end(), [item_id](const s_autobuff_buffitems& v) {
				return v.item_id == item_id;
				}),
			sd->ab.autobuff_buffitems.end()
		);
	}
	else {
		auto itBuffItem = std::find_if(sd->ab.autobuff_buffitems.begin(), sd->ab.autobuff_buffitems.end(), [item_id](const s_autobuff_buffitems& v) {
			return v.item_id == item_id;
			});

		if (itBuffItem != sd->ab.autobuff_buffitems.end()) {
			itBuffItem->is_active = is_active;
			itBuffItem->status = item_status;
		}
		else {
			s_autobuff_buffitems buffitems = { is_active, item_id, item_status };
			sd->ab.autobuff_buffitems.push_back(buffitems);
		}
	}
}

void handleAutoResurrection(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool autobuff_resurrection = std::stoi(result.at(1));
	sd->ab.autobuff_resurection = autobuff_resurrection;
}

void handlePMConfiguration(const std::vector<std::string>& result, TBL_PC* sd, struct party_data* p) {
	int player_pos = 0;

	if (result.size() != 2 && result.size() != 3)
		return; // Invalid input

	if (result.size() == 2) { // Handle pm configuration type
		ab_pmcmdconfig allow_pm_cmd = static_cast<ab_pmcmdconfig>(std::stoi(result.at(1)));
		sd->ab.allow_pm_cmd = allow_pm_cmd;

		if (allow_pm_cmd != AB_PM_NAMEDPLAYER && !sd->ab.allow_pm_char_id.empty())
			sd->ab.allow_pm_char_id.clear();
	}
	else if (result.size() == 3) { // Handle player addition/removal
		bool add_player = std::stoi(result.at(1));
		uint32 player_id = static_cast<uint32>(std::stoi(result.at(2)));

		ARR_FIND(0, MAX_PARTY, player_pos, p->party.member[player_pos].char_id == player_id);

		if (player_pos < MAX_PARTY) {
			if (add_player)
				sd->ab.allow_pm_char_id.insert(player_id);
			else
				sd->ab.allow_pm_char_id.erase(player_id);
		}
	}
}

void handleAutoBuffPotionState(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool state_autobuff_potions = std::stoi(result.at(1));
	sd->ab.state_autobuff_potions = state_autobuff_potions;
}

void handleReturnToSavepoint(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool return_to_savepoint = std::stoi(result.at(1));
	sd->ab.return_to_savepoint = return_to_savepoint;
}

void handleResetAutoBuffConfig(TBL_PC* sd) {
	sd->ab.following_player = 0;
	sd->ab.dist_to_leader = 2;
	sd->ab.autobuff_heal.clear();
	sd->ab.autobuff_buffskills.clear();
	sd->ab.autobuff_resurection = true;
	sd->ab.autobuff_buffitems.clear();
	sd->ab.autobuff_potions.clear();
	sd->ab.allow_pm_cmd = AB_PM_ALLOWPM;
	sd->ab.state_autobuff_potions = true;
	sd->ab.return_to_savepoint = false;
	sd->ab.autobuff_token_siegfried = false;
	sd->ab.autobuff_disable_alone = true;
	sd->ab.allow_pm_char_id.clear();
	sd->ab.party_msg.clear();
	sd->ab.order_msg.clear();
}

void handlePriorizeBuff(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool priorize_buff = std::stoi(result.at(1));
	sd->ab.priorize_buff = priorize_buff;
}


void handleTokenOfSiegfried(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool autobuff_token_siegfried = std::stoi(result.at(1));
	sd->ab.autobuff_token_siegfried = autobuff_token_siegfried;
}

void handleDisableWhenAlone(const std::vector<std::string>& result, TBL_PC* sd) {
	if (result.size() != 2)
		return; // Invalid input

	bool autobuff_disable_alone = std::stoi(result.at(1));
	sd->ab.autobuff_disable_alone = autobuff_disable_alone;
}

int handleGetautobuffint(TBL_PC* sd, const int value) {
	int result = 0;

	switch (value) {
	case 0:
		result = static_cast<int>(sd->ab.autobuff_heal.size());
		break;
	case 2:
		result = sd->ab.autobuff_resurection;
		break;
	case 3:
		result = static_cast<int>(sd->ab.autobuff_buffskills.size());
		break;
	case 4:
		result = sd->ab.state_autobuff_potions;
		break;
	case 5:
		result = static_cast<int>(sd->ab.autobuff_buffitems.size());
		break;
	case 6:
		result = sd->ab.return_to_savepoint;
		break;
	case 7:
		result = sd->ab.autobuff_token_siegfried;
		break;
	case 8:
		result = sd->ab.autobuff_disable_alone;
		break;
	case 11:
		result = sd->ab.priorize_buff;
		break;
	default:
		return result;
		break;
	}

	return result;
}