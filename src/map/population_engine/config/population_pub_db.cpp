// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
#include "population_pub_db.hpp"
#include "population_config.hpp"

#include <algorithm>

PopulationPubDatabase::PopulationPubDatabase()
	: TypesafeYamlDatabase("POPULATION_PUB_DB", 1, 1)
{
}

const std::string PopulationPubDatabase::getDefaultLocation()
{
	return population_config_db_path_pubs_yaml();
}

uint64 PopulationPubDatabase::parseBodyNode(const ryml::NodeRef& node)
{
	std::string map_name;
	if (!this->asString(node, "Map", map_name) || map_name.empty())
		return 0;
	std::string title;
	if (!this->asString(node, "Title", title) || title.empty())
		return 0;

	int32_t x = 0, y = 0, limit = 12;
	this->asInt32(node, "X", x);
	this->asInt32(node, "Y", y);
	if (this->nodeExists(node, "Limit"))
		this->asInt32(node, "Limit", limit);

	if (x <= 0 || y <= 0) {
		this->invalidWarning(node, "population_pubs.yml: pub \"%s\" needs positive X and Y.\n", title.c_str());
		return 0;
	}

	auto entry = std::make_shared<PopulationPubEntry>();
	entry->id       = this->next_id++;
	entry->map_name = map_name;
	entry->title    = title;
	entry->x        = static_cast<int16_t>(x);
	entry->y        = static_cast<int16_t>(y);
	entry->limit    = static_cast<uint8_t>(std::clamp(limit, 2, 20));
	this->put(entry->id, entry);
	return 1;
}
