#ifndef AUTOBUFF_H
#define AUTOBUFF_H

#include <deque>
#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <iostream>
#include <set>
#include <sstream>

#include <common/mmo.hpp> // item_id
#include <common/showmsg.hpp>
#include <common/database.hpp>
#include <map/status.hpp>  // ADD THIS LINE - defines sc_type

constexpr auto AB_PREFIX_NAME = "[AUTO]";
const std::vector<int> AB_HATEFFECTS = { 97, 131, 123, 31, 3 };

class map_session_data;
struct block_list;

enum ab_targettype : uint16 {
	AB_SELF = 0,
	AB_LEADER,
	AB_NAMEDPLAYER,
	AB_EVERYONEEXCEPTSELF,
	AB_ALL
};

enum ab_buffconfig : uint16 {
	AB_INBATTLE = 0,
	AB_NOTINBATTLE,
	AB_EVERYTIME
};

enum ab_pmcmdconfig : uint16 {
	AB_PM_ALLOWPM = 0,
	AB_PM_ALLOWPMFROMLEADER,
	AB_PM_DISABLEPM,
	AB_PM_NAMEDPLAYER
};

struct s_autobuff_buffskills {
	bool is_active;
	uint16 skill_id;
	uint16 skill_lv;
	t_tick last_use;
	ab_targettype priorize_buff; // priorize heal for support, others party member, leader, named players (need to be in party)
	std::set<uint32> priorize_buff_char_id; // list of player to buff

	s_autobuff_buffskills()
		: is_active(true), skill_id(0), skill_lv(0), last_use(0), priorize_buff(ab_targettype::AB_ALL) {
	}

	// Constructor personalized
	s_autobuff_buffskills(bool active, uint16 id, uint16 lv, ab_targettype prioritize)
		: is_active(active), skill_id(id), skill_lv(lv), last_use(0), priorize_buff(prioritize) {
	}
};

struct s_autobuff_heal {
	bool is_active;
	uint16 skill_id;
	uint16 skill_lv;
	uint16 min_hp;
	t_tick last_use;
	ab_targettype priorize_heal; // priorize heal for support, others party member, leader, named player (need to be in party)
	std::set<uint32> priorize_heal_char_id; // list of player to heal

	s_autobuff_heal()
		: is_active(true), skill_id(0), skill_lv(0), min_hp(0), last_use(0), priorize_heal(ab_targettype::AB_ALL) {
	}

	// Constructor personalized
	s_autobuff_heal(bool active, uint16 id, uint16 lv, uint16 hp, t_tick use, ab_targettype prioritize)
		: is_active(active), skill_id(id), skill_lv(lv), min_hp(hp), last_use(use), priorize_heal(prioritize) {
	}
};

struct s_autobuff_buffitems {
	bool is_active;
	t_itemid item_id;
	int status;
};

struct s_autobuff_potions {
	bool is_active;
	t_itemid item_id;
	uint16 min_hp;
	uint16 min_sp;
};

struct s_autobuff {
	s_autobuff()
		: following_player(0),
		dist_to_leader(2),
		autobuff_heal{},        // Initialise à un vecteur vide
		autobuff_buffskills{},  // Initialise à un vecteur vide
		autobuff_resurection(false),
		autobuff_buffitems{},   // Initialise à un vecteur vide
		autobuff_potions{},     // Initialise à un vecteur vide
		skill_cd(0),
		allow_pm_cmd(AB_PM_ALLOWPM),
		state_autobuff_potions(false),
		return_to_savepoint(false),
		autobuff_token_siegfried(false),
		autobuff_disable_alone(false),
		allow_pm_char_id{},     // Initialise à un set vide
		party_msg{},            // Initialise à une map vide
		order_msg{},             // Initialise à une deque vide
		loadtimer(0),
		priorize_buff(0)
	{}

	uint32 following_player;
	uint16 dist_to_leader;
	std::vector<s_autobuff_heal> autobuff_heal;
	std::vector<s_autobuff_buffskills> autobuff_buffskills;
	bool autobuff_resurection;
	std::vector<s_autobuff_buffitems> autobuff_buffitems;
	std::vector<s_autobuff_potions> autobuff_potions;
	t_tick skill_cd;
	ab_pmcmdconfig allow_pm_cmd;
	bool state_autobuff_potions;
	bool return_to_savepoint;
	bool autobuff_token_siegfried;
	bool autobuff_disable_alone;
	std::set<uint32> allow_pm_char_id;
	std::vector<std::pair<std::string, t_tick>> party_msg;
	std::deque<std::pair<std::string, map_session_data*>> order_msg;
	int loadtimer;
	t_tick duration_;
	bool priorize_buff;
	unsigned int unique_id;
	uint32 client_addr; // remote client address
};

// status
int ab_status(map_session_data* sd, enum sc_type type);
void ab_token_respawn(block_list* target, int flag);
bool ab_walk(struct block_list* bl, short x, short y, unsigned char flag);
bool ab_partymessage(map_session_data* sd, std::string key, char* message, int delay);
bool ab_canuseskill(map_session_data* sd, uint16 skill_id, uint16 skill_lv);
int ab_targetresu(struct block_list* bl, va_list ap);
// timer functions
int ab_delayed_restore_timer(int tid, int64 tick, int id, intptr_t data);
//chrif
void ab_save(map_session_data* sd);

// pc
void ab_load(map_session_data* sd);

class ab_commandHandler {
public:
	// Constructor
	ab_commandHandler();

	// Autobuff command msg functions
	bool ab_clear_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_stop_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_start_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_stay_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_move_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_sit_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_stand_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_logout_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_go_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	int ab_buff_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_follow_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);
	bool ab_warp_command(map_session_data* sd, map_session_data* ssd, std::vector<std::string> cmd);

	std::unordered_map<std::string, std::function<bool(map_session_data*, map_session_data*, std::vector<std::string>)>> command_map;
};

void auto_heal_action(bool& skip, map_session_data* sd, map_session_data* leader_sd, struct party_data* p, int leader_pos);
void auto_buff_action(bool& skip, map_session_data* sd, map_session_data* leader_sd, struct party_data* p, int leader_pos);

bool ab_changestate_autobuff(map_session_data* sd, int flag);

#endif // AUTOBUFF_H