# RANDOM slots: game research (reverse-engineer pass, 2026-11-08 night; saved by the main session)

Read-only. Scratch scripts in `re\random-slots\scripts\`, outputs (game data: never commit) in `re\random-slots\out\`. Nothing run in game.

## Key finding
The online lobby has NO racer grid and NO course grid: both are single slots stepped with left/right. "A tile at the end of each grid" = one more stop after the last item, before the wrap.

## 1. Racer select (CONFIRMED)
- CHANGE RACER popup `a653f569` in the `fe_net_lobby_popups_<LANG>` scene (EN 0F1A9E40, FR 7F54F327, GE 5F14ACB5, IT 7B071E02, SP 675C647C, US 1D52A45D, JP A743FCA1): one portrait `20a43a61` (248x236) and the name in two text layers (`6a53b173` / `fc4d712a`).
- Built by `tools\netmenu\racer.py` into all three lobby modules (stock, Roster, `_metal`); same structure, rows 21 / 25 / 22.
- Order `RACER_ORDER` = the offline grid order (stock `fe_charselscreen` g31 at 0x2532..0x25a2, 21 racers ending with Tails); `ROSTER_ORDER` / `METAL_ORDER` end with Metal. `cycle()` wraps both ends (racer.py:63); only racers marked in `AVAILABLE` (g38).
- Index: `CANDIDATE` (g37); Enter commits to `CHARACTER` (g2f); frame native 7a16 arg 0 (steam.py:799); Steam member data `"racer"` = row index, -1 none (steam_mp.cpp:954); the host's Start turns it into `contract.roster[i]` (race.cpp:4674-4683).
- Tile: pseudo-row R = len(ROWS), `AVAILABLE[R]=1`, a draw branch for our picture + text key; a sprite + text key in every popups scene in every variant base; Start accepts R and rolls it.

## 2. Course select (CONFIRMED)
- The host's SETTINGS popup `EB17FB50`, row 0, text only (`6634bccc` / `45326dd1`, two-line names). Cycles 25 courses (g12 order, Egg Hangar last), wraps both ends (`settings.change_setting`).
- `DRAFT[0]` while open; Enter commits to `COURSE` (g41). Host publishes lobby data `"rules"` = "course,laps,..." (steam_mp.cpp:1323); guests read via native 7a1a, which refuses values above 24 (natives.cpp:839); guests draw panel art `7ac9264c` (396x141) and the one-line name `9cfb0450`.
- Tile: a 26th entry like Egg's (`settings.egg_insertions`), made available by the 2003 hook (race.cpp:2424, already answers command 73 for Egg); art + name in lobby and popups scenes; raise the 7a1a limit; `CourseUsable(RANDOM)` false; host rolls in `RaceHostStart`.

## 3. Stock "?" picture (CONFIRMED)
- Sprite `70e16366`, 90x55 ring portrait in `fe_net_lobby_<LANG>`. `draw_bars` shows it when native 7a17 returns 0 (steam.py:651-659); 7a17 returns 0 for any racer value with no hash (natives.cpp:794). Publishing a RANDOM marker >= the racer count shows "?" on every screen (own bar too), no scene change.
- Don't use 0 (= Sonic; CODEX-FINDINGS' "Racer 0 = unselected" is Codex's retired helper record) or -1 (= not ready, natives.cpp:800).
- The popups scene has no "?" portrait (all 51 sprites checked).

## 4. Blue "???" picture (CONFIRMED)
- Sprite `48bbdf57`, 135x135, page 0 (512x512 DXT3, no mips) at (208,172)-(343,307) in `fe_character_select_sub_<LANG>` (EN AFB7E20B). Another racer mod's picker image node `3c5dd47c` default (0229/022b at `fe_charselscreen` 0x34ac/0x34c1). Same pixels in `fe_license_portrait_*`. Crop: `re\random-slots\out\sprite_48bbdf57.png` (matches the user's screenshot crop).

## 5. Text (CONFIRMED)
- No "RANDOM" text in any language (1421 archive members); closest is the item roulette's "LUCKY DIP!". Lobby / popup text tables are UTF-8; fonts `38afd459`, `67bc5010`/`a36c432a`, `1262d7ba` have É/Ä/Ü. The game's COURSE: PARCOURS / STRECKE / PERCORSO / TRAZADO. Add a key per scene with `codexui/build_ui.merge_text` (as `mod_icons`, `egg_lobby`).

## 6. "What the normal list offers" (CONFIRMED)
- Racers: in g14 (`fe_charselscreen` export DC8A3C6D (0x2838) = g31 filtered by 2003(72) = the exe's unlock check 0x547a00) AND `RacerOn(i)` (Metal only when on; the stock list's hidden last row excluded); Start also refuses the two racers that can't race online (race.cpp:4678).
- Courses: 2003(73) via 0x547aa0 (false for an unknown hash) AND `CourseUsable`. Our 2003 hook sees every answer: C++ can record them and roll from the same lists.

## 7. Risks
- Script size (CONFIRMED): main loop's 16-bit jumps have 1647 bytes left in the 25-racer build (3167 stock, 2787 Metal); a RANDOM row costs ~150-250 bytes: put logic in `call_chunk` subroutines or C++.
- Metal Sonic and the racer-pack variants replace all 14 lobby/popup scenes: add sprites + text to every variant base in `reference\navigator-bases`. Egg Hangar replaces none (CONFIRMED from its payload list); its module swap list doesn't touch ours (INFERRED from a project copy of its source, not the installed 1.2.0).
- Offline untouched (CONFIRMED): these scenes load only from `fe_network_game`.
- `build_menus.py` hardcodes 25 courses (lines 286-288, 417); `build.configure` asserts the racer order covers exactly the rows.
- Duplicates: players may share a racer today (the user's 2026-10-23 choice); RANDOM needs its own exclusion + an empty-pool fallback.
- Who rolls: the host builds the lineup at Start; a literal "each game rolls its own" needs an extra round trip. Simpler: each RANDOM player publishes its offered list, the host rolls in seat order (one authority, fixed tie rule). Design decision (user answer Q8 said each player's game rolls its racer: check what matters to them).
- Picture: source 135 px; popup slot needs ~1.8x upscale, the 396x141 course card a crop + ~2.9x upscale. "Racer-portrait shape": popup portrait is a free-form 248x236 render, bar portrait a 90x55 ring.
- Minor: `lobby_bars.SCENES` and `egg_lobby.POPUP_SCENES` have JP/US keys swapped vs the file-name hash (harmless: both get English).

## Smallest safe way
- Racer: pseudo-row R in racer.py + a popups sprite + `STEAMMP_RANDOM` key; publish R as is (the "?" follows); `RaceHostStart` and the ready check learn R; roll in C++ from the recorded 2003(72) answers, `RacerOn`, and the online-allowed racers.
- Course: an Egg-style 26th entry, Hook2003 answers 73 = 1 for it; lobby + popups art and text; raise the 7a1a limit; host rolls at Start.
