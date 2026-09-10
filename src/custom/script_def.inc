// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Script Commands
 * Place the definition of custom script commands in this file.
 * Format:
 *	BUILDIN_DEF(command_name,"parameters"),
 **/

//BUILDIN_DEF(example,""),
BUILDIN_DEF(issitting,""),
BUILDIN_DEF(getautocombatv, "i"),
BUILDIN_DEF(getautocombat, "i?"),
BUILDIN_DEF(setautocombat, "i?????"),
BUILDIN_DEF(getskillname, "i"),
BUILDIN_DEF(getavailablemobs, "r"),
BUILDIN_DEF(getautocombatduration, "ii"),
BUILDIN_DEF(getspbstate, ""),
BUILDIN_DEF(getbattleinfostate, ""),

// Population / default ammo (skill require.ammo); see custom script.inc
BUILDIN_DEF(setarrow, "?"),
BUILDIN_DEF(setbullet, "?"),
BUILDIN_DEF(setkunai, "?"),

// Population engine arena
BUILDIN_DEF(population_arena_start, "si????"),
BUILDIN_DEF(population_arena_stop,  "s"),
BUILDIN_DEF(population_arena_jobs,  ""),

// SQL-backed Rebirth System
BUILDIN_DEF(rebirth,       ""),
BUILDIN_DEF(rebirth_count, ""),
BUILDIN_DEF(rebirth_cost,  ""),