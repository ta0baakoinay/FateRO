# Population Engine / Fake Players — Start-to-Finish Tutorial

This tutorial explains how to run the **Population Engine** (aka "fake players")
in this project. It is written for someone who has the server compiled and
running but has never used the engine before.

Everything below was taken from the actual source in this repository:

- `src/map/population_engine.cpp` and `src/map/population_engine.hpp`
- `src/map/population_engine/` (config parser, runtime, combat, path, AI)
- `src/custom/atcommand.inc`, `src/custom/atcommand_def.inc`, `conf/import/atcommands.yml`
- `src/custom/battle_config_*.inc`, `conf/battle/population_engine.conf`,
  `conf/import/battle_conf.txt`
- `db/population_*.yml`
- `sql-files/population_engine.sql`
- `src/common/mmo.hpp`, `src/map/CMakeLists.txt`, `src/map/Makefile.in`

Where a feature is documented in a YAML header comment but **not** actually
implemented by the parser, it is called out explicitly.

---

## 1. What the Population Engine is

The engine creates **runtime PC "shells"** — fake `map_session_data` objects that
look and behave like real players. They are not stored in the database; they are
generated in memory each time the map-server boots. They:

- walk around towns/fields/dungeons (Wander),
- optionally fight monsters with real skills (Combat),
- optionally open real vends players can buy from (Vendor),
- say ambient overhead chat lines and can whisper back,
- wear randomly-selected equipment from **gear sets**.

Shells use a dedicated account/char ID range so they never collide with real
players:

```
src/common/mmo.hpp
  POPULATION_ENGINE_ACCOUNT_ID_BASE 95000000
  POPULATION_ENGINE_ACCOUNT_ID_END  100000000
  POPULATION_ENGINE_CHAR_ID_BASE    95000000
```

There are two ways shells appear:

| Path | Trigger | Controlled by |
|------|---------|---------------|
| **Autosummon** | Automatic, every ~10 s after boot | `db/population_spawn.yml` + `db/population_vendors.yml` |
| **`@populate`** | Manual GM command | command arguments only |

Autosummon and `@populate` are **mutually exclusive at runtime**: `@populate`
refuses to start if the engine is already running (and autosummon starts it at
boot). In practice, on a normal server the autosummon system is the one you use;
`@populate` is a manual/testing tool that fills a single map.

---

## 2. Prerequisites

### 2.1 rAthena setup

- A working **compiled** map-server from this repository (login + char + map).
- **Pre-renewal** mode. `src/config/renewal.hpp` has `#define PRERE` active, so
  the pre-renewal item DB (`db/pre-re/item_db*.yml`) and job DB are what the
  engine validates gear/jobs against. All bundled gear sets in
  `db/population_gear_sets.yml` are pre-renewal compatible.
- The engine calls into the normal PC subsystem (`pc_equipitem`, `status_calc_pc`,
  skills, vending, pathfinding). No extra external service is required.
- **Client / PACKETVER:** shells are sent to clients like real players. Use a
  PACKETVER your client supports (unchanged from the rest of the server). The
  hat-effect option (`population_engine_hat_effect`) requires a client build
  whose `HAT_EF_MIN < id < HAT_EF_MAX`; set it to `0` if unsure.

### 2.2 Required files

All of these ship in the repo and are loaded from the `db_path` directory
(`db/` by default — **not** `db/pre-re/`):

| File | Purpose | Required? |
|------|---------|-----------|
| `db/population_gear_sets.yml` | Shared gear-set pools (`GearSetName:` entries) + is where `Profile:` refs fall back | Yes |
| `db/population_engine.yml` | PvE profiles (town/field/dungeon jobs) | Yes for PvE |
| `db/population_spawn.yml` | Autosummon map lists + target populations per profile | Yes for autosummon |
| `db/population_names.yml` | Name-generation profiles | Yes |
| `db/population_chat.yml` | Ambient chat line pools | Yes if chat enabled |
| `db/population_skill_db.yml` | Per-job skill rotations for Combat behavior | Yes if attack-skills enabled |
| `db/population_pvp.yml` | Transcendent/expanded PvP profiles; also auto-derives the **arena job pool** | Optional (needed for `population_arena_*` script commands) |
| `db/population_vendors.yml` | Vendor inventory + `VendorPlacement` map blocks | Optional (vendor shells) |
| `db/population_vendor_pop.yml` | Vendor shell profiles (`TownBehavior: vendor`) | Optional (vendor shells) |
| `conf/battle/population_engine.conf` | All runtime tunables (`population_engine_*`) | Yes |

### 2.3 Dependencies / integration points already applied in this repo

The "supplied patch" wires the engine into the server. You do **not** need to do
these yourself — they are already committed/staged — but you must **rebuild** so
they take effect:

- **Build system**
  - `src/map/CMakeLists.txt`: `list(FILTER MAP_SOURCES EXCLUDE REGEX ".*/population_engine/.*")`
  - `src/map/Makefile.in`: `-not -path "./population_engine/*"`
  - Reason: files under `src/map/population_engine/` are *unity-built* — they are
    `#include`d into `population_engine.cpp`. Compiling them as separate
    translation units causes duplicate-symbol link errors.
- **Server init** (`src/map/map.cpp`, `do_init`):
  ```
  do_init_population_engine();                 // register autosummon timer func
  do_init_population_engine_combat();
  do_init_population_engine_load_databases();  // load YAML, start timers
  ```
  and `do_final_population_engine()` on shutdown.
- **Battle config**: `conf/import/battle_conf.txt` contains
  `import: conf/battle/population_engine.conf`, plus
  `src/custom/battle_config_struct.inc` / `battle_config_init.inc` declare every
  `population_engine_*` key.
- **At-commands**: `src/custom/atcommand.inc` / `atcommand_def.inc` add
  `@populate` and `@reloadpopenginedb`; `conf/import/atcommands.yml` gives them
  help text. **Add group permissions yourself** (see §6).
- **Custom script commands**: `src/custom/script_def.inc` / `script.inc` add
  `population_arena_start`, `population_arena_stop`, `population_arena_jobs`.
- **Website (optional)**: `sql-files/population_engine.sql` creates
  `cp_population_stats` (a single-row table the map-server updates with the live
  shell count so FluxCP can show it). Not required for the engine to work.

---

## 3. Installation

### 3.1 File locations

Nothing to move. The engine sources live in `src/map/` and
`src/map/population_engine/`; the databases live in `db/`; the config lives in
`conf/battle/`. All paths are already correct in the repo.

### 3.2 Configuration files you must edit / review

1. **`conf/import/battle_conf.txt`** — must contain
   `import: conf/battle/population_engine.conf` (already added).
2. **`conf/battle/population_engine.conf`** — review the tunables (see §4).
3. **`db/population_spawn.yml`** — set which maps get shells and how many
   (see §4.4). If you do **not** want autosummon, empty the `Body:` list or
   delete the file (autosummon is then disabled with a warning and you use
   `@populate` manually).
4. **`db/population_engine.yml`** — profiles / job→gearset mapping (see §4.5).
5. **`db/population_gear_sets.yml`** — the gear pools (see §5).
6. Group permissions for `@populate` / `@reloadpopenginedb` in
   `conf/groups.yml` (see §6).

### 3.3 Database changes

Only the **optional** website table:

```sh
mysql -u <user> -p <ragnarok_db> < sql-files/population_engine.sql
```

Skip this if you are not using FluxCP's population widget. The engine tolerates
the table being absent.

### 3.4 Source changes

Already applied by the supplied patch (§2.3). After pulling them:

```sh
# from repo root
./configure          # only if you use the autotools/Makefile path
make clean && make server -j$(nproc)
#   — or, CMake —
cmake --build build --target map-server -j$(nproc)
```

### 3.5 Services to start

Just the normal trio. The engine has **no separate daemon**:

```sh
./login-server &
./char-server &
./map-server        # loads population DBs and starts autosummon/chat/wander timers
```

On boot you should see the population YAML files load in the map-server console
and, within ~10 s, shells begin spawning on the maps listed in
`db/population_spawn.yml`.

---

## 4. Configuration

### 4.1 Number of fake players

- **Global hard cap:** `population_engine_max_count` in
  `conf/battle/population_engine.conf` (repo value `5000`; code default `1000`;
  allowed range `1`–`30000` per `battle_config_init.inc`). Both autosummon and
  `@populate` respect it.
- **`@populate` per-command cap:** `5000` (hard-coded in `atcommand.inc`).
- **Autosummon target totals:** per profile, per category, in
  `db/population_spawn.yml` (`TownsPopulation`, `FieldsPopulation`,
  `DungeonsPopulation`).
- **Spawn rate:** `population_engine_autosummon_batch_size` — max shells spawned
  per ~10 s autosummon tick across everything. Repo value `0` = unlimited
  (everything spawns on the first tick — a boot spike). Set e.g. `50` to ramp up
  smoothly.

### 4.2 Population / spawn settings (`db/population_spawn.yml`)

Structure (`Header.Type: POPULATION_SPAWN_DB`, `Version: 1`):

```yaml
Body:
  - Profile: combat_pve            # must match a Profile: in population_engine.yml
    Towns:    [ prontera, geffen, payon ]
    Fields:   [ gef_fild00, gef_fild01 ]
    Dungeons: [ prt_sewb1, prt_sewb2 ]
    TownsPopulation:      60       # total across all Towns maps
    TownsMaxPerMap:       0        # per-map cap, 0 = none
    FieldsPopulation:     200
    FieldsMaxPerMap:      0
    DungeonsPopulation:   100
    DungeonsMaxPerMap:    0
```

Distribution rule (from the header + parser): each map gets `floor(N/count)`
shells; the first `N % count` maps get `+1` so the totals hit `N` exactly.
`MaxPerMap` clamps each map afterwards. Omit or `0` a `*Population` key to
disable that category for the profile.

### 4.3 Class / job distribution

Job distribution is **driven by profiles**, not by the spawn file:

- A `Profile:` in `db/population_engine.yml` lists a `Jobs:` map
  (`JobName: GearSetName`).
- At autosummon time the engine resolves the profile to that job list and picks
  **one job at random per shell**. So a single spawn entry covers every job in
  the profile, evenly weighted.
- To change ratios: add/remove job lines in the profile, or split into multiple
  profiles with their own spawn entries and populations (a profile with more
  spawn maps / higher `*Population` produces more of those jobs).

Job names are the rAthena `JobName` tokens (e.g. `Swordsman`, `Mage`, `Archer`,
`Knight`, `Crusader`, `Wizard`, `Hunter`, `Monk`, `Assassin`, `Rogue`,
`Alchemist`, `Blacksmith`, `Gunslinger`, `Ninja`, `Taekwon`, `StarGladiator`,
`SoulLinker`, `AssassinCross`, …). A job present on the map but not listed in the
profile falls back to the engine's built-in class default for that job.

### 4.4 Maps

Maps are plain map names in the `Towns:` / `Fields:` / `Dungeons:` lists of
`db/population_spawn.yml` (and `VendorPlacement.Map` for vendors). They must be
real, loaded maps (present in `db/map_index.txt` / your `maps.conf`). Invalid map
names are skipped.

`@populate <n> <map>` takes a single map name argument; with no map it uses the
GM's current map.

### 4.5 Profiles (`db/population_engine.yml`) — the important fields

`Header.Type: POPULATION_ENGINE_DB`, `Version: 2`. Each `Body:` entry is a
`Profile:`. Fields actually parsed:

| Field | Meaning |
|-------|---------|
| `Profile:` | Unique name; referenced by `db/population_spawn.yml`. |
| `Jobs:` | Map of `JobName: GearSetName` (gear-set names from `db/population_gear_sets.yml`). |
| `NameProfile:` | Name profile key from `db/population_names.yml` (default `default`). |
| `ChatProfile:` | Chat pool key from `db/population_chat.yml` (default `default`). |
| `Hair:` `[min,max]` | Hair style range. |
| `HairColor:` `[min,max]` | Hair color range. |
| `ClothesColor:` `[min,max]` | Clothes color range. |
| `BaseLevel:` `[min,max]` | Base level assigned at spawn (affects what gear the shell can actually equip). |
| `JobLevel:` `[min,max]` | Job level range. |
| `Str/Agi/Vit/Int/Dex/Luk:` `[min,max]` | Raw stat ranges, each clamped to `[1,99]`. |
| `TownBehavior:` / `FieldBehavior:` / `DungeonBehavior:` | Behavior per zone (see §4.6). |
| `Flags:` | List: `immortal` (default), `mortal`, `attack_only`, `skill_only`. |
| `Script:` | Inline NPC script run once at spawn (e.g. `setriding;`, `setarrow;`, `setcart;`). |
| `VendorKey:` | (vendor profiles) selects the inventory block in `db/population_vendors.yml`. |

Per-job fields also parsed in the same file family: `Movetype` (`-1` turret,
`0` melee, `1` ranged, `3` hybrid) and `Role` (`none`/`tank`/`support`/
`attacker`) — these are AI hints used by the combat runtime.

**Discrepancy to be aware of:** the header comment in `population_engine.yml`
lists `idle` as a behavior value. The parser
(`population_eq_parse_behavior_str`) does **not** accept `idle` — valid values
are `none`, `wander`, `combat`, `support`, `sit`, `social`, `vendor`, `guard`.
An unrecognized value logs a warning and the job/profile falls back to `combat`.

### 4.6 AI / behavior settings

**Behavior state** (what a shell does when idle vs. engaged), set per zone on the
profile:

- `wander` — random walk within spawn radius.
- `guard` — wander, but switch to combat on aggro.
- `combat` — always seek targets; never wander.
- `sit` — sit idle at spawn.
- `social` — wander + frequent chat/emotes.
- `vendor` — open a real vend (vendor profiles only).
- `none` — generic attacker, no override.

**AI behavior bitmask** — `population_engine_ai` in
`conf/battle/population_engine.conf` (repo value `0x3D2`). Add flags:

```
0x001 Chase refresh        0x010 Flee on low HP       0x100 Combo awareness
0x002 Target switch        0x020 Support priority     0x200 Boss avoidance
0x004 Skill while chasing  0x040 Pack behavior
0x008 Reactive conditions  0x080 Kite (ranged keep distance)
```

**Combat tick / skill tuning** (all in `population_engine.conf`):
`population_engine_shell_timer_ms` (100), `..._shell_skill_interval_ms` (100),
`..._shell_attackskill` (on = use skill rotations from
`db/population_skill_db.yml`; off = melee only),
`..._shell_basic_attack_chance` (70 = % of ticks forced to basic attack),
`..._shell_skill_min_sp_pct` (15), `..._shell_sticky_target_ms` (2000),
`..._shell_skill_los_check` (on), `..._shell_skill_strict_gate` (on),
`..._shell_mdetection_cells` (30 = aggro scan radius),
`..._shell_move_min` / `..._shell_move_max` (combat roam distance).

**Wander** (ambient idle movement): `population_engine_wander_enable` (on),
`..._wander_tick_ms` (500), `..._wander_radius` (12),
`..._wander_cooldown_ms` / `..._wander_cooldown_jitter_ms`,
`..._wander_max_per_tick` (50), `population_engine_path_attempts` (16).

**Chat** (ambient overhead lines from `db/population_chat.yml`):
`population_engine_chat_enable` (on), `..._chat_tick_ms` (1500),
`..._chat_max_per_tick` (10), `..._chat_cooldown_ms` (15000) + jitter,
`..._chat_reply_enable` (on = whisper/mention reply).

**Daily cycle:** `population_engine_daily_cycle` (repo: `off`). When `on`, **town**
shells override behavior by server hour: 06:00–17:59 configured behavior,
18:00–21:59 Social, 22:00–05:59 Sit. Field/dungeon shells are unaffected.

**Vending economy:** `population_engine_vending_enable` (on) — `vendor` shells
open a real vend priced at NPC buy value (sell < buy, so no arbitrage).

**Names:** `population_engine_name_bot_fallback` (off) — when the name pool is
exhausted, `off` keeps generating from syllables, `on` allows `Bot_<id>`.

**Visual:** `population_engine_hat_effect` (repo `56`; `0` inherits
`db/autocombat_config.yml`'s `HatEffect`). Sent when a shell enters autocombat.

### 4.7 Names (`db/population_names.yml`)

`Header.Type: POPULATION_NAMES_DB`, `Version: 1`. One `GlobalLists:` block
(shared syllable/word pools + `Blocklist:` of rejected substrings) plus any
number of `Profile:` entries. Strategies actually supported by the parser:

- `syllables` — `SyllablesStart` + `SyllablesMid` + `SyllablesEnd`.
- `adjective_noun` / `adj_noun` — random `Adjectives` + `Nouns`.
- `pick_one` — a full name straight from a `Pool:` / `Names:` list.
- `bot_index` — `Bot_<n>` (debug/fallback).
- `prefix_number` — `<prefix><number>`.

Per-profile `MinLen` / `MaxLen` bound the result; a name matching `Blocklist`
(case-insensitive substring) is rejected and regenerated (counted in
`name_retries`). Reference a profile from a population profile via
`NameProfile: <key>`.

### 4.8 Chat (`db/population_chat.yml`)

Line pools keyed by profile name; a population profile selects one with
`ChatProfile:`. Omitting it uses `default`. Requires a map-server restart (or
`@reloadpopenginedb`, which also reloads chat) after edits.

---

## 5. Using `db/population_gear_sets.yml`

### 5.1 Purpose

This file is the **shared equipment pool library**. It contains **only**
`GearSetName:` entries — no profiles. Population profiles in
`db/population_engine.yml`, `db/population_pvp.yml`, and
`db/population_vendor_pop.yml` reference these sets by name, so the same
equipment pool is reused across many jobs and files without duplication. It is
also the fallback source for `GearSet:` / `Profile:` lookups that a per-source
file cannot resolve.

`Header.Type: POPULATION_ENGINE_DB`, `Version: 2`.

### 5.2 YAML structure

Each entry is one gear set with up to ten equipment-slot pools:

```yaml
Body:
  - GearSetName: para_swordsman     # unique name, referenced as "GearSet: para_swordsman"
    HeadTop:                        # list of item AegisNames — one is picked at random per shell
      - Helm_
      - Sahkkat
    HeadMid: 0                      # 0 (or omit) = leave this slot empty
    HeadBottom:
      - Cigar
    Armor:
      - Coat_
      - Chain_Mail
      - Padded_Armor
    Weapon:
      - Bastard_Sword_
      - Slayer_
      - Blade_
    Shield: 0
    Garment:
      - Muffler_
      - Manteau
    Shoes:
      - Boots_
      - Shoes_
    AccL:
      - Swordman_Figure
    AccR:
      - Swordman_Figure
```

Rules enforced by the parser (`parseEquipSlotPool` in
`src/map/population_engine/config/population_config.cpp`):

- Slot keys: `HeadTop`, `HeadMid`, `HeadBottom`, `Armor`, `Weapon`, `Shield`,
  `Garment`, `Shoes`, `AccL`, `AccR` (snake_case aliases also accepted).
- A slot value is either `0` (empty), a single value, a YAML list, or an inline
  list of **numeric IDs** (e.g. `Weapon: [1201]`).
- Entries may be item **AegisName** strings (resolved via `item_db`) or numeric
  item IDs. **Do not invent IDs** — every entry is validated.
- Each item must exist in `item_db` **and** its `equip` mask must match the
  slot's equip flag (e.g. a `Weapon` entry must have `EQP_HAND_R`), or the
  entry is skipped with a warning.
- Validation strictness is governed by
  `population_engine_equipment_strict_load` (repo `off`): `off` = zero out the
  bad entry, keep the rest; `on` = discard the **entire** equipment DB if any
  row fails.

### 5.3 How gear is assigned to a fake player

1. A shell is created for job `X` under profile `P`.
2. `P.Jobs[X]` gives a gear-set name; the engine copies that set's ten slot
   pools onto the shell's `PopulationEngine` equipment record
   (`population_engine_resolve_equipment`). If the job isn't listed, a built-in
   class default is used.
3. At spawn, for each slot the engine picks **one random item** from that slot's
   pool (`pick_pool`), then `pc_additem` + `pc_equipitem` it onto the shell.
4. `pc_equipitem` still enforces normal **job and level restrictions**. If the
   shell's rolled `BaseLevel` / job cannot wear the rolled item, that slot ends
   up empty — so keep pools appropriate to the profile's level range.
5. Weapons drive `Movetype: 3` (hybrid) melee-vs-ranged choice and, together
   with `db/population_skill_db.yml`, the skill rotation.

### 5.4 Add or edit a gear set (pre-renewal example)

Add a new set for a Hunter build:

```yaml
  - GearSetName: prere_hunter
    HeadTop:
      - Apple_Of_Archer
      - Binoculars
    HeadMid: 0
    HeadBottom: 0
    Armor:
      - Mink_Coat
      - Coat_
    Weapon:
      - Hunter_Bow
      - Composite_Bow_
      - Great_Bow
    Shield: 0
    Garment:
      - Muffler_
      - Hood_
    Shoes:
      - Shoes_
      - Boots_
    AccL:
      - Glove
    AccR:
      - Clip
```

Then point a job at it in `db/population_engine.yml`:

```yaml
  - Profile: pve_archer
    Jobs:
      Hunter: prere_hunter
```

Verify each AegisName exists in the pre-renewal DB before saving:

```sh
grep -n "AegisName: Hunter_Bow$" db/pre-re/item_db_equip.yml
```

All items in the bundled sets have been verified against
`db/pre-re/item_db_equip.yml` / `item_db_etc.yml` / `item_db.yml`.

---

## 6. Group permissions for the commands

`@populate` and `@reloadpopenginedb` are declared but you must grant them. In
`conf/groups.yml`, add to an admin group's `commands:` map:

```yaml
        populate: true
        reloadpopenginedb: true
```

(or `@command`/`char_command` forms as your server convention requires), then
restart the map-server or `@reloadatcommand`.

---

## 7. Starting the fake players

### 7.1 Autosummon (normal operation)

No command needed. With `db/population_spawn.yml` populated and the map-server
running, the autosummon timer (registered in
`do_init_population_engine_load_databases`, fires every **10 000 ms**, first tick
~100 ms after load) spawns shells until every profile/category reaches its target
population, capped by `population_engine_autosummon_batch_size` per tick and
`population_engine_max_count` overall. It also refills as shells die (mortal) or
are removed.

Vendor shells autosummon separately from `VendorPlacement` blocks in
`db/population_vendors.yml` using the profiles in `db/population_vendor_pop.yml`.

### 7.2 `@populate` (manual, single map)

```
@populate <quantity> [map]      (alias: @fakeplayers)
@populate stop
@populate stats
@populate status
@populate timing                (wander-tick profiler)
@populate timingreset
```

- `@populate 50` — 50 shells on your current map.
- `@populate 200 prontera` — 200 shells on prontera.
- Quantity range: `1`–`5000`.
- Refuses to start if the engine is already running (autosummon counts as
  running). Use `@populate stop` first.
- `@populate` shells spawn as **visible shells with no autocombat** (they
  wander); they are not the same as autosummon combat shells.

### 7.3 Arena PvP shells (script only)

From an NPC script (`src/custom/script.inc`):

```c
.@n = population_arena_start("guild_vs1", 10);   // map, shell_count (1..25)
                                                 // optional: x, y, job_override, team_id
population_arena_stop("guild_vs1");
.@jobs$ = population_arena_jobs();               // CSV of the arena job pool
```

The arena job pool is auto-derived from `db/population_pvp.yml` entries. There is
**no `@arena` at-command** — only these three script buildins.

---

## 8. Stopping / removing the fake players

- **`@populate stop`** — stops the engine and cleans up **all** shells
  (`population_engine_stop`), autosummon included. They do not come back until
  the engine is restarted (map-server reboot) unless you re-`@populate`.
- **Disable autosummon permanently** — empty the `Body:` of
  `db/population_spawn.yml` (and remove `VendorPlacement` blocks from
  `db/population_vendors.yml`), then restart the map-server. Missing/invalid
  `population_spawn.yml` logs `autosummon disabled until fixed` and no shells
  spawn.
- **Throttle instead of stop** — lower `population_engine_max_count` and the
  `*Population` totals, then `@reloadpopenginedb` reloads profiles/chat
  (it does **not** despawn existing shells; excess shells simply aren't
  replaced as they go away).
- **Shutdown** — `do_final_population_engine()` tears every shell down cleanly on
  map-server exit; nothing is persisted.
- **Arena** — `population_arena_stop("<map>")` releases that map's arena shells.

There is no per-shell "kick" command; management is all-or-nothing plus
autosummon attrition.

---

## 9. Testing / verification

1. **Engine running?**
   `@populate status` → `Population engine is RUNNING`.
   `@populate stats` → active / created / errors, chat lines, walk failures,
   name retries, heap estimate.

2. **Shells spawning?**
   Watch the map-server console on boot for the population YAML load lines, then
   `@populate stats` a minute later — `created` should climb to your configured
   totals. `@who` / `@users` on a target map shows the shell names.

3. **On the intended maps?**
   Warp to a map listed in `db/population_spawn.yml` (e.g. `@go prontera`) and
   confirm wandering players with generated names. Cross-check counts against the
   distribution rule in §4.2.

4. **Correct classes?**
   `@jobinfo <name>` / clicking a shell shows its job sprite. Only jobs listed
   in the profile's `Jobs:` (or present on the map with a built-in default)
   should appear. Change the `Jobs:` map and `@reloadpopenginedb` +
   re-spawn to confirm.

5. **Configured gear?**
   Right-click / `@viewequip <name>` (if enabled) or observe the sprite —
   weapon, headgear, garment should be drawn from the referenced gear set. The
   map-server console prints warnings for any gear entry it had to skip
   (bad ID, wrong slot, job/level restriction).

6. **AI / behavior working?**
   - Wander: shells in a town should move on the wander tick (~every 0.5–2 s).
     `@populate timing` shows wander-tick timings and walk counts.
   - Combat: on a `FieldBehavior: combat` map, shells should engage monsters,
     use skills (if `population_engine_shell_attackskill: on` and the job has a
     rotation in `db/population_skill_db.yml`), and — if `mortal` — die and
     respawn.
   - Chat: overhead lines should appear within a chat cooldown window; whisper a
     shell by name to test `population_engine_chat_reply_enable`.
   - Vendor: `vendor` shells should show a vend shop you can buy from.

---

## 10. Changing settings later

| Goal | Edit | Apply |
|------|------|-------|
| Increase/decrease population | `*Population` in `db/population_spawn.yml`; `population_engine_max_count` in the conf | `@reloadpopenginedb` grows/limits gradually; restart for an immediate rebuild |
| Change class ratios | `Jobs:` map of the `Profile:` in `db/population_engine.yml` (add/remove job lines, or split into more profiles/spawn entries) | `@reloadpopenginedb`, then `@populate stop` + reboot for a clean re-roll |
| Change maps | `Towns:` / `Fields:` / `Dungeons:` lists in `db/population_spawn.yml`; `VendorPlacement.Map` in `db/population_vendors.yml` | restart map-server |
| Change equipment | gear-set pools in `db/population_gear_sets.yml`, or repoint a job to another `GearSetName` in the profile | `@reloadpopenginedb` (new shells only) |
| Change behavior | `TownBehavior` / `FieldBehavior` / `DungeonBehavior` / `Flags` / `Role` / `Movetype` on the profile; `population_engine_ai` bitmask + combat/wander/chat tunables in the conf | `@reloadpopenginedb` for YAML; conf changes need a map-server restart (battle_config is read at boot) |
| Change spawn frequency / ramp | `population_engine_autosummon_batch_size` (per-10 s tick cap) in the conf | restart map-server |
| Change respawn-on-death | `mortal` vs `immortal` in `Flags:` | `@reloadpopenginedb` + re-spawn |

`@reloadpopenginedb` reloads `db/population_names.yml`,
`db/population_engine.yml`, and `db/population_chat.yml`. It does **not** reload
the spawn DB, the conf tunables, or despawn existing shells.

---

## 11. Troubleshooting

**Fake players not spawning at all**
- `@populate status` — if `NOT running` and you expected autosummon, the spawn
  DB failed to load: check the console for
  `population_spawn.yml missing or invalid; autosummon disabled`.
- `population_engine_max_count` too low, or already reached.
- `db/population_spawn.yml` profile names don't match any `Profile:` in
  `db/population_engine.yml` (case-sensitive).
- Map names invalid / not loaded — they are silently skipped.
- `@populate` prints `already running` — autosummon owns the engine; use
  `@populate stop` first (this also kills autosummon shells).

**Configuration not loading / changes ignored**
- Conf tunables (`population_engine_*`) are read only at map-server boot —
  `@reloadpopenginedb` does not touch them. Restart.
- `conf/import/battle_conf.txt` missing the
  `import: conf/battle/population_engine.conf` line → every tunable falls back to
  its code default (e.g. `max_count` 1000, chat off, hat effect 0).
- Edited `db/population_spawn.yml` or `db/population_vendors.yml` and expected a
  hot reload — those need a restart.

**YAML errors**
- The map-server console names the file and (usually) the line. Common causes:
  tabs instead of spaces, a `-` list item at the wrong indent, `Header.Version`
  not `2` for `POPULATION_ENGINE_DB` files / not `1` for names & spawn.
- Validate quickly:
  ```sh
  grep -nP "\t" db/population_gear_sets.yml        # no tabs
  python3 - <<'EOF'
  import sys
  # lightweight structural sanity check (no PyYAML needed)
  bad=[i for i,l in enumerate(open('db/population_gear_sets.yml'),1)
       if l.rstrip() and not l.startswith(('#',' ')) and l.strip() not in ('Body:','Header:')]
  print('suspicious top-level lines:', bad or 'none')
  EOF
  ```

**Invalid item IDs / wrong-slot items**
- Console prints `Unknown item name '<x>'` or
  `Item <id> ... cannot be equipped in this slot`. The entry is dropped
  (`strict_load: off`) or the whole DB is discarded (`strict_load: on`).
- Verify names against the **pre-renewal** DB (this server is `#define PRERE`):
  ```sh
  grep -n "AegisName: <Name>$" db/pre-re/item_db_equip.yml
  ```
- Renewal-only "Para Team" / practice items (`Para_Team_Uniform*`,
  `Para_Team_Boots*`, `Para_Team_Manteau`, `P_Slayer*`, `P_Sabre*`, `P_Staff*`,
  `P_Bow*`, `P_Dagger*`, `P_Mace*`, `P_Katar*`, `C_BeginnerMark`) do **not**
  exist in `db/pre-re/` and must not be used. (`Para_Team_Hat`, `P_Revolver1/2`
  happen to exist in pre-re and are fine.)

**Players spawning with incorrect / no equipment**
- Slot pool empty or `0`.
- Every item in a slot pool failed validation (see above) → slot silently empty.
- `pc_equipitem` job/level restriction: the shell's rolled `BaseLevel` / job
  can't wear the rolled item. Widen the profile's `BaseLevel` range or use
  lower-requirement items.
- Weapon type wrong for the job → equips but the skill rotation / Movetype logic
  misbehaves. Match weapons to the job.
- `population_engine_equipment_strict_load: on` + one bad row anywhere →
  **all** gear sets discarded, every shell naked. Check earlier warnings.

**Build / compile errors**
- Duplicate symbols / multiple definition from `population_engine/*.cpp`: the
  build isn't excluding that directory. Confirm
  `src/map/CMakeLists.txt` has the `list(FILTER MAP_SOURCES EXCLUDE REGEX
  ".*/population_engine/.*")` line and `src/map/Makefile.in` has
  `-not -path "./population_engine/*"`, then `make clean` / clean CMake build.
- Undefined reference to `population_engine_*` / `do_init_population_engine*`:
  `src/map/map.cpp` init hooks or the `#include "population_engine.hpp"` missing.
- `battle_config.population_engine_*` unknown: `src/custom/battle_config_struct.inc`
  / `battle_config_init.inc` not applied.
- `@populate` "unknown command": `ACMD_DEF(populate)` missing from
  `src/custom/atcommand_def.inc`, or group permission not granted.

**Database / map problems**
- `cp_population_stats` errors in the map-server log: run
  `sql-files/population_engine.sql`, or ignore (cosmetic, FluxCP-only).
- Shells stuck on one cell in walled maps: raise
  `population_engine_path_attempts` (e.g. 32–64).
- Shells spawning on blocked cells: shouldn't happen (the cell picker validates
  walkability); if it does, the map cache for that map is stale — regenerate it.

---

## 12. Complete worked example

Goal: ~40 wandering, mortal **Swordsman/Mage/Archer** shells split across
prontera and geffen, plus 20 combat Knights in Geffen fields, all pre-renewal.

### 12.1 `db/population_gear_sets.yml` (reuse bundled sets, add one)

Bundled `para_swordsman`, `para_mage`, `para_bow`, `para_knight_base` already
exist and are pre-renewal. Nothing to add for this example — but if you wanted a
lighter starter sword set:

```yaml
  - GearSetName: prere_starter_sword
    HeadTop: 0
    HeadMid: 0
    HeadBottom: 0
    Armor:
      - Cotton_Shirt_
      - Mink_Coat
    Weapon:
      - Sword_
      - Blade_
    Shield:
      - Buckler_
    Garment:
      - Muffler_
    Shoes:
      - Sandals_
    AccL: 0
    AccR: 0
```

### 12.2 `db/population_engine.yml` (profiles)

```yaml
  - Profile: demo_town
    Jobs:
      Swordsman: para_swordsman
      Mage:      para_mage
      Archer:    para_bow
    NameProfile: default
    ChatProfile: default
    Hair: [0, 42]
    HairColor: [0, 131]
    ClothesColor: [0, 699]
    BaseLevel: [15, 45]
    JobLevel:  [10, 40]
    Flags:
      - mortal
    TownBehavior: wander

  - Profile: demo_knight
    Jobs:
      Knight: para_knight_base
    NameProfile: default
    ChatProfile: default
    Hair: [0, 42]
    HairColor: [0, 131]
    ClothesColor: [0, 699]
    BaseLevel: [50, 75]
    JobLevel:  [20, 50]
    Str: [55, 85]
    Agi: [40, 70]
    Vit: [50, 80]
    Int: [1, 20]
    Dex: [40, 70]
    Luk: [10, 40]
    Flags:
      - mortal
    FieldBehavior: combat
    Script: |
      setriding;
```

### 12.3 `db/population_spawn.yml`

```yaml
  - Profile: demo_town
    Towns:
      - prontera
      - geffen
    TownsPopulation: 40

  - Profile: demo_knight
    Fields:
      - gef_fild00
      - gef_fild02
    FieldsPopulation: 20
```

### 12.4 `conf/battle/population_engine.conf` (relevant lines)

```
population_engine_max_count: 200
population_engine_autosummon_batch_size: 25
population_engine_wander_enable: on
population_engine_chat_enable: on
population_engine_shell_attackskill: on
```

### 12.5 Start

```sh
mysql -u ragnarok -p ragnarok < sql-files/population_engine.sql   # optional
make clean && make server -j$(nproc)                              # if patch just applied
./login-server & ./char-server & ./map-server
```

Within ~10–20 s autosummon fills prontera+geffen with 40 wandering
Swordsman/Mage/Archer shells (20 each map, evenly) and gef_fild00/02 with 10
mounted combat Knights each.

### 12.6 Verify

```
@go prontera
@populate status      -> RUNNING
@populate stats       -> active ~60, errors 0
@who                  -> generated names, mixed jobs
@go gef_fild00        -> mounted Knights fighting monsters, dying & respawning
```

### 12.7 Tweak

- More town shells: `TownsPopulation: 80` → `@reloadpopenginedb`.
- Fewer mages: drop `Mage:` from `demo_town.Jobs` → `@reloadpopenginedb` +
  `@populate stop` + reboot for a clean re-roll.
- Different gear: repoint `Swordsman: prere_starter_sword` →
  `@reloadpopenginedb` (affects newly spawned shells).
- Turn everything off: blank the `Body:` of `db/population_spawn.yml`, restart.

---

## 13. Known gaps / things that are *not* implemented

- **`idle` behavior value** — appears in the `population_engine.yml` header
  comment but is rejected by the parser; use `sit` (stationary) or `none`.
- **No `@arena` at-command** — arena PvP shells are reachable only via the
  `population_arena_start` / `population_arena_stop` / `population_arena_jobs`
  **script** buildins.
- **No per-shell removal command** — you stop the whole engine or wait for
  autosummon attrition.
- **`@reloadpopenginedb` scope** — reloads names/profiles/chat only; not the
  spawn DB, vendor DB, or conf tunables, and never despawns live shells.
- **Conf tunables are boot-time only** — no runtime reload for
  `conf/battle/population_engine.conf`.
- **`cp_population_stats`** — purely a FluxCP display convenience; the engine
  functions without the table and without FluxCP.
