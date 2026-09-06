// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// Named pub / social-room definitions for population Social shells
// (db/population_pubs.yml). Access via population_pub_db().
#pragma once

#include <cstdint>
#include <string>

#include <common/database.hpp>

/// One named pub. A Social town shell walks here, and the first one to arrive
/// opens a real public chatroom (rAthena's Alt+C mechanism, chat_createpcchat)
/// with this Title; the rest join it (chat_joinchat) and sit.
struct PopulationPubEntry {
	uint32_t    id = 0;        ///< Auto-assigned load order id (registry key).
	std::string map_name;      ///< Map the pub lives on.
	std::string title;         ///< Chatroom title (the "pub name").
	int16_t     x = 0;
	int16_t     y = 0;
	uint8_t     limit = 12;    ///< Max members (owner + joiners), 2..20.
};

class PopulationPubDatabase : public TypesafeYamlDatabase<uint32, PopulationPubEntry> {
public:
	PopulationPubDatabase();
	const std::string getDefaultLocation() override;
	uint64 parseBodyNode(const ryml::NodeRef& node) override;
private:
	uint32 next_id = 1;
};
