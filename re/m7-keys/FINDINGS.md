# Footer key pictures: how the game draws them and what adding new ones takes (2026-10-23; researcher pass, saved by the main session)

C = seen in bytes/images; I = inferred; N = notes' claim, not re-verified. Outputs in `out\` (unpacked EN scene `deep\31_0720c59e_48.bin`,
Metal Sonic's `metal_navinfo_inflated.bin`, `page0-2.png`, `keys_sheet.png`, `keys_zoom.png`, `key_f00N_*.png`).

## Pipeline (C)
- fe_navigator (5429 bytes in FrontEndScripts.sbn) maps icon index -> a 4-byte UTF-8 private-use character: 0 F000 Enter, 1 F001
  Esc, 2 F002 F1, 3 F003 F2, 6 F005 up/down, 7 F004 left/right, 8 F006 four arrows, 9 F007 (unused anywhere), 10-17 repeats of
  F000-F002 (used by fe_charselscreen). 16 dict entries at file 0x1450..0x148f; keys 4 and 5 don't exist. Growing the dict means
  substituting the fe_navigator module (byte-identical in stock and Roster Pack, N); or our native writes the footer string with the
  character itself (I).
- The font ff7f8ec5 lives in the navigator_info scene copies in SumoToolResources.xpac (7 languages: 0720c59e = EN, 08538f72,
  0b11e779, 11c1615b, 5706e9cd, a4877a54, dfb2da15). Entry container: 0x48-byte header of 6 sections, the PTEX zlib stream at 0x48
  (`tools\steam_tiles.py sections`/`pack`). PTEX (EN): 52 sprites (40-byte records {hash, 8 floats, page}), 3 DDS pages (1024x128
  DXT5, 1024x1024 DXT3, 1024x512 DXT3), then chunks KFRM, OBJS, SCNE, FONT (0x1a5f00, size 0x10480), TEXT. FONT: font table at
  +0x1c, 0x20 per font, 10 fonts; ff7f8ec5: 199 glyphs (0x18 each: {sprite hash, u16 char, u16 u, v, w, h, s16 xoff, adv, yoff}),
  304 kerns, line height 14. The glyph arrays are packed back to back and sorted by char (0xffff last): adding one shifts every later
  font's offsets and the FONT size, and the TEXT chunk after it.
- The key glyphs all use sprite e484cf68 = page 1 rect (0,464)-(512,719): 73x73 cells in row v = 0: F2 u0, Esc u73, Enter u146,
  F1 u219, four arrows u292, left/right u365, up/down u438; the visible cap is ~35x33 px centred in the cell (dark bezel ~(74,69,74),
  rounded face gradient ~(140,146,148) -> (90,91,96), white bold label top-left). No other key caps (A-Z, Space, Ctrl...) exist in
  any font or scene (scan of every PTEX in SumoToolResources, Base, Select, MissionItems, Racers), and no key-name strings in the exe.

## Adding a key (e.g. "V" as icon 9 = F007, no script change)
1. Paint the cap into page 1 (repack the key row: the sprite has no free 73x73 cell; ~19 px margins per cell can be reclaimed, or a
   new sprite on page 2's free area, which adds a record and shifts offsets). 2. Re-encode the changed DXT3 blocks
   (`steam_tiles.reencode`). 3. Insert the glyph record before 0xffff; glyph count +1; shift the later fonts' offsets; FONT size +0x18.
4. Repack (`steam_tiles.pack`) and ship as a plan "replace" of key 0720C59E (and the 6 other languages, each with its own layout:
   EN 199 glyphs, 11c1615b 200). 5. Footer call `[9, label]`. More keys: grow the fe_navigator dict or write the characters ourselves.
- **Blocker (C/N):** the Metal Sonic mod replaces all 7 navigator scenes (its scene differs from stock only by +1 sprite and +3 pages;
  its FONT/TEXT/page 0-1 are byte-identical), and the loader allows one mod per archive member (N: `notes\MODS-OVERVIEW.md` 217,
  `CODEX-FINDINGS.md` 69). So our own navigator payload can't ship beside Metal Sonic: either the keys go into Metal Sonic's payload
  too (a build per mod set), or the loaded font/texture is patched in memory (complex), or key art is drawn as our own sprites.
- Art spec for a generated cap: a 73x73 RGBA cell, transparent, the cap ~35x33 centred (or the cap alone at 36x36), the style above,
  DXT3-friendly (no fine alpha gradients); wide keys (Space, Shift, Backspace) need wider cells.
- Risks: glyph xoff/yoff semantics unverified (re-derive from a screenshot); whether a glyph rect may leave its sprite; whether the
  glyph array must stay sorted; each language's scene differs; nothing run in game.

## Decision (main session, 2026-10-23): parked, then built the same night (the user's "go ahead" and "update Metal Sonic")
With the console-true lobby flow (Enter = SELECT / START, F2 INVITE, F1 SETTINGS, Esc BACK, arrows) every footer key already has a
picture; the only console key without a PC picture is REFRESH on the search screen (the console's START button).

**Built (0.7.0-m7zz, `tools\footer_keys.py`):** one new glyph, F007 (icon 9) = an F3 cap, in all 7 navigator scenes. Facts found
on the way (C): the key sprite's hash differs per language (EN e484cf68, SP 42ad6a17, ...): read it from the F000 glyph record and
take the row's page/rect from the sprite record (8 floats u0 v0 u1 v0 u0 v1 u1 v1, then the page index). Glyph records are 0x18
bytes: {sprite, u16 char, u, v, w, h, s16 xoff, adv, yoff, u16 1, u16 0}; the 0xffff record is the missing-glyph box (u 0x77 v 0x97
w 0x19 h 0x16). The FONT chunk: 16-byte header {tag, size, payload, 0x44332211}, 8 zero bytes, u32 font count, the table at
+0x1c (0x20 per font: hash, kerns<<16 | glyphs, glyph offset, kern offset, 0, line height, 2 floats); offsets count from the
chunk start + 0x10; the chunk size is align64(0x10 + payload) and the padding differs per language (EN 8 bytes, JP 0x30).
Inserting the record before 0xffff and adding 0x18 to every later glyph/kern offset, the count and the sizes (with the padding made
up again) leaves every other font's first record and kern table byte-identical (checked by the tool). The key row was re-laid into
8 cells (each cap keeps its position; cells 60/60/60/60/72/68/56 + F3 73, 4-px aligned; 956 DXT3 blocks re-encoded, mean error
0.03). The F3 cap = the F1 cap with the "1" erased by the face 7 px to its right and a "3" from Arial Bold (11 px, scaled 8x down)
where F2's "2" sits (x 33, y 26, 4x8 px). Metal Sonic's scene has the same FONT/TEXT bytes and page 1; its container puts the
data at 0x60 with 32-byte alignment (the tool detects the layout rule by reproducing the entry). Delivery: the loader allows one
mod per archive member, so the pictures ship (a) inside Metal Sonic's update 1.1.0 Beta 4 (`tools\metal_update.py`: the published
zip with the 7 payloads swapped, plan/mod.ini/MANIFEST/version/changelog updated) and the Roster Pack's Metal layer likewise, and
(b) as the separate "Steam Multiplayer Keys" mod folder (stock scenes + keys, an inert runtime) for players without Metal Sonic. A
loader-side solution (a replace operation naming variant payloads for other mods' versions of the member) was designed and
dropped: it needs a ChaoGarage change the user has not asked for.
