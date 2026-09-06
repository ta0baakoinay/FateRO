#ifndef	AUTOCOMBAT_HPP
#define	AUTOCOMBAT_HPP

#include <stack>
#include <common/cbasetypes.hpp>

#include "skill.hpp"
#include "status.hpp"

#define AUTOCOMBAT_DEFAULTNEXTTICK 250 // in milliseconds, must not be above 1000
#define AUTOCOMBAT_TARGETRANGE 14
#define AUTOCOMBAT_SIT_MAX_HPSP 80
#define AUTOCOMBAT_E_TELEPORT_MINHP 5
#define AUTOCOMBAT_E_TELEPORT_MAXHP 90
#define AUTOCOMBAT_MAX_ASTAR_SEARCH 5 // max search retries before teleporting


struct s_autocombat_skill_delay {
    uint16 skill_id;
    int delay;
};

extern std::vector<s_autocombat_skill_delay> autocombat_custom_delays;


/**
 * AUTOCOMBAT_LOOTING_CONFIG <param>
 * @param 0 `Do not loot at all` - good for server that has @autoloot or @alootid
 * @param 1 `Default config` - character walk up to the loot, and loot normally
 * @param 2 `Autoloot to inventory` - servers that DONT have @autoloot nor @alootid but want autoloot for Auto Combat
 */
#define AUTOCOMBAT_LOOTING_CONFIG 1

/**
 * AUTOCOMBAT_DURATION_CONFIG <param>
 * @param 0 `24/7` - Auto combat can be used all the time
 * @param 1 `Per character` - duration is based on character variable
 * @param 2 `Account wide` - duration is based on account variable
 * @param 3 `Per Gepard Unique ID` - not implemented, extra modules required. pls pm me if you want this
 * @param <item_id> `Item Based` - duration is based on rental item
 */
#define AUTOCOMBAT_DURATION_CONFIG 0

struct s_buff_items {
	int item_id;
	enum sc_type status;
	enum e_special_effects effect;
	int tick;
	int val1;
	int val2;
	int val3;
	int val4;
};

const struct s_buff_items buff_items[] = {
	// ===== BUFF ITEMS SETUP ==========================================================
	// Add/remove items that are allowed to be used. 31 max items
	// Put 0 zero for <effect_type> if you want nothing to show
	// { <item_id>, <status type>, <effect_type>, <tick>, <val1>, <val2>, <val3>, <val4> },
	{ 645, SC_ASPDPOTION0, EF_POTION_CON, 1800000, 0, 0, 0, 0 },          // Concentration Potion
	{ 656, SC_ASPDPOTION1, EF_POTION_, 1800000, 0, 0, 0, 0 },             // Awakening Potion
	{ 657, SC_ASPDPOTION2, EF_POTION_BERSERK, 1800000, 0, 0, 0, 0 },      // Berserk Potion
	{ 14509, SC_ASPDPOTION0, EF_POTION_CON, 1800000, 0, 0, 0, 0 },        // Light Concentration Potion
	{ 14510, SC_ASPDPOTION1, EF_POTION_, 1800000, 0, 0, 0, 0 },           // Light Awakening Potion
	{ 14511, SC_ASPDPOTION2, EF_POTION_BERSERK, 1800000, 0, 0, 0, 0 },    // Light Berserk Potion
	{ 12215, SC_BLESSING, EF_BLESSING, 240000, 10, 0, 0, 0 },             // LV10 Blessing Scroll
	{ 12216, SC_INCREASEAGI, EF_INCAGILITY, 240000, 10, 0, 0, 0 },        // LV10 Agil Scroll
	{ 12217, SC_ASPERSIO, EF_ASPERSIO, 180000, 5, 0, 0, 0 },              // LV5 Aspersio Scroll
	{ 12218, SC_ASSUMPTIO, EF_ASSUMPTIO2, 100000, 5, 0, 0, 0 },           // LV5 Assumptio Scroll
};

static const uint16 heal_skill_id[] = {
	AL_HEAL, AM_POTIONPITCHER, PF_HPCONVERSION
};

static const uint16 attack_skills[] = {
	SM_BASH, SM_PROVOKE, SM_MAGNUM, // Swordsman
	KN_PIERCE, KN_BRANDISHSPEAR, KN_SPEARSTAB, KN_SPEARBOOMERANG, KN_AUTOCOUNTER, KN_BOWLINGBASH, KN_CHARGEATK, LK_SPIRALPIERCE, LK_HEADCRUSH, LK_JOINTBEAT,
	CR_SHIELDCHARGE, CR_SHIELDBOOMERANG, CR_HOLYCROSS, CR_GRANDCROSS, PA_PRESSURE, PA_SACRIFICE, PA_SHIELDCHAIN,
	MG_NAPALMBEAT, MG_SOULSTRIKE, MG_COLDBOLT, MG_FROSTDIVER, MG_STONECURSE, MG_FIREBALL, MG_FIREWALL, MG_FIREBOLT, MG_LIGHTNINGBOLT, MG_THUNDERSTORM, // Mage
	WZ_FIREPILLAR, WZ_SIGHTRASHER, WZ_METEOR, WZ_JUPITEL, WZ_VERMILION, WZ_WATERBALL, WZ_FROSTNOVA, WZ_STORMGUST, WZ_EARTHSPIKE, WZ_HEAVENDRIVE, WZ_QUAGMIRE, WZ_SIGHTBLASTER,
	HW_GANBANTEIN, HW_GRAVITATION, HW_SOULDRAIN, HW_MAGICCRASHER,
	AC_DOUBLE, AC_SHOWER, AC_CHARGEARROW, HT_BLITZBEAT, HT_POWER, SN_FALCONASSAULT, SN_SHARPSHOOTING, HT_PHANTASMIC, // Archer
	BA_MUSICALSTRIKE, DC_THROWARROW, CG_ARROWVULCAN,
	AL_HEAL, AL_HOLYLIGHT, PR_TURNUNDEAD, PR_MAGNUS, // Acolyte
	MO_INVESTIGATE, MO_FINGEROFFENSIVE, MO_EXTREMITYFIST, MO_CHAINCOMBO, MO_BALKYOUNG, MO_COMBOFINISH, CH_PALMSTRIKE, CH_TIGERFIST, CH_CHAINCRUSH,
	MC_MAMMONITE, MC_CARTREVOLUTION, AM_DEMONSTRATION, AM_ACIDTERROR, AM_SPHEREMINE, CR_ACIDDEMONSTRATION, WS_CARTTERMINATION, // Merchant
	TF_POISON, AS_SONICBLOW, TF_SPRINKLESAND, TF_THROWSTONE, AS_VENOMDUST, AS_SPLASHER, ASC_BREAKER, ASC_METEORASSAULT, AS_VENOMKNIFE, // Thief
	RG_BACKSTAP, RG_RAID, RG_INTIMIDATE,
	GS_FLING, GS_TRIPLEACTION, GS_BULLSEYE, GS_TRACKING, GS_PIERCINGSHOT, GS_RAPIDSHOWER, // Gunslinger
	GS_DESPERADO, GS_GATLINGFEVER, GS_DUST, GS_FULLBUSTER, GS_SPREADATTACK, GS_GROUNDDRIFT,
	NJ_SYURIKEN, NJ_KUNAI, NJ_HUUMA, NJ_ZENYNAGE, NJ_TATAMIGAESHI, NJ_KASUMIKIRI, NJ_KIRIKAGE, NJ_KOUENKA, NJ_KAENSIN, // Ninja
	NJ_BAKUENRYU, NJ_HYOUSENSOU, NJ_SUITON, NJ_HYOUSYOURAKU, NJ_HUUJIN, NJ_RAIGEKISAI, NJ_KAMAITACHI, NJ_ISSEN,
	TK_JUMPKICK, // Taekwon
	SL_STIN, SL_STUN, SL_SMA, SL_SWOO, SL_SKE, SL_SKA,
};

static const uint16 buff_skills[] = {
	SM_ENDURE, SM_AUTOBERSERK, KN_TWOHANDQUICKEN, KN_ONEHAND, LK_AURABLADE, LK_PARRYING, LK_CONCENTRATION, LK_BERSERK, // Swordsman
	CR_AUTOGUARD, CR_REFLECTSHIELD, CR_PROVIDENCE, CR_DEFENDER, CR_SPEARQUICKEN, CR_SHRINK, PA_SACRIFICE,
	MG_SIGHT, MG_ENERGYCOAT, HW_MAGICPOWER, SA_AUTOSPELL, PF_DOUBLECASTING, PF_MEMORIZE, // Mage
	SA_FLAMELAUNCHER, SA_FROSTWEAPON, SA_LIGHTNINGLOADER, SA_SEISMICWEAPON,
	AC_CONCENTRATION, SN_WINDWALK, SN_SIGHT, BA_DISSONANCE, BA_WHISTLE, BA_ASSASSINCROSS, BA_POEMBRAGI, BA_APPLEIDUN, // Archer
	DC_UGLYDANCE, DC_HUMMING, DC_DONTFORGETME, DC_FORTUNEKISS, DC_SERVICEFORYOU,
	AL_RUWACH, AL_INCAGI, AL_ANGELUS, AL_BLESSING, PR_IMPOSITIO, PR_SUFFRAGIUM, PR_ASPERSIO, PR_KYRIE, PR_MAGNIFICAT, PR_GLORIA, HP_ASSUMPTIO, // Acolyte
	MO_CALLSPIRITS, MO_EXPLOSIONSPIRITS, CH_SOULCOLLECT,
	MC_LOUD, BS_ADRENALINE, BS_WEAPONPERFECT, BS_OVERTHRUST, BS_MAXIMIZE, WS_CARTBOOST, WS_MELTDOWN, WS_OVERTHRUSTMAX,// Merchant
	AS_ENCHANTPOISON, AS_POISONREACT, ASC_EDP, ST_PRESERVE, ST_REJECTSWORD, // Thief
	GS_GLITTERING, GS_MADNESSCANCEL, GS_ADJUSTMENT, GS_INCREASING, // Gunslinger
	NJ_BUNSINJYUTSU, NJ_UTSUSEMI, // Ninja
	TK_READYSTORM, TK_READYDOWN, TK_READYTURN, TK_READYCOUNTER, TK_SEVENWIND, // Taekwon
};

// Script commands constants
enum ac_type : uint8 {
	AC_HEALSKILL,
	AC_HPPOTION,
	AC_SPPOTION,
	AC_SIT,
	AC_BUFFSKILL,
	AC_ATTACKSKILL,
	AC_BUFFITEM,
	AC_LOOTITEM,
	AC_MOBS,
	AC_NORMALATK,
	AC_TELEPORT,
	AC_RETALIATE,
	AC_ENDCONDITION,
};

enum loot_item_config : int8 {
	AC_LOOT_NONE = -1,
	AC_LOOT_ALL,
	AC_LOOT_GROUP_1,
	AC_LOOT_GROUP_2, // add more groups above AC_LOOT_GROUP_MAX
	AC_LOOT_GROUP_MAX,
};

typedef std::pair<int16, int16> Pair;

typedef std::pair<float, std::pair<int16, int16>> pPair;

typedef struct {
    int parent_i, parent_j;
    float f, g, h;
} aCell;

struct s_heal_skills {
	uint16 skill_lv;
	uint16 min_hp_sp;
};

struct s_hp_potions {
	int item_id;
	uint16 min_hp;
};

struct s_sp_potions {
	int item_id;
	uint16 min_sp;
};

struct s_buff_skills {
	uint16 skill_id;
	uint16 skill_lv;
};

struct s_attack_skills {
	uint16 skill_id;
	uint16 skill_lv;
};

struct s_teleport {
	bool disable_tp_skill;
	bool disable_flywing;
	bool tp_when_mvp;
	uint16 emergency_hp;
	uint32 no_mob_delay;
};

struct s_auto_combat {
	int16 mapindex;
	int64 last_teleport, last_hit, last_move;
	int skill_cd;
	int attack_target_id, target_id;
	bool disable_normal_atk;
	bool retaliate;
	bool element_switch;
	bool refresh_after_menu; // Add this line
	uint8 sit_min_hp, sit_min_sp;
	uint32 buffitems;
	enum loot_item_config loot_item_config;
	uint8 end_status_config; // 0 - do nothing, 1 - warp to savepoint, 2 - logout
	struct s_teleport teleport;
	struct s_heal_skills healskills[3]; // Fixed array 0 = Heal, 1 = Potion Pitcher, 2 = Health Conversion
	std::vector<uint32> mob_id;
	std::vector<t_itemid> loot_item_selected;
	std::vector<s_hp_potions> hp_potions;
	std::vector<s_sp_potions> sp_potions;
	std::vector<s_buff_skills> buffskills;
	std::vector<s_attack_skills> attackskills;
	std::stack<Pair> walk_xy;
	Pair destination;
	int16 last_x, last_y;
	int walk_retries;
		// Performance optimization variables
	int64 last_target_search;
	int64 last_buff_check;
	int64 last_potion_check;
	uint16 idle_ticks;
#if AUTOCOMBAT_LOOTING_CONFIG == 1
	int loot_item_id;
#endif
#if AUTOCOMBAT_LOOTING_CONFIG == 3
	uint16 loot_score;
#endif
#if AUTOCOMBAT_DURATION_CONFIG >= 1 && AUTOCOMBAT_DURATION_CONFIG <= 3
	uint16 duration;
	uint32 max_duration;
#endif
};

void autocombat_status_start(map_session_data *sd, int64 tick);
void autocombat_status_end(map_session_data *sd);
void autocombat_main(map_session_data *sd, int64 tick);

// Fake-player party leaders only (population-engine shells). Reuses the
// AutoSupport config/helpers to support party members. Never called for real
// players — normal @autocombat / @settings behaviour is unaffected.
void autocombat_seed_fake_leader(map_session_data *sd);
void autocombat_support_party(map_session_data *leader, int64 tick);
void autocombat_pc_damage(map_session_data *sd, struct block_list *src, bool was_sitting);
void autocombat_mob_damage(struct block_list *src);
void autocombat_pc_login(map_session_data *sd);
bool autocombat_autoloot(map_session_data *sd, t_itemid nameid);
void autocombat_autoloot_storage(map_session_data *sd, int16 index, int amount, unsigned int weight);
bool autocombat_can_use_item(map_session_data *sd, struct item_data *item);
void do_init_autocombat(void);

#endif