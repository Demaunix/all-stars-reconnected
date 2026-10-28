# Steam bridge for the multiplayer menus (M2)

Plan written 2026-10-20. Status: design; facts marked C (confirmed in the exe) or I (inferred / Codex claim).

## Decision

Our own small bridge on top of the user-tested Preview5 menus (tools/netmenu), not the console's network script.
- The PC exe has **no console network natives** (C): native groups 0x00..0x21 exist; the group-0x22 cell
  (0x6f69f0 + 0x22*8 = 0x6f6b00) is unrelated float data, and dispatcher 0x54bb00 does no bounds check.
  The console/Mac script's 22xx calls (create 2202, search 2209, count 220A, result 2215, join 220B, ...)
  would jump into garbage on PC. Re-implementing their unknown semantics is a bigger, riskier job.
- Codex's bridge (researcher summary, 2026-10-20): it replaced the stock 1B00 table slot (0x6f5c28) and
  told its calls apart by a key hash plus pinned call sites; ints only; the script polled every frame; it
  never filled Game Search rows or passed strings.
- Ours: a 7-byte detour at the dispatcher 0x54bb00 (`8b 44 24 04 0f b6 d4`, resume 0x54bb07) that sends
  one private group number (> 0x21, so no stock script can call it) to our handler; everything else falls
  through untouched. Collision lead: AllStarsRosterPack.dll contains 0x54bb00 and 0x6f69f0
  (being analysed: notes/NATIVE-BRIDGE.md).

## Threads

- Steam runs on our worker thread (M1 design). Natives run on the game's main thread (I).
- Mailbox, one lock: the game thread posts commands; the worker applies them and publishes a snapshot
  (state, error, search rows, lobby members, settings). Natives only copy from the snapshot. No game
  pointer is ever kept or touched off the main thread.

## Native set (group TBD, ints unless noted)

| op | meaning | returns |
|---|---|---|
| Status | Steam ready? | 0/1 |
| Host(mode) | create lobby: 1 Friends = friends-only, 0 Anyone = public | 0 ok / <0 busy |
| Search(mode) | start search (Friends: friends' lobbies via GetFriendGamePlayed; Anyone: RequestLobbyList filtered on our key) | 0 / <0 |
| State | idle, creating, searching, results, joining, in lobby, error | enum |
| Error | last error: create failed, join failed, mods differ, lobby full, lobby gone | enum |
| ResultCount / ResultField(i, f) | rows (players, max) | int |
| ResultName(i, buffer) | host persona into a script text buffer (type 7) | 0/1 |
| Join(i) / Leave / Cancel | | 0 / <0 |
| MemberCount / MemberName(i, buffer) / MemberField(i, f) | lobby rows: name, racer, ready, host | int |
| SetLocal(racer, ready) / SetSettings(...) | member data / lobby data | 0 |

Strings reach the script only by writing into script-owned text buffers (native 1610 objects); details in
notes/NATIVE-BRIDGE.md.

## Game Search rows (C: decoded bytecode, 2026-10-20)

Stock PC constructor 0x2EB..0x50C builds global 0x0F = 8 row tuples:
`[rowGroup, node1, node2, node3, node4, node5, buf6, buf7, buf8]` (bufs = native 1610 text buffers).
Row groups: 30ef8abe 86597424 0162cf47 8cb75065 b2ddd2a5 81316b31 555f08ba ec479cc2. Row 0 nodes:
4fe590fb f31d7dad 1d88b4c3 1e09c98a 37d7e2e7 (others: decode the same range).

Mac fe_network_game (FrontEndScripts.sbn, sha256 be27c26b…; module
1fb8b642…), search update 0x1F0E, row loop 0x1FA6..0x20D0, per row i with result r = native 2215(i):
- `021e(node2, buf6)`, `021e(node3, buf7)`: bind the two text fields.
- `021f(node4, cf13b055 | 4bca8636)`: localized status, chosen by a result flag (r[4] == 0 or not).
- `0215(rowGroup, 74e7e7bc)`: reset row animation.
- i < count: `0217(rowGroup)` show; `0228(node1, g15[..][0])` signal art; `1613(buf6, name)`;
  `1614(buf7, "{0}/{1}", players, max)` (native 1614 = formatted text). Else `0218(rowGroup)` hide.
- After the loop: footer `55379672` with [6 f1141a0a, 3 72c62bd9, 2 d9529529, ...] when rows exist.
Mac search input, export f79faa54 at 0x22E9 (C, decoded):
- `0421` Up: if g36 > 0, g36--. `0422` Down: if g36 < g34-1, g36++. (g36 = selected row, g34 = count, g35 = previous.)
- Highlight on change: old row `020e(rowGroup, d279b4c1, 0d09208a, 0)`, new row `020e(rowGroup, 0d09208a, d279b4c1, 0)`
  (plus `1c04` calls on g0C we can skip), then g35 = g36.
- `041d` → helper 0x1291 (`220a` count > 0 → `2204`, `220b(selected)`) = join selected; then state dcfb2e31.
- `0418` and `041f` → refresh helper 0x1DEE. `0419` with property df63d5e2 == 1 leaves Search (create path).
- Footer icons (from our lobby/settings footers): 0 = `0419` Enter, 1 = `041a` Esc, 2 = `041d` (F1),
  6 = up/down, 7 = left/right. Labels: f1141a0a Select, d9529529 Create Game, 3a648438 Back,
  72c62bd9 probably Join (Mac search footer).

Our search screen design: show = Search(mode) + "searching" state; 0 results → the existing
Refresh/Create/Cancel dialog with neutral text ("No games were found..."); rows → Up/Down highlight,
Enter joins, F1 creates, Esc back; poll State each frame while joining, then open the lobby as guest.

## How the original online menus behave (UNVERIFIED reference)

From descriptions of clips of the original online menus (unverified; two were wrong until corrected from the frames).
Use as targets for M2; check each against the game before relying on it.
- Search: SEARCHING spinner under GAMERTAG | PLAYERS | STATUS | SIGNAL. No results → popup "No matches
  were found! Would you like to create a match?" REFRESH / CREATE / CANCEL (frame clip5, real).
- Result row: `<host> | 1/8 | LOBBY | signal bars`. Footer: REFRESH, [F2] VIEW GAMER CARD, [F1] CREATE GAME,
  [Esc] BACK, [Enter] JOIN. (Our build labels Enter with 72c62bd9, which reads REFRESH: wrong.)
- Create: LOBBY with a CREATING spinner and an empty panel (frame clip6, real), then SELECT RACER.
- Joining player: JOINING spinner and a "?" portrait on the others' screens until they pick a racer.
- Lobby bars: host "1P" red bar first, guests blue; racer icon; ready check (or a voice icon).
- Footer: host [F1] SETTINGS and START RACE (glyph missing on PC); guests [F2] Invite Friends, [Esc] BACK,
  [Enter] CHANGE RACER. Settings changes appear on guests' screens immediately.
- Host Back → "Are you sure you want to quit? This will end the session for other players." NO / YES.
  Guests then see "There was an error and the connection to the host was lost." OK → Select Mode.
- Start alone → "There must be at least two players in the lobby to start a game." OK (even with AI on).
- Race: loading ~1 s apart, countdown in sync; after the winner finishes, "RACE ENDS IN 30" countdown.
- SunshineFix (KartinX) drives the native menus over its own direct-IP transport; no public code. Other
  people's mods are out of scope (user, 2026-10-20): no detection or compatibility work; never copy its code.

## Script side (tools/netmenu stage 5)

- Search screen: replace the offline notice with Search(mode) + per-frame poll; fill the 8 result rows
  (row groups 30EF8ABE ... EC479CC2; their text/art nodes still to be found); Refresh / Create / Cancel.
- Create: Host(mode), then the existing local lobby screen; Join: Join(i), poll, lobby screen as guest.
- Lobby: row 1 from MemberName; Back = Leave + return to Search.
- Remove "OFFLINE PREVIEW" text.
- Startup no longer creates a test lobby; Steam "Join Game" into the menus comes later.
