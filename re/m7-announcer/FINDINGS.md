# Announcer sync: how lines and takes are chosen, and how to get "same lines, same time" (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`, own Ghidra copy in `ghidra\` (don't commit), capstone).
C = confirmed in bytes/decompile, I = inferred. All hook slots below hold the same values in the on-disk `re\bin\SSASR.exe`
(.rdata not encrypted; `scripts\slots.py`); none is near the Roster Pack's 12 fingerprinted ranges.
User's goal (2026-10-22): every screen hears the same announcer line at the same moment (course intro and race commentary).

## Short answer
- **Take pick (C):** the MSVC CRT `rand()` (`0x45f3e0` -> `0x64aaad`, per-thread `_holdrand` at ptd+0x14, LCG 0x343fd/0x269ec3),
  inside **iSAL2_SoundInfoGroup::Select `0x476830`** (vtable `0x6a4fc0`, slot +0x1c = **data slot `0x6a4fdc`**). All 710
  commentary groups in every language XML are `perf_type RANDOM type=shuffle norepeat=1`: `rand() % count` with a per-take
  "played" flag, up to 10 draws for an unplayed take, flags reset when all played. Reseeding is not enough (session history
  per PC, many other rand() users).
- **Force the take (C):** hook data slot `0x6a4fdc`, filter return address **`0x46d423`** (the only commentary call site, the
  `call edx` at `0x46d421` in the resolver `0x46d3e0`); return the wanted child hash.
- **Course intro:** no message needed: every PC picks the INT1/INT2 take from the race-contract seed in that hook; TrackIntro
  enter runs at the same moment everywhere.
- **Race commentary:** the host is the only commentator. Host capture: Select slot `0x6a4fdc` + SoundInfo::GetSource slot
  **`0x6a5530`** (`0x479a90`), both filtered on `0x46d423`: cue (entry hash), take (SoundInfo hash), wait ms. Guest
  suppression: **`[[0xc29fc0]+0x1350] = 1`** (the game's own "commentary muted", stock native 095f sets it; every request checks
  it). Guest playback: stock `0x46d6b0(CommentMan, entry)` with the take forced through the Select hook.
- **Driver voice (VO)** uses the same speech queue (C): mirror it for identical timing. Guest: per-car VO cooldowns clamped to 0
  (data). Host: capture new VO slots in the speech-queue Update hook, data slot **`0x6a4650`**. Guest replay: `0x474a00(0)` then
  `0x45fef0`. The VO take is drawn when the line starts (CRT rand in SounderGroup::Play, slot `0x6a4718`); forcing it is optional.
- **Corrections to `re\m6-reactions` §2 (C):** `0x4bc290` is the local player's **lap** callout (LAP2/LAP3/LAP4/FLAP) + lap
  jingle + **queue flush**, not a position callout. "Near view" `0x4bbe10` is always true with a local player. "Item get/fire"
  fire only for the All-Star (ALLST/ALLSTU). `0x4bcd90` is ALLSTM on racer message 0x28, not a lap line. No "you're in 3rd" line.

## 1. Request chain and data (C)
`0x4bbec0(cue, waitMs, flush)` cdecl. Gates in order: `[0xc29fdb]==0` (dormant mute, also mutes the countdown voice); game type
1 or 5, or state 0x2c on the stack; no RestartRace (0x35) on the stack; **`[[0xc29fc0]+0x1350]==0`** at `0x4bbf01`
(`a1 c0 9f c2 00 80 b8 50 13 00 00 00 75 1e`); then `flush ? 0x460000()` (drops every queued line but the playing one, via
`0x46e2a0`); then `0x45ffb0` -> `0x46db60` (entry by `0x46d5e0`) -> `0x46daf0(entry, waitMs)`. The race end `0x4bce30` inlines
the same gates (+0x1350 at `0x4bcf13`, `0x4bd015`) and calls `0x45ffb0` directly.

Entries built in `0x4bd300` from the exe table `0x6aa8d8`: 701 x 16 bytes `{hash, b4, prob, cooldown ms, pauseAfter}`, ending
at `0xd3f77803` (`out\comtables.txt`). Each entry has one item `{own hash, 0.0f}`: **entry hash = soundinfo_group id**
(e.g. GAIN_SONIC); 21 single-take entries are a SoundInfo (race: COM_ALL_T_40/41/42).
Entry: +4 hash; +0xc/+0x10/+0x14 items/count/cap; +0x1c prio 0x80 (never sorted by); +0x1d probability (255 most, 191
OFFTK_*/WNGWY_*, 127 some GEN_*); +0x20 cooldown ms (mostly 30000); +0x24 wait ms; +0x28 link (INT1/INT2); +0x2a pause-after;
+0x30 elapsed ms; +0x34 deferred; +0x35 active; +0x38 CommentMan.
`0x46daf0`: active and cooling down -> deferred if `elapsed + wait >= cooldown` (plays later from the entry's Update `0x46d9c0`,
data slot `0x6a45dc`, inside CommentMan update `0x46d630`), else dropped; otherwise gate `(u8)rand() <= prob` (`0x46db1a`:
`e8 c1 18 ff ff 3a 46 1d 77 13`), then `0x46d6b0`.
`0x46d6b0` thiscall(CommentMan, entry) `ret 4` -> AL (prologue `83 ec 14 53 55 8b e9 8b 4d 2c`, epilogue `0x46d7b2`
`83 c4 14 c2 04 00`): room check (`items < cap - count`); per item the resolver `0x46d3e0(item, audioMgr)`: hash -> object
(`0x463e30`, audioMgr+0x9c) -> `vt+0x1c` until the hash no longer resolves: group -> **Select** -> SoundInfo (take) hash;
SoundInfo (vtable `0x6a5514`) -> **GetSource `0x479a90`** (`8b 41 0c c2 04 00`) -> ChannelData hash `CD_<take>`. Then
`0x46e2e0(queue, cdHash, wait (+30000 per extra item), prio, pauseAfter, 0, serial, link, entry+0x2c)` at `0x46d788`
(`e8 53 0b 00 00`), the only live caller (`0x45ff5a` is in an unreferenced API).
CommentMan `[0xbdbf0c]`: +0x10 audio manager, +0x14 entry array, +0x18 count, +0x20 shared iterator (`0x46d5e0`, `0x46d590`,
`0x46d520`, the `0x46d630` loop), +0x2c speech queue, +0x30 = 1. Speech queue `[0xbdbf04]`, vtable `0x6a464c` **shared with the
music queue `[0xbdbf08]`**: +0x10 slot pointers, cap +0x14 (8), count +0x16, own sounders +0x18 / count +0x1c, run state +0x24,
Update `0x46e430` (data slot **`0x6a4650`**) thiscall(dt ms) `ret 4`, called each frame at `0x45f993`
(`8b 0d 04 bf bd 00 85 c9 74 08 8b 01 8b 50 04 53 ff d2`), before the music queue and CommentMan (not nested).
Queue slot 0x14: +0 sounder, +4 wait ms (counted down; dropped at <=0; -1 never), +8 prio, +9 pause-after, +0xa serial,
+0xc link, +0xe (1 = VO), +0x10.

## 2. Other random parts (C)
- 25% generic vs character cue: **shared MT** (`0x4be180` -> `0x4be040`, ECX=0xc28cd1) inside each trigger, before 0x4bbec0.
  Generic table `0x6f50e0[21]`; per character `vehicle+0x72c[21]` (`0x54d550`, `"<prefix><CHAR>"`); prefixes `0x6f5088`:
  0 DRV_INT, 1 GAIN, 2 LOST, 3 CONT, 4 LEAD, 5 TRAIL, 6 HIT, 7 OFFTK, 8 WNGWY, 9 ALERT, 10 ALLST, 11 ALLSTU, 12 ALLSTM, 13 ATTACK,
  14 ATTFAIL, 15 FINISH, 16..19 RESULT1..4, 20 FINFAIL.
- Overtake subject `0x4c3410`: CRT `rand()&1` twice (VO order after 5 s; overtaker GAIN/LEAD or overtaken LOST/TRAIL).
- Probability gate (+0x1d) and take (Select): CRT rand().
- Pre-existing: triggers draw the shared MT under local conditions, so the shared RNG diverges between PCs during a race
  (unchanged by suppression; draws happen before 0x4bbec0). The countdown reseed (race.cpp kCountdownSalt) is before this.

## 3. Reachable race-time triggers (all also need `[0xcf7a51]` (`0x4cc110`) and the car not finished (`0x51a010`))
| Trigger (call site) | Event -> cue, wait ms | Cooldown / randomness | Local player? |
|---|---|---|---|
| `0x4bbf60` (`0x4bbfce`), Racing tick `0x5b2110` | LEAD_<leader> once at 7 s, 3000 | `ce7ba8`/`ce7ba5`; >=2 racers | no |
| `0x4bc180` (`0x4bc261`) <- `0x4c3410` <- RC `0x4c4840` | overtake GAIN/LOST (25% generic) or LEAD/TRAIL, 2500 | `ce7bd4` >=45 s; >4 racers; >=7 s; rand subject | a local human in any place, others top 3/4 |
| `0x4bc290` (`0x4bc376`: `6a 01 68 e0 2e 00 00 53 e8 45 fb ff ff`) | LAP2/LAP3/LAP4/FLAP, 12000, flush=1; lap jingle d92a3b4e/28ad1d27/4ab51112 first | `[0xce7be0]` last lap (written `0x4bc342`); finish crossing or lap>=5 not final still flushes with cue 0 | **yes** (the local player's laps) |
| `0x4bc390` (`0x4bc445`) | HIT (25% generic), 2500 | `ce7bb4` >=45; excitement +0.25/tick | no |
| `0x4bc510` (`0x4bc5f3`/`0x4bc631`) | INT1_xx + INT2_xxx, 20000, linked | none | no |
| `0x4bc6d0` (`0x4bc7cc`/`0x4bc834`) | GEN_SPREAD/CLOSE, GEN_QUIET/BUSY/ENERGY (`ce7bb0` <2 / 2-5 / >=5), 5000 | `ce7bc4` >=121, `ce7bc0` >=91; >=5 racers | no |
| `0x4bc850` (`0x4bc8c2`) <- AddGadget `0x53981a` | ALLST (kind 0xb678e022 only), 25% generic, 3000 | none | no |
| `0x4bc8d0` (`0x4bc942`) <- FireGadget `0x53b389` | ALLSTU (25% generic), 5000 | none | no |
| `0x4bc970` (`0x4bc9aa`) <- `0x5e9b10` | COM_ALL_T_41/40/42, 5000 | none | no |
| `0x4bc9d0` (`0x4bcb7a`) <- lock change `0x536d10` | ALERT (25% generic), 1000 | `ce7bb8` >46; distance >=50; place <= local's; 50% skip (MT) unless local | partly |
| `0x4bcba0` (`0x4bcc79`) <- Human state `0x5af150` | WNGWY (25% generic), 5000 | `ce7bbc` >=46 | **yes** (local human) |
| `0x4bcd90` (`0x4bce18`) <- racer msg 0x28 | ALLSTM_<sender>, 5000 | none | no |
| `0x4bce30` <- WFF enter `0x5b24e9` | RESULT1/2/3 top 3 (25000) + VO 0/1; RESULT4/FINFAIL (35000) for the first local human past 3rd; stops there | none | partly |
| `0x4bd0a0` / `0x4bd150` (`0x4bd13b`/`0x4bd1eb`) | ATTACK / ATTFAIL for the projectile owner, 5000 | `ce7bb8` | no |
| `0x4bd3b0` (`0x4bd4c6`) | COM_PO/GT/NO (last car, <5 racers, 10-19 s, once), 10000 | `ce7bdc`/`ce7bcc`/`ce7ba4` | no |
| `0x5b30c0` (`0x5b3256`) results enter | GEN_END, 10000 | none | no |
Not reached in online type-1 races: mission lines (`0x4ff430`, `0x56d830`, `0x5b3960`, `0x5c3d40`, `0x5c3fd0`, `0x5b24a3`), hud
native 1e02 (`0x4dc610`). Unreferenced: `0x4bc460` FINISH, `0x4bcc90` OFFTK, `0x4bd040`, `0x4bd200` FINFAIL, `0x4bc660`.
Outside the queue: the countdown voice (COM_INT_C_05..02, `0x5b19b0`/`0x5b1b9f..`: same everywhere); the lap jingle, homing
warning `e92fab3c`, wrong-way sound `72b85ad1` are local HUD sounds.

## 4. Driver voice (C)
`0x54d0a0(idx, is3D, wait)`: vehicle `[racer+0x9f4]`, sounder `vehicle+0x4b4+4*idx`, 20 s cooldown `vehicle+0x4d0+4*idx`
(ticked at `0x555dcd`..`0x555e6d`: `f3 0f 10 83 d0 04 00 00 f3 0f 58 05 1c a0 c2 00`); 3D lines only within 2000 units of the
local listener (cooldown resets either way); queued by `0x45fef0(sounder, wait, 0, 1)` cdecl (e.g. `0x54d241`:
`6a 01 6a 00 51 56 e8 a4 2c f1 ff 83 c4 10`) into the same speech queue.
| Idx | Call | When | 3D | Wait |
|---|---|---|---|---|
| 0 | `0x54d330` | overtake / race-end winner | overtake 3D, winner not | 3000 |
| 1 | `0x54d350` | overtake / 2nd-3rd / hits | varies | 3000 |
| 2/3 | `0x54d370`/`0x54d400` | finish | no | 5000 |
| 4 | `0x54d490` | weapon fired | 3D | 3000 |
| 5 | `0x54d4a0` | REVENGE_WARNING (All-Star fired) | no | 10000 |
| 6 | `0x54d2a0` | CharacterIntro enter, local player 0 only (`0x4bbda0`) | no | 3000 |
VO take: drawn when the queue starts the line: sounder `vt+0x38` -> SounderGroup::Play `0x470bf0` (vtable `0x6a46e0`, type
`[0xbdbff4]`, data slot `0x6a4718`) -> RANDOM `0x46fc40` (CRT rand); may skip the line by the probability at +0xc9.

## 5. Recommended design (game thread only; network receive fills a queue the hooks drain; install a data slot only if it holds
the stock value; `_ReturnAddress()` == `0x46d423` in the hooks)
**Step 1, intro take (no wire change):** hook `0x6a4fdc` (stock `0x476830`, prologue `83 ec 0c 8b 41 14 53 55 56 8d 71 14`). If
the return address is `0x46d423` and `g = [ECX+4]` is one of the 34 INT1_/INT2_ hashes: walk children with `0x55afb0(ECX+0x14, i)`
(thiscall `ret 4` -> item {+0 hash, +4 played}, count `[ECX+0x14]`), return the child c minimizing `mix32(seed ^ c)`, set
played = 1. Every PC (host too). Same take across ENG/FRE/GER/SPA/ITA, and EN2/JAP where it exists. The intro-end voice query
(m7-intro-voice) keeps working (lines still queued locally).
**Step 2, race commentary host -> guests** (window: Racing enter / countdown release .. teardown 090a).
Host: Select hook at `0x46d423`: `pendingGroup = [ECX+4]`, call original. GetSource hook `0x6a5530` (stock `0x479a90`) at
`0x46d423`: `take = [ECX+4]`; `cue = pendingGroup ? pendingGroup : take`; clear pendingGroup; find the entry by scanning
`[[0xbdbf0c]+0x14][0..[+0x18])` for `+4 == cue` (**never call 0x46d5e0 here**: Select can run inside 0x46d630's loop on the +0x20
iterator); `wait = entry+0x24`; `flushFirst = [0xce7be0] changed since last check`; send `{tick, cue, take, wait, flushFirst}`.
Once per tick, send a bare `{flush, tick}` when `[0xce7be0]` changes with no line following.
Guest: at window start write `[[0xc29fc0]+0x1350] = 1`, clear +0x34 on every entry (blocks local lines and the local lap flush;
trigger timers and MT draws run as stock; the lap jingle still plays). Writers of the byte (C): native 095f `0x58f140`
(`a1 c0 9f c2 00 c6 80 50 13 00 00 01 c3`), race-flow init `0x4cc930` (`88 9a 50 13 00 00`, ebx = 0), scene setup `0x49ff8d`.
Clear it at teardown or when the host leaves. Drain in a hook on the speech-queue Update `0x6a4650`, only when
`ECX == [0xbdbf04]`, before the original; per line with tick <= ShownTick: late = ms since tick; flushFirst -> `0x460000()`;
`entry+0x24 = wait - late` (drop if <= 0); arm force = {cue, take}, call `0x46d6b0` thiscall([0xbdbf0c], entry), disarm; mirror
`0x46daf0`: `entry+0x35 = AL`, `+0x30 = 0`. The Select hook returns `take` when `g == force.cue` and take is one of this PC's
children, else the stock pick (language mismatch).
Which lines: every reachable race line host -> all, incl. LAP*/FLAP with the mirrored flush ("the TV follows the host"), WNGWY,
ALERT, RESULT4/FINFAIL from the host's view (none is a "you" sentence; they name a character or are generic: I, audio not heard).
Keep local: the CharacterIntro VO 6, the countdown voice, HUD sounds. Optional later: guests report their wrong-way start
(`racer+0xa28` going 1 in `0x5af150`) and the host calls `0x4bcba0(slot)`; lap callouts following the leader instead of the host.
**Step 3, VO mirror.** Guest: each tick write 0.0f to `vehicle+0x4d0..+0x4e8` (7 floats) for every car (HookRacerCommit). Host:
in the queue Update hook, slots whose sounder is new since the last frame and not one of the queue's own (`queue+0x18`/`+0x1c`);
map sounder -> (car slot, idx) through `vehicle+0x4b4..+0x4cc`; send `{tick, slot, idx, wait = slot+4}`. Guest replay:
`0x474a00(sounder, 0)` (thiscall `ret 4`), then `0x45fef0(sounder, wait - late, 0, 1)`. Optional: force the VO take via
`0x6a4718` and child k's `0x46f1c0` + `vt+0x38` (I).

## 6. Risks
Game thread only (the main loop calls the audio update `0x45f930`); no Steam calls in the hooks. The queue vtable is shared with
the music queue: filter on ECX. Pause: the queue advances only at run state 1; with the pause flags `[0xc2a014..16]` lines wait
(whether our online pause sets them: not checked). Late guests: subtract lateness, drop expired lines; a full queue (8) drops
lines as stock. Languages: cue hashes are language-neutral (all 710 groups in every XML); take lists match ENG/FRE/GER/SPA/ITA;
EN2 differs in 16 groups, JAP in 10 (fallback covers); different recording lengths can shift later lines by seconds. Japanese
COM_JAP streams are missing (as stock; a failed play drops the line `0x46e7be`; missing file fail vs hang: I). Roster Pack
extra racers have no entries: their cues are silent everywhere. The hook must be the direct target of the `call edx` (no
stack-changing trampoline).

## Confidence
High (C): take RNG and shuffle bag, the chain, layouts, hook slots in both exes, the +0x1350 gate and writers, the trigger list,
near view always true, VO mechanics, language list differences. Medium (I): in-game timing (stream start latency, lateness
compensation), "Select means queued", what the lines say, XML-to-language mapping, VO take forcing. Nothing tested in game.
Files: `scripts\` (run.sh, Decomp.java, l1-l13.txt, peek.py, comtables.py, callscan.py, slots.py), `out\` (d1-d13.txt,
comtables.txt).
