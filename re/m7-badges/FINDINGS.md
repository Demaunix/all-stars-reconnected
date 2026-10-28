# M7 badges for players 5-8: results, pause title, progress bar, name tags (reverse-engineer report, 2026-10-21)

Saved by the main session. Static analysis only; nothing run in game; no game file changed. C = confirmed in bytes,
decompile or game data; I = inferred. Work folder `re\m7-badges\` (`scripts\`, `out\` with dumps, decompiles r1..r10,
scans and crops; `data\`, `out\scene\` and `ghidra\` hold game data / a project copy: not committed).

**Main finding: the gap does not exist.** The stock row builder already puts +0x9dc into row[7] for any value. Players
5-8 get PlyrBadge_5P..8P from the existing Hook0912 with kMaxHumans = 8. The art, elements and texts for 5..8 exist in
every language variant of the results and HUD scenes.

**Correction to the independent check (2026-10-21):** `0x58da00 cmp ebx,3; ja 0x58da23` does not skip the badge. It
skips only the blackboard read `multiplayer_scores[n]` that fills row[14].

The `results`, `pause_menu` and `hud` modules are byte-identical in the stock FrontEndScripts (c1f5ac48...) and the Roster
Pack's (1585d0de...) (C). All race.cpp ScriptSite byte patterns match in both packages (C, `scripts\sites.py`): 0912 row
at results 0x422d; 0913 finish panel at results 0x37ec; 1612 name at results 0x4255; badge range check at results
0x4461; 0f0f title at pause_menu 0x0be9; title range check at pause_menu 0x0c1f.

## Q1. What 0x58d890 does with +0x9dc
- Native 0912 = 0x58de90 (slot 0x6f6038): checks `pos < scene+0x1100`, `idx = 0x4cc2d0(pos)` (byte at [0xcf7950+8*pos]),
  checks `idx < scene+0x1100`, calls `0x58d890(d, idx, 0)`. Native 0913 = 0x58ded0 (slot 0x6f603c): `0x58d890(d, arg, 0)`
  if `arg < scene+0x1100` (C).
- At the branch (C):
```
0x58d9fa 8b 9b dc 09 00 00   mov ebx,[ebx+0x9dc]   ; the racer's human number
0x58da00 83 fb 03            cmp ebx,3
0x58da03 89 5c 24 18         mov [esp+0x18],ebx    ; row[7] value, stored BEFORE the branch
0x58da07 89 7c 24 28         mov [esp+0x28],edi    ; row[14] value = 0
0x58da0b 77 16               ja 0x58da23
0x58da0d..0x58da1f           [esp+0x28] = *0x4a4100("multiplayer_scores", n)   ; only for n <= 3
```
  Entry 7 is written at 0x58dccb..0x58dce4 from [esp+0x18] with type 2 (int): row[7] = {+0x9dc, int} for every value
  (0..7, and 8 for AI/remote cars). The > 3 case changes only row[14] ({0, 2} instead of {multiplayer_scores[n], 2}).
  Nothing else writes those two stack slots (0x4cc4e0 takes no arguments, 0x4c90d0 is `ret 4`).
- Row entries (C, 0x58db20..0x58de8e): [0] record+0x80 (type 3); [1] name sprintf "%s" of [racer+0x1f0]+0x190 (heap
  string, "PLAYER %d" fallback); [2] +0x1f0; [3] +0x2b0; [4] +0x230; [5] +0x270; [6] idx; [7] +0x9dc; [8] time text of
  record+0x68 (0x58c760); [9] time text of record+0x60; [10] record+0x60 != 0; [11] 6 - record[1]; [12] lap/score;
  [13] +0xc8; [14] multiplayer_scores or 0; [15] arg 3; [16] 0x4ccfa0(idx) or the rounded time; [17] 0x4cc2f0(idx).
- Readers (C): row[14] only by SetupMissionLeague (results 0x4ef8, rows from native 0944). row[7] at our hooked site:
  only the badge code at results 0x4461. row[7] at other 0912 sites (not hooked): race-win stats at 0x53de, 0x5449,
  0x5480, 0x54b7, 0x54fa (`0912(0)[7] == 0400()` then `2004(83/84, ...)`), mission rank at 0x6281; other sites: Grand
  Prix 0913 at 0x49d8, 0x4a1a, 0944 at 0x4e77, 0945 at 0x51d5.
- The row array object (C): created by 0x48ed30 (thiscall ECX = 0xc28b20, args &d->retValue and 18; manager vtbl+0x60
  = 0x48eb40). Manager registration (0x48c970): type id 8 (`mov dword [eax+4],8` at 0x48c9c8), vtable 0x6a81f8, name
  "sarray" (0x6a825c) = the script's MKARR type byte 08. Object (0x10 bytes, 0x4bd7a0): +0 count, +4 capacity, +8
  elements (count x {u32 value, u32 type}, zeroed at creation), +0xc u16 refcount (0xffff = never freed). addref
  0x48bc00; release 0x48e8f0 (releases every element, frees at 0); setter 0x48cb00 (`int3` if the slot's type !=
  [0xc28b24], bounds-checked, copies via 0x48c440); getter 0x48bd30 (the static none 0xc28b34 out of range). 0x48ed30
  stores {arr, 8} into d->retValue/retType with an addref; the descriptor's destructor 0x4928d0 releases its temporary
  copy, so d->retValue holds the only reference (C); the VM takes it as the call's result; a fresh object per call (C).

## Q2. The smallest safe way to show badges 5P..8P: the current Hook0912 already does it (C)
- Script (results): 0x4461 tests `row[7] >= 0 && row[7] <= 7` (bytes `07 02 51 00 49 07 03 51 07 47 4f 14 da 00`);
  0x446f..0x44ff build an 8-string array "PlyrBadge_1P.tga".."PlyrBadge_8P.tga" (`22 08 00 08 00 00 00`); 0x4506..0x452c
  call `0228(NODE("RACERESULTS\PLYRBADGE_BAR0"+(i+1)), NODE(arr[row[7]]))`; 0x453c shows the element, 0x4550 hides it
  otherwise. Runs for every row whatever ARG3 is.
- Game data (C, `out\resultscheck.txt`): all 14 language variants of the RACERESULTS scene (entries 137, 391, 412, 561,
  620, 774, 788, 828, 1020, 1026, 1091, 1138, 1209, 1409; 1209 and 1409 English) have PTEX sprites PlyrBadge_1P..8P.tga
  (entry 137: records 57, 25, 109, 14, 124, 123, 59, 101) and elements RACERESULTS\PLYRBADGE_BAR01..08,
  PLYRPORTRAIT_BAR01..08, BAR01..08, STRING_RACERBAR01..08 (4 occurrences each, as 01..04). Crops at full size:
  `out\crops\PlyrBadge_5P.tga.png` ("5P", purple) and `..._8P.tga.png` ("8P", green) in the 1P style. Element ids are
  the name hash of the full path (`name_hash("MAPIMAGE\PROGRESS_BAR")` = a7750c77 as in 0x5017f0; 0x5007b0 sprintf,
  0x4947e0 with seed 0x4c11db7, lookup 0x47f260) (C).
- Conclusion: Hook0912 writes `+0x9dc = k` for every human around the one 0912 call (results 0x422d, PC 0x4235); row[7]
  = 4..7 then shows 5P..8P (C by code and data; I until seen in game). The only difference from k <= 3 is row[14] = 0,
  which this flow never reads (C). No stock code needs changing.
- Optional alternative with no write to the racer: after the original call at the same site, if `d->retType ==
  *(uint32_t*)0xc28b24` (8), `arr[0] >= 18` and `e[7].type == 2` (e = (ScriptValue*)arr[2]), set `e[7].value = k` (an
  int element, no refcount, row-private) (C). Not needed.
- Other +0x9dc readers (capstone sweep: 5 readers, 7 writers; `out\disp9dc.txt`) (C): 0x58d9fa (above); 0x58ec2e native
  0944 (SetupMissionLeague, results 0x4e77; its local_40[16] holds 8 indices and 8 scores, loops to scene+0x1104: safe to
  8); 0x58fb17 native 0f0f (the pause title, returns +0x9dc as type 3 through 0x48c640); 0x5b3c37 / 0x5b3c87 in 0x5b3b80
  (vtable slot 0x6b23d8; bumps "multiplayer_scores" only for n < 4, posts SHOW_MISSION_LEAGUE). Writers: 0x4e6391 /
  0x4e639e (racer init 0x4e6360; the m6 note's 0x4e638b is off), 0x4f3aa1 (sets 8), 0x4f72eb (reset 0); 0x550c10,
  0x5546c4, 0x55a4cf are other classes' float fields. Human numbers at creation (0x49b2c5..0x49b2fc): default 8;
  controller category 1 or 3 gets scene+0x1104 (if < 8), which then increases: on each PC the local player is 0 and every
  other car 8. None of these readers run inside the synchronous 0912 call; 0944 and 0x5b3b80 are reached only through the
  Mission/League path (Race_Results update 0x5b25e7 `cmp [scene+0x1104],1; jbe` -> state 0x4c), which our flow avoids
  (scene+0x1104 stays 1 outside the Racing/WaitingForFinishers updates); even if reached, a `multiplayer_scores` read at
  index >= 4 returns none (C).

## Q3. Pause title, progress bar, name tags (C)
- Pause title: pause_menu 0x0bc2..0x0be2 build {"1P".."8P"}; 0x0bec calls 0f0f(car); 0x0c15 `0222(HUD, f408e0bf)`; if 0 <=
  n <= 7 it appends " " + `0222(HUD, NODE(arr[n]))` (0x0c1f..0x0c49). The HUD scene TEXT table ("TEXT" chunk at stream
  offset 0x906680 in entry 61: sorted {key hash, offset} pairs, 228) gives, in all 14 HUD variants (entries 61, 386,
  580, 643, 700, 791, 895, 948, 1065, 1127, 1165, 1178, 1213, 1286; `out\hudcheck.txt`): English "5P".."8P" / "PAUSED";
  German "S5".."S8" / "PAUSIERT"; French "J5".."J8" / "EN PAUSE"; Spanish "J5".."J8" / "EN PAUSA"; Italian "G5".."G8" /
  "PAUSA"; 4 placeholder variants "Empty". Our PauseTitleBefore (k < 8) already gives "PAUSED 5P".."PAUSED 8P".
- Progress bar: 0x5017f0 loops k = 0..7 and fills rival entries E(k+2) (`MAPIMAGE\PROGRESS_BAR_MARKER_RIVAL_n` /
  `HEAD_RIVAL_n`) and human entries E(k+10) (`MARKER_nP`, `HEAD_nP` (`HEAD_1` for n = 1), `MARKER_nP_TEXT`). Entries end at
  H+0x1f8; lap markers at H+0x1f8..0x244 (4 x 0x14); rocket and racer slots E(21..28); nothing overlaps. All 14 HUD
  variants contain `MAPIMAGE\PROGRESS_BAR_MARKER_5P..8P`, `HEAD_5P..8P`, `MARKER_5P..8P_TEXT` and `RIVAL_5..8`. Marker
  records (entry 61 at 0x8f0514 + n*0xec) = {self, text child, head child, image, 10601bec, self}; images 1P 01650a39
  (the init's default), 5P cef632bc, 6P 1889c52c, 7P 283df1bb, 8P d5a6719f, all PTEX sprites on page 27 (crops: 1P blue,
  5P purple, 8P green). UpdateProgressBar (k+2 and k+10 for k <= 7) needs no change.
- Name tags: `HUD\LFT_TAG_01..07` and `HUD\LFT_TXT_01..07` exist in the single-screen "HUD" layout in all 14 variants
  (answers m6's open question). `LFT_TAG_08` does not exist and nothing needs it: the tags are draw slots (hud
  0x2061..0x21eb build 7-entry arrays; g98 = LEN(g90) = 7; g97 the slot counter): each screen tags at most 7 other cars.
  g_aa = {"1P".."8P"} (hud 0x2495..0x24bc), every key in TEXT.

## Q4. Other limits of 4 that 8-player lobbies could hit (C)
- Finish panel 0913(0), results 0x37ee: Hook0913 passes g.local (0..7); 0x58ded0 bounds-checks against scene+0x1100 =
  8; reads row[0], [3], [5], [16] (0x352b/0x361a read [9]/[10]); no human-number tables.
- Result rows: text buffers g1f/g20/g21 are 8 each (results 0x0eb2..0x0f77); BAR01..08, PLYRPORTRAIT_BAR01..08,
  STRING_RACERBAR01..08, PLYRBADGE_BAR01..08 exist, indexed by row.
- Profile stats: 0x49dbf0 and 0x49d930 count only a local category-1 human; the results race-win stat compares the first
  row's real +0x9dc with 0400(), unhooked.
- Race-end wait: RaceUpdate raises scene+0x1104 by exactly 1, only inside the update: at most 2 whatever the player count.
- The only real limits of 4: "multiplayer_scores" (guarded < 4 in 0x58d890 and 0x5b3b80; the split-screen league path)
  and the HUD's 4 lap markers (laps, not players).

## Confidence and next step
High that badges, the pause title, the progress-bar markers and the tag slots for players 5-8 need no new code (bytes,
decompile, scene data and crops agree). Not tested in game; rendering is I. Smallest fix: none. Optional cheap check
with 2 PCs (I): a temporary debug offset (k+4) in Hook0912 and PauseTitleBefore would show 5P/6P badges and "PAUSED 5P".
