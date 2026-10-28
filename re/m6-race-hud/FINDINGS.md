# M6 race HUD: progress-bar tags, name tags, results names, finish cues (reverse-engineer report, 2026-10-21)

Read-only; nothing run in game. C = confirmed in bytes/decompile, I = inferred. Script offsets are module
offsets (the PC after an 8-byte native call is offset + 8, as `kResultsRowPc`). `hud`, `results` and
`pause_menu` are byte-identical in the stock and Roster Pack FrontEndScripts (C). Helper scripts and raw
output: `re\m6-race-hud\scripts\`, `re\m6-race-hud\out\` (`data\` = game script copies: don't commit).

## (a) Top progress bar
- HUD map object H = [scene+0x1174] (scene = [0xc29fc0]). Init 0x5017f0 (thiscall ECX=H), from native 1f16
  (0x5ecf40, slot 0x6f68d0) and the C++ AI takeover at 0x4f3aba. Per-frame update 0x501550 via 0x5017e0, from
  native 1f15 (0x5ecf10, slot 0x6f68cc). hud element table entry {0x45d83c8f, "MAPIMAGE\PROGRESS_BAR", init
  export d6fae43c -> 1f16 (hud 0x69ed), update export 44f738fe -> 1f15 (hud 0x69fb)} at hud 0x0911..0x0939. (C)
- Human test 0x4e8f90: `[racer+0x9f0]+0x18 == 1` (controller category 1 = Human17), called at 0x501bfa (init)
  and 0x5015ac (update). (C)
- Marker entries E(k) = H + k*0x1c. Rival (not category 1): k = list index + 2, sprites
  `MAPIMAGE\PROGRESS_BAR_MARKER_RIVAL_n` / `..._HEAD_RIVAL_n`, default image 0xc72c4bc8. Human: k = racer+0x9d8
  (creation index) + 10, sprites `PROGRESS_BAR_MARKER_nP`, `PROGRESS_BAR_HEAD_nP` (`HEAD_1` for n=1),
  `PROGRESS_BAR_MARKER_nP_TEXT`, default image 0x1650a39, n = creation index + 1. Fields: +0 marker sprite, +4
  head sprite (character icon), +8 text sprite, +0xc racer, +0x10 node (marker+0xe4), +0x14/+0x15 flash and
  All-Star flags, +0x18 default image. Node calls: init hides all with vtbl+0x54(1), shows present racers'
  with vtbl+0x48(); update hides with vtbl+0x50(); position vtbl+0x1c(0x3e3460a8, 0xf5fda6b0, t, 0). (C)
- nP comes from the creation index (+0x9d8), not +0x9dc (no +0x9dc read in 0x4ff000..0x502800). (C) Above/below
  is in the sprites' layout (I; matches the user and the PS3 footage). "1P2P" merging: nothing reads the
  _TEXT sprite (E+8; written 0x501993, cleared 0x500257): console-only code or overlapping sprites (I).
- Don't flip the controller category, even inside 1f15/1f16: the window centres on the FIRST category-1 racer
  (0x5000c0, used by 0x500de0, 0x5011b0, 0x501550); 0x501300/0x500840 take the LAST for H+0x33c; 0x4e8f90 has
  ~100 callers (finish count 0x4cbc70/0x4ccad0, race end, stats 0x49d930/0x49dbf0, AI, items, camera). (C)
- Proposal: swap the remote car's marker sprites. Wrap 1f16 and 1f15. After the original 1f16, and at the
  start of every 1f15 (catches a C++ re-init like 0x4f3aba), for each connected remote human k (racers at
  scene+0x10e8 + k*0x2110, +0x9d8 = k: list index = creation index, C): R = H+(k+2)*0x1c, U = H+(k+10)*0x1c;
  only if R[0] differs from what we stored at the last swap: hide R[+0x10] with vtbl+0x54(1); swap dwords +0,
  +4, +8, +0x10, +0x18 between R and U; KEEP R[+0xc] (the racer; the update dereferences it without a null check
  at 0x501730); clear bytes R[+0x14], R[+0x15]; show the new R[+0x10] with vtbl+0x48(); head icon as init
  does: icon = 0x4ffea0(racer) (cdecl), then 0x47fb00 (thiscall ECX=0xc282a8, args R[+4], icon, 0x3a6dfc06,
  ret 0xc); store R[0]. Preconditions H != 0, H+0x379 == 1, [0xc2a044] == 1 (one local player). A player who
  leaves: drop k and call 0x5017f0 (ECX=H) once, as the console does at 0x4f3aba (restores the rival
  sprites). Risks: a missing sprite comes back null and is skipped (C); racers missing from 0x4ffea0's 25-entry
  table get an empty head (stock).

## (b) Floating name tags
- The console's network tag code is gone on PC (C): for game type 5 the hud per-frame export baa59db4 (hud
  0x3638) calls 0x5b02 and 0x622c, both empty (17 1b). g3a set from 0916 at HUD_START (hud 0x448e). No
  gamertag/nametag/PlayerName strings in the exe beyond MSG_BOX_GAMERTAG_CHANGED_*. "Racer Over Head Icon"
  (0x504e00) is a gadget icon (I).
- What remains is the split-screen "nP" tag system (C): gate hud 0x36b6 `if (0938 > 1) CALL 0x5b07` (0938 =
  0x58ef70 = scene+0x1104; this branch runs only when the game type isn't 5). 0x5b07: for each racer i with
  0911(i) true (0x58d830 -> 0x4e8f90, call at hud 0x5b1d) calls TagFunc 0x5b4f(i, g_aa[i], 1, 0), g_aa =
  {"1P".."8P"} by list index. TagFunc: per local viewport p = 0609(v) (0x6000a0) skips i == p; projects with 1f13
  (0x5ed4e0 -> 0x537100); needs 0954(i,p) (0x58efe0) <= 100.0 and visibility (I); draws `<HUD scene>\LFT_TAG_0n`
  / `LFT_TXT_0n` (max 7 per frame). ARG3 = 1: `021f(TXT, NODE(label))` (localized key, 0x4855d0); 0: 021e + 1613
  (free text).
- To enable (UNVERIFIED in game), native-result overrides at exact ScriptSites in `hud`: (1) 0938 at hud 0x36b6
  (PC 0x36be; bytes `32 00 38 09 00 00 18 04 | 51 01 48 14 0c 00`): 2 during an online race; real value at
  results 0x20f6/0x2305/0x2cae and pause_menu 0x704. (2) 0911 at hud 0x5b1d (PC 0x5b25; `07 01 39 | 32 01 11 09 00
  00 8b 09 | 14 17 00`): 1 for connected remote humans; leave hud 0x3c94 and 0x434a alone. (3) Names instead
  of "nP": at TagFunc's 021f (hud 0x5c62, PC 0x5c6a, `32 02 1f 02 00 00 d3 09`), when arg1 is the hash of "kP",
  call 0x47f3a0 instead (thiscall ECX=0xc282a8, args element hash and a text object; native 0248's helper);
  0x485410 copies the text into a new owned buffer (C), so a static {0x6a71f8, 0, cap, char*} object is safe.
  First test: does the single-screen "HUD" layout contain LFT_TAG_0n / LFT_TXT_0n? (Not checkable statically.)
  If not, fall back to (a).

## (c) Results rows: the player's name
- results 0x422d `0912(pos)` (0x58de90): racer = 0x4cc2d0(pos) = byte [0xcf7950+8*pos]; row built by 0x58d890,
  18 entries: [1] character name sprintf("%s", [racer+0x1f0]+0x190); [2],[5] portraits; [6] racer index;
  [7] racer+0x9dc badge (read 0x58d9fa); [8] time source; [17] 0x4cc2f0(i). (C)
- results 0x425b `1612(g1f[k], row[1])` (0x5fd200, unbounded sprintf "%s"); g1f[k] bound live with 021e to
  `RACERESULTS\STRING_RACERBAR0<k+1>` and `...STRING_RACERBORDER_BAR0<k+1>`; g1f = 8 text buffers of 32 bytes
  (results 0x0e6c.., `UINT8 32; NAT 1610`). (C)
- Badge: results 0x4461..0x452c shows `PlyrBadge_<row[7]+1>P.tga` for row[7] 0..7 (our Hook0912 supplies it). (C)
- Proposal: wrap native 1612 only at results 0x425b (PC 0x4263; `07 06 51 01 23 3b | 32 02 12 16 00 00 9f 09 |
  18`): Hook0912 remembers 0x4cc2d0(pos); Hook1612, if that racer is a human with a known name, checks vtable
  0x6a71f8 and capacity obj+8 (32), copies whole UTF-8 code points ('?' above U+FFFF), NUL-terminates, skips the
  original. Otherwise the original. 1612 returns nothing. The finish panel (0913 at results 0x36d2/0x37ee)
  shows no character name. (C)

## (d) Finish, podium, announcer
- RaceOver enter 0x5b23b0: the finish effect 0x49db20 runs only when scene+0x1104 == 0x4cbc70() (finished
  category-1 humans). (C)
- Race_Results enter 0x5b3960 (slot 0x6b23b4): podium only when scene+0x1104 == 1, local player 0
  ([0xc2a068], 0x4a1300); the cue 0x4bbec0(hash) comes from the record of creation index 0 (0x4ca600(0) ->
  0x4c9910(0) -> 0x4cbed0(0)): on a guest, the host's car. (C; what it sounds like I)
- Profile stats (0x5b30c0 -> 0x49dbf0 / 0x49d930) count only a category-1 human whose controller pad (+0x1c)
  equals the profile's pad (+0x960). Remote cars never count. (C)
- 0x4ccbb0 posts events 10 / 0xc / 0xb / 0xd + 0x1e for any racer, no category test: remote finishes added by
  our Finished path get the same events (which reactions they trigger: I).

## Other +0x9dc readers / writers (C)
Readers: 0x58d9fa (results row[7]; also row[14] "multiplayer_scores" when < 4), 0x58ec2e (native 0944,
results 0x4e77), 0x58fb17 (native 0f0f, pause_menu 0xbec), 0x5b3c37, 0x5b3c87. Writers: 0x4e638b/0x4e639e,
0x4f3aa1 (sets 8), 0x4f72eb, 0x550c10, 0x55a4cf, 0x5546c4. None of the proposals change +0x9dc or the category.
