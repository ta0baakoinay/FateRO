// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#ifndef REBIRTH_HPP
#define REBIRTH_HPP

#include <common/cbasetypes.hpp>

class map_session_data;

/// Permanent (SQL-backed) per-character variable that stores how many times the
/// character has gone through the repeatable Rebirth. Stored in `char_reg_num`
/// via the standard rAthena registry, so it auto-loads on login and auto-saves.
///
/// NOTE: the script string table (add_str) is case-INSENSITIVE, so this name must
/// not collide with any script identifier - in particular not with the
/// rebirth()/rebirth_count()/rebirth_cost() buildins. "REBIRTH_TOTAL" is safe.
#define REBIRTH_COUNT_VAR "REBIRTH_TOTAL"

/// Progression requirements / tuning. Kept as constants so the NPC text and the
/// server-side validation can never drift apart (NPC reads them through the
/// rebirthinfo() script command).
enum e_rebirth_tuning : int32 {
	REBIRTH_MAX_COUNT      = 80,      ///< Hard cap on rebirths.
	REBIRTH_REQ_BASE_LEVEL = 99,     ///< Required base level to rebirth.
	REBIRTH_REQ_JOB_LEVEL  = 70,     ///< Required job level to rebirth.
	REBIRTH_COST_STEP      = 100000, ///< Zeny cost added per rebirth (1st = 1x, 80th = 80x).
};

/// Result codes returned by rebirth_perform() / the rebirth() script command.
/// Success returns the NEW rebirth count (always >= 1). Failures are negative.
enum e_rebirth_result : int32 {
	REBIRTH_ERR_NO_PLAYER   = -1, ///< No player attached / internal error.
	REBIRTH_ERR_MAXED       = -2, ///< Already at REBIRTH_MAX_COUNT.
	REBIRTH_ERR_LEVEL       = -3, ///< Base/Job level requirement not met.
	REBIRTH_ERR_JOB_STATE   = -4, ///< Character is not in a valid class to rebirth.
	REBIRTH_ERR_ZENY        = -5, ///< Not enough zeny for the next rebirth.
};

/// True only for the accepted transcendent 2nd-class job ids (explicit allow-list).
bool rebirth_is_accepted_class( int32 job_id );

/// Returns the character's stored rebirth count, clamped to [0, REBIRTH_MAX_COUNT].
int32 rebirth_get_count( map_session_data* sd );

/// Zeny cost of going from `count` -> `count + 1` rebirths.
int64 rebirth_get_cost( int32 count );

/// Fully validates and executes a rebirth for `sd` server-side (level, class,
/// cap and zeny checks, progression reset, count increment, stat recalc).
/// Atomic: performs no script yield, so it cannot be raced or duplicated.
/// @return new rebirth count on success, or a negative e_rebirth_result.
int32 rebirth_perform( map_session_data* sd );

/// Applies the permanent, cumulative milestone bonuses derived purely from the
/// stored rebirth count. Called from status_calc_pc_sub() so the bonuses are
/// re-applied automatically on login, on rebirth and on every stat recalc.
void rebirth_calc_bonus( map_session_data* sd );

#endif /* REBIRTH_HPP */
