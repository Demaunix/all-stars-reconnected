# The console's "downloadable content" question (for 1.1.0 ENABLE MODIFICATIONS)

Researcher pass, 2026-10-24 (session "Egg Hangar Gets a Grid Slot"); saved from the main session. Labels: CONFIRMED (read in the
data), INFERRED, UNKNOWN. Scratch scripts (not in the repo): the session scratchpad `dlcpop\` (`bedis.py` big-endian SMOS
disassembler, `bemap.py` definition maps, `tu2txt.py` TU2 text reader, `fng_be.txt` the full TU2 fe_network_game listing).

The user's request (2026-10-24): when the host creates a lobby, the console's "enable downloadable content" question, with
"modifications" in place of "downloadable content"; YES opens a styled popup listing the installed mods the multiplayer supports
(Metal Sonic, Egg Hangar) to turn on or off for that lobby; the game list shows "(modded)" by the host's name; a joiner sees a
styled popup of the mods the lobby needs and joins only with those mods at the same versions.

## Source
Xbox 360 title update 2 (the Metal Sonic & Death Egg Zone pack's update), `FrontEndScripts` member 4E5EF10B of the TU2 xpac
(read with a big-endian package reader). Not in the other console builds' data or the PC's (CONFIRMED by scans).

## 1. The question (CONFIRMED)
- TU2 `dialogs` (7928 bytes at package +0x3d34, 88 definitions; PC 76): **EB933573** = [title 1BF17EB0 INFORMATION, body
  **6E2FEAAB**, buttons 97E89505 NO, 1F733628 YES] (NO = button 0, YES = button 1; map key at module +0xf28, array +0x1d48).
- **AB172EE2** = [INFORMATION, body F62AF9AD, OK 0A9F2DCC]: the join refusal "You cannot join this game because the host has
  enabled the Metal Sonic & Death Egg Zone downloadable content. To play with this player you must purchase the content from Xbox
  LIVE Marketplace." (map key +0xd58, array +0x1860).
- PC: neither definition, nor the callback F3FF3C79, nor the body strings. 97E89505 / 1F733628 / 1BF17EB0 exist on PC
  (definition DD492AF5 uses them). The PC reads only the body and the buttons, never the title.

## 2. Who posts it (CONFIRMED offsets; meanings INFERRED)
- TU2 `fe_network_game`: `160c(dialogs, dfea0f93, [EB933573, fe_network_game 0x8398d0bc, export F3FF3C79])` at 0x22a6 and 0x27c6
  (the search screen's CREATE GAME key, `041d`) and 0x4692 (CREATE in the "No matches were found" callback 2FB94C9A).
- Only when native `2001(1)` is true (INFERRED: "the DLC pack is installed"); otherwise straight to the lobby.
- Callback F3FF3C79 at 0x473e: -1 returns with no change; NO sets global 0xa = 0; YES sets it to 1; then into the lobby as without
  the question. Global 0xa = "DLC enabled" for the session: at 0x11f8 a pack bitmask goes into lobby data slot 7; the racer picker
  skips DLC racers when it is 0 (0x3db6, 0x3edb, 0x435b, 0x43a4).
- Join (INFERRED, 0x26e6): a game whose DLC mask needs a pack you lack posts AB172EE2 (OK) and refuses the join.

## 3. Strings (CONFIRMED in TU2, absent on PC)
TU2 dialogs text tables: EN 8E69DF5D, FR A5295CE4, GE 0901CFB2, IT CB7683DF, SP 2B320419. Key 6E2FEAAB:
- EN "Would you like to enable your downloadable content?"
- FR "Voulez-vous activer votre contenu téléchargé ?"
- GE "Möchtest du den heruntergeladenen Inhalt aktivieren?"
- IT "Vuoi abilitare il contenuto scaricato?"
- SP "¿Quieres activar el contenido descargable?"
Key F62AF9AD (the join refusal) in all 5 languages too. The PC dialogs tables have little spare room (text_tables.py: dialogs_EN
~60 bytes) and `patch_text` only edits existing keys: add keys by rebuilding the TEXT chunk (codexui `merge_text` does that) or
reuse a free key.

## 4. The game list's DLC column (CONFIRMED header only)
Header key E588EDC2, a one-string table per language in TU2: EN B2A086F8 "DLC", FR 22DADBDF "CT", GE 029A956D "IZH", IT 1E8D06BA
"CS", SP 0AE24D34 "DDC" (language order INFERRED from the archive order). TU2 fe_network_game 0x2036..0x2075 applies
`0220(node, E588EDC2, 8687AF3F)` to three nodes; the PC and retail 360 show SIGNAL (D976B726) there. How a row's DLC cell is
drawn: UNKNOWN (TU2 fe_network_game also uses sprite E76B1254, which the DLC's lobby scenes carry: a likely candidate).

## What an installed mod still changes when "off" (researcher pass 2, 2026-10-24; scratchpad `modsinv\`)
- Purely additive for racers and courses (CONFIRMED by decoded diffs): the Roster Pack's RacerParams.dat only adds MechaSonic
  rows and flips AIUnlocked (and the platform field) for the console-only racers; no stock racer's physics row
  changes; the Xbox TU2 RyoBike camera tweak is not applied. SetupObjects / Behaviours: additions only. Egg Hangar's TrackParams
  only adds a DLC table; its course hooks act only on `doomeggzone_dlc` (its profile/save hooks run for everyone, local only).
- UNKNOWN: Metal Sonic's 30 code/data patches (`generated_plan.hpp kPatches`, VAs 0x4ffea0, 0x54c577.., 0x585fe2, 0x5863ea,
  0x4f4f20, 0x59e6b0.., 0x535710, 0x4e7831, 0x624338, 0x61026c, data 0x6e9070..0x6e9084, 0x5080b0.., 0x5a8616) and the Pack's
  4 (0x5fe7be, 0x547661, 0x4d4c35, data 0x6f661c): whether any changes stock racers / items / All-Stars when Metal isn't raced.
  A reverse-engineer pass settles it.
- Racers between players: lobby member data `racer` = an index into the local list (stock 21 = the Roster list's first 21);
  the race contract carries character hashes; the host's AI pool comes from the host's list; a guest aborts on an unknown hash
  (`RosterKnown`). So an "off" Pack host must keep its picks and AI pool to the shared racers.
- **Public Metal Sonic 1.1.0 Beta 3 (standalone) gets NO online menus today (CONFIRMED by hash):** its fe_network_game is
  04ad1a0c… (stock + one Metal row, 8725 bytes), which `src\script_subst.cpp` doesn't accept (only stock 929d14fb… and the
  Pack's dda10019…). Beta 3 changes 7 FrontEndScripts modules: fe_charselscreen, fe_game_bridge, fe_leaderboards,
  fe_license_records, fe_network_error_handler, fe_network_game, fe_trackselect; its fe_game_bridge differs from both the stock
  and the Pack's, so the race-start ScriptSites (`kBridge*PcRoster/Stock`, Hook0905's) need its offsets too. Supporting Beta 3 =
  a third lobby variant (21 + Metal), its racer table, and its bridge sites.

## Open for the design
- What a turned-off mod still changes in a race when it stays installed (Metal Sonic's Roster Pack layer: racer lists 25 vs 21,
  racer stats, the save); the mod check must loosen only for the supported mods.
- The "modifications" wording in 5 languages (the user's words; EN "Would you like to enable your modifications?").
