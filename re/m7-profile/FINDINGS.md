# VIEW PROFILE / GAMER CARD on the results, the search rows and the lobby (2026-10-22)

Researcher pass (read-only), saved from its report by the main session (session "Night Shift"). C = seen in a file or computed,
I = inferred. Disassemblies: PC = `re\m7-messages\out\results.txt`, Mac = `re\m7-messages\out\mac\results.txt` (same format,
offsets are module offsets). The user's wish (PROGRESS box "VIEW PROFILE on the results"): the PS3 results' gold row is the VIEW
PROFILE cursor (footer "□ VIEW PROFILE / ✕ CONTINUE"); on Steam up/down moves the highlight, F1 (the PC's □) opens the player's
Steam profile in the overlay; computer racers' and test players' rows have none.

## Short answer
- The PC `results` module kept most of the console's cursor machinery but lost the part that runs it: the 8 per-row highlight
  animations are in the scene, the initial highlight call is still in the script; the Up/Down/F1 handler is missing, and it is
  the same size as one block in the Mac script (C).
- Plan: substitute a patched `results` module (like `fe_mainmenu`) that calls one new C++ native per frame; the native handles
  the keys, the cursor and the row-to-player lookup; a new worker command opens the overlay. About the size of the M6 footer or
  join work.
- The label that fits the footer is in the PC string table: `VIEW_GAMERCARD` (C2827EF0) and `GAMERCARD` (2B963CEE), both
  "GAMER CARD" in RaceResults_EN (hashes checked with `name_hash`, `re\m5-items\scripts\hashmap.py`) (C).

## (a) What the PC data has
### Results module
- Name hash `results` = 0x8a972456 (C). File `re\m7-messages\out\results.smosbin`, 26458 bytes; byte-identical in the stock and
  Roster Pack .sbn per `re\m7-results-title\FINDINGS.md` (not re-verified).
- `GetResultsSequence` (0x20ee) sets g33; 5 maps to g33 = 6 (kNetworkRaceResults). On our Single Race g33 is not 6, so the PC's
  `g33==6` branches never run (I: Hook0916 answers 5 only at 0x22fd and the pause sites, `src\race.cpp` ~1790-1860).
- PC STATE_RESULTS_LIST at 0x25cb:
  ```
  0x25cb HASH dialogs; HASH 53335ccb; NAT 1607/2; u3d; JF -> 0x2685     (a dialog is busy: skip)
  0x25e1 g36++ ; 0916()==5 ? g0c=g9 : g0c=g8       (limits g8 = 750, g9 = 3000 ticks; op52 at 0x06e2/0x06e8)
  0x2604 if (g36 > g0c) || NAT 0419(Enter) -> DoResultsList (call 0x2513), 1e00 FE_SELECT, 1b03(Results_ExitState,1), STATE_RESULTS_IDLE
  0x2685 FRAME; RET                       <- PC ends here
  ```
  The Mac has the same state, but 0x2612's JF goes to the cursor block at 0x2685..0x27d7 (see (b)); PC 0x2612 and 0x25de jump
  straight to the RET.
- Row highlight (C): PC `SetupRaceResults` init, 0x45df-0x4602:
  ```
  GETG 0x33; INT8 6; EQ; JF ...
  INT8 0; PUTG 0x14                                   (cursor g14 = 0)
  STR "RACERESULTS\BAR0" + (g14+1); HASH 0d09208a; HASH d279b4c1; INT8 0; NAT 020e/4
  ```
  (the same at 0x4664 on the other export; 0x464d is the un-highlight). `020e(node, A, B, 0)` plays the row animation from
  marker A to marker B: highlight (0d09208a -> d279b4c1), un-highlight (d279b4c1 -> 0d09208a): the pair the lobby uses for the
  local bar (`tools\netmenu\steam.py:46`, ROW_HIGHLIGHT/ROW_PLAIN). SumoToolResources.xpac entry 137 (the scene with
  RACERESULTS 58bacb6a and BUTTONS 8f20859f): the markers sit as 8 pairs at stride 0x3e8 (offsets 0x28b444.. for 0d09208a and
  0x28b46c.. for d279b4c1 in the inflated nested stream), records `name hash, frame, ...`: 0d09208a is frame 70, d279b4c1 frame
  80 (checked with `re\m7-results-title\scripts\sceneoffs.py`). Bars `RACERESULTS\BAR01..BAR08` (the Mac builds
  "RACERESULTS\BAR0" + (index+1); exact numbering I). Whether frames 70-80 look gold is I (the PS3 screenshot shows gold).
- Footer (C): `SetupButtonBar` (PC 0x3fa7, 4 args) calls `1607(pause_menu 791427eb, export fc0a943c, [a1..a4])`; pause_menu
  0x1950 builds one text: for each non-NONE argument i it adds glyph i, a space and the localized label `0222(BUTTONS 7cecc470,
  key)`; the text goes into a buffer bound to elements 56ff27d8 and a8541e22. Glyph table `pause_menu` 0x0086-0x00a8 (PUTG 4):
  U+F000 Enter, F001 Esc, F002 F1, F004 left/right for argument indexes 0, 1, 2, 3: argument 3 draws the F1 glyph.
  `SetupResultsBar` (0x4090) passes `["CONTINUE", NONE, NONE, NONE]`, called from 8 sites (0x4097, 0x4885, 0x4d74, 0x507d,
  0x5c97, 0x62c8, 0x6448, 0x649d). The Mac passes the same four values (0x425f); the console's second button is the scene element
  `BUTTONS\BTN02` (hash 725099a5, shown with 0217 / hidden with 0218; 4 times in entry 137). The PC has no code for it.
- F1 is `041d` (pressed this frame), Up/Down `0421`/`0422` (`re\m6-lobby\FOOTER-FINDINGS.md`, C). The PC `results` module calls
  041d nowhere; 0421/0422 only at 0x2903/0x2946 (the retry/quit menu's state, not the list).

## (b) How the console did it (Mac `results`, STATE_RESULTS_LIST)
Mac 0x2685, the block the PC compiled out (g14 is the cursor):
```
0x2685 GETG 0x33 ; INT8 6 ; EQ ; JF -> 0x27d9
0x268d GETG 0x14 ; NAT 226e/1          ; "this row has a gamer card / profile"
0x2698 JF -> 0x26ac
0x269b HASH 725099a5 (BUTTONS\BTN02) ; NAT 0217/1      ; show the button
0x26ac HASH 725099a5 ; NAT 0218/1                       ; else hide it
0x26ba INT8 0 ; INT8 1 ; NAT 090c/2                     ; (unknown, also at many setup sites)
0x26c6 GETG 0x14 ; NAT 0421/0 (Up) ; JF -> 0x2733
0x26d3 if g14 > 0 { g14-- }
0x26e3 "RACERESULTS\BAR0"+(old+1) : 020e(node, d279b4c1, 0d09208a, 0)     ; old row off
0x2711 "RACERESULTS\BAR0"+(g14+1) : 020e(node, 0d09208a, d279b4c1, 0)     ; new row on
0x2733 NAT 0422/0 (Down) ; if g14 < count-1 { g14++ } ; same two 020e calls (0x2751, 0x2770)
0x2792 NAT 041d/0 (F1) ; JF -> 0x27d7
0x279d STR "--> (results.smos) display gamertag here!\n" ; op16 (log)
0x27cb GETG 0x14 ; NAT 226d/1          ; "show gamer card of row g14"
```
- 226d/226e are Mac group 0x22 natives (names I; the script's use is clear: 226e(row) a bool, 226d(row) acts on the row). They
  appear only in `results`; the Mac lobby and search scripts contain no gamer card call (Mac `fe_network_game` search: 041d =
  join, 0418 and 041f = refresh, 0x238c-0x2413). The console's search-row and lobby "VIEW GAMER CARD" isn't driven by script
  natives we have (C absence; behaviour I).
- Native count diff PC vs Mac `results` (C): Mac only: 226d, 226e, 2203, 041d; Mac has extra calls of 1b03, 1b01, 090c, 160c,
  0916, 0217, 0218, 0421, 0422 and 020e.

## (c) Design for Steam
### 1. Results (the main part)
**Script patch** (new target in `src\script_subst.cpp` g_targets[]: `{0x8a972456, "results", {{<sha256 of the stock results
module>, kModResults}}}`; `tools\build_menus.py` outputs it like `mainmenu_with_steam_join`, lines 94-118 and 148).
- Compute the stock module's SHA-256 from the shipped .sbn; `tests\test_script_subst.cpp` already loops stock/pack/other.
- Insert a 12-byte `FRAME; CALL argc0 -> sub; DROP` before PC 0x25e1 (just after the busy-dialog JF): it runs each non-busy
  list frame. Do NOT insert at 0x2685: `smos_reloc.insert` maps a branch to the insertion point to after the chunk (`loc(p)`
  counts `k<=p`), so both exit JFs would skip it.
- The subroutine goes at the module's code end, like `Code.call_chunk` (`tools\netmenu\steam.py`) and `mainmenu_with_steam_join`;
  the module is 26.4 KB with 16-bit jumps, so ~300 bytes fit.
- Subroutine, in script words:
  ```
  s = 7a20(up=0421, down=0422, f1=041d)     ; C++: moves the cursor, handles F1, returns the new row (-1 = none)
  old = 7a21()                              ; C++: previous cursor row (-1 = none)
  if s != old: 020e(BAR[old+1], d279b4c1, 0d09208a, 0) if old >= 0 ;  020e(BAR[s+1], 0d09208a, d279b4c1, 0) if s >= 0
  if 7a22(): SetupButtonBar("CONTINUE", NONE, "VIEW_GAMERCARD", NONE)   ; CALL argc 4 -> 0x3fa7, only when the visible state changes
  else:      SetupButtonBar("CONTINUE", NONE, NONE, NONE)
  ```
  The `STR "RACERESULTS\BAR0"` concat is copied from Mac 0x26e3. The F1 glyph appears for argument 3 and the Esc glyph for
  argument 2, so "VIEW_GAMERCARD" goes in argument 3 (C by the glyph table; the label showing is I).
- New allowlisted natives in the `results` build: 7a20..7a22 plus 041d/0421/0422/020e (0421/0422/020e already exist in the PC module).

**C++ natives** (`src\natives.h` next free `0x20`; `Dispatch` already routes group 0x7A):
- `kNatProfileCursor = 0x20` (up, down, f1) -> the new row. Rows are places: `place -> human k` recorded by Hook0912
  (`src\race.cpp` ~1724-1736 computes `g.rowHuman` from `kFinishList[place*8]` and `RecordIndex(racers[k])`): add
  `g.placeHuman[8]` filled there, -1 for AI rows. A row is selectable only with a human and `!IsTestPlayer(id)`
  (`src\lobby.h`); the id is `g.c.humans[k]`. The cursor starts on the first selectable row (the console started at row 0);
  Up/Down skip rows without a profile. On F1 it posts `kCmdProfile` with the id's two dwords (a log line), at most once per half second.
- `kNatProfilePrev = 0x21`, `kNatProfileShown = 0x22`: the previous row, and whether the current row has a profile. Reset the
  cursor state when the results start (`resultsStarted`).
- Native 0916 at 0x25e9 with the ScriptSite pattern (`race.cpp` ~1711-1722), returning 5 there: the list limit becomes g9 = 3000
  ticks instead of g8 = 750 (the console had 3000 for online races, C at 0x25e9; 750 ticks is short for browsing, I).

**Worker command** (`src\lobby.h` commands and `src\steam_mp.cpp` ~390-397, the invite pattern):
```
kCmdProfile,   // args[0..1]: a Steam id (low, high dword): the overlay's profile page
...
if (c.kind == kCmdProfile) {
    CSteamID id(((uint64)(uint32)c.args[1] << 32) | (uint32)c.args[0]);
    if (id.IsValid() && !IsTestPlayer(id.ConvertToUint64())) SteamFriends()->ActivateGameOverlayToUser("steamid", id);
    Log(... IsOverlayEnabled ...);   // as the invite's
    return; }
```
On the worker; don't require `m_state == kLobbyInLobby` (the player may be mid-results). The command queue already carries two
dwords (`kCmdJoinLobby`, `LobbyPostArgs`).

### 2. Steam side (C)
- `third_party\steamworks\sdk\public\steam\isteamfriends.h:285`: `virtual void ActivateGameOverlayToUser( const char *pchDialog,
  CSteamID steamID ) = 0;`; the dialog "steamid" is documented at lines 273-274 ("opens the overlay web browser to the
  specified user or groups profile").
- `kCmdInvite` today (`steam_mp.cpp` ~390-397) calls `SteamFriends()->ActivateGameOverlayInviteDialog(m_lobby)` and logs "Steam
  says its overlay is off in this game" if `SteamUtils()->IsOverlayEnabled()` is false; the game thread posts through `LobbyPost`.

### 3. Search rows (VIEW GAMER CARD, key 039FF914, the console's search footer F2)
- F2 (`041f`, steam.py's INVITE_KEY) is unused on the search screen. The footer is `steam.py:134-142`; add `(3, 0x039ff914)` to
  `pairs` when browsing, and a `041f` check in the search update that calls a new `7a23(selected row)`.
- `LobbyRow` needs `uint64_t hostId`: the host sets lobby data `hostid` = its SteamID64 next to `"host"` (`steam_mp.cpp` ~715) and
  rows read it (~611). `GetLobbyOwner` needs membership (`isteammatchmaking.h:238-242`), so lobby data is the way. An older build
  gives hostId 0 and the key does nothing.

### 4. Lobby profile card
- The main lobby view reads no arrows (`FOOTER-FINDINGS.md`, C), so Up/Down is free. F1 (host SETTINGS), F2 (INVITE), Enter (host
  START), Esc and left/right are used. A new key would need another device id; the footer has no glyph for it.
- A cheap selection cursor: the bar highlight (020e on `g10[i][0]`) already marks the local bar (red), so reusing it would look
  like "me"; better a `> ` prefix on the selected member's name, written by C++ in `kNatMemberText` (ASCII font check I).
- Suggestion: results and search first; the lobby after the user picks a key.

### 5. Testing
- Offline: the new natives, the place-to-human table (test_race fake of 0912 with `g.rowHuman`), cursor wrap/skip (AI and test
  rows skipped), the `kCmdProfile` post (test_natives), the module build asserts (`build_menus.py`, offsets, stock-module hash,
  `test_script_subst`), the `ScriptSite` bytes for 0x25e9.
- Only in game: the highlight's look (gold?), the BTN02 element if left shown, the "GAMER CARD" label and glyph, the overlay
  opening on a friend and on a non-friend, the 4x list timeout.
- A cheap in-game look before building: make `0916` answer 5 at the 0x20ee call (a ScriptSite in race.cpp): the PC's own 0x45df
  code then highlights BAR01. Side effects of a type-6 sequence unchecked: a test only.

## (d) Open questions
1. Which lobby key opens a profile card? Suggested: results and search rows first.
2. Label: the PC strings have only "GAMER CARD" (RaceResults_EN 2B963CEE/C2827EF0); "VIEW PROFILE" (d1386d0c) is in
   navigator_info only (footer). Accept "GAMER CARD" on the results footer, or add a string?
3. Native 090c(0, 1) (the console's cursor block calls it every frame; PC at 8 setup sites): meaning unknown; needed? (probably not, I).
4. What `BUTTONS\BTN02` draws on PC and whether to hide it (0218) as the console did for rows without a profile.
5. The local player's own row: Steam allows it; the console's 226e answer for it is unknown.
6. Row-to-place numbering (BAR01 = first place) is I until a row is logged; DNF rows last (M7) to check.
7. A departed player's row: `g.c.humans[k]` still holds the id (fine for the overlay, I).

## Confirmed vs not re-verified
Confirmed: the disassembly offsets and snippets, the scene marker offsets, the name hashes, the header signature at
`isteamfriends.h:285`, the C++ lines cited. Not re-verified: the `results` module byte-identical in the Roster Pack (from
`re\m7-results-title\FINDINGS.md`), the PS3 footage details, everything marked I.
