// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "equipment_skin.hpp"

#include <common/showmsg.hpp>

#include "itemdb.hpp"

EquipmentSkinDatabase equipment_skin_db;

const std::string EquipmentSkinDatabase::getDefaultLocation() {
    return std::string(db_path) + "/equipment_skin.yml";
}

uint64 EquipmentSkinDatabase::parseBodyNode(const ryml::NodeRef& node) {
    int itemid;

    // Parse ItemID (numeric or Aegis name)
    if (this->nodeExists(node, "ItemID")) {
        if (node["ItemID"].is_seed()) {
            if (!this->asInt32(node, "ItemID", itemid))
                return 0;
        } else {
            std::string aegis_name;
            if (!this->asString(node, "ItemID", aegis_name))
                return 0;

            std::shared_ptr<item_data> item = item_db.search_aegisname(aegis_name.c_str());
            if (!item) {
                this->invalidWarning(node["ItemID"], "Unknown ItemID '%s', skipping entry.\n", aegis_name.c_str());
                return 0;
            }
            itemid = item->nameid;
        }
    } else {
        this->invalidWarning(node, "Missing ItemID, skipping entry.\n");
        return 0;
    }

    std::shared_ptr<equipment_skin_data> skin = this->find(itemid);
    bool exists = skin != nullptr;

    if (!exists) {
        skin = std::make_shared<equipment_skin_data>();
        skin->itemid = itemid;
    }

    // Parse ItemName (optional)
    if (this->nodeExists(node, "ItemName")) {
        std::string name;
        if (this->asString(node, "ItemName", name))
            skin->itemname = name;
    }

    // Parse SkinID (numeric or Aegis name)
    if (this->nodeExists(node, "SkinID")) {
        if (node["SkinID"].is_seed()) {
            if (!this->asInt32(node, "SkinID", skin->skinid))
                return 0;
        } else {
            std::string aegis_name;
            if (!this->asString(node, "SkinID", aegis_name))
                return 0;

            std::shared_ptr<item_data> item = item_db.search_aegisname(aegis_name.c_str());
            if (!item) {
                this->invalidWarning(node["SkinID"], "Unknown SkinID '%s', skipping entry.\n", aegis_name.c_str());
                return 0;
            }
            skin->skinid = item->nameid;
        }
    } else {
        this->invalidWarning(node, "Missing SkinID, skipping entry.\n");
        return 0;
    }

    // Parse SkinName (optional)
    if (this->nodeExists(node, "SkinName")) {
        std::string name;
        if (this->asString(node, "SkinName", name))
            skin->skinname = name;
    }

    // Determine type (weapon or shield) - you'll need to add Type field to YAML
    if (this->nodeExists(node, "Type")) {
        std::string type_str;
        if (this->asString(node, "Type", type_str)) {
            if (type_str == "LOOK_WEAPON")
                skin->type = 0;
            else if (type_str == "LOOK_SHIELD")
                skin->type = 1;
            else {
                this->invalidWarning(node["Type"], "Invalid Type '%s', defaulting to LOOK_WEAPON.\n", type_str.c_str());
                skin->type = 0;
            }
        }
    } else {
        skin->type = 0; // Default to weapon
    }

    if (!exists)
        this->put(itemid, skin);

    return 1;
}

equipment_skin_data* EquipmentSkinDatabase::find_weapon_skin(int itemid) {
    auto skin = this->find(itemid);
    if (skin && skin->type == 0)
        return skin.get();
    return nullptr;
}

equipment_skin_data* EquipmentSkinDatabase::find_shield_skin(int itemid) {
    auto skin = this->find(itemid);
    if (skin && skin->type == 1)
        return skin.get();
    return nullptr;
}

void do_init_equipment_skin() {
    equipment_skin_db.load();
}

void do_final_equipment_skin() {
    equipment_skin_db.clear();
}
