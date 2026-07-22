// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef EQUIPMENT_SKIN_HPP
#define EQUIPMENT_SKIN_HPP

#include <common/cbasetypes.hpp>
#include <common/database.hpp>
#include <string>

struct equipment_skin_data {
    int itemid;
    std::string itemname;
    int skinid;
    std::string skinname;
    int type; // 0 = weapon, 1 = shield
};

class EquipmentSkinDatabase : public TypesafeYamlDatabase<int, equipment_skin_data> {
public:
    EquipmentSkinDatabase() : TypesafeYamlDatabase("EQUIPMENT_SKIN_DB", 1) {}

    const std::string getDefaultLocation() override;
    uint64 parseBodyNode(const ryml::NodeRef& node) override;

    // Helper methods
    equipment_skin_data* find_weapon_skin(int itemid);
    equipment_skin_data* find_shield_skin(int itemid);
};

extern EquipmentSkinDatabase equipment_skin_db;

void do_init_equipment_skin();
void do_final_equipment_skin();

#endif
