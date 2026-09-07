# FateShield (Gepard-style client protection) — forensic report & integration

Status: **implemented, builds clean, servers start & link**. Client-side
handshake requires the proprietary FateShield/Gepard client DLL and cannot be
exercised in this Linux build environment (documented under *Testing*).

All server code is branded **FateShield**; the on-wire protocol constants,
hashes and packet IDs are unchanged from the historical Gepard implementation
so the existing patched client keeps working.

---

## 1. Historical forensics (evidence used)

| Evidence | Location | What it proves |
|---|---|---|
| `Gepard.diff` (2418 lines) | `F:\Reverse Gepard\Gepard.diff` | The complete server-side Gepard Shield 3.0 integration, already written against **current** rAthena trunk (`CODE_VERSION 2025040101`, `PACKETVER 20250716`, `rathena::server_core`, `global_core->get_type()`, `PACKET_AC_ACCEPT_LOGIN`). This is the authoritative artifact — it *is* the "after" state. |
| `plugin.ini` | `F:\Reverse Gepard\Client File for LGP\plugin.ini` | **Not a Gepard/anti-cheat file.** It is the **RCX / LGP cell-effect config**: `[LGP::CELL_IMAGE]`, `[LGP::CELL_COLOR]`, `[LGP::CELL_FADE_OUT_TIME]` mapping AoE skill IDs (Storm Gust `0x10`, Meteor `0x11`, Lord of Vermilion `0x12`, Safety Wall, Pneuma, traps, songs…) to bitmaps and ARGB colours. Consumed by the LGP client plugin, which the server enables via `SC_FATESHIELD_SETTINGS` (0x5395). Stays in the **client root**. |
| `data/texture/lgp/*.bmp` | same folder | Plain 40×40 BMP cell markers (`circle/square/aoes/none/default_image.bmp`, ~4.8 KB each). Not encrypted, not packed. These are the LGP "selected resources". |
| `rdl.dll` | `F:\Reverse Gepard\rdl.dll` | **Not a Gepard component.** PDB path `…\August DiscordRPC_20250604\Lothbrok\Release\rdl.pdb`; strings `[Lothbrok DiscordRPC]`, `bigfootrpc`, Discord IPC pipe handling, `IGN: %s`, `Lvl: %d | Job Lvl: %d`, app id `1541308574607876179`, invite `discord.gg/ER68AVbBwk`. It is a **Discord Rich Presence** plugin that reads IGN/level/job from `FateMMO.exe` `.data` offsets (see memory `discord-rpc-offsets.md`). It does not hook security, validate integrity, or talk to the map-server. |
| `WARP0716-main/` (`Settings.yml`, `LastSession.yml`, `FateMMO.epi`) | `F:\Reverse Gepard\WARP0716-main` | **WARP** client patcher. Patches `2025-07-16_Ragexe_175220998_unpacked_community.exe` → `FateMMO.exe`. Inputs are ordinary client customisations: `$newclientinfo`, `$viewIDLimit=64000`, `$maxHomun=7000`, `$customWindowTitle`, `$translationFile`, `$dataINI`, `$copyNCDdll`, `$newItemInfo/$newMapInfo/...`. **No Gepard patch is applied by WARP.** Gepard protection is added separately by the Gepard client DLL + loader that already ships in the RO client root. |
| `FateMMO.exe.secure.txt` | `WARP0716-main/` | Hash manifest (MD5/SHA1/SHA256/SHA512/SHA3/CRC32/CRC64) of the *patched* exe — an integrity reference for the operator, not a runtime component. |
| FateShield GitHub repo | `github.com/ta0baakoinay/FateShield` | **Empty** (no branches, no commits) — nothing to import. |
| SphinxRO commits `80110d4…` / `f5ba7a3…` | — | Not reachable in any public repo (FateRO, gepard-me/rathena, SphinxRO). The `Gepard.diff` supersedes them. |

### What the historical Gepard server code actually does (from `Gepard.diff`)

* **Session crypt** — `struct gepard_crypt_unit` (256-byte keystream + 3 positions) per
  direction (`recv_crypt` / `send_crypt` / `sync_crypt`) stored on `socket_data`.
  `gepard_session_unit()` seeds it from a 16-bit key; `gepard_enc_dec()` is the
  stream cipher. Keys are exchanged in `SC_GEPARD_INIT` (0x4753).
* **Handshake** — server sends `SC_GEPARD_INIT` with `LICENSE_ID`, `CODE_VERSION`,
  `PACKETVER`, `start_tick`, server type (`GEPARD_LOGIN`/`GEPARD_MAP`) and a
  data hash. Client replies `CS_GEPARD_INIT_ACK` (0xC392) carrying the
  **unique id**, license version, MAC bytes, the client's *real* move/skill/action
  packet ids, and optionally the real client IP (proxy-aware). Bad hash →
  `gepard_send_info(GEPARD_INFO_INVALID_INIT_ACK)`.
* **Per-packet encryption** — `gepard_process_cs_packet` / `gepard_process_sc_packet`
  decrypt/encrypt only the movement / action / skill / whisper / login / unit
  idle+walking / state-change packets (the ones bots forge), leaving the rest
  untouched for performance.
* **Sync / anti-nodelay** — `CS_GEPARD_SYNC` (0x8285) & `CS_GEPARD_SYNC_2` (0x8280,
  128-byte) update `sync_tick`; `clif_fateshield_process_packet` drops the
  session (`clif_authfail_fd(...,15)`) if no sync for 40 s. Sync-2 also carries
  client tamper/diagnostic reports → `fateshield_report_log`.
* **Unique-id ban** — `login`/`char` link packets `0x5000-0x5004`
  (`FATESHIELD_M2C_BLOCK_REQ` … `_SAVE_REPORT`). Tables `fateshield_block`,
  `fateshield_block_log`, `fateshield_report_log`; `login.last_unique_id`,
  `login.blocked_unique_id`, `loginlog.unique_id`. Ban is checked at
  `CS_GEPARD_INIT_ACK` time on the login-server (`account_fateshield_check_unique_id`).
* **GM tools** — `@fateshield_block_nick / _account_id / _unique_id` and the three
  `_unblock_*` counterparts; script command `get_unique_id()`.
* **Auth-log rework** — every `login_log()` call in the login client/char path is
  duplicated as `login_fateshield_log()` which also stores `unique_id`.
* **LGP module** — `SC_FATESHIELD_SETTINGS` (0x5395) tells the client to turn the
  LGP overlay on; `clif_getareachar_skillunit` tweaks Storm Gust / LoV / Pneuma
  cell reporting; skill-unit moves emit `clif_skill_delunit` so the client
  re-renders LGP cells.
* `PACKET_OBFUSCATION` is **disabled** in `src/config/packets.hpp` — Gepard does its
  own transport crypto and the two are mutually exclusive.

---

## 2. Current implementation

### Files modified (server)

```
conf/battle/feature.conf                     autobuff_gepardlimit -> autobuff_fateshieldlimit (rename only)
src/config/packets.hpp                       disable PACKET_OBFUSCATION
src/common/socket.hpp / .cpp                 crypt units, handshake, init, per-packet enc/dec, config reader
src/char/char_mapif.cpp                      block / unblock / save-report link handlers (0x5000-0x5004)
src/login/account.hpp / .cpp                 auto-create tables + columns, ban check, last_unique_id update
src/login/loginlog.hpp / .cpp               login_fateshield_log(), loginlog.unique_id auto-column
src/login/login.cpp                          startup/shutdown logging via login_fateshield_log
src/login/loginchrif.cpp / loginclif.cpp    fateshield packet interception in the login parser + auth paths
src/map/chrif.hpp / .cpp                     req/ack block+unblock, save-report (map<->char)
src/map/clif.hpp / .cpp                      clif_fateshield_process_packet(), clif_fateshield_send_lgp_settings(), init on WantToConnection, parser hook
src/map/atcommand.cpp                        6 @fateshield_(un)block_* commands
src/map/pc.cpp                               send LGP settings on pc_reg_received
src/map/script.cpp                           get_unique_id() script command
src/map/skill.cpp                            LGP cell re-render on skill-unit move (2 call sites)
src/map/autobuff.cpp, autocombat.hpp,        pre-existing "gepard" wording -> "fateshield" (comments / stub
  battle.cpp, battle.hpp,                    config key / enum label only; no behaviour change)
  population_engine_combat.{cpp,hpp}
```

### Files added

```
conf/fateshield.conf        -> `fateshield_enabled: yes`  (auto-created if absent)
sql-files/fateshield.sql    -> reproducible schema (also auto-applied on login-server start)
doc/fateshield_integration.md
```

### Naming

`gepard` → `fateshield`, `Gepard` → `FateShield`, `GEPARD_` → `FATESHIELD_`
across all touched files. **Protocol values are untouched**: packet ids
(`SC_FATESHIELD_INIT = 0x4753`, `CS_FATESHIELD_INIT_ACK = 0xC392`, sync
`0x8285`/`0x8280`, link `0x5000-0x5004`, LGP settings `0x5395`), `LICENSE_ID`,
`CODE_VERSION`, and all crypt/hash constants are byte-for-byte identical, so the
existing patched client is compatible with no client change.

### Deviations from the historical diff (with reason)

| Historical hunk | Decision | Reason |
|---|---|---|
| Remove custom `@ignoredrop` / `OPTION_IGNOREDROP` / `IGNORE_DROP` / `clif_send_sub` `bool_` plumbing | **skipped (already satisfied)** | Current FateMMO trunk has no `ignoredrop` feature at all — those removal hunks are no-ops here. |
| `skill_dance_overlap()` forced `return 0` | **not ported** | Invasive gameplay change (kills dissonance/song overlap cancellation) that the current signature (`skill_unit&`, `e_dance_overlap`) doesn't match; LGP works without it. |
| `clif_skill_delunit(unit1)` on cell move | **ported, adapted** to `clif_skill_delunit(*unit1)` for the current reference-taking signature. |
| `account_gepard_check_license_version()` | declared only (as in the original) — no definition, no call. Left as-is. |

---

## 3. Security coverage

| Historical capability | Status | Notes |
|---|---|---|
| Dynamic packet encryption (per-session stream cipher) | **Implemented** | `fateshield_enc_dec`, 3 crypt units per session. Server half fully ported. |
| Dynamic key handshake / init-ack | **Implemented** | `SC_FATESHIELD_INIT` / `CS_FATESHIELD_INIT_ACK`, data-hash validated. |
| Packet authentication (only forge-prone packets crypted) | **Implemented** | move/action/skill/whisper/login/unit/state packets. |
| Unique player id (not MAC-based) + blocking | **Implemented** | client-provided `unique_id`; `fateshield_block*` tables; `@fateshield_block_*`. MAC bytes are stored but not used for identity. |
| Nodelay / sync-loss protection | **Implemented** | 40 s sync watchdog in `clif_fateshield_process_packet`. |
| Malformed / crafted packet hardening | **Implemented (server half)** | size guards on every fateshield packet; oversized `CS_*_INIT_ACK` → `set_eof`. |
| Client crash / diagnostic reporting | **Implemented** | `CS_GEPARD_SYNC_2` report → `fateshield_report_log`. |
| LGP / selected-resource overlay | **Implemented** | `SC_FATESHIELD_SETTINGS`; cell re-render on move; skillunit reporting tweaks. `plugin.ini` + `data/texture/lgp/*.bmp` stay client-side. |
| Automatic SQL/config initialisation | **Implemented** | `conf/fateshield.conf` auto-written; tables/columns auto-created on login-server boot; `sql-files/fateshield.sql` for reproducible deploys. |
| Auth-log with unique id | **Implemented** | `login_fateshield_log` + `loginlog.unique_id`. |
| EXE / code-section integrity, DLL integrity, anti-DLL-injection, WPE/RPE/OpenKore, VM block, cheat-tool block, active-window limit, mouse/keyboard-emulation block, Warsaw/RCX compat, `!ping` `!vsync` `!crash`, older RagexeRE support | **Requires proprietary client component** | These live entirely inside the Gepard/FateShield **client** DLL. The server only sees the encrypted stream + `unique_id` + reports. Nothing to port server-side; not reproducible from the available evidence without the proprietary module. `FateMMO.exe.secure.txt` gives operators a hash to pin for manual exe-integrity checks. |
| `rdl.dll` behaviour | **Not applicable** | Discord Rich Presence plugin, unrelated to protection. |
| Warp / launcher requirement | **Not required for protection** | WARP only patches client customisations; it does not carry Gepard. |
| Auras / Color Nicks / Color Items | **Not present** in `Gepard.diff` | No evidence these were server-side Gepard features in this codebase. |

---

## 4. Testing performed

| Test | Command | Result |
|---|---|---|
| Clean build, all servers | `make server -j16` | **PASS** — login-server, char-server, map-server, web-server link with no new errors/warnings. |
| SQL migration | `mysql … < sql-files/fateshield.sql` (+ split loginlog ALTER into log db) | **PASS** — `fateshield_block`, `fateshield_block_log`, `fateshield_report_log` created in `ragnarok_main`; `login.last_unique_id`, `login.blocked_unique_id` present; `loginlog.unique_id` present in `ragnarok_logs`. |
| login-server startup | `./login-server` | **PASS** — "login-server is ready", char-server auth accepted, no SQL errors (auto-init found tables already present). |
| char-server startup + link | `./char-server` | **PASS** — "char-server is ready", connected to login-server, received 1242 maps from map-server. |
| map-server startup + link | `./map-server` | **PASS** — "Server is 'ready'", logged on to char-server, 1541 `OnInit` NPCs, "Map Server is now online", castles/clans received. |
| Existing systems intact | startup logs | Population engine (fake players), AutoCombat, AutoBuff, NPCs/scripts, guild castles, clans all load. `PACKET_OBFUSCATION` now off (Gepard requirement). |
| Regression scan | `grep -i "error\|crash\|SQL" fs_*.log` | none. |

### Not tested (environment limits — stated explicitly)

* **Client↔server FateShield handshake, packet crypt, sync watchdog, unique-id
  ban enforcement, LGP overlay** — need the proprietary FateShield/Gepard client
  DLL + a Windows RO client; not runnable on this Linux build host. Evidence the
  server half is correct: it is a line-accurate port of the shipped `Gepard.diff`
  (same constants/packet ids/hashes) and compiles + boots against the exact
  trunk the diff targets.
* Map **sharding (East/West)** was not started in this test (single map-server
  instance used). No FateShield code touches the shard split — `fateshield_init`
  runs per map-server instance exactly like the stock parser hook — so the
  existing multi-instance setup is unaffected. Start it the usual way.
* `@fateshield_*` atcommands were verified only by successful registration in the
  build (`ACMD_DEF(fateshield_block_nick)` … compile), not by in-game execution.

---

## 5. Deployment

### Server operator

1. `git pull` this branch, `make server -j16` (or `./configure && make server`).
2. Ensure `conf/fateshield.conf` contains `fateshield_enabled: yes` (auto-created
   on first boot if missing). Set `no` to fully disable — all hooks are guarded
   by `is_fateshield_active`.
3. Import `sql-files/fateshield.sql` — `login`/`fateshield_*` parts into the
   **login** DB (`login_server_db`), the `loginlog` ALTER into the **log** DB
   (`log_db_db`). Skippable: the login-server auto-creates them on start.
4. Keep the existing East/West sharded start scripts unchanged.
5. Optional: pin the client hash from `FateMMO.exe.secure.txt` in your patcher /
   launcher checks.

### Client (unchanged — user already has these)

RO client **root** must contain, as today:

```
FateMMO.exe                     (WARP-patched from 2025-07-16 Ragexe 175220998)
plugin.ini                      (LGP cell-effect config — stays in root)
rdl.dll                         (Discord Rich Presence — optional, cosmetic)
<Gepard/FateShield client DLL + loader files>   (proprietary, already in your client)
<LGP .lgp resource file(s)>     (already prepared)
data\texture\lgp\*.bmp          (circle/square/aoes/none/default_image.bmp)
*.dll, *.grf, DATA.INI, clientinfo.xml, System*, ...
```

No client rebuild or re-patch is needed for this server change — the wire
protocol is identical to the historical Gepard build.

---

## 6. Maintenance / rebasing on future rAthena

* All additions are fenced with `// (^~_~^) FateShield Start` … `End` (and
  `// (^~_~^) LGP Start/End`). `grep -rn "(^~_~^)" src/` lists every touch point
  (~30 hunks across 20 files).
* The integration is a mechanical re-application of `F:\Reverse Gepard\Gepard.diff`
  with `gepard`→`fateshield` renaming. When rAthena moves:
  1. `git apply --3way` / `patch --fuzz=3` the fenced hunks; only *context* drifts,
     never the design.
  2. Watch the four fragile spots: `chrif_parse` loop body, `clif_parse` head,
     `socket_data` struct, `skill_unit_move_unit_group` (reference vs pointer for
     `clif_skill_delunit`).
  3. Protocol constants in `src/common/socket.hpp` must never change value.
* If a future rAthena re-introduces `PACKET_OBFUSCATION` by default, keep it
  disabled while FateShield is active.
* Keep the DB schema in `sql-files/fateshield.sql` in sync with
  `account_fateshield_init` / `loginlog_fateshield_init` if columns are added.
