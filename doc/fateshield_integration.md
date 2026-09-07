# FateShield (Gepard-style client protection) — forensic report & integration

Status: **implemented; builds clean; login + char + single and East/West-sharded
map-servers start, inter-link and run; session cipher round-trip and ban-check
SQL path verified on Linux.** The end-to-end client↔server handshake and the
LGP overlay still require a Windows RO-client runtime test (see §4).

All server code is branded **FateShield**; the on-wire protocol constants,
hashes and packet IDs are unchanged from the historical Gepard implementation
so the existing patched client keeps working.

"Gepard" below refers only to historical / proprietary evidence; the delivered
system is **FateShield**.

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

### 1a. Windows client-file forensics — full inventory of `F:\Reverse Gepard`

Every file in the folder was inspected (`objdump -h/-p`, `cmp`, `strings`, byte
signature scan for the Gepard constants `LICENSE_ID 0xFF3E0C42`,
`SESSION_CONST_1 0x5E8EBDC4`, hash consts, sync magic `0xDDCCBBAA`).

| File | Size | Type / role | Evidence |
|---|---|---|---|
| `FateMMO.exe` (= `WARP0716-main/FateMMO.exe` = `FateMMOx.exe`, md5 `bb6421df…`) | 16,147,456 | The shipping game client — stock Ragexe 2025-07-16 with **WARP** patches only. | `objdump -h`: sections **byte-identical in size** to the stock exe except WARP's `.xdiff` (+3 KB) and `.lotus` (present in both). `cmp -l` vs stock = 4 894 differing bytes, all in the PE header + `.xdiff`. **No `gepard`/`shield`/`unique_id`/`LGP`/`plugin.ini` strings; no Gepard byte constants.** `shield`/`nProtect`/`GameGuard` strings appear identically (38×/2×/9×) in **both** exes → stock Gravity anti-cheat stubs, not Gepard. |
| `WARP0716-main/2025-07-16_Ragexe_175220998_unpacked_community.exe` | 16,144,384 | Stock unpacked Ragexe (already WARP-preprocessed — has `.lotus`+`.xdiff`). Patch source. | `objdump -p` import table = only stock RO DLLs (`ws2_32`, `d3d9`, `granny2`, `Mss32`, `binkw32`, `wininet`, …). |
| `rdl.dll` | 61,440 | **Discord Rich Presence** ("Lothbrok DiscordRPC" / `bigfootrpc`). **Not security, not networked.** | `objdump -p`: **one export `bigfootrpc`**; imports **only** `KERNEL32` + `MSVCP140`/`VCRUNTIME140`/CRT — **no `ws2_32`, no `wininet`** → cannot talk to any server. Talks to the local Discord IPC named pipe (KERNEL32 `CreateFile`). Reads IGN/level/job from `FateMMO.exe` `.data` (memory `discord-rpc-offsets`). Loaded by a WARP "CustomDLL" patch that injects `rdl.dll` + `bigfootrpc` into the client import table (strings `rdl.dll`, `bigfootrpc`, `GetPrivateProfileStringA`, `Shlwapi.dll` are the only new import-side strings in `FateMMO.exe`). |
| `WARP0716-main/win32/*.dll` — `Qt5Core/Gui/Qml/Network/Quick/…`, `GATE.dll`, `libEGL`, `libGLESv2`, `d3dcompiler_47`, `msvcp140`, `vcruntime140`, `qwindows.dll`, `qml/*plugin.dll`, `YAML.dll` | — | **The WARP patcher application's own Qt 5 runtime.** Not part of the game client. | `GATE.dll` `objdump -p`: imports `Qt5Qml.dll`/`Qt5Core.dll`, 299 exports — a QML plugin. Sits beside `WARP.exe` / `WARP_bench.exe` / `WARP_console.exe`. |
| `WARP0716-main/FateMMO.epi`, `Settings.yml`, `LastSession.yml` | — | WARP patch recipe. Applied patches are ordinary client mods: `TranslateClient`, `IncrViewID`, `CustomInventoryLimit`, `CustomWinTitleX`, `DataFolderFirst`, `GRFsFromIni`, `NoGravityLogo/Ads`, `UseOldLogin`, **`NoNagle`** (client TCP_NODELAY — the abuse that FateShield's server-side 40 s sync watchdog counters), `RestoreSongsEffect0X`, `CustomFadeDelayPLg`, … **No anti-cheat / Gepard / FateShield patch in the list.** | `strings FateMMO.epi`; `diff` of `strings` vs stock exe. |
| `Client File for LGP/plugin.ini` | 4,836 | LGP cell-effect config (see main table). **`FateMMO.exe` never references `plugin.ini`** → it is read by a *separate* client module (RCX-style / the Gepard-client LGP component), which is **not in this folder**. | string scan of both exes + all DLLs: zero `plugin.ini` hits. |
| `Client File for LGP/data/texture/lgp/*.bmp` | 4,854 ea | LGP source cell markers (`circle/square/aoes/none/default_image.bmp`). Plain BMP, not packed/encrypted. | `find`, header bytes. |
| `Gepard.diff` | 67,267 | The historical **server** patch (RE reference — see main table). | — |
| `Fate Shield Logo Byson.jpg` | 726,407 | Branding artwork. | — |

**Conclusion (client side): there is NO Gepard/FateShield client-protection DLL
in `F:\Reverse Gepard`.** The only DLLs present are `rdl.dll` (Discord RPC) and
WARP's Qt tooling. `FateMMO.exe` itself carries **no** Gepard code — it is a
WARP-customised stock client. Therefore:

* the FateShield **client** component (packet-crypt endpoint, integrity checks,
  anti-injection, the `plugin.ini`/LGP renderer, the init-ack responder) is a
  proprietary module that lives only in the user's **deployed** RO client root
  and could **not** be statically analysed here — it is genuinely absent from
  this folder, not "assumed missing";
* nothing in the available client files performs EXE/DLL integrity validation,
  packet encryption, or server authentication — those are all inside that
  absent module;
* `rdl.dll` is conclusively **not** part of the protection architecture.

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

## 4. Testing

Results are split into the three categories requested.

### 4.1 Actually runtime-tested on this Linux host — PASS

| Test | Command | Result |
|---|---|---|
| Clean build, all servers | `make server -j16` | login-server, char-server, map-server, web-server link; no new errors/warnings. |
| SQL migration | `mysql < sql-files/fateshield.sql` (login/`fateshield_*` → `ragnarok_main`; `loginlog` ALTER → `ragnarok_logs`) | `fateshield_block`, `fateshield_block_log`, `fateshield_report_log` created; `login.last_unique_id` / `login.blocked_unique_id` present; `loginlog.unique_id` present. |
| login-server boot + auto-init | `./login-server` | "login-server is ready"; `account_fateshield_init` / `loginlog_fateshield_init` ran with `fateshield_enabled: yes`, no SQL errors; char-server auth accepted. |
| char-server boot + link | `./char-server` | "char-server is ready"; connected to login-server. |
| **single** map-server boot + link | `./map-server` | "Server is 'ready'" (port 5121); logged on to char-server; 1541 `OnInit` NPCs; "Map Server is now online"; 1242 maps; castles/clans received. |
| **East/West sharded** map-servers | `./map-server --map-config conf/map_athena_a.conf` + `./map-server2 --map-config conf/map_athena_b.conf` (per `athena-start`) | **BOTH shards up:** shard A port 5121 = 992 maps, shard B port 5122 = 250 maps; both "logged on to Char Server", both "Map Server is now online"; char-server: "Map-Server 1 connected: 992 maps … port 5121", "Map-Server 0 connected: 250 maps … port 5122"; guild castles + clans replicated to both. **No FateShield/SQL/dup errors in any of the 4 logs.** |
| Existing custom systems intact | boot logs (single + sharded) | Population engine / fake players, AutoCombat, AutoBuff, NPCs/scripts, guild castles, clans all load on both shards. |
| `PACKET_OBFUSCATION` disabled | build + boot | required by FateShield transport crypto; no obfuscation-key warnings. |
| **Session cipher round-trip** | `gcc fs_crypt_test.c && ./fs_crypt_test` — `fateshield_session_unit` + `fateshield_enc_dec` extracted verbatim from `src/common/socket.cpp` | **PASS 5000/5000**: two units seeded from the same session key; `enc` then peer-`dec` reproduces the plaintext for random sizes 1–800 B. Confirms the cipher and key-schedule are self-consistent and symmetric. |
| **Ban-check SQL path** | seed `fateshield_block` row + run the exact `SELECT unban_time,reason FROM fateshield_block WHERE unique_id=?` and `UPDATE login SET last_unique_id=?` from `account_fateshield_check_unique_id` / `account_fateshield_update_last_unique_id` | **PASS**: row inserted, selected back with correct `unban_time`/`reason`, `last_unique_id` written and read back; cleanup ok. The schema + queries the ban logic depends on are correct. |
| `@fateshield_*` atcommands | build | 6 commands compile + register (`ACMD_DEF(fateshield_block_nick)` …). In-game execution not run (needs a client). |

### 4.2 Verified through source / static / client-file analysis (not executed end-to-end)

| Item | What static analysis establishes | What it does **not** establish |
|---|---|---|
| **Client↔server handshake** | Server half runtime-loads: `SC_FATESHIELD_INIT` (0x4753) is emitted from `fateshield_init()` on `clif_parse_WantToConnection` (map) and on first login packet (login); `CS_FATESHIELD_INIT_ACK` (0xC392) parsing + data-hash check compiled in and reachable (`logclif_parse` / `clif_fateshield_process_packet`). Wire layout fully known from `Gepard.diff`. Client-file analysis shows **`FateMMO.exe` contains no handshake code**, so the responder is the absent proprietary DLL. | That a real client actually completes INIT→INIT_ACK, that field offsets/`LICENSE_ID`/`CODE_VERSION`/`PACKETVER` match the client build, that the data-hash agrees. **Needs Windows runtime.** |
| **Packet crypto** | Algorithm verified correct & symmetric on Linux (§4.1 round-trip). `fateshield_process_cs_packet` / `_sc_packet` select exactly the move/action/skill/whisper/login/unit/state packet ids for enc/dec; the rest pass through. | That the client's stream stays byte-synchronised with the server over a live session (depends on both sides enc/dec-ing the identical set of packet ids in the identical order). **Needs Windows runtime.** |
| **Sync watchdog** | `clif_fateshield_process_packet` computes `gettick() - sync_tick`; `> 40000` ms → `clif_authfail_fd(fd,15)` (disconnect). `CS_FATESHIELD_SYNC` (0x8285) / `_SYNC_2` (0x8280) refresh `sync_tick`; sync-2 also routes tamper reports to `fateshield_report_log`. Compiled + reachable from the map parser. | That a real client sends sync at the expected cadence and that a stalled/no-delay client is actually dropped at 40 s. **Needs Windows runtime.** |
| **Ban enforcement** | SQL schema + every query runtime-verified (§4.1). Decision logic source-verified: on `CS_FATESHIELD_INIT_ACK`, login-server calls `account_fateshield_check_unique_id`; if `now <= unban_time` it clears `is_init_ack_received` and sends `FATESHIELD_INFO_BANNED`; map<->char link packets `0x5000-0x5004` drive `@fateshield_block/unblock_*`; live sessions matching a newly-blocked `unique_id` are scrambled + kicked in `chrif_fateshield_ack_block`. | That a client presenting a blocked `unique_id` is actually refused at connect, and that the client renders the ban dialog. **Needs Windows runtime.** |
| **LGP overlay** | Server half loads: `SC_FATESHIELD_SETTINGS` (0x5395, `LGP=1, mode=1`) sent from `pc_reg_received`; `clif_skill_delunit` re-render calls on skill-unit move compiled in; `clif_getareachar_skillunit` SG/LoV/Pneuma tweaks active. `plugin.ini` + `data/texture/lgp/*.bmp` inspected — plain config + BMPs, no prep needed. | That the client LGP module consumes `SC_FATESHIELD_SETTINGS` and renders the cell bitmaps/colours from `plugin.ini`. The consuming module is **not** in `F:\Reverse Gepard` (`FateMMO.exe` doesn't reference `plugin.ini`). **Needs Windows runtime + that client module.** |
| No client-side EXE/DLL integrity, anti-injection, VM/cheat-tool block in the available files | Confirmed by full inventory of `F:\Reverse Gepard` (§1a): `FateMMO.exe` = WARP-customised stock client, `rdl.dll` = Discord, rest = WARP Qt tooling. | Whether the user's *deployed* client root contains the proprietary module that does these. Not inspectable here. |

### 4.3 Requires actual Windows runtime testing (RO client connected to the server)

* Full INIT / INIT_ACK handshake against a live client, including field-offset and
  `LICENSE_ID` / `CODE_VERSION` / `PACKETVER` agreement and the data-hash check.
* Live-session packet-crypto synchronisation over movement/skill/whisper traffic
  (stream stays aligned for the whole session).
* Sync watchdog actually disconnecting a stalled / nodelay client at 40 s;
  tamper-report round-trip into `fateshield_report_log`.
* Ban enforcement: a client with a blocked `unique_id` refused at connect;
  `@fateshield_block_nick/_account_id/_unique_id` kicking a logged-in player;
  `@fateshield_unblock_*` restoring access; `get_unique_id()` in a script.
* LGP overlay rendering from `plugin.ini` + `data/texture/lgp/*.bmp` after
  `SC_FATESHIELD_SETTINGS`.
* `login_fateshield_log` writing real `unique_id`s into `loginlog`.
* Multiple legitimate client windows within the configured limit (the limit
  itself is enforced by the proprietary client module, not the server).

### 4.4 Map-sharding (East/West) compatibility review

Code review of every FateShield hunk against the multi-instance model, plus the
live 2-shard run in §4.1:

| Concern | Finding |
|---|---|
| Multiple map-server instances | `fateshield_read_configs()` runs in `socket_init()` — once per process, same as stock. `fateshield_init()` is per-session on `clif_parse_WantToConnection`. No global/singleton or cross-process state. **OK** — both shards initialised independently. |
| East/West shard config / separate ports | FateShield adds nothing to `map_athena_*.conf` parsing or `map_port`. Shard A (5121) and Shard B (5122) both came up. **OK.** |
| Player sessions | State lives entirely on `socket_data` (`fateshield_info`, 3 `*_crypt` units) — per-fd, allocated by the stock socket layer. A player is only ever on one shard at a time. **OK.** |
| Inter-server communication | New map↔char link packets `0x5000-0x5004` reuse the existing `char_fd` channel and are dispatched per instance in `chrif_parse`; the char-server handles them per connected map-server. Block broadcast (`chrif_fateshield_ack_block`) iterates that instance's own `mapit_getallusers()`, so a block issued on one shard reaches players on that shard; a cross-shard target is covered because the ban is also persisted to `fateshield_block` and re-checked at (login-server) connect time. **OK.** |
| Map ownership / `maps_b.conf` split | FateShield never enumerates maps or assumes a map set. **OK.** |
| DB access from two map-servers | Map-servers issue no FateShield SQL (all FateShield SQL is on login/char). `fateshield_block` writes come only from the char-server. No write contention introduced. **OK.** |

Live result: 4-process run (`login` + `char` + `map-server`@5121/992 maps +
`map-server2`@5122/250 maps) — both shards online, castles/clans replicated,
zero FateShield/SQL/duplicate errors.

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
<FateShield client protection DLL + loader>     (proprietary; NOT present in
                                                 F:\Reverse Gepard — it lives in
                                                 your deployed client root and is
                                                 what reads plugin.ini / does the
                                                 INIT_ACK, crypto, integrity)
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
