# Steam Join Game / invites into the lobby screen (reverse-engineer report, 2026-10-21)

Saved by the main session. Static analysis only (nothing run in game). C = confirmed in bytes/code, U = unverified.
Scripts: `re\m6-join\scripts\` (run.sh, Decomp.java, calls.py, sites.py, find2004.py); outputs `re\m6-join\out\`
(cur-fe_mainmenu.txt, cur-fe_network_game.txt: our built modules disassembled; r1-r4 decompiles).

## 1. "License chosen" test
- REFUTED: `[0xcdb530] <= 3`: .bss (0 at start = license 1). Only writer 0x4a98d0 (`a3 30 b5 cd 00` at 0x4a98db),
  caller 0x5f82b8 = native 2004 -> 0x5f9650 -> jmp 0x5f81f0, sub-id 0x14 (case 0 at 0x5f82af). Only script:
  fe_mainmenu export 906cdb81 (`51 14 0b 01 32 02 04 20 ...` stock 0x0976, ours 0x0a02; Roster Pack identical).
- Menu test (C): fe_mainmenu g16 = current menu; license select = 0x6cb83a85 (4 items call 906cdb81(i)); every
  forward Show resets g16 to it (ours 0x1718), Back keeps it; it leaves 0x6cb83a85 only after a license is picked
  (906cdb81 -> 756923b2(root 0x683c3520)) or created (fe_license_mii: 2004(21,0), then
  fe_mainmenu.83d8a37d(683c3520,0)). So "main menu idle and g16 != 0x6cb83a85" = license chosen.
- DLL test (C): Dispatch saw 2004(int 20, int i) and `byte[0xc2a520 + i*0x3af0 + 0x14] == 0` (the "empty slot"
  flag 2003(21) reads, 0x545ec0 `8a 41 14 c3`). Clear it when the active screen becomes fe_startscreen 0x172e17aa
  or fe_attract 0xfd87b4d0 (the attract demo race / title must not be "quit to the lobby").

## 2. Front-end screens (C; stock = Roster Pack for game, fe_navigator, fe_mainmenu, pause_menu, results)
- 1607 calls a module export (sync), 160b too, 160c queues a message, 160e reads the inbox. 160c = 0x4bb220
  cdecl(sender, target, msg, const {u32 value, u32 type}*, 0): al = 1 queued; 0 if 128 queued ([0xce5488]) or the
  target isn't running.
- fe_navigator: 6633d168 goto(screen), 4fce5e20 back(screen), dcfb2e31 full front-end reset (reloads resources).
  Goto: hide old (160b(old,1f6231b8,0), 160c(old,ad25bb91)), Show new (160b(new,52669830,prev)), 160c(new,331fb6b5).
- Active screen: blackboard key 0xb0567d36 (navigator writes the screen at the end of a transition, 0x0acc/0x0c1d
  `02 36 7d 56 b0 57 0a 32 02 03 1b`; "none" 0x987b66c0 during one, 0x09c3/0x0b15). game +0x038b writes "game"
  0xbed107e7; main_menu writes 0x7fe653ea; results: results_race 0x916482a8 (+0x379c/+0x4619), results_retry_quit
  0x603658c4 (+0x20d4, after the autosave), 0x31c9de8c. DLL read on the game thread: thiscall 0x4a4030(ecx =
  0x6eca14, key) -> {value, type}*; missing -> 0x6eca24 = {0, 2} (as the 1b01 handler 0x602100).
- Main menu -> lobby = Codex's Friends callback, p5 export 786fbb8e (ours 0x0cca): 1607(fe_network_game 8398d0bc,
  d1223911, [1]) (g23 = Friends), then CALL goto export 0ef84600 at 0x1317 (sounds, 160c(fe_navigator, 6633d168,
  fe_network_game), g3 = 0 at 0x141c).
- Idle gate in our fe_mainmenu (= Preview5): 0x1bd8..0x1c06 `g3 && !1c03(g7) && !1607(dialogs,53335ccb) && !0009()`
  = `57 03 57 07 32 01 03 1c 01 00 41 04 3d 4f 02 97 e6 4f 69 02 cb 5c 33 53 32 02 07 16 01 00 41 04 3d 4f 32 00 09
  00 01 00 41 04 3d 4f 14 ce 02`; gated input from 0x1c07 `32 00 19 04 01 00 5a 04`; input branches exit to the save
  check at 0x1ec2 `57 13 14 0e 00`.
- front_main's key 0x72d1c72b from the menu: NO (read only at start-up +0x0433 after a race; re-running it =
  1602/1601 restart = navigator dcfb2e31 full reload). After a race: +0x052b `07 01 01 a7 c9 b1 7a 4a 14 20 00` ->
  fe_network_game.c5cccce2 (g21 = 1, g22 = 0).
- Where the player is: fe_mainmenu (not the license select) safe; fe_network_game safe (ours); license select,
  fe_startscreen, fe_attract, license creation: wait; any other screen (charsel, trackselect, gp*, mission,
  multimode, shopping, options, leaderboards, credits, license records/trophies/friendslist) U: a DLL navigator goto
  would skip their exit code: keep the request until the player is back on the main menu; other mods' screens: wait.

## 3. Leaving a race from code (C, script bytes)
- Pause EXIT (pause_menu +0x06d4..0x080e): CALL 0x41d, 092d (unpause), the return key, 0439 (no-op 0x601580),
  2116 (0x5fab70 -> 0x4d4c40 clears GP state [0xd124f0]), 210a (0x5fa0f0: mission manager +0x1a020 = 0), 093f
  (0x58e820: [0xd12be4] = 0), 1b03(0x72d1c72b, key), 160c(node(4a8c2b95 "game"), node("QUIT_GAME"), 0); bytes at
  +0x07e0 `02 2b c7 d1 72 07 03 32 02 03 1b 01 00 55 01 18 02 95 2b 8c 4a 32 01 01 1b 01 00 56 01 3c 07 02 3c 51 00
  32 03 0c 16 01 00 56 01 18`.
- Results RETURN TO LOBBY / EXIT: CALL 0x5f83, then QUIT_GAME(1) (+0x29e2, +0x2b2e).
- game module (+0x058d `07 01 01 fe 3d 2e 68 4a 14 94 00`; QUIT_GAME 0x682e3dfe, game 0xbed107e7): leaves its loop,
  hides results/HUD; payload 1: loading 7c3c5f94, waits for the HUD to unload, 2010 and 1304 (save); payload 0: 095e.
  Then fe_game_bridge.10818739 and "Wait for loading screen"; exit export 7814851c -> 090a (Hook090a: EndRace +
  SendAbort(kAbortQuit)) -> 1601("main_menu") -> front_main.
- Hook point: the game loop's 160e(1), game +0x048c `51 01 32 01 0e 16 02 00 2f 00` (PC after = 0x0496), every
  frame while the race's message loop runs.

## 4. Recommended design
- Pending request in the steam_mp.cpp mailbox (g_boxLock): lobby id, friend name, source. GameLobbyJoinRequested_t,
  GameRichPresenceJoinRequested_t and the cold +connect_lobby only store it (replaces TakesSteamJoin's worker join).
  Drop it if it names our lobby; clear when Steam goes down or a newer one replaces it; optionally expire.
  kCmdJoinLobby (id in args[0..1]) is a session command with optimistic state Joining in Post's first[] (else the
  -5 state reads the old lobby's IN_LOBBY).
- Natives: 7a1c JoinPoll([menu]) -> 1 if pending, Steam up, menu != 0x6cb83a85; 7a1d JoinTake() -> takes the id,
  Leave if not idle, JoinLobby; kNatFrame result kFrameJoin = 4 when pending and no popup (before the race poll).
- Main menu (cases 1-4): fragment in our fe_mainmenu at 0x1c07: `GETG 16; NAT 7a1c/1; JF skip; HASH 8398d0bc; HASH
  d1223911; INT8 1; MKARR 1; NAT 1607/3; DROP; FRAME; HASH 8398d0bc; CALL 0x1317 argc1; DROP; JMP 0x1ec2; skip:`
  (g3 = 0 in 0ef84600 stops a second trigger). Needs fe_mainmenu re-pinned in build_menus.py (ships Preview5 as is).
- fe_network_game: in `initial` after the g22 check (current 0x1324..0x1333): JoinTake -> hide rows, footer(False),
  ACTION = -5; the same after `dialog_idle` in the search branch; in the lobby kFrameJoin ->
  leave_lobby_to_search(..., -2) (call_chunk), then the search check takes it. Failures: the existing notices (the
  user wants "notice, then the main menu").
- Case 5 (race): in RaceNativeBefore at the game 160e site, post once (latched) when a request is pending, the DLL
  license test passes, a Countdown/Racing/WFF update ran, not paused ([0xc2a015] == 0) and !g_netPause, and the
  active screen is "game" or results_retry_quit (never during the autosave): call the 2116, 210a, 093f handlers,
  then 0x4bb220(0, 0xbed107e7, 0x682e3dfe, &{0,2}, 0) (payload 1 at results_retry_quit, as results does). Hook1b01
  at front_main +0x0440: when pending, return {0x7ab1c9a7, type 3} ahead of g_returnToLobby (license test);
  front_main opens fe_network_game and its initial chunk takes the join.
- Risks: a posted QUIT_GAME skips the pause UI close (post only unpaused); skipping 2116 leaves a GP active; a host
  leaving an online race strands its guests (known); a request while a popup is open waits; the 128-message queue
  is shared (retry when 4bb220 returns 0).
