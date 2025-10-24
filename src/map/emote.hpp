/***********************************/
/***********    Shakto      ********/
/**    https://ronovelty.com/     **/
/***********************************/

#ifndef EMOTE_HPP
#define EMOTE_HPP

#include <map>
#include <vector>

#include "itemdb.hpp"

#include <common/database.hpp>
#include <common/db.hpp>
#include <common/mmo.hpp>

enum e_emotemessage_result : uint8_t{
	ZC_EMOTE_NOMONEY = 0,
	ZC_EMOTE_SALESENDED = 1,
	ZC_EMOTE_ALRDYPURCHASE = 2,
	ZC_EMOTE_ALRDYPURCHASE_TYPE = 3,
	ZC_EMOTE_BASICRREQUIREMENT = 4,
	ZC_EMOTE_CANTPURCHASEYET = 5,
	ZC_EMOTE_PURCHASEFAILED = 6,
};

enum e_emoteaddtobuylist_result : int8_t{
	ZC_EMOTE_BUYLIST_SALESENDED = 0,
	ZC_EMOTE_BUYLIST_CANTUSENOTPURCHASED = 1,
	ZC_EMOTE_BUYLIST_CANTUSENOBASIC = 2,
	ZC_EMOTE_BUYLIST_UNKNOWNERROR = 3,
};

struct s_emote_db {
	uint32 id;
	std::string name;

	time_t startTime;
	time_t endTime;
	uint32 rentalHours;
	uint8 type;
	bool keepinshop;

	int emote_timer;

	std::unordered_map<t_itemid, uint16> materials;
};

class EmoteDatabase : public TypesafeYamlDatabase<uint32, s_emote_db> {
public:
	EmoteDatabase() : TypesafeYamlDatabase("EMOTE_DB", 1) {

	}

	const std::string getDefaultLocation() override;
	uint64 parseBodyNode(const ryml::NodeRef &node) override;
};

extern EmoteDatabase emote_db;

TIMER_FUNC (emote_start_timeout);
void emote_save(map_session_data* sd);
void emote_load(map_session_data* sd);

void emote_db_reload();
void do_init_emote();
void do_final_emote();

#endif /* EMOTE_HPP */