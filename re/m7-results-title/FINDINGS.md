# The host's results screen without its RESULTS title and CONTINUE (2026-10-22)

Reverse-engineer pass, static only (saved from its report by the main session). C = confirmed in bytes/decompile, I = inferred.
Seen in game: race f5eabf08 (0.7.0-m7q), the host's results lacked the gold RESULTS title and the CONTINUE footer, the guest's
had both; once before on m7e/m7f.

## Cause
The results screen and the pause menu use the same global scenes, TOPBAR (182699c8, the title) and BUTTONS (7cecc470, the
footer), kept by name with no reference count. The host's online pause was open while its own car (driven by the AI) finished:
the results module's creates found the pause menu's scenes and made nothing new; 1.27 s later the race ended, our Hook1b01 let
the pause menu see the results key, and its close destroyed TOPBAR, BUTTONS, TXTBOX and RESULTSOPTIONS. The title and footer
texts then went into scenes that no longer existed; RACERESULTS (the table) isn't one of the pause menu's, so it stayed.

## Evidence
- `results` (26458 bytes) and `pause_menu` (7379 bytes) are byte-identical in the stock and Roster Pack .sbn (C).
- Title and footer (C): SHOW_RESULTS_LIST (results 0x1bbc) -> export f7951b4e SetupRaceResults (0x4570) -> 0x4090("RESULTS",
  91f6df99, 0): SetupButtonBar 0x3fa7 `1607(pause_menu 791427eb, export fc0a943c, ["CONTINUE",NONE,NONE,NONE])` at 0x3fe2
  (the pause_menu export 0x1950 writes the label into BUTTONS' elements 56ff27d8 / a8541e22); SetupHeaderBar 0x3e12
  `021f(f1ad8c5a / 2070352e, "RESULTS")`, `0217(c6ec0173)`; export 13e76722 (0x45b7) shows 42670268 (header), 58bacb6a (table),
  8f20859f (button bar).
- Scene roots (C; SumoToolResources.xpac entry 137 'SCNE'): TOPBAR = 42670268, BUTTONS = 8f20859f, TXTBOX = d0a85650,
  RACERESULTS (aae955f4) = 58bacb6a, GPPOS = b6b6d5e9 (the finish panel). c6ec0173 under TOPBAR: I.
- The results create their scenes at RESULTS_START (C): handler 0x1ade: `1b03(86d2420e, 1)` at 0x1ae5, a yield at 0x1b63, then
  0x1752 creates its 9 scenes (g15: TXTBOX, TOPBAR, BUTTONS, RACERESULTS, RESULTSOPTIONS, ...), one `0205` and one yield each.
  SHOW_RESULTS_LIST never creates them again (ShowSegaMiles 0x1f0a does: why the later RETURN TO LOBBY menu worked).
- The pause menu owns the same scenes (C): its open (0x04f4..0x0539) `0205`s RESULTSOPTIONS, BUTTONS, TXTBOX, TOPBAR, 53c7e6d0;
  every close (0x091d..0x0972) `0206`s BUTTONS, f7580a13, RESULTSOPTIONS, TXTBOX, TOPBAR. Its race-end poll at 0x0676 is
  `0916==5 && 1b01(86d2420e)`: true -> g11 = 0, key 0, the menu closes. TOPBAR appears exactly twice in each .sbn (this open
  and close).
- No reference count in the exe (C): 0205 (0x4a6a70) -> CreateScene 0x47fdc0 (ECX 0xc282a8) returns 1 at once if 0x47fcd0(hash)
  finds the scene's active byte +0x2c (`call 0x47fcd0; test al,al` at 0x47fdd8); 0206 (0x4a6ab0) -> 0x47f790 -> 0x482e20 clears
  +0x2c and frees its elements and buffer. The last `0206` wins, whoever created the scene.
- Only the host (logs C, reading I): both games opened the online pause (host 16:29:09.573, guest 16:29:11.306); both finish
  times slow (AI driving both paused cars). Our Hook1b01 returns 0 at the pause reads (pause_menu 0x068e, 0x0ab1) until
  RaceOver(), so the close waited for the race end, not for this game's own finish as on the console. Host: own finish
  (RESULTS_START) 26.006, scenes made ~0.26 s later, race over 27.273, then the close destroyed them. Guest: own finish 27.249,
  race over 27.282, 1-2 script frames apart: the close came before or around the creates, which rebuilt them (I).
- Pausing after the own finish is impossible (C): 0918 (0x58ed50) checks 0x58ecc0, which refuses a racer whose lap record counts
  as finished (0x51a010). The bad path: paused before the own finish, still paused at it, the race ending later.
- Ruled out (I): WFF / scene+0x1104, DNF marking, the announcer capture, 0916 at 0x22fd, the 0913 finish panel: none calls
  0205/0206 on these scenes.
- The 30 s RETURN TO LOBBY is the stock timeout: STATE_RESULTS_LIST (0x25cb) leaves when g36 > g0c (750, or 3000 if 0916 is 5
  at 0x25e9) or on Enter (0419). Code C; the exact 30 s match I.

## Fix (built in 0.7.0-m7r)
Close the pause the console's way, at this game's own results start: the stale-key filter stays (a key left from an earlier
race), but the real value goes through once this race's RESULTS_START (results 0x1ae5, `HASH 86d2420e; INT8 1; NAT 1b03/2`,
bytes `02 0e 42 d2 86 51 01 32 02 03 1b 00 00 c6 02` from PC-15) has written it. The pause menu then sees the key during the
yield at 0x1b63, before TOPBAR's create on the third frame: it destroys its own copies and the results build fresh ones. Side
effect (I): an AI-driven paused car leaves the pause at its finish line (the console's), not at the race end; the close path
still calls 0929. Rejected: skipping the pause's `0206`s (its PAUSED header and items would stay over the results).

Diagnostics proposed: at 0x098d while the pause is open, how long after this game's results start it closed (over ~0.2 s = no
title); at results 0x3fea (`1607`; code from PC-8 `32 03 07 16 00 00 e6 08`) once per race, TOPBAR / BUTTONS present via
`bool(__thiscall*)(void*, uint32_t)` 0x47fcd0 with ECX 0xc282a8 (a read-only lookup, `ret 4`).

Repro on one PC: Esc on the host right after the countdown, stay paused until the host's car finishes, the guest's car finishes
1 s or more later: no title or CONTINUE on the host before the fix.

Files: `ghidra\` (own copy, don't commit), `scripts\` (run.sh, Decomp.java, findhash.py, strref.py, bytes.py, sbnfind.py,
cmpmod.py, nh.py, sceneoffs.py, dumpat.py), `out\r1.txt`..`r7.txt`; disassembly `re\m7-messages\out\results.txt`,
`pause_menu.txt`.
