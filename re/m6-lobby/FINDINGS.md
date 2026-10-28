# M6 lobby bars: what the console/Mac lobby does (reverse-engineer report, 2026-10-21)

Read-only research; nothing run in game. Scripts and dumps: `re\m6-lobby\scripts\`, `re\m6-lobby\out\`
(`out\scene\1252_e01cf5dc_48.bin` = the PC fe_net_lobby_EN scene, `out\sprites\*.png` atlas crops).
`re\m6-lobby\data\` holds copies of game script packages: game data, don't commit.

Sources: Mac FrontEndScripts.sbn (be27c26b…), stock PC sbn (c1f5ac48…; its fe_network_game = the pinned
stub), Roster Pack sbn (1585d0de…), the Mac i386 binary (native cells 0x663550, group 0x22 at 0x664094),
PC exe `re\bin\SSASR-unpacked.exe`, PC lobby scene `SumoToolResources.xpac` entry 1252 (hash e01cf5dc).

## LOBBY_TUPLES (g0x10), CONFIRMED
Stock constructor 0x050D..0x068C: 8 tuples `[group, n1, n2, n3, n4, n5, buf6]` (buf6 = 1610 text buffer of
32 bytes: 31 characters). Mac constructor identical.

| row | group | n1 badge | n2 name | n3 portrait | n4 voice | n5 ready |
|---|---|---|---|---|---|---|
|0|cec457a2|7be7f85f|7d63b234|a2eab366|4e7d2ee9|072d68f4|
|1|c1afeaef|dc36fbfa|c04132b7|02e6dafc|e9cc98e1|5d40547b|
|2|77e51b23|c234a04a|95a72e05|0b01e8b2|6c2a3eb0|4d5162c6|
|3|cfd654a0|b1c61d3d|1c46920f|2e72fd7f|e9cb15e3|23b15302|
|4|8601b69f|dbd30dd0|9f8f28f1|9a1c0c73|9f1504f9|78b4ca21|
|5|1055715d|4e4979d8|0b6a58ec|b68eb8a0|00154c74|58698126|
|6|e47d740b|e94457bd|07234dbf|e032e23d|b9fa1d90|63a9f597|
|7|56e40343|6dcfd09f|652ea4e4|1487cf4a|fcc01dd8|341eb8e0|

Roles confirmed by the Mac draw code, the PC scene defaults (n1 = its own nP badge sprite, n3 = Sonic's
lobby portrait 6d5ee0b1, n5 = 9309f511 green check, n4 = 24064704: a white speech bubble, 54x46, atlas page 0 (the
voice icon; CORRECTED 2026-10-23: the earlier "fully transparent" reading came from `scripts\atlas.py` taking the page
index from before the hash, i.e. the previous record's; `re\m7-voice\out\list2.py` parses the records right)) and atlas crops. "?" portrait art: 70e16366. Badges 1P..8P (g2b, constructor 0x0F2F):
`106e548c a579ecc3 9bc6eef3 88ddffbf f7b4fb3b ea5e1e3e f07b78ba e9521b6e`.

## How the Mac draws a bar (export ea107c57, row loop 0x2DA7..0x30B9), CONFIRMED
n = 2219() members; for row i < n, r = 221c(i), ch = g13[r[6]]:
- name: `021e(g10[i][2], g10[i][6])`, `1613(g10[i][6], r[0])`
- portrait: r[6] == 0 ? `0228(g10[i][3], 70e16366)` : `0228(g10[i][3], ch[0])`
- `0217(g10[i][0])`; voice r[8] ? 0217 : 0218 (n4); ready r[2] ? 0217 : 0218 (n5)
- badge: r[1] >= 0 ? {0217(n1); 0228(n1, g2b[r[1]])} : 0218(n1)
Rows i >= n: 0218(group). Lobby entry (d9166bdd 0x29CC): every row `021e(n2, buf)`, `0215(group,
74e7e7bc)`, `0218(group)`.

Member info 221c(i): [0] name, [1] slot 0..7 or -1, [2] ready = P[0x3cec] bit 2 (set by 2205(pad, flag):
1 when idle in the lobby, 0 while Settings or the racer popup is open: the check means "in the lobby,
not in a menu"), [5] in race, [6] character hash (type 3) or 0 = none, [8] voice.

Local bar: not separate art but the row highlight. Lobby entry: g2c = -1, g2d = the local member's row
(2221(i)). Each idle frame (0x3696), if g2d != g2c: `020e(g10[g2c][0], d279b4c1, 0d09208a, 0)` then
`020e(g10[g2d][0], 0d09208a, d279b4c1, 0)`, g2c = g2d. Scene keyframes: 74e7e7bc frame 1, 0d09208a frame
10, d279b4c1 frame 20. PS3 frames show exactly one red, wider bar (the viewer's): that this animation makes
it red is inferred (check in game).

## g13, CONFIRMED
Constructor 0x092B..0x0BD2: dict (MKARR type 9) of `name_hash(identity) -> [lobby portrait, popup portrait,
CHARACTER_* text key, in-race progress icon]`; Roster Pack 25 entries. The lobby portraits are in the PC
lobby scene for the 24 stock racers + the console-only racers; mechasonic's e76b1254 is not in the scene file
(the M2 test showed Metal's icon in the lobby, so the Metal mod most likely adds it at runtime).

## "?" / choosing, CONFIRMED
"?" = r[6] == 0: no character yet. Reopening Change Racer clears ready but keeps the old character (others
see the old portrait without the check). While a race runs, waiting joiners alternate "WAITING IN LOBBY"
(597ebb48) with their name and get no badge.

## PC VM pitfalls, CONFIRMED in the PC exe
- Dict lookups need a type-3 key: 0x48b7c0 says "not less" for different key types, so `g13[int]` returns
  some other entry's tuple. Branch on "no racer" before indexing. A missing hash key gives none
  (0x48cfb0); `none[0]` gives none (0x49023e).
- JF (0x48bf30) treats non-zero int and hash as true.
- Opcode 0x3C does nothing to ints or hashes (0x48af40); strings get name-hashed.
- 0228 with an unknown element or art does nothing (0x4a7240 -> 0x485f20): no crash, the old art stays.
- Our module overwrites g2b (Preview2's PRIVATE_SLOTS = 0x2b); g2a, g2c, g2d are taken too: use literal
  badge hashes.
- Settings `draw_lobby` / Preview2 `draw` write row 0's name (bind 0x7d63b234 to buffer 6, racer label) and
  portrait (0228 0xa2eab366) on every redraw: drop or redirect them if row 0 isn't the local player.
