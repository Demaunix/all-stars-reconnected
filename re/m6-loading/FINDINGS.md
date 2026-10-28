# Lobby -> loading -> race (reverse-engineer report, 2026-10-21)

Saved by the main session. C = confirmed in bytes/code (stock and Roster Pack identical; RP offsets = stock + 0xa2),
U = unverified. Scripts and dumps: `re\m6-loading\scripts\`, `re\m6-loading\out\` (its `ghidra\` copy is not for git).

**Bottom line:** the PC already shows the console's online loading screen for our races: the "SONIC SAYS..." tip
box is the ordinary Single Race loading screen (scene `Loading/Loading_tips` over the course's loading picture).
No game-type-5 branch picks it; no hook needed. Hold A keeps it up on every PC until all have loaded.

## 1. Today's path (C)
- Our lobby (steam.py start_race) sends fe_game_bridge characters, devices, course name (g6), start `9b3cf8f9`.
- Bridge stock 0x25cd `07 01 01 f9 f8 3c 9b 4a 14 0c 00 17 34 00 b8 f8`: on 9b3cf8f9 CALL 0x1e9b (race routine);
  its end 0x226d `02 f9 f8 3c 9b 34 01 5c f8 ff ff`: CALL 0x1ad8(9b3cf8f9) (show loading), then CALL 0x1b1d.
- 0x1ad8: 0002(0); 1607(fe_navigator, d562ebef) (info bar out); 0x12fa(type) (builds the loading screen);
  160b(fe_navigator, 86f2986c) (front end torn down: our lobby page gets 1f6231b8/ad25bb91, navigator scenes unloaded).
- 0x1b1d: return key 72d1c72b = 7d6ee1fc, 1cde073e = setup object, MUSIC_MAIN_MENU, 1602(front_main), 1601(game)
  (game starts hud, results, pause_menu, loading, gp_podium).
- 0x12fa (`0b 01 27 0b 00 51 00 27 11 00 04 "LOADING_TIPS" 27 0d 00`): g0d = "LOADING_TIPS"; loads and waits for
  Loading/Loading_tips (0x137c, 0200 then 0203), loads Loading/ + g6 (course picture, root d60d97fb), shows both
  (0205); type 9b3cf8f9 handled as bcfa8fab (0x15ec) -> 0x9ce(1,0) (0x15bb): a random index (1d02, shared MT19937
  0x4be040, state 0xce7c58), 4 consecutive tips from g16 (76 text keys, built at 0x0960) into the box's 4 text
  pairs, animation 39f446f4 (cycling the 4: inferred, PS3 showed 3 tips in ~20 s).
- Variants (all in fe_game_bridge, chosen by the start type): Loading_tips (Single Race and other starts 2a65cc8a,
  ec433708 test drive with MSG_TUTLOAD tips, c7473f18); Loading (missions 1a50d454; tips after 10 s, 0x3024);
  Loading_gp (GP a22601ef); Loading_sellscreen / Loading_demo (demo builds, blackboard g1); Loading_tips type 8c8f5b15
  (race -> front end, 0x10818739 via 0x9ad). The `loading` module is only the race resource loader.
- Removal: REMOVE_LOADING_SCREEN is posted only from 0x5b0e70 (`0x5b0ecb 68 8c 94 6b 00`, the only reference to the
  string at 0x6b948c), called only by WaitingToStart 0x5b0f50 (slot 0x6b20b8) / 0x5b0f40 (slot 0x6b20dc); loading
  0x0c30 sends the bridge 7b8f757e; bridge 0x2460 -> 0xddc hides and unloads every loading scene.

## 2. The console's online loading screen survives (C)
- SumoToolResources.xpac entry f7b73863 (nested zlib at +0x48) = Loading_tips_EN: root 4830496e LOADING_TIPS, 8 tip
  elements, 354c23a1 "SONIC SAYS...", 603df105 "LOADING", 83 strings. The PS3 footage's four tips are in it and in
  g16 (facf1d75, a5446f56, 6d0bbdb6, bc52bb88). PS3 6:00-6:38: lobby, tip box over the course picture, flyover, grid.
- Chosen only by the start message (0x1ad8/0x12fa argument): 0916 is called only in hud, pause_menu and results.
- Only type-5 branch near the start: TrackIntro enter 0x5b0fa2 `call 0x4cc130; cmp eax,5; je` skips 0x4bc510
  (INT1_/INT2_ course voice lines); our races are type 1: the voice plays (cosmetic).
- Codex's "black loading previews" fit a hash course name (`Loading/1641522085` has no picture).

## 3. Change: none needed. No network waits in the loading code (only scene loads 0203, the sell-screen 4 s, the
mission/GP 10 s swap). Optional (U): the tip draw happens before our SetupRace reseed, so PCs usually show different
tips; reseeding with the contract seed before MSG_START might match them. A course without a Loading/<name> picture
gives a plain background (0203 skips a missing file, U).

## 4. Holds and intros (C, plus earlier in-game logs)
Hold A keeps REMOVE_LOADING_SCREEN unposted: each PC stays on the tip screen (menu music) until released (M3: the
first to load waited 531-625 ms; M5 internet: 5125 ms). Then TrackIntro (enter 0x5b0f60: scenic camera,
INGAME_LOADING, voice line; update 0x5b1120 sends HUD_TRACKINTRO; ends with the flyover or a local skip),
CharacterIntro (enter 0x5b1390: camera from one shared-RNG draw after the reseed; update 0x5b1640, skippable after
1.0), Countdown (enter 0x5b17a0: UNLOAD_TRACK_INTRO_RESOURCES, HUD_COUNTDOWN_INIT). Intros can drift (each player
skips their own); hold B re-aligns everyone before "3". Possible polish (U): gate TrackIntro/CharacterIntro updates
(slots 0x6b2100 / 0x6b2124) so one skip skips for everyone.
Not verified: a PC screenshot of the loading screen (ask the user: course picture plus "SONIC SAYS..." with tips).
