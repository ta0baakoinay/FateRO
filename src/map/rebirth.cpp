// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "rebirth.hpp"

#include <climits>             // SHRT_MIN / SHRT_MAX

#include <common/mmo.hpp>      // JOB_NOVICE_HIGH
#include <common/nullpo.hpp>
#include <common/utils.hpp>    // cap_value

#include "map.hpp"             // JOBL_* flags, RC_ALL
#include "pc.hpp"              // map_session_data, pc_* helpers
#include "status.hpp"          // status_calc_pc, SC_COMMON_*
#include "log.hpp"             // LOG_TYPE_NPC
#include "script.hpp"          // add_str
#include "population_engine.hpp" // population_engine_is_population_pc

/**
 * The exact set of transcendent 2nd-class job ids that may enter the Rebirth
 * system. Explicit allow-list - anything not here (trans 1st, baby, expanded,
 * 3rd/4th, the mounted alt-sprite ids JOB_LORD_KNIGHT2 / JOB_PALADIN2) is
 * rejected and never offered.
 */
bool rebirth_is_accepted_class( int32 job_id ){
	switch( job_id ){
		case JOB_LORD_KNIGHT:      // 4008
		case JOB_HIGH_PRIEST:      // 4009
		case JOB_HIGH_WIZARD:      // 4010
		case JOB_WHITESMITH:       // 4011
		case JOB_SNIPER:           // 4012
		case JOB_ASSASSIN_CROSS:   // 4013
		case JOB_PALADIN:          // 4015
		case JOB_CHAMPION:         // 4016
		case JOB_PROFESSOR:        // 4017
		case JOB_STALKER:          // 4018
		case JOB_CREATOR:          // 4019
		case JOB_CLOWN:            // 4020
		case JOB_GYPSY:            // 4021
			return true;
		default:
			return false;
	}
}

/**
 * Read the SQL-backed rebirth counter for this character.
 * The value lives in `char_reg_num` (permanent char registry), so it is loaded
 * automatically on login and saved automatically on the normal save cycle.
 */
int32 rebirth_get_count( map_session_data* sd ){
	if( sd == nullptr )
		return 0;

	int64 raw = pc_readregistry( sd, add_str( REBIRTH_COUNT_VAR ) );

	if( raw < 0 )
		return 0;
	if( raw > REBIRTH_MAX_COUNT )
		return REBIRTH_MAX_COUNT;

	return (int32)raw;
}

/**
 * Zeny price to go from `count` rebirths to `count + 1`.
 * 1st rebirth = 100,000 ... 80th rebirth = 8,000,000.
 */
int64 rebirth_get_cost( int32 count ){
	if( count < 0 )
		count = 0;
	if( count >= REBIRTH_MAX_COUNT )
		count = REBIRTH_MAX_COUNT - 1;

	return (int64)( count + 1 ) * (int64)REBIRTH_COST_STEP;
}

/**
 * Server-authoritative rebirth transaction. Every requirement is validated here
 * from server state (never from anything the client sent). The whole function
 * runs without a single script yield, so two rebirths cannot interleave and a
 * character cannot be rebirthed twice from one confirmation.
 */
int32 rebirth_perform( map_session_data* sd ){
	if( sd == nullptr )
		return REBIRTH_ERR_NO_PLAYER;

	// Milestone bonuses and rebirth itself are for real player characters only.
	// Population-engine shells must never enter this path.
	if( population_engine_is_population_pc( sd->id ) )
		return REBIRTH_ERR_NO_PLAYER;

	int32 count = rebirth_get_count( sd );

	// --- Hard cap -----------------------------------------------------------
	if( count >= REBIRTH_MAX_COUNT )
		return REBIRTH_ERR_MAXED;

	// --- Level requirement ------------------------------------------------
	if( sd->status.base_level < REBIRTH_REQ_BASE_LEVEL ||
		sd->status.job_level < REBIRTH_REQ_JOB_LEVEL )
		return REBIRTH_ERR_LEVEL;

	// --- Class / progression state --------------------------------------
	// Prerequisite: the character must be one of the accepted TRANSCENDENT 2nd
	// classes. This is an explicit allow-list - the mounted/alt sprite ids
	// (JOB_LORD_KNIGHT2 4014, JOB_PALADIN2 4022), trans 1st classes, baby and
	// expanded classes are all rejected. Nothing beyond this list is offered.
	// Each Prestige Rebirth then reuses the native transcendent reset (jobchange
	// back to High Novice + level reset), not a parallel progression track.
	if( !rebirth_is_accepted_class( sd->status.class_ ) )
		return REBIRTH_ERR_JOB_STATE;
	if( sd->class_ & ( JOBL_THIRD | JOBL_FOURTH ) )
		return REBIRTH_ERR_JOB_STATE;

	// --- Zeny cost (checked and charged atomically) --------------------
	int64 cost = rebirth_get_cost( count );

	if( sd->status.zeny < cost )
		return REBIRTH_ERR_ZENY;
	// pc_payzeny re-checks funds and rejects negative amounts.
	if( pc_payzeny( sd, (int32)cost, LOG_TYPE_NPC, 0 ) != 0 )
		return REBIRTH_ERR_ZENY;

	// --- Native transcendent-style progression reset ------------------
	// Lock the re-climb to the exact same path. The Job Master's linear
	// class-change feature (.LastJob) expects `lastJob` to hold the
	// NON-transcendent 2nd-class job id (e.g. High Wizard -> JOB_WIZARD),
	// which its `lastJob + Job_Novice_High` math turns back into the
	// transcendent 2nd class. Derive it from the current class with the
	// upper flag stripped so, after reverting to High Novice, the only
	// available options are High <mage> -> High Wizard again.
	int32 base2nd = pc_mapid2jobid( sd->class_ & ~JOBL_UPPER, sd->status.sex );
	pc_setglobalreg( sd, add_str( "lastJob" ), ( base2nd > 0 ) ? base2nd : sd->status.class_ );

	pc_jobchange( sd, JOB_NOVICE_HIGH, 1 );
	pc_resetlvl( sd, 1 );

	// --- Persist the new milestone count ------------------------------
	int32 newcount = count + 1;
	pc_setregistry( sd, add_str( REBIRTH_COUNT_VAR ), newcount );

	// Re-derive every milestone bonus from the stored count.
	status_calc_pc( sd, SCO_FORCE );

	return newcount;
}

/**
 * Accumulate a "resist effect" rate entry the same way pc_bonus2(bResEff) does.
 * val is in 1/100 % (1000 = 10%), capped at 10000 (100%).
 */
static void rebirth_add_reseff( map_session_data* sd, uint16 sc, int32 val ){
	for( auto& it : sd->reseff ){
		if( it.id == sc ){
			int32 sum = it.val + val;
			it.val = cap_value( sum, -10000, 10000 );
			return;
		}
	}

	struct s_item_bonus entry = {};
	entry.id = sc;
	entry.val = cap_value( val, -10000, 10000 );
	sd->reseff.push_back( entry );
}

/**
 * Permanent, cumulative milestone bonuses. Computed purely from the stored
 * rebirth count, so reaching 80 rebirths automatically grants every tier.
 * Invoked from status_calc_pc_sub() after equipment/card/bonus_script parsing
 * and before the derived stats (max HP/SP, weight, ASPD, MATK) are finalised,
 * so it feeds the same fields the native `bonus` commands use.
 */
void rebirth_calc_bonus( map_session_data* sd ){
	if( sd == nullptr )
		return;

	// Never let population-engine shells pick up player milestone bonuses.
	if( population_engine_is_population_pc( sd->id ) )
		return;

	int32 c = rebirth_get_count( sd );
	if( c <= 0 )
		return;

	// 10: +10% EXP gain (vs. all races)
	if( c >= 10 )
		sd->indexed_bonus.expaddrace[RC_ALL] += 10;

	// 20: +10% Drop Rate (vs. all races)
	if( c >= 20 )
		sd->indexed_bonus.dropaddrace[RC_ALL] += 10;

	// 30: +1,000 Max Weight (rAthena weight is stored *10, so 10000 raw units
	// == 1,000 weight as shown to the player, matching `bonus bAddMaxWeight`).
	if( c >= 30 )
		sd->add_max_weight += 10000;

	// 40: +20 ATK / +20 MATK (flat)
	if( c >= 40 ){
		sd->bonus.eatk = cap_value( sd->bonus.eatk + 20, SHRT_MIN, SHRT_MAX );
		sd->bonus.ematk += 20;
	}

	// 50: +10% Max HP / +10% Max SP
	if( c >= 50 ){
		sd->hprate += 10;
		sd->sprate += 10;
	}

	// 60: +10% resistance to ALL (common) status effects
	if( c >= 60 ){
		for( uint16 sc = SC_COMMON_MIN; sc <= SC_COMMON_MAX; sc++ )
			rebirth_add_reseff( sd, sc, 1000 );
	}

	// 70: +5% physical attack / +5% magical attack
	if( c >= 70 ){
		sd->bonus.atk_rate += 5;
		sd->matk_rate += 5;
	}

	// 80: +5 ASPD
	if( c >= 80 )
		sd->bonus.aspd_add -= 10 * 5;
}
