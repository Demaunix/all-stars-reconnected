# M7 "joining while a race runs": lobby progress bar, race HUD toast, console joiner flow (reverse-engineer report, 2026-10-21)

Saved by the main session. Static analysis only; nothing run in game; no game or installed files touched. C = confirmed in
bytes or decompile; I = inferred. Script offsets are module offsets (the PC after an 8-byte native call is offset + 8).
Sources: `re\bin\SSASR-unpacked.exe` (Ghidra copy `re\m7-join-race\ghidra`, read-only, not committed); the Mac
FrontEndScripts and Mac binary (natives in group 0x22); our built `menus\out\fe_network_game*.smosbin`; scenes from
Codex's backup `...\outputs\Metal-Sonic-Race-Test\backups\SumoToolResources-original.xpac` (same size as the installed
archive; its lobby entry e01cf5dc is byte-identical to `re\m6-lobby\out\scene\1252_e01cf5dc_48.bin`, C). Helper scripts:
`re\m7-join-race\scripts\` (symstack.py, scenetree.py, dumpmod.py, vmops.py, fhash.py, findptr.py, crop1.py,
scenetab.py); outputs `re\m7-join-race\out\` (game data; not committed).

## Q1. The lobby's "race in progress" display

### 1a. The progress bar is on PC: scene `fe_net_lobby_posbar` (C)
- Archive entries: EN = F0FEDAE8 (hash of `.\Resource/SumoToolResources/fe_net_lobby_posbar_EN.stz`); GE 40F8E95D, FR
  61392FCF, IT 5CEB5AAA, SP 4940A124. Dump: `out\posbar-EN.txt`.
- One scene, 3ee6042a, root group dd942497. Root keyframes: b2244a6d ANIM_ON_START (t0, colour 00ffffff), 02f0b24c
  ANIM_ON_END (t10, colour bfffffff), aff91553 (t11); all at y 0.6917 (below the bars, above the LOBBY title). The 00
  alpha byte means the root is invisible at frame 0 (I, ARGB).
- Root children (C): 6e406156 the track line (sprite 555455f4); c65f5733 the checkered flag (sprite e13d06af, x 0.852);
  8 marker groups, each a coloured nP triangle plus a head icon (default Sonic head 1c24b2a6):

| k | marker group | nP triangle (sprite) | head icon node |
|---|---|---|---|
| 0 | 05d75801 | 80e365e0 (01650a39, blue 1P) | ea88ea7d |
| 1 | ac2c9bf7 | bbc73e53 (c46e6ff6, red) | a20f540f |
| 2 | cbaddd74 | 5b6de9f3 (306888c2, yellow) | bf672c37 |
| 3 | dd4ce362 | f4cd4e36 (99c433a2) | 718f80b2 |
| 4 | bacfbf36 | d38ff118 (cef632bc) | 7bb3b824 |
| 5 | 9df3374a | b56c847b (1889c52c) | 77d6cef9 |
| 6 | 877b8649 | 164d2418 (283df1bb) | a91bc8b3 |
| 7 | 1b0d3544 | 24182d44 (d5a6719f) | 045680e0 |

- Marker movement: each marker group moves on keyframes 3e3460a8 (t11, x -0.498) to f5fda6b0 (t51, x 0.195); the flag
  sits at x 0.852. Text table: 1P..8P, START, END, "CHEAT STRING" (no TEXT object uses them). Character icons: every stock
  character's in-race icon (g13[...][3]) is in this scene's atlas: 20/20 stock, 24/25 Roster Pack (missing: Metal Sonic
  mechasonic 9cabac57, icon cff23477; the Metal mod may add it at runtime, I).
- Our lobby already loads and attaches this scene: load `0200("Resource/SumoToolResources/fe_net_lobby_posbar",1)` at
  ours-stock 0x15f9..0x162b (with the lobby and popups loads); attach `0205(3ee6042a)` in the stock Show helper export
  33d02ae6 (stock 0x1ba7 `02 2a 04 e6 3e 32 01 05 02 01 00 7b 04`; ours-stock 0x7091; ours-RP 0x76a1/0x76a6);
  `search.remove_lobby` already removes it.
- A frame of PS3 online footage shows exactly this layout (C, visual): the track line with a head
  icon near the flag, under the bars; waiting joiners with "?" portraits and no badge; footer INVITE FRIENDS / BACK.

### 1b. The stock "show the bar" function exists on PC but nothing calls it (C)
Stock 0x1ccf (= Mac 0x285a): `51 01 27 25 00 57 22 3d 14 6f 00 02 97 24 94 dd 32 01 17 02 ...`: g25 = 1; if !g22:
`0217(dd942497)`, `0215(dd942497, 02f0b24c)`, then 0218 on every g28 marker and g29 node (g29 = 7 nodes ca1ac51c... in no
scene of the archive). No callers (no CALL to 0x1ccf in stock, nor to 0x71b9 in our build). **Do not call it**: our
module reuses g26, g27 and g2a (Codex's SELECT, LAPS, BUFFERS), and g22 is our RETURNED. Use literal hashes instead.

### 1c. How the console lobby drew it (Mac fe_network_game export ea107c57, the per-frame lobby function) (C)
- Race data (Mac natives, group 0x22; the PC has none): `226a()` -> [racing (bool), total laps, racer count] (Mac
  0x213c60 reads the race-status block R = [net]+0x1374: R+0 count (non-zero = running), R+4 laps). `2269(slot)` ->
  [place index, lap, fraction (float), valid] (Mac 0x213ad0: R+8[slot] place via 0x19c8f0; R+0x28[slot] lap; R+0x70[slot]
  float fraction; R+0x48[slot] & 1 valid). R is serialized to the network by its method at 0x19c900 (I).
- Gate: the bar and status show only when `g25 && !g22 && 226a()[0] == 1` (g25 = race in progress for this viewer; g22
  = came back from a race): only fresh joiners see it; racers who return early via RETURN TO LOBBY do not.
- Per bar row i, for a member r = 221c(i) with r[5] (in race) and slot r[1] >= 0: p = 2269(r[1]); if p[3]:
  `0228(g2a[i], g13[r[6]][3])` sets the head icon (Mac 0x2f48..0x2f5c); `0217(g28[i])` shows the marker; `0216(g28[i],
  3e3460a8, f5fda6b0, float clamp(p[2], 0, 1))` positions it (0x2f72..0x2f87, `07 04 38 32 04 16 02`; op 0x38 casts to
  float). Otherwise `0218(g28[i])`. The marker index is the bar row, not the badge slot.
- Status text (Mac 0x2de5..0x2edb): alternates with the name: g27 counts 0..2*g26 with g26 = 250 (op 0x53 0xfa): 250 lobby
  frames name, 250 frames status. Racing member: `021e(name node, buf)` then `1614(buf, 0222(a363a411, 1de2f2dd),
  0222(a363a411, POS_k), lap, totalLaps)` = "1ST (LAP 2 OF 3)" (POS_1ST..POS_8TH = 7cea650b..7df05266 "1ST".."8TH"; the
  script's "POS_5ND" typo hashes to f474c981 "5TH"). Non-racing member: `021f(name node, 597ebb48 "WAITING IN LOBBY")`,
  unless 2234().
- Race over (Mac 0x2c7e..0x2d57), when `2234() && 223a()`: g25 = g22 = 0; `0218(dd942497)`; for each marker 0218 plus
  `0216(m, 3e3460a8, f5fda6b0, 0.0)`; if `2249(0) == 0` (local player has no racer, I) the racer popup opens automatically
  (`020e(a653f569, ANIM_ON_START, ANIM_ON_END, 0)`, g31 = 30e24bb3, CALL 0x43f6); else a guest's footer is redrawn (CALL 0x4279).
- Text available on PC in the lobby file (fe_net_lobby_EN e01cf5dc) (C): 1de2f2dd "{0} (LAP {1} OF {2})", c8aa0584 "(LAP
  {0} OF {1})"; 597ebb48 "WAITING IN LOBBY", 1c2a9041 "VIEWING RESULTS"; "1ST".."8TH" (c8aa0584 and 1c2a9041 unused in
  both the Mac and PC scripts).
- Fallback nodes: the lobby scene (8c2f0f09) has no status column. Status text can go into each bar's name TEXT node (row
  k node n2, e.g. 7d63b234), whose buffer is 32 bytes / 31 chars ("1ST (LAP 2 OF 3)" and "WAITING IN LOBBY" are 16). Left
  panel group 135fd519: course name 9cfb0450 (our COURSE_NAME), course art 7ac9264c; its labels and values are the rules'.

### 1d. Engine helpers (C)
All called with ECX = 0xc282a8 as the native thunks do; the element helpers ignore that ECX and use the element map 0xc28340.

| native | helper | arguments | returns with | effect |
|---|---|---|---|---|
| 0217 show | 0x47f1a0 | (hash, 1) | ret 8 | vtbl+0x4c |
| 0218 hide | 0x47f1d0 | (hash, 1) | ret 8 | vtbl+0x54 |
| 0215 / 0216 pose | 0x47f090 | (hash, keyA, keyB, float t) | ret 0x10 | 0215 = (h, k, k, 0.0) |
| 020e play | 0x47f8c0 | (hash, keyA, keyB, c, loop) | ret 0x14 | 4-arg form passes loop = 1 |
| 0228 art | 0x47f400 | (hash, art) | ret 8 | |
| 021e bind live | 0x47f370 | (hash, textobj) | ret 8 | sets el+0x108 = obj, not owned |
| 021f localized key | 0x47f3d0 | (hash, key) | | unbinds the buffer: 0x4855d0 calls 0x4853c0(0) |
| 0213 idle | 0x47f130 | (hash) | ret 4, al = 1 | el+0xd6 == 0; returns 0 if the node is missing |
| 0222 text | 0x47fb50 | thiscall ECX=0xc282a8 (sceneHash, key) | ret 8 | char* or 0 |
| find TEXT element | 0x483860 | ECX=0xc28340 (hash) | | text object at el+0x108 |

Group propagation: a GROP's time window (vtbl+0x10 = 0x488050) and show/hide (0x4887a0/0x488810) recurse into children,
so 0216 or 020e on a group moves its sprites (C). 0216 needs a real float (`fld dword [args+0xc]` at 0x4a6cd7).

### 1e. Smallest safe implementation for Q1
1. In C++ inside our per-frame lobby native (kNatFrame, 0x7a16): it runs every lobby frame in script context (no new
   bytecode; avoids the 16-bit jump limits).
2. While "race running && I am not in it": once, show 0x47f1a0(dd942497, 1) and pose 0x47f090(dd942497, 02f0b24c,
   02f0b24c, 0.0), hide all 8 markers; each frame, per racing member with row k: art 0x47f400(head_k, g13 icon), show
   0x47f1a0(marker_k, 1), pose 0x47f090(marker_k, 3e3460a8, f5fda6b0, frac).
3. When it ends: hide dd942497 and every marker, reset each marker with frac 0.0.
4. Status text: every 250 frames, write "<POS> (LAP n OF N)" or "WAITING IN LOBBY" straight into the bar's bound buffer
   ([el+0x108]: check vtable 0x6a71f8 and capacity at +8), then restore the name. Strings from 0x47fb50(0xa363a411, key).
   Don't use 021f/0x47f3d0: it unbinds the buffer.
5. Data: each racer publishes {place, lap, laps, fraction} about once a second (Steam lobby member data is enough).
   place = 0x4cbc40(creation index), 0-based; lap = rec+0x7c ([0xcf7a68]+i*0x90; notes\LAPS.md); fraction =
   0x51a000(rec) / (laps * lapLength), 0x51a000 thiscall on the record = 0x519f40() - rec+0x88, where 0x519f40 =
   (rec+0x7c - 1) * lapLength + in-lap distance, lapLength = [[0xd12560]+100] (C formula). The HUD uses H+0x348 as the
   divisor in split screen (0x501490, C); its writer not found (I).
Risks: the root's alpha multiplying its children and the scene being invisible now (nobody reported a stray bar); the
8-bar layout vs the bar position (screenshot); Metal Sonic's icon missing (the marker keeps its previous head); never call
0x71b9 or the stock function.

## Q2. The race HUD toast

### 2a. The element exists in the PC single-screen HUD (C)
- CommonHud_EN 68F7E707 holds 13 scenes. The single-screen race uses 91c494ea "HUD": `hud` 0x55c9 builds g3d = ["",
  "HUD", "HUD_2P_SPLIT", ...][0917()]; `0205(g3d)` at hud 0x5663.
- Toast nodes: group c49502fc is a root of scene "HUD" (keyframes fbc94a42 t0, 3e3460a8 t1, f5fda6b0 t480). Child TEXT
  ca4107ae: font 276607d1, placeholder key 8a1964cb "PLAYER DAZZLER HAS LEFT THE GAME.", align 0; scrolls on keys
  5e391a57 t0, e2be7156 t1 (x 1.0039) and e33aacae t480 (x -0.918), at y 0.9056: a right-to-left ticker of about 8 s at
  60 fps (I). Clip SCIS 834bf6ee at x 0.075, y 0.876. The PS3 frame `sheets\04200L.png` shows it bottom right (C).
- Font 276607d1 has only ASCII 0x20..0x7E plus U+FFFF (C): map non-ASCII name characters to '?'.
- Binding: the hud creates g_a9 = `1610(64)` at hud 0x2488 (`55 40 32 01 10 16 00 00 cf 02 27 a9 00`), binds it with
  `021e(ca4107ae, g_a9)` at hud 0x58b6 (`02 ae 07 41 ca 57 a9 32 02 1e 02 00 00 ec 08`), frees it at hud 0x883c. Nothing
  else on PC writes g_a9. Stock and Roster Pack hud identical (SHA-256 prefix 397b8e2c, C).

### 2b. The console's toast code is the Mac hud function at 0x62c0; on PC an empty stub (C)
- Call site: the per-frame export baa59db4 has, if g3a == 5: CALL 0x5b02 (name tags), then CALL toast. PC hud 0x36a8:
  `34 00 7a 2b 00 00 00 00 13 04` calls 0x622c = `17 1b` (empty). Mac 0x36a8 calls 0x62c0 (real code).
- Mac 0x62c0 step 1 (queue): reads network events n = 227c(); e = 227d(i) = [id, kind, name] when id > -1; queues names
  by kind: 0 joined lobby (ga7, 8da10d88 "{0} HAS JOINED THE LOBBY."); 1 left game (ga1, 993d0c07); 2 left lobby (ga3,
  967e55cf); 4 returned (ga5, 265035ad); 3 ignored.
- Step 2 (show one): if `0213(ca4107ae)` (text idle; Mac 0x637d), shows one queued name. Priority: left game > left lobby
  > returned > joined. The joined case at Mac 0x656d: `1614(ga9, 0222(91c494ea, 8da10d88), name)` then `020e(c49502fc,
  3e3460a8, f5fda6b0, 0)`, then pop the queue (bytes `02 88 0d a1 8d 32 02 22 02 ... 32 03 14 16 ... 02 fc 02 95 c4 02 a8
  60 34 3e 02 b0 a6 fd f5 51 00 32 04 0e 02`).
- Formatters: 1614 (0x5fd280): `{d}` with a single digit, string arguments only, writes into obj+0xc with no bounds check
  (C). "RACE ENDS IN {0}" uses the same pattern: hud 0x84c4 `1614(g5f[arg3], 0222(N(g3b), N("RACE_ENDS_IN")), str(arg2))`,
  name_hash("RACE_ENDS_IN") = 7cb8f963.

### 2c. Smallest safe hook for Q2
1. Don't make g3a 5 (it would also skip the split-screen tag branch (0938 at hud 0x36b6) M6 relies on).
2. Run a C++ ToastUpdate in RaceNativeAfter for native 0938 at hud 0x36b6 (PC 0x36be; bytes `32 00 38 09 00 00 18 04`,
   identical in stock and Roster Pack): the else-branch of the console's toast call, once per HUD frame in script context.
3. Copy the Mac logic: queue (kind, steamName) with the same priority; if 0x47f130(0xca4107ae) is true: el =
   0x483860(ECX=0xc28340, 0xca4107ae); obj = [el+0x108]; require [obj] == 0x6a71f8 and [obj+8] == 64; fmt =
   0x47fb50(ECX=0xc282a8, 0x91c494ea, key), falling back to the English text; replace "{0}" with the name (ASCII only, at
   most 63 bytes, NUL-terminated) and write it to [obj+0xc]; play 0x47f8c0(0xc49502fc, 0x3e3460a8, 0xf5fda6b0, 0, 1).
4. No timer needed: 0213 goes true when the 480-frame scroll ends.
5. Safe outside a race: in the front end CommonHud isn't loaded, 0x47f130 returns 0 and the element lookup returns 0.
Risks: that rendering reads the bound buffer each frame (I; the Mac binds first and writes later, which supports it);
scroll speed and clip box (I).

## Q3. The console's lobby behaviour for a mid-race joiner (Mac, plus the PS3 frame) (C unless marked)
- Entry: front_main maps 1b01(72d1c72b) == 0x7ab1c9a7 to fe_network_game export c5cccce2 (g21 = 1, g22 = 0); 0xb0a0d459
  maps to 45b79703 (g21 = 1, g22 = 1). The lobby entry (d9166bdd, Mac 0x2a7f) with g21 set does `2205(pad, 1)` and CALL
  0x285a (show bar), and opens no racer popup. The idle state 9150e377 (Mac 0x31d0) does the same when 2233() (race
  running): 0x285a, then g31 = bf65a0ce.
- Can the joiner pick a racer or ready up? No racer pick during the race: the racer key (041f) is skipped while g25 (Mac
  0x355c `57 25 3d 14 42 00`). The guest footer while g25 is just [1 BACK] (Mac 0x4312); the PS3 adds INVITE FRIENDS
  (frame). The joiner's bar: "?" portrait (r[6] = 0, art 70e16366), no badge (slot r[1] = -1), name alternating with
  WAITING IN LOBBY. The ready check follows 2205 (I).
- Racing players' bars keep badge, portrait and ready, with the name alternating with "<POS> (LAP n OF N)". No "IN GAME" state.
- A running race doesn't pull the joiner in: the guest race start (CALL 0x2be5, sends fe_game_bridge 19f7af08) runs only
  if `g25 == 0 && g22 == 0 && 2233()` (Mac 0x3280).
- Race end (2234() && 223a()): the bar is hidden, g25 and g22 cleared; the joiner without a racer gets the racer popup
  automatically, else the guest footer becomes [3 CHANGE RACER, 1 BACK] (Mac 0x4279); returning racers come in through
  45b79703 (g22 = 1) and never see the bar.
- Toasts exist only in the race HUD; the lobby scene has no toast node. Racers still racing get JOINED (kind 0), LEFT
  THE LOBBY (2), LEFT THE GAME (1) and RETURNED TO THE LOBBY (4). Trigger meanings inferred from the key text (I).

## Q4. Stock code that refuses or handles a join during a race (C)
Nothing on PC: no group-0x22 natives; 0x7ab1c9a7 is read only in front_main 0x052d (no script or exe dword sets it;
72d1c72b isn't an exe constant); the stock fe_network_game and fe_network_error_handler packages are the stub with dead
console code; the game-type-5 paths don't touch joins. The guards are ours to write: a joiner must not receive or apply
race messages, or be put into the armed contract; the guest's FRAME_START must be ignored while "race running && not in
the contract" (the console's g25 gate); the host's next Start should wait for the joiner's racer pick and "net" flag.

## Not verified
In-game rendering: root alpha, scroll speed, bar overlap with 8 bars. The H+0x348 writer. The exact meanings of 2234,
223a, 2249 and 2233 beyond their script context.
