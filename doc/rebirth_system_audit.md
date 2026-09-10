# SQL-backed Rebirth System — Audit Report

Date: 2026-09-10
Scope: repeatable "Prestige Rebirth" on top of the native transcendent system.

---

## 1. Files changed

| File | Type | Change |
|---|---|---|
| `src/map/rebirth.cpp` | **new** (201 lines) | All rebirth logic: count read/clamp, cost curve, `rebirth_perform()` transaction, `rebirth_calc_bonus()` milestone bonuses. |
| `src/map/rebirth.hpp` | **new** (57 lines) | Tuning constants, result codes, function decls, storage-var name. |
| `src/map/status.cpp` | mod | `#include "rebirth.hpp"`; one call to `rebirth_calc_bonus(sd)` in `status_calc_pc_sub()` right after `pc_bonus_script(sd)`. |
| `src/map/script.cpp` | mod | `#include "rebirth.hpp"`. |
| `src/custom/script.inc` | mod | 3 buildins: `rebirth()`, `rebirth_count()`, `rebirth_cost()`. |
| `src/custom/script_def.inc` | mod | `BUILDIN_DEF` entries for the 3 buildins. |
| `npc/custom/jobmaster.txt` | mod | `Prestige_Rebirth` menu function + gated entry point; `.ThirdClass`/`.FourthClass`/`.FourthExpanded = false`; Prestige config block in `OnInit`. |
| `npc/scripts_custom.conf` | mod | Enabled `npc/custom/jobmaster.txt` (was commented out). |
| `FreshServer.sql` | mod | Doc comment + additive `char_reg_num(key)` index. **No schema change required.** |

No unrelated systems were modified.

---

## 2. SQL changes

**No schema change is required.** The rebirth counter is a standard permanent
character variable stored in the existing `char_reg_num` table:

```
char_id = <char>,  key = 'REBIRTH_TOTAL',  index = 0,  value = 0..80
```

It auto-loads on login and auto-saves on the normal save cycle / logout, exactly
like `lastJob`, bank vault, etc. It survives relog and server restart.

`FreshServer.sql` gains one **purely additive** line (mirrors the existing
`acc_reg_num` index, only speeds up "top rebirths" queries — not needed by the
feature):

```sql
ALTER TABLE `char_reg_num` ADD INDEX IF NOT EXISTS `key` (`key`);
```

### One-time migration (only if you already ran an earlier build)

An earlier build stored the counter under the key `rebirth_count`, which
**case-insensitively collided** with the `rebirth_count()` buildin name in the
script string table. The var was renamed to `REBIRTH_TOTAL`. If any character
already has a `rebirth_count` row, migrate it:

```sql
UPDATE `char_reg_num` SET `key` = 'REBIRTH_TOTAL' WHERE `key` = 'rebirth_count';
```

(Already applied to `ragnarok_main` during this audit — 1 row.)

---

## 3. Exact implementation summary

### Progression / requirements
* Prerequisite to enter the system: the character's job id is on the **explicit
  allow-list** of accepted transcendent 2nd classes at **Base 99 / Job 70**:
  Lord Knight (4008), High Priest (4009), High Wizard (4010), Whitesmith (4011),
  Sniper (4012), Assassin Cross (4013), Paladin (4015), Champion (4016),
  Professor (4017), Stalker (4018), Creator (4019), Clown (4020), Gypsy (4021).
  Everything else is rejected and never offered — trans 1st, baby, expanded,
  3rd/4th, and the mounted alt-sprite ids `JOB_LORD_KNIGHT2` (4014) /
  `JOB_PALADIN2` (4022). `rebirth_is_accepted_class()` in the source and
  `.PRebirth_Jobs` in the NPC are the two mirrored copies of this list.
* Each rebirth = native transcendent reset: `pc_jobchange(sd, JOB_NOVICE_HIGH, 1)`
  (keeps the `JOBL_UPPER` flag) + `pc_resetlvl(sd, 1)` → back to **High Novice,
  Base/Job level 1**, skills/stats reset. Zeny, inventory, storage, guild, party,
  account/char data untouched.
* `lastJob` char var is set to the **non-transcendent** 2nd-class job id
  (`pc_mapid2jobid(class_ & ~JOBL_UPPER, sex)`), so Euphy Job Master's `.LastJob`
  linear-path lock forces the exact same climb back (e.g. High Priest →
  High Novice → High Acolyte only → High Priest only). Verified across all 2nd
  classes: the id offset is a constant `+4001` (non-trans 7..21 → trans 4008..4022).
* Max **80** rebirths. 81st is rejected.

### Cost
`cost(n → n+1) = (n + 1) × 100,000` zeny.
1st = 100,000 … 10th = 1,000,000 … 80th = 8,000,000. Full 1→80 sum = 324,000,000.
Charged with `pc_payzeny()` (re-checks funds, rejects negatives).

### Anti-exploit
* **All** validation is server-side in `rebirth_perform()`; nothing trusts client
  input. The NPC only displays state and asks for confirmation.
* The whole transaction (cap → level → class → zeny charge → reset → count++ →
  recalc) runs with **no script yield**, so it cannot be raced or interleaved and
  one confirmation cannot rebirth twice. Double-dialog / spam just charges the
  next (higher) tier each time — never a free or negative-cost rebirth.
* Population-engine fake players are rejected (`population_engine_is_population_pc`)
  in both `rebirth_perform()` and `rebirth_calc_bonus()`.
* Buildin return codes: `-1` no player, `-2` maxed, `-3` level, `-4` class state,
  `-5` not enough zeny; success returns the new count (≥ 1).

### Milestone bonuses — `rebirth_calc_bonus()`
Called from `status_calc_pc_sub()` every recalc; **derived purely from the stored
count**, permanent and cumulative (reaching 80 grants all 8 tiers). No toggles/flags.

| Count ≥ | Bonus | Field written (native `bonus` equivalent) |
|---|---|---|
| 10 | +10% EXP | `indexed_bonus.expaddrace[RC_ALL] += 10` |
| 20 | +10% Drop | `indexed_bonus.dropaddrace[RC_ALL] += 10` |
| 30 | +1,000 Max Weight | `add_max_weight += 10000` (raw ×10 units = 1,000 shown) |
| 40 | +20 ATK / +20 MATK | `bonus.eatk += 20`, `bonus.ematk += 20` |
| 50 | +10% MaxHP / +10% MaxSP | `hprate += 10`, `sprate += 10` |
| 60 | +10% resist ALL statuses | `reseff[SC_COMMON_MIN..MAX] += 1000` (10%) |
| 70 | +5% physical / +5% magical atk | `bonus.atk_rate += 5`, `matk_rate += 5` |
| 80 | +5 ASPD | `bonus.aspd_add -= 50` |

Applied after equipment/card/`bonus_script` parsing and before derived-stat
finalisation (maxHP/SP, weight, ASPD, MATK), so it stacks correctly and is
re-applied automatically on login, on rebirth (`SCO_FORCE`) and every recalc.

### UI — Job Master (`npc/custom/jobmaster.txt`)
* The **"Prestige Rebirth"** menu item is shown **only when every requirement is
  met right now**: `rebirth_count() < 80 && BaseLevel ≥ 99 && JobLevel ≥ 70 &&
  inarray(.PRebirth_Jobs, Class) != -1` — the same explicit allow-list the
  source enforces. When shown, the screen displays: rebirths done / 80,
  next rebirth number, Base/Job requirement, zeny cost, next-milestone progress,
  every milestone bonus already unlocked, and a confirmation prompt before
  `rebirth()` is called.
* 3rd- and 4th-class job changes are **disabled** (`.ThirdClass = .FourthClass =
  .FourthExpanded = false`) — Transcendent 2nd class is the cap, progression
  continues only through Prestige Rebirth. Native one-time transcend
  (`.RebirthClass`) stays enabled so players can reach Trans 2nd in the first place.

---

## 4. Compile / test results

### Build
`make map -j16` → **clean link, 0 warnings/errors** for `rebirth.cpp`,
`status.cpp`, `script.cpp`. Binary `map-server` rebuilt.

### Server boot (login + char + map, new binary)
* Map server reaches `Server is 'ready'`, `Map Server is now online`.
* `Event 'OnInit' executed with '1553' NPCs` — **no script parse errors**, no
  buildin-conflict/duplicate warnings. `jobmaster.txt` (on `maintown`) loads.

### Functional test — 10 rebirths, char `admin1` (char_id 150000)
Live RO-client play-through is **not possible in this environment** (no game
client). Instead the exact state mutations of `rebirth_perform()` were applied
against the real `ragnarok_main` DB, 10 iterations, re-levelling to 99/99 between
each (as a player would), asserting every invariant and the `char_reg_num`
round-trip. Script: `scratchpad/sim_rebirth.py`.

```
  # pre-count       cost  result   zeny after  class  base/job REBIRTH_TOTAL lastJob
  1         0    100,000       1    5,900,000   4001       1/1             1       8
  2         1    200,000       2    5,700,000   4001       1/1             2       8
  3         2    300,000       3    5,400,000   4001       1/1             3       8
  4         3    400,000       4    5,000,000   4001       1/1             4       8
  5         4    500,000       5    4,500,000   4001       1/1             5       8
  6         5    600,000       6    3,900,000   4001       1/1             6       8
  7         6    700,000       7    3,200,000   4001       1/1             7       8
  8         7    800,000       8    2,400,000   4001       1/1             8       8
  9         8    900,000       9    1,500,000   4001       1/1             9       8
 10         9  1,000,000      10      500,000   4001       1/1            10       8
 total spent: 5,500,000  (zeny 6,000,000 -> 500,000)
```
Rejection paths (all correct):
* 11th with only 500k zeny → `-5` ERR_ZENY
* Base 98 / Job 69 → `-3` ERR_LEVEL
* Class High Novice 4001 / Thief High 4007 / non-trans Priest 8 → `-4` ERR_JOB_STATE
* Mounted alt-sprite ids Lord Knight2 4014 / Paladin2 4022 → `-4` ERR_JOB_STATE
* All 13 allow-listed trans-2nd ids pass the class gate (fall through to the
  zeny check) → verified
* `REBIRTH_TOTAL = 80` → `-2` ERR_MAXED
* Cost curve 1..80: first 100,000, last 8,000,000, sum 324,000,000 ✓
* Milestone thresholds fire at exactly 10/20/30/40/50/60/70/80, cumulative ✓
  (verified by code trace against the consuming rAthena code paths — see §3 table)

**All assertions passed.** `admin1` was left as a clean Transcendent 2nd class
(High Priest, 99/99, 645,950,000 z, no rebirth regs) ready for a live click-test.

---

## 5. Flaws found and fixed during the audit

1. **Class check rejected every real transcendent 2nd class.**
   `(class_ & (JOBL_UPPER|JOBL_2)) != (JOBL_UPPER|JOBL_2)` — `JOBL_2` is
   `JOBL_2_1|JOBL_2_2` and a 2nd class only ever has one of those bits.
   Fixed to `!(class_ & JOBL_UPPER) || !(class_ & JOBL_2)`.
2. **Storage var name collided with the buildin name.** `"REBIRTH_COUNT"` vs the
   `rebirth_count()` buildin — the script string table is case-insensitive, so
   the var aliased the function symbol (it round-tripped, but was fragile and
   wrote a `type=C_FUNC` str entry). Renamed to `REBIRTH_TOTAL` (+ migration SQL).
3. **Menu shown when requirements not met.** The old entry condition had a
   `rebirth_count() > 0` bypass that showed the option to a freshly-rebirthed
   level-1 High Novice. Removed; the menu now appears only when a rebirth can
   actually be performed.
4. **3rd/4th job change still offered** (e.g. Paladin → Royal Guard, which also
   errored on non-renewal). Disabled via the Job Master's own config flags.
5. **Max Weight units.** Changed `+= 1000` → `+= 10000` so the player sees
   "+1,000" (rAthena stores weight ×10). Flip back to `1000` if you meant raw.
6. **Class gate widened too far.** The flag check `(class_ & JOBL_UPPER) &&
   (class_ & JOBL_2)` also accepted the mounted alt-sprite ids `JOB_LORD_KNIGHT2`
   (4014) / `JOB_PALADIN2` (4022). Replaced with an explicit 13-id allow-list
   (`rebirth_is_accepted_class()` + `.PRebirth_Jobs`) — nothing outside the list
   you specified is accepted or offered.

---

## 6. Remaining limitations

* **No live client test.** §4's 10-rebirth run is a faithful DB-state simulation
  plus a clean server boot and a source trace, not an in-client play-through.
  The stateful rAthena calls (`pc_payzeny`, `pc_jobchange`, `pc_resetlvl`,
  `pc_setregistry`) are used exactly as the stock Job Master uses them and one
  real rebirth had already persisted correctly before the audit.
* **Job Master path-lock depends on `.LastJob = true`** (default). If you set it
  false, linear class changes stop being enforced for everyone and a rebirthed
  player could pick a different trans class on the way back up.
* **`lastJob` is shared** with the native one-time transcend feature (same var,
  by design). Fine as long as both write the non-trans 2nd-class id, which they
  now do.
* **"Resist ALL statuses" = the common status group** (`SC_STONE..SC_STONEWAIT`:
  stone/freeze/stun/sleep/poison/curse/silence/blind/bleeding/…). It reduces
  affliction chance (and, in renewal, duration) by 10%; it does not touch
  non-common debuffs.
* Milestone bonus numbers are duplicated as display text in the NPC — keep the
  NPC table and `rebirth_calc_bonus()` in sync if you retune them.
* `char_reg_num` is `ENGINE=MyISAM` (stock rAthena) — no transactional guarantee
  at the storage layer, but the rebirth transaction itself is atomic in the
  single-threaded map server and the row is written after the zeny charge.

---

## 7. Commands for you to run manually

```bash
# 1. (already built during audit, but to be sure)
cd /home/admin/FateMMO && make map -j16

# 2. Migrate any existing counter rows (safe no-op if none) — run once per DB
mysql -uadmin -padmin ragnarok_main -e \
  "UPDATE char_reg_num SET \`key\`='REBIRTH_TOTAL' WHERE \`key\`='rebirth_count';"
#   (already applied to ragnarok_main; run on any other live DB / your prod DB)

# 3. Additive index (already in FreshServer.sql for fresh installs; for existing DBs)
mysql -uadmin -padmin ragnarok_main -e \
  "ALTER TABLE char_reg_num ADD INDEX IF NOT EXISTS \`key\` (\`key\`);"
#   (already applied to ragnarok_main)

# 4. Restart the servers with the new map-server binary, then in-game:
#    - log in admin1 (left as High Priest 99/99), go to Job Master on 'maintown'
#    - "Prestige Rebirth" -> confirm -> verify count/zeny/level reset
#    - re-level and repeat; check `SELECT * FROM char_reg_num WHERE `key`='REBIRTH_TOTAL';`
```

Nothing here has been committed — all changes are in the working tree for you to
review and commit yourself.
