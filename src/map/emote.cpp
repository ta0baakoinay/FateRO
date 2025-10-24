/***********************************/
/***********    Shakto      ********/
/**    https://ronovelty.com/     **/
/***********************************/

#include "emote.hpp"

#include <stdlib.h>
#include <iostream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>

#include <common/cbasetypes.hpp>
#include <common/database.hpp>
#include <common/db.hpp>
#include <common/ers.hpp>
#include <common/nullpo.hpp>
#include <common/random.hpp>
#include <common/showmsg.hpp>
#include <common/strlib.hpp>

#include "clif.hpp"
#include "itemdb.hpp"
#include "log.hpp"
#include "pc.hpp"

using namespace rathena;

EmoteDatabase emote_db;

/**
 * Get location of emote database
 * @author [Shakto]
 **/
const std::string EmoteDatabase::getDefaultLocation() {
	return std::string(db_path) + "/emote_db.yml";
}

/**
 * Read emote YML db
 * @author [Shakto]
 **/
uint64 EmoteDatabase::parseBodyNode(const ryml::NodeRef& node) {
	uint32 id;

	if (!this->asUInt32(node, "Id", id))
		return 0;

	std::shared_ptr<s_emote_db> emote = this->find(id);
	bool exists = emote != nullptr;

	if (!exists) {
		if (!this->nodesExist(node, { "Name", "Type" }))
			return 0;

		emote = std::make_shared<s_emote_db>();
		emote->id = id;
	}

	if (this->nodeExists(node, "Name")) {
		std::string name;

		if (!this->asString(node, "Name", name))
			return 0;

		emote->name = name;
	}
	else
		return 0;

	//ShowError("Emote id %d, name %s \n",emote->id,emote->name.c_str());

	if (this->nodeExists(node, "RentalHours")) {
		uint32 rentalHours;

		if (!this->asUInt32(node, "RentalHours", rentalHours))
			return 0;

		emote->rentalHours = rentalHours;
	}
	else
		emote->rentalHours = 0;

	if (this->nodeExists(node, "Starttime")) {
		std::string startTimeStr;

		if (!this->asString(node, "Starttime", startTimeStr))
			return 0;

		std::istringstream inputStream(startTimeStr);
		std::tm startTimeTm = {};

		inputStream >> std::get_time(&startTimeTm, "%Y-%m-%d");

		if (!inputStream.fail()) {
			time_t startTime = std::mktime(&startTimeTm);
			emote->startTime = startTime;
		}
		else {
			this->invalidWarning(node["Starttime"], "Fail to parse startdate %s.\n", startTimeStr.c_str());
			return 0;
		}

		//ShowError("Starttime : %ld \n",emote->startTime);
	}

	if (this->nodeExists(node, "Endtime")) {
		std::string endTimeStr;

		if (!this->asString(node, "Endtime", endTimeStr))
			return 0;

		std::istringstream inputStream(endTimeStr);
		std::tm endTimeTm = {};

		inputStream >> std::get_time(&endTimeTm, "%Y-%m-%d");

		if (!inputStream.fail()) {
			time_t endTime = std::mktime(&endTimeTm);
			emote->endTime = endTime;
		}
		else {
			this->invalidWarning(node["Endtime"], "Fail to parse startdate %s.\n", endTimeStr.c_str());
			return 0;
		}
		//ShowError("endTime : %ld \n",emote->endTime);
	}

	if (this->nodeExists(node, "KeepInShop")) {

		bool keepinshop;

		if (!this->asBool(node, "KeepInShop", keepinshop))
			return 0;

		emote->keepinshop = keepinshop;

	}
	else
		emote->keepinshop = false;

	uint16 type;

	if (!this->asUInt16(node, "Type", type))
		return 0;

	emote->type = static_cast<uint8>(type);
	//ShowError("Type %d \n",type);

	if (this->nodeExists(node, "Prices")) {
		for (const ryml::NodeRef& materialNode : node["Prices"]) {
			std::string item_name;

			if (!this->asString(materialNode, "Material", item_name))
				return false;

			std::shared_ptr<item_data> item = item_db.search_aegisname(item_name.c_str());

			if (item == nullptr) {
				this->invalidWarning(materialNode["Materials"], "Emote %d item %s does not exist, skipping.\n", emote->id, item_name.c_str());
				continue;
			}

			t_itemid material_id = item->nameid;
			bool material_exists = util::umap_find(emote->materials, material_id) != nullptr;
			uint16 amount;

			if (this->nodeExists(materialNode, "Amount")) {

				if (!this->asUInt16(materialNode, "Amount", amount))
					return 0;

				if (amount > MAX_AMOUNT) {
					this->invalidWarning(materialNode["Amount"], "Amount %hu is too high, capping to MAX_AMOUNT...\n", amount);
					amount = MAX_AMOUNT;
				}
			}
			else {
				if (!material_exists)
					amount = 1;
			}

			if (amount > 0)
				emote->materials[material_id] = amount;
			else
				emote->materials.erase(material_id);

			//ShowError("Prices item id %d amount %d \n",item->nameid,amount);
		}
	}

	if (emote->startTime > time(NULL))
		emote->emote_timer = add_timer(gettick() + 60000, emote_start_timeout, emote->id, 0);

	if (!exists)
		this->put(emote->id, emote);

	return 1;
}

void emote_save(map_session_data* sd) {
	for (const auto& emote_data : sd->emotes) {
		//ShowError("Save emote pack %d, expire %d, type %d \n",emote_data.id,emote_data.expire_time,emote_data.type);

		if (emote_data.expire_time && time(NULL) > emote_data.expire_time)
			continue;

		switch (emote_data.type) {
		case 1:
			// account emote pack
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT IGNORE INTO `account_emote` (`account_id`,`pack_id`,`expire_time`) "
				"VALUES (%u, %u, %u) "
				"ON DUPLICATE KEY UPDATE "
				"`expire_time` = %u ",
				sd->status.account_id, // account_id
				emote_data.id,
				emote_data.expire_time,
				emote_data.expire_time
			)) {
				Sql_ShowDebug(mmysql_handle);
			}
			break;
		case 2:
			// char emote pack
			if (SQL_ERROR == Sql_Query(mmysql_handle,
				"INSERT IGNORE INTO `char_emote` (`char_id`,`pack_id`,`expire_time`) "
				"VALUES (%u, %u, %u) "
				"ON DUPLICATE KEY UPDATE "
				"`expire_time` = %u ",
				sd->status.char_id, // char_id
				emote_data.id,
				emote_data.expire_time,
				emote_data.expire_time
			)) {
				Sql_ShowDebug(mmysql_handle);
			}
			break;
		}
	}
}

void emote_load(map_session_data* sd) {

	if (!sd->emotes.empty())
		sd->emotes.clear();

	if (Sql_Query(mmysql_handle,
		"DELETE "
		"FROM `char_emote` "
		"WHERE `char_id` = %d and `expire_time` < %ld and `expire_time` > 0",
		sd->status.char_id, time(NULL)) != SQL_SUCCESS)
	{
		Sql_ShowDebug(mmysql_handle);
	}
	Sql_FreeResult(mmysql_handle);

	if (Sql_Query(mmysql_handle,
		"DELETE "
		"FROM `account_emote` "
		"WHERE `account_id` = %d and `expire_time` < %ld and `expire_time` > 0",
		sd->status.account_id, time(NULL)) != SQL_SUCCESS)
	{
		Sql_ShowDebug(mmysql_handle);
	}
	Sql_FreeResult(mmysql_handle);

	if (Sql_Query(mmysql_handle,
		"SELECT `pack_id`, `expire_time` "
		"FROM `char_emote` "
		"WHERE `char_id` = %d",
		sd->status.char_id) != SQL_SUCCESS)
	{
		Sql_ShowDebug(mmysql_handle);
	}

	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		s_emote_data emote;
		char* data;
		Sql_GetData(mmysql_handle, 0, &data, NULL); emote.id = atoi(data);
		Sql_GetData(mmysql_handle, 1, &data, NULL); emote.expire_time = atoi(data);
		emote.type = 2;

		sd->emotes.push_back(emote);

		//ShowError("Load char emote pack %d, expire %d, type %d \n",emote.id,emote.expire_time,emote.type);
	}
	Sql_FreeResult(mmysql_handle);

	if (Sql_Query(mmysql_handle,
		"SELECT `pack_id`, `expire_time` "
		"FROM `account_emote` "
		"WHERE `account_id` = %d",
		sd->status.account_id) != SQL_SUCCESS)
	{
		Sql_ShowDebug(mmysql_handle);
	}

	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		s_emote_data emote;
		char* data;
		Sql_GetData(mmysql_handle, 0, &data, NULL); emote.id = atoi(data);
		Sql_GetData(mmysql_handle, 1, &data, NULL); emote.expire_time = atoi(data);
		emote.type = 1;

		sd->emotes.push_back(emote);

		//ShowError("Load account emote pack %d, expire %d, type %d \n",emote.id,emote.expire_time,emote.type);
	}
	Sql_FreeResult(mmysql_handle);

	clif_list_emote(sd);

}

TIMER_FUNC(emote_start_timeout) {
	std::shared_ptr<s_emote_db> emote = emote_db.find(id);

	if (emote == nullptr) {
		return 0;
	}

	if (emote->emote_timer != tid) {
		//ShowError("emote_start_timeout timer %d != %d\n",emote->emote_timer,tid);
		emote->emote_timer = INVALID_TIMER;
		return 0;
	}

	// Remove the current timer
	emote->emote_timer = INVALID_TIMER;

	//ShowError("emote_start_timeout, startTime %ld time %ld \n",emote->startTime, time(NULL));
	if (emote->startTime > time(NULL))
		emote->emote_timer = add_timer(gettick() + 60000, emote_start_timeout, emote->id, 0);
	else
		map_foreachpc(clif_addtobuylist_emote_sub, id);

	return 0;
}

void do_init_emote() {
	emote_db.load();
	add_timer_func_list(emote_start_timeout, "emote_start_timeout");
}

void do_final_emote() {
	emote_db.clear();
}