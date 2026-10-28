# Return to the lobby after an online race (M5 groundwork)

Static analysis 2026-10-20 (reverse-engineer subagent, end of the M4 session): decrypted exe, stock
`FrontEndScripts.sbn` (`c1f5ac48…`) and the Roster Pack copy (`1585d0de…`); the `results`,
`pause_menu` and `front_main` code below is byte-identical in both except front_main's tail. Scripts and
dumps: `re\m5-return\` (`data\` holds a copy of another mod's .sbn: never commit it). Nothing run in game
yet. C = confirmed in bytes/decompile, I = inferred.

## Finding
The PC scripts still hold the console's whole "back to the lobby" path. It doesn't run because the
results and pause menus choose their items by game type (we force type 1, Single Race), and front_main
goes to fe_network_game only for return keys no PC code writes. The empty screen after Escape on the
course select is fe_trackselect's Back with no target.

## Facts
| # | Fact | Where / bytes | Tag |
|---|---|---|---|
| 1 | The return screen is chosen in one place: front_main's startup on a return reads blackboard key `0x72d1c72b`: `0x906825eb`→fe_trackselect (fe_leaderboards for time trial), `0x30c8780a`→fe_multimode, `0x3ff581f5`→fe_gpmode, `0x22b6cbcc`→fe_mission, `0xb9bff4b5`→fe_shopping, `0x7fe653ea`→fe_mainmenu, **`0x7ab1c9a7`→fe_network_game export `0xc5cccce2`**, **`0xb0a0d459`→export `0x45b79703`** (both then go to fe_network_game). Then navigator `0xdcfb2e31(screen)`, bridge `0x82b4c524`, navigator `0x6633d168(screen)`. | front_main +0x3e0…0x650; the read at +0x438 `32 01 01 1b 01 00 92 00` | C |
| 2 | fe_game_bridge clears the key at race start (`1b03(0x72d1c72b, 0x7d6ee1fc)`), saves `0x1cde073e` = SetupObjectHash, stops front_main, starts `game`. No bridge message sets a return target. | fe_game_bridge +0x1bbf (from the Single Race path +0x24a3) | C |
| 3 | Results sets the key at the end. GetRetryExitMenu (+0x22fd) builds the menu from `0916` (game type `[0xcf7a58]`), `0938` (local players), `2112` (GP). Type 1, 1 player → menu 0: PLAY AGAIN (local restart), GO SHOPPING, EXIT TO MENU (key `0x906825eb`). **Type 5 → menu 6: one RETURN_TO_LOBBY item → `DoSelect_NetworkMainMenu`** (export `0x16c5d1c0`, +0x2ec4): key `0xb0a0d459`, then QUIT_GAME(1). | menu tables +0x0b00…0xe27; call at +0x22fd `32 00 16 09 00 00 2a 04` | C |
| 4 | Pause menu items (+0x0cdd) for type 5: [CONTROLS?, RETURN_TO_LOBBY, CONTINUE], no RESTART; RETURN_TO_LOBBY uses the EXIT handler. Its Quit code (+0x6d4…0x80e) has no network branch: 1 player → key `0x906825eb`, QUIT_GAME(0). | `32 00 16 09 01 00 2f 02` at +0x0cdd; key write +0x7e7 | C |
| 5 | The exports keep their stock bodies in our built lobby modules: `0x45b79703` sets g21=1, g22=1; `0xc5cccce2` g21=1, g22=0 (ours at 0x9bdc / 0x9bcd). Only our code reads g22; the lobby Show resets it (preview2 `assign(0x22,0)`). So g22=1 means "came back through the network return". | stock stub 0x1da7/0x1db6 | C |
| 6 | Script instances die only by `1602`, package unload or exit; nothing stops fe_network_game during a race, so its globals (racer, course, rules, FLOW) survive the race. | 0x4bafe0 callers; all 1602 sites | C |
| 7 | On entry our module runs Show (`0x52669830`, reloads fe_net_lobby), activate, then `0x331fb6b5` (g3=1, CALL 0x11db = our `initial` chunk = search_show with ACTION=-2): a return today would show Search and start a new search. | ours 0x1592 / 0x1734 / 0x17ee | C |
| 8 | GetRetryExitMenu is called only from the retry/quit code (+0x5c6b, 0x5eaa, 0x5f83); `0916` is read elsewhere for the finish display (+0x3529, +0x3618, list timer +0x25e9): an override must be limited to the one call site. | | C |
| 9 | Native slots (outside the Roster Pack's fingerprinted ranges): `0916` at **0x6f6048** = `a0 d5 58 00`; `1b01` at **0x6f5c2c** = `00 21 60 00`. `1b01` returns the stored value and type; every writer of the key stores a hash (type 3). | | C |
| 10 | The results flow (SHOW_RESULTS_LIST → SHOW_SEGA_MILES → SHOW_AUTOSAVE → START_RETRY_QUIT) is driven by the exe (strings at 0x6b9634…) and saves before the retry/quit menu (+0x1f5a). | | C (script order) / I (exe) |

## Why Escape on the course select leaves an empty screen (C logic, I runtime)
fe_trackselect's Back sends navigator goto-back `0x4fce5e20` with its global 2 (+0x1578…0x15e6), which
starts as NONE (+0x76) and is set only when the navigator opens track select going forward (+0x10ae).
After a race front_main resets the navigator (`0xdcfb2e31`: current and previous = NONE) and opens track
select with payload NONE, because our lobby never went fe_charselscreen → fe_trackselect. goto-back(NONE)
hides and unloads track select and opens nothing: backdrop, no input handler. (After an earlier offline
race in the same session, Back would open a stale fe_charselscreen instead.)

## Recommended design (data-slot hooks and our own lobby module only)
1. **DLL flag** "online race, return to the lobby": set when `NAT_RACE_LAUNCHED(1)` succeeds (host and
   guest), even if the contract fails later; kept outside `g` (EndRace resets `g`).
2. **front_main's read (covers every exit path, results and pause Quit):** hook `1b01` (slot 0x6f5c2c).
   When the flag is set, the call site is PC − module = **0x440** with code `32 01 01 1b 01 00 92 00` at
   PC − 8 (same style as `ResultsRowCall` in race.cpp), arg0 = {0x72d1c72b, type 3} and the stored type
   is 0 or 3: return {**0xb0a0d459**, 3} and clear the flag. Stock front_main then calls export
   `0x45b79703` and goes to fe_network_game.
3. **Console results menu:** hook `0916` (slot 0x6f6048): flag set and PC − module = **0x2305** with code
   `32 00 16 09 00 00 2a 04` → return 5. Results then offers only RETURN TO LOBBY (no local PLAY AGAIN,
   which would desync the pair).
4. **Console pause menu:** the same `0916` hook at pause_menu PC **0x0ce5** (`32 00 16 09 01 00 2f 02`):
   no RESTART, RETURN TO LOBBY instead; step 2 still picks the destination.
5. **Lobby re-entry (tools/netmenu/steam.py):** in the `initial` chunk (outside the main loop), after
   search_show: `if g22: g22 = 0; ACTION = -7`; in the frame's search branch `ACTION == -7 →
   shared_reenter_lobby`. Split shared_enter_lobby into its reset part (creation flags, rules and course
   defaults, privacy) and a common tail (ACTION=-1, FLOW=1, LOBBY_REV=-1, OTHERS=0,
   forest(0x0a, remove), `0206`(SEARCH_SCENE), g0x34=-1, lobby Show). `shared_reenter_lobby`:
   CREATING=0, CREATE_RESULT=0, AUTO_CREATE=0, START_RESULT=0, then the tail: keeps racer, course and
   rules, no racer popup, no Steam calls. The existing ERROR/OFF check shows "The host left the game." if
   the Steam lobby was lost meanwhile.
6. **Member data:** publish `ready=0` at launch; the lobby frame republishes 1 on re-entry, so the host
   can't start the next race while a guest is still on the results screen.

**16-bit jump limit (C, current build):** the main message-loop back-jump is at 0x949f → 0x1543 (32,607
bytes): only **160 bytes** left in the Roster Pack module (2,751 in the stock one). Step 5 adds ~35-50
bytes inside the loop. Before more lobby features, move the inline lobby Show block (1,920 B;
`re\m5-return\scripts\chunksizes.py`) into an uncalled pre-loop stub (e.g. stock 0xf9e `17 1b`) and
`call()` it (~1.9 KB freed); update build.py's call-target assert.

**Saves:** autosave (SEGA Miles, stats) runs before the retry/quit menu and is untouched;
DoSelect_NetworkMainMenu sends the same QUIT_GAME(1) as EXIT TO MENU and skips only the GP-end call;
pause quits don't save (stock). The call-site filters keep the save step seeing type 1. Low risk (C/I);
still compare the save against the backups (M5 box).

## Built and tested (0.5.0-m5b, 2026-10-21)
Steps 1-6 built as designed (step 1: the flag is set at our race's first 0905 call and cleared by
front_main's read or by an offline race's SetupRace). In game: the results menu shows one RETURN TO
LOBBY button (the PC text database has the label); the results' own choice is already `0xb0a0d459`, the
pause quit's is `0x906825eb` (our 1b01 hook turns it into the lobby); both PCs re-entered the lobby with
racer, course and rules kept, and a second race started from it. 0916's return type is 3 (hash), not
2. Jump margin after the change: 126 bytes (Roster Pack module), 2,717 (stock): move the Show block
before adding more lobby code.

## Open questions for the in-game test (answered above, except the last)
- Does the PC text database have a label for RETURN_TO_LOBBY (results and pause buttons)?
- Does the 1-button results layout (ANIMIN_END_1BTN) look right?
- On re-entry: are rows, icons and settings redrawn? Does F2 start a second race on both PCs?
- Only the Roster Pack's front_main was checked; another mod's front_main could differ (the byte checks
  then fall back to stock behaviour).
