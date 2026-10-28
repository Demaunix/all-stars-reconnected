# M6 finish: end-of-race presentation, which car each piece shows, intro skips (reverse-engineer report, 2026-10-21)

Saved by the main session. Read-only; nothing run in game. C = confirmed in bytes/decompile, I = inferred. Script
offsets are module offsets (PC after an 8-byte native call = offset + 8). `hud` and `results` are byte-identical in
the stock and Roster Pack FrontEndScripts (C). Raw output was in the session scratchpad (`fin\`).

## 0. Short answer
- The wrong end-of-race "portrait" was most likely the 1-player finish panel (place, character art, name at the
  line): it asked 0913 for creation index 0. Seen at M4 (208e52a, 0.4.0-m4d), fixed at M5 (e54c10d, 0.5.0-m5f; guest
  log `the finish panel shows this game's car (racer 1)`). With that fix, nothing else in a 1-local-player Single
  Race picks creation index 0 on a guest (C).
- The PC-only Podium shows the top 3 of the synced finish list; the winner celebrates (the host's car on a guest if
  the host won: correct data). The console's online flow skips the Podium (C, §2): dropping it online is the
  console-like option (§5).

**Correction (user, 2026-10-21, PS3 footage "Online Races! PS3 w/ SB_Crimson", Shadow/Shadow/Sonic on the ground at
the finish):** the console's online races DID show the on-the-ground top-3 lineup. The "console skips the Podium"
conclusion below (from the PC's AutoSave type test) is wrong for the console; keep the Podium online (user's choice).

## 1. Correction to re\m6-race-hud\FINDINGS.md (d)
0x5b3960 (slot 0x6b23b4, vtable 0x6b23ac) is State_Mission_Results (id 0x4b), not Race_Results (factory 0x50d420,
table 0x50dfa4; GetName 0x50c414). RaceOver update 0x5b2530: type 1 or 5 -> 0x4ccd10(0x46) State_Race_Results
(vtable 0x6b22c8, enter 0x5b2750 slot 0x6b22d0, update 0x5b25a0 slot 0x6b22d4); type 2 -> 0x48; type 3 -> 0x4b. So
(d)'s "podium only when scene+0x1104 == 1", the cue 0x4bbec0 from record 0 and LAPS.md's scenic camera 0x5b3985 are
mission-only.

## 2. PC Single Race flow (C)
Flow table [0xcf7a78] = 0x574490(type) (types 1/2: key 3, table 0x6f5238; record +4 state, +8 next, +0xc phase
seconds, +0x10 HUD message). No table for type 5 on PC (0x574490 returns 0): never set type 5 globally.
Before: WaitingToStart 0x37 -> TrackIntro 0x39 (5 s) -> CharacterIntro 0x3a (5 s) -> Countdown 0x3b -> Racing 0x3c
-> WaitingForFinishers 0x3d. After: RaceOver 0x3e (3 s) -> Race_Results 0x46 -> SegaMiles 0x43 (enter 0x5b30c0,
update 0x5b3290 waits for `Results_ExitState`) -> AutoSave 0x42 (enter 0x5b2f00, update 0x5b2f90, slot 0x6b2244) ->
**Podium 0x40, type 1 only** -> RetryExit 0x41 (enter 0x5b2d20). AutoSave's decision at 0x5b2fdf `83 ff 01 0f 95 c0
83 c0 40`: type 1 -> 0x40 Podium, any other (console's 5 too) -> 0x41 RetryExit. Tournament/GP: 0x41 or 0x47 (GP
podium). Scene+0x1179 -> 0x35 restart; scene+0x1178 -> 0x31 quit.

## 3. Each piece and its racer (1 local player)
- RaceOver enter 0x5b23b0: HUD_FINISH (hud 0x5253 only prints); finish music 0x49db20 when scene+0x1104 == finished
  category-1 humans; announcer 0x4bce30 by finish place 0..2 + a 4th-or-lower line, stops at the first category-1
  racer = the local human (C).
- Results finish panel (results 0x379c, chosen at 0x3573 when g33 is 5, or 0 with 0917 = [0xc2a044] == 1; g33 from
  GetResultsSequence results 0x20ee = 0 here): 0913(0) at 0x37ee (row[0] place, row[3] icon, row[5] art, row[16]):
  our ScriptSite override = own car (fixed). 0913(0) at 0x352b / 0x361a fetch row[9]/[10], unused.
- Finish camera: per-viewport timer (results 0x2550..0x25b6): HUD_HIDE, 060b(p) at 0x25ad -> 0x600110 ->
  0x4a1cb0(0x4dd010([0xc2a068+p]), p): TrackCamera [0xc2a0ec+p] (ReplayCam trackside), target = the chase camera's
  racer, set only by 0908 -> 0x4a09a0 (fe_game_bridge 0x3269, StartingGridHuman racers), 0x4a0a00, 0x4a1d20: own car.
- Race_Results enter 0x5b2750: SHOW_RESULTS_LIST, 0x5b2700 -> 0x5b2650; [0xc2a044] == 1: camera on [0xc2a048]+0x9d8
  = own car.
- Results rows (0x421e..0x4569): 0912(pos) in finish order; PLYRPORTRAIT_BAR0n row[2]/[5], badge row[7].
- SegaMiles: stats 0x49dbf0 = own car; cue 0x30a67086 not racer-specific.
- Podium enter 0x5b2b00 (slot 0x6b21f8): TrackCamera at [0xd11b18+0x834] (mission target, 0 here, I), then
  0x4a1300(cam,0) and 0x4a1300([0xc2a068],0) (0x5b2b62/0x5b2b70): viewport 0 ends on this PC's chase camera (own car;
  what it frames: I). 0x534f00 sends event 0xe to every controller (-> Control_Podium 0x1a), places finish
  positions 1..3 (0x4cc2d0 -> record+0x8c) on podium nodes (type 0xb, fallback 4), skips slots past the car count.
  Control_Podium (vtable 0x6b1d28; enter 0x5b0440, update 0x5b06f0) hides cars not in the top 3 (racer+0xbc bit 2).
  0x4e7430: slot 0 winner VO (0x54d400 index 3), flag 0x20; others VO 2, flag 0x10. START_PODIUM only sets
  STATE_DO_PODIUM in `results`.
- RetryExit 0x5b2d20: g_ScenicCam fly-by + START_RETRY_QUIT; GetRetryExitMenu results 0x22fd (our 0916 -> 5 at PC
  0x2305) -> menu 6, one RETURN TO LOBBY item.
- Creation index 0 elsewhere only in unused modes: 0936 (0x58e620 -> 0x49cb50(0)) time-trial panel 0x36c2; mission
  0913(0) at results 0x5e5e/0x601d/0x60f4; hud 0x72e7 090c(0,4); 0x534f00's type-3 branch.

## 4. Guest
Nothing in §3 shows the host's car because of index 0 (C). By design it shows as a top-3 / winner car on the Podium,
in the announcer's lines for places ahead, in its results row. On the guest the Podium camera follows the guest's
own car; not top 3 = hidden at the podium (I).

## 5. Smallest fixes
- Right car: no new override needed (C). Don't touch 0x534f00 or the category.
- Console-like: drop the PC-only Podium online. Swap the AutoSave update slot 0x6b2244 (data, `90 2f 5b 00`); for a
  race from our lobby (`g_returnToLobby`): thiscall 0x54ab30(ECX=0xd214d0) nonzero (still saving) -> return; 0x4d4260()
  (tournament) -> original; else cdecl 0x4ccd10(scene[0x1179] ? 0x35 : scene[0x1178] ? 0x31 : 0x41). Otherwise the
  original. (= stock for type 5, no global written.) Rejected: writing [0xcf7a58]=5 around the call; calling
  0x4ccd10 again after the original (0x50e140 -> 0x50d210 would run the queued Podium's enter first). Risks (I): cars
  never switch to Control_Podium, so they stay visible during the RetryExit fly-by; DAT_00defcb4 and [0xd12d1c] keep
  earlier values (only Podium / GP podium use them).
- Confirming question (guest, host wins): the wrong car at (a) the place/portrait at the line (fixed; log line),
  (b) the trackside camera during the results table (contradicts §3), or (c) after SAVING, three cars on steps (by
  design, or removed by the fix above)?

## 6. Press-to-continue screens and intro skips
| Screen | Waits for |
|---|---|
| Loading, WaitingToStart | no press; our hold A |
| TrackIntro | a press, or the fly-by's end (shot sequencer 0x534730 -> 0x4cce60 after its last shot; fly-by: I) |
| CharacterIntro | a press after >= 1.0 s, or its 5 s timer |
| Countdown | no press; hold B |
| RaceOver | 3 s |
| Race_Results | `Results_ExitState` from `results` (CONTINUE key: I) |
| SegaMiles | the same flag (preset 1 when nothing to show) |
| AutoSave | the save |
| Podium | after 1 s: pad button 0x1f (0x4ac9a0(0x1f,0)) or Enter (scancode 0x1c), or 4 s (update 0x5b2c60) |
| RetryExit | the menu choice |
Nothing links the PCs but holds A/B and the lobby's ready flags; after the race each PC advances on its own player's
presses. The PC shows the fly-by + RETURN TO LOBBY screen (RetryExit, menu 6). PS3 order: results -> SEGA MILES ->
RETURN TO LOBBY fly-by -> SAVING -> lobby; on PC SAVING comes before the fly-by and the Podium sits between.

(a) Skips: TrackIntro update 0x5b1120 (slot 0x6b2100), P = [[0xc2a1b4]+0x14]: pad byte +0xf7 bit 4 of
[P+0xc+[P+0x960]*4] (button 0x23) at 0x5b1234; keyboard [P+0x4c]->vtbl+0x28(0x1c) Enter at 0x5b124f; also blackboard
cmdline_autotest (0xaf60de6a) / cmdline_auto_sh_gen (0x162ca82) or no course graph; needs state+0x10 == 1; then sound
0x91beb24c and 0x4cce60 (0x5b1257). CharacterIntro update 0x5b1640 (slot 0x6b2124): after 0x4cc450() >= 1.0 with
state+0x10 set (false in the tutorial): pad +0xf3 bit 4 (button 0x1f) at 0x5b1706 or Enter at 0x5b1721; then
0x91beb24c and 0x4cce60 (0x5b1745); or its timer (0x4cbbb0 at 0x5b16d3).
(b) Online: no type-5 test in the intro updates (C); TrackIntro enter skips only the voice line (0x5b0fa2). `hud`
track-intro setup hides two elements for type 5: `NAT 0916; ==5 -> 0218(faf4c381); 0218(d80223a4)` at hud 0x511b,
bytes `32 00 16 09 00 00 d1 07 | 51 05 4a 14 1c 00`, both packages (C); the hashes appear nowhere else (I: very likely
the CONTINUE prompt). So the console most likely ran the track intro online without the prompt and without a skip (I).
(c) A player who skips: CharacterIntro -> Countdown enter 0x5b17a0 (HUD_COUNTDOWN_INIT, grid); HookCountdown holds
the stock update until the others arrive or 30 s (race.cpp): the skipper sees the grid, countdown not started.
(d) Console-like: 1. No skip online: Hook0916 ScriptSite at hud PC 0x5123 (bytes above) -> 5 while
`g_returnToLobby` and the intros run (hides the prompt); in HookTrackIntro/HookCharacterIntro, online, read the same
inputs (read only) and skip the stock call on a tick where one is set (risk I: "held" bits would pause CharacterIntro's
timer while held). 2. Alternative, one skip skips for everyone: on a local skip ([0xcf7a70] changed after the call
with the inputs set) send a reliable IntroSkip{race, stage}; on receipt, in the same hook with the stock
preconditions (+0x10 == 1; CharacterIntro also 0x4cc450() >= 1.0), call cdecl 0x4cce60() and 0x577ea0(0x91beb24c,0,1)
as stock does. Hold B stays the final sync.
