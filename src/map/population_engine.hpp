// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder
//
// Population engine: main public API (implementation in population_engine.cpp).

#ifndef POPULATION_ENGINE_HPP
#define POPULATION_ENGINE_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

class map_session_data;

struct PopulationEngine;

/// Equipment YAML row for `sd->status.class_`, else same row for engine base-job fallback.
const PopulationEngine *population_engine_resolve_equipment(uint16_t job_id);
/// Swordman/Mage/… first class for `job_id` (same mapping as population spawn fallback).
uint16_t population_engine_job_base_class(uint16_t job_id);

struct PopulationEngineConfig {
	uint32_t num_units = 0;
	int16_t map_id = 0;
	int spawn_x = 0;
	int spawn_y = 0;
	bool spread_units = true;
};

struct PopulationEngineStats {
	uint32_t total_created = 0;
	uint32_t active_units = 0;
	uint32_t errors = 0;
	std::vector<int32_t> unit_ids;
	// Runtime metrics (accumulated since last engine start / reset).
	uint32_t chat_lines_emitted = 0;  ///< Ambient + reply chat lines sent.
	uint32_t name_retries = 0;        ///< Blocklist retries across all spawns.
	uint32_t walk_failures = 0;       ///< unit_walktoxy failures in wander timer.
};

void do_init_population_engine();
/// Load population_names.yml and population_engine.yml. Call after do_init_itemdb and do_init_pc (job_db + item_db).
/// Starts autosummon / chat / wander timers per `conf/battle/population_engine.conf`.
void do_init_population_engine_load_databases();
void do_final_population_engine();
/// Reload db/population_engine.yml and db/population_chat.yml. Returns false if equipment YAML could not be read/parsed.
/// If out_entry_count is non-null, set to the number of job profiles after reload (0 if strict mode discarded all).
bool population_engine_reload_equipment(uint32_t *out_entry_count = nullptr);
bool population_engine_start(const PopulationEngineConfig &config, PopulationEngineStats &stats);
void population_engine_stop();
PopulationEngineStats population_engine_get_stats();
bool population_engine_is_running();
bool population_engine_is_population_pc(int32_t id);
/// Returns true if the shell has PSF::Mortal set (will take damage; default is immortal).
bool population_engine_shell_is_mortal(const map_session_data *sd);
/// Called from pc_dead when a mortal population shell reaches 0 HP. Schedules a respawn.
void population_engine_on_shell_death(map_session_data *sd);
/// Called from pc_damage whenever a population shell receives damage. Records gettick() and the
/// attacker id on sd->pop so reactive conditions (SelfTargeted, etc.) can detect non-mob attackers
/// (real PvP players, traps, etc.).
void population_engine_on_shell_damaged(map_session_data *sd, struct block_list *src);
/// Called after a population shell kills a real (non-shell) player. Shell may trash-talk.
void population_engine_on_shell_kills_player(map_session_data *killer_sd, map_session_data *victim_sd);
size_t population_engine_get_count();

// ============================================================
// Manual GM control API (used by @populate add / remove / list / find / where).
// These are always available regardless of population_engine_autosummon_enable.
// ============================================================

/// Additively create `count` fake players, one job rolled per shell.
///
/// Map selection is JOB-AWARE and driven by db/population_spawn.yml: each rolled
/// job resolves to its Population Engine profile, and that profile's
/// Towns/Fields/Dungeons lists are the maps that job may use.
///   - `map_id` >= 0 and the rolled job's profile lists that map  -> spawn there
///   - `map_id` >= 0 but the job's profile does NOT list it        -> spawn on a
///        random map from the job's own lists (Towns > Fields > Dungeons)
///   - the job has no profile / no spawn-DB entry / empty lists     -> `map_id`
///        if given, else a safe default (prontera, else the first configured town)
/// This is why e.g. a Novice can never land in gl_prison unless gl_prison is in
/// the Novice profile's spawn lists.
///
/// Jobs, gear and (default) appearance come from the normal resolution path; AI
/// starts immediately. Respects battle_config.population_engine_max_count.
/// If `out_redirected` is non-null it receives the number of shells that were
/// moved off the requested map because it was not valid for their job.
/// Returns the number of shells actually created.
int  population_engine_manual_add(int32_t map_id, uint32_t count, int *out_redirected = nullptr);

/// @populate vending <amount> <map|allmap>. Creates `count` Merchant-class fake
/// players that each open a REAL rAthena vend (MC_VENDING + cart +
/// vending_openvending). Stock comes from db/population_vendors.yml (VendorKey)
/// when configured, else a built-in default list. map_id < 0 = distribute across
/// eligible town maps. No permanent vending SQL is written (fake-player
/// protection). Returns the number of vendors actually created.
int  population_engine_manual_add_vendors(int32_t map_id, uint32_t count, int *out_redirected = nullptr);

/// Count live fake players that currently have a real vend open (state.vending).
size_t population_engine_vendor_count();

/// Run exactly one db/population_spawn.yml distribution pass right now (same
/// logic the autosummon timer uses), even when autosummon is disabled.
/// Returns the number of shells created by the pass.
int  population_engine_manual_fill_from_spawndb();

/// Release up to `count` fake players (newest first). They are NOT recreated.
/// Returns the number actually removed.
int  population_engine_manual_remove(uint32_t count);

/// Release every fake player without tearing down the chat/combat/wander timers
/// (unlike population_engine_stop). Returns the number removed.
int  population_engine_manual_remove_all();

/// Release the single fake player whose name matches `name` (case-insensitive).
/// Returns true if one was found and removed.
bool population_engine_manual_remove_named(const char *name);

/// Release up to `count` fake players currently on `map_name` (0 = every one on
/// that map). Manual shells go first, then auto. Returns the number removed, or
/// -1 if `map_name` is not a valid loaded map.
int  population_engine_manual_remove_on_map(const char *map_name, uint32_t count);

/// @recall / @recallall support. Fake players are not in pc_db / nick_db.
/// find_by_name: exact (case-insensitive) live fake player, or nullptr.
map_session_data *population_engine_find_by_name(const char *name);
/// Warp every live fake player to (map_index,x,y). Returns how many moved.
/// Recall is a move — identity, AI, party and tracking are preserved.
int  population_engine_recall_all(uint16_t map_index, int16_t x, int16_t y, bool ignore_nowarp);

/// Monster-difficulty-aware job selection (used by manual + automatic population).
/// tier: -1 no monsters | 0 trivial | 1 early | 2 mid | 3 high, derived from the
/// map's actual monster spawn levels (mob_db). job_tier: 0 Novice | 1 first job |
/// 2 second job | 3 transcendent (from the job's MAPID).
int  population_map_difficulty_tier(int16_t map_id);
int  population_job_tier(uint16_t job_id);
bool population_engine_job_fits_map(uint16_t job_id, int16_t map_id);

/// Lightweight snapshot row for GM listing commands.
struct PopulationShellSnapshot {
	char     name[24];
	char     map_name[12];
	int16_t  x;
	int16_t  y;
	uint16_t job_id;
	uint32_t account_id;
	bool     dead;
	bool     is_auto;   ///< true = belongs to an @populate auto target; false = manual.
	bool     vending;   ///< true = fake player currently has a REAL vend open.
};

/// Fill `out` with one row per live fake player. If `map_name_filter` is non-null
/// and non-empty, only shells on that map are included. Returns out.size().
size_t population_engine_snapshot(std::vector<PopulationShellSnapshot> &out,
                                  const char *map_name_filter = nullptr);

// ---- manual vs automatic population -------------------------------------------

/// Number of fake players by ownership.
size_t population_engine_manual_count();   ///< source == manual
size_t population_engine_auto_count();     ///< source == auto (any auto target)

/// Enable / update an automatic population target. `map_id` >= 0 keeps that many
/// AUTO fake players on that specific map (only jobs whose profile lists the map
/// are used); `map_id` < 0 means "allmap" — keep that many AUTO fake players in
/// total, each placed on a map appropriate for its job. Multiple targets coexist.
/// Arms the auto manager (10s) and does one immediate top-up. Returns the current
/// auto count for that target, or -1 if `map_id` names an invalid map.
int  population_engine_auto_set(int32_t map_id, uint32_t target);

/// Remove one automatic target (`map_id` < 0 = the allmap target). Existing fake
/// players are NOT removed. Returns true if a target was found and removed.
bool population_engine_auto_clear(int32_t map_id);

/// Remove ALL automatic targets and disarm the auto manager. Fake players stay.
void population_engine_auto_clear_all();

/// True while at least one automatic target is active.
bool population_engine_auto_active();

struct PopulationAutoStatusRow {
	int16_t  map_id;    ///< -2 = allmap target
	char     map_name[12];
	uint32_t target;
	uint32_t current;
};
/// One row per active automatic target (current = live AUTO shells for it).
size_t population_engine_auto_status(std::vector<PopulationAutoStatusRow> &out);

// ---- population map eligibility ---------------------------------------------
// "allmap" means "all ELIGIBLE population maps", never every registered map.
// One shared filter (population_map_is_valid) backs all of these.

/// May fake players be placed on `map_name`? On false, *reason (if non-null) gets
/// a short cause ("instance map", "PvP map", "GvG/WoE map", "Battleground map",
/// "restricted zone", "jail", "map is not loaded on this map-server", ...).
bool   population_engine_map_allowed(const char *map_name, const char **reason);

/// Count loaded maps that are eligible / excluded for population.
void   population_engine_map_policy_counts(size_t *eligible, size_t *excluded);
/// Stop combat mode for a population shell (teardown hat effect, cleanup, set state=false).
/// Safe to call even if not in combat. Used by clif quit/restart paths.
void population_engine_combat_shell_stop(map_session_data *sd);
/// Whisper to a population PC: send one random chat line back to the sender (no fake-client packet).
void population_engine_on_whisper_to_population_pc(map_session_data *from_sd, map_session_data *bot_sd, const char *message);
/// Map chat: any population PC on the same map whose name appears in `message` (case-insensitive) may reply overhead.
void population_engine_on_global_chat_mention(map_session_data *from_sd, const char *message);

/// Arena PvP: spawn `shell_count` shells on `map_name` (must be a PvP map).
/// Shells target real (non-shell) players on the map so a player can observe AI behaviour.
/// Optional spawn_x/spawn_y set the spawn-center; 0 = random spread.
/// map_search_freecell validates walkability so shells never appear on blocked cells.
/// Optional job_override forces every shell to use that JOB_ id (0 = use ArenaJobPool / built-in mix).
/// team_id: 1 = enemy shells (target real players), 2 = allied shells (target team-1 shells).
/// Returns total shells spawned (0 on error).
int  population_engine_arena_start(const char* map_name, int shell_count,
                                   int spawn_x = 0, int spawn_y = 0,
                                   uint16_t job_override = 0, int team_id = 1);
/// Arena PvP: release all arena shells on `map_name`.
void population_engine_arena_stop(const char* map_name);

/// Arena PvP: returns the job-id pool used by population_engine_arena_start when
/// no explicit job_override is supplied. The pool is auto-derived from
/// db/population_pvp.yml entries (one entry per Profile.Jobs row).
std::vector<uint16_t> population_engine_arena_job_pool();

struct block_list;
/// Arena PvP: classify the relation between two block_list entities so that
/// `battle_check_target` can treat ally shells (team 2) as friendly to the
/// real player on the same map and to each other, and enemy shells (team 1)
/// as hostile to both. Returns:
///    +1 = allies (force BCT_PARTY, strip BCT_ENEMY)
///    -1 = enemies (force BCT_ENEMY)
///     0 = no opinion (caller falls through to normal party/guild logic)
/// Implementation lives in population_engine.cpp.
int population_engine_arena_relation(const block_list *s_bl, const block_list *t_bl);

/// Arena PvP: PC-only friendly check used by the population shell's own ally
/// target finder. Returns true when both PCs share the same effective arena
/// team on the same map (real player implicitly team 2). This is the standalone
/// hook used by the population engine's support behaviour — it does NOT touch
/// the autosupport subsystem.
bool population_engine_arena_is_ally(const map_session_data *a, const map_session_data *b);

#endif // POPULATION_ENGINE_HPP
