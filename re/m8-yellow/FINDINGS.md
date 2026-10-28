# Yellow bars for muted players, and MUTE / UNMUTE (2026-10-23, reverse-engineer pass, session "Key Party")

The user's design: a muted player's lobby bar is YELLOW (base blue, cursor red), the popup's button reads MUTE or UNMUTE by that
player's state, and the "(muted)" word goes. C = confirmed, I = inferred.

## How a bar is coloured today (C)
- Two sprites stacked: the blue base (sprite 5da8c97e, always drawn) and a red overlay (sprite bcf09c8c, its own node; only its
  alpha is animated). The cursor is `020e(group, 0d09208a -> d279b4c1)` (group frames 10 -> 20, children follow: the red fades in).
  Nothing is tinted. Group keys: t0 fbc94a42, t1 74e7e7bc (reset), t10 0d09208a (plain), t20 d279b4c1 (highlighted).
- Overlay keys: rows 0-1: t0 hidden, t9 alpha 0 (e33aacae), t19 opaque (e2be7156); rows 2-7 the same at t10 / t20 (rows 0-1 keep a
  ~10% red veil at t10).
- Keyframe record 0x28 bytes (0x4897e0): +4 frame, +8..+0xb R G B A, +0xc/+0x10 position, +0x14/+0x18 scale, +0x1e visible.
  `020e` = 0x4a6b90 -> 0x47f8c0; the group's slot 4 (0x488050, GROP vtable 0x6a7858) passes frames to every child; sprite draw
  0x485e90 -> 0x47d530 (vertex colour = pose RGBA x the parent's).
- Rows (base / red overlay / name), same in all 7 lobby scenes, stock and Metal: 0 a25dd079 / f0425ec4 / 7d63b234; 1 6ec737ad /
  755c443f / c04132b7; 2 36cb75a9 / 332ad949 / 95a72e05; 3 fe74649d / ec90d403 / 1c46920f; 4 68e2774b / a87ef5ca / 9f8f28f1;
  5 1583ba8a / cb3e4b15 / 0b6a58ec; 6 6043e324 / b2a4694a / 07234dbf; 7 74d20750 / a98082f1 / 652ea4e4.

## No yellow exists (C)
- The lobby atlas (110 sprites) has bars only on page 3: a971909f blue / f94714d8 red (972x44, search rows), 5da8c97e blue
  (0,232)-(640,276) and bcf09c8c red (0,276)-(640,320) (lobby bars). Gold: only font sheets and 40490e9f (968x9 rule).
- Keyframe colours: only white opaque / white transparent. Art lookup is per scene (0x47f400 -> 0x483890 -> 0x485f20 ->
  0x47b630 / 0x480560, binary search of `[elem+0xe8]+0x2a4`): the results' gold can't be borrowed.
- The colour setting multiplies (I, strong): native 0224 (0x4a70d0) / C++ 0x47f460(elem, 0xf0) + 0x47f230(elem) bytes 8..0xb can
  make the white NAME text yellow, never a blue/red bar.

## Recommended build
1. New sprite dd4e2bb1 = `name_hash("STEAMMP_LOBBY_BAR_MUTED")` (free everywhere): a yellow copy of 5da8c97e (hue 48 deg,
   brightness x0.92, white border kept; preview `out\yellow_preview.png`) at page 3 (0,320)-(640,364) (rows 320-512 are fully
   transparent in all 7 scenes, stock and Metal), DXT3 block rows 80-90 x columns 0-159 (`steam_tiles.reencode`). UVs (0,0.625),
   (0.625,0.625), (0.625,0.7109375), (0,0.7109375), page 3. PTEX record inserted sorted at index 99 (between dceb13dc and
   de4817cf).
   - Stock layout (E01CF5DC): count +0x10 0x6e -> 0x6f; 40 bytes inserted at chunk +0xf94; +0x18 0x113c -> 0x1164; page offsets
     +0x28 (0x115c / 0x1011dc / 0x20125c / 0x3012dc); used 0x38135c -> 0x381384; size align64(0x10+used) 0x381380 -> 0x3813c0;
     later chunks (KFRM onward) move 0x40; section 3's decoded size +0x40. SECTION 4 (RELO) must change: the PTEX entries 0x113c /
     0x1144 / 0x114c / 0x1154 each +0x28 (entry 0x8 stays), then recompress section 4 (today's packers copy it unchanged).
   - Metal layout (page table at the chunk end): +0x18 0x3a54e4 -> 0x3a550c; page offsets +0x28; the 7 RELO PTEX page entries
     +0x28; used 0x3a551c -> 0x3a5544; size 0x3a5540 -> 0x3a5580. Precedent: Metal's own e76b1254 at index 100.
   - The 7 lobby scenes: 1A87C210 SP, 1D952462 IT, 841A31F1 FR, 9BDCF15B GE, E01CF5DC EN, 3943D953 / 7F94F89F (placeholders).
     Metal Sonic replaces all 7: each needs a stock version and a variant on Metal's (optional, like the key pictures).
2. C++ (kNatMemberText / the bar draw): per bar `ok = 0x47f400(base[k], muted ? 0xdd4e2bb1 : 0x5da8c97e)` (returns in AL: 1 set,
   0 not in this scene); muted and not ok -> blue + keep " (muted)" as the fallback; otherwise drop the word. The mute toggle
   already bumps the snapshot revision (redraw). The red overlay covers yellow when the cursor is on a muted bar.
3. UNMUTE: reuse the dialogs text key 72259212 "NOTHING" (unused on PC: no definition, no script in any of the 24 .sbn incl.
   SuMemcardScripts, not in the exe or other mods' DLLs). Texts (ASCII; tables are UTF-8, not cp1252 as text_tables.py says):
   EN 9421F31B "UNMUTE" (42 bytes left), FR 9AC5FB72 "ACTIVER LE SON" (fits exactly), GE F08557E0 "TON AN" (17), IT 91EF54FD
   "ATTIVA AUDIO" (32), SP 55CB9117 "ACTIVAR SONIDO" (15). JP 81912A8C / US D4B24208 are placeholder tables: leave them. Metal
   replaces all 7 dialogs tables (variants needed). Optional: German MUTE "EINSTELLEN" (a mistranslation) -> "TON AUS".
   Text lookup: binary search (0x486770, `[scene]+0x2c8`); a new key would also be possible but costlier.
4. Popup definitions (draft `scripts\defs_unmute.py` -> `out\dialogs-unmute-proto.smosbin`, d6d74b62..., 0x1d1c, validates):
   append host UNMUTE `{6,6,9,ffff}` [11d8a1b2, 571f6e9c, BFB811CD, 72259212, 476D4A25, 21A53CCF] at 0x1ca4 and guest `{5,5,9,ffff}`
   [1bffb5e2, f1dce0c6, BFB811CD, 72259212, 21A53CCF] at 0x1ce4; repoint +0xcac (940FE7B4) 0x8d8 -> 0xff8 and +0xa9c (18C51485)
   0x568 -> 0x1248; +8 = 0x1d1c. Script: a new frame result (8) for a muted target posts 940FE7B4 (host) / 18C51485 (guest) with the
   same popup states (1 / 40); answers unchanged. C++ check first: the target is muted and `g_sceneText(0xc282a8, 0, 0x424b961f,
   0x72259212)` is not a stock string (NOTHING / RIEN / NICHTS / NIENTE / NADA / Empty); else post the MUTE popup.

## Side finding: 77B81411 is not unused (C)
build_menus.py:260 calls it unused, but SuMemcardScripts `mc_autosave` posts it at 0x0bbf (SaveDataTimeError, when
`130c(940a8fdf)` is not SUCCESS) and `mc_autoload` lists it (0xed4): a save warning would show our lobby buttons if that state
happens on PC (probably not). Move the host popup to a definition with zero references (940FE7B4 and 18C51485 have none; check
D4879E10, the guest's, too) and put 77B81411's original buttons back.

Scripts: `scripts\` (pages, cols, textroom, defuse, strhash, bytescan, lobbyscenes, ptexorder, relo, unusedsprites, yellowbar,
defs_unmute, unmute_text, natgroup, vtab, immscan, Decomp.java, run.sh). Outputs `out\` and `ghidra\` (gitignored).
