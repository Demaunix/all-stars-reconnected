# The auto-update's in-game notices: dialog definitions, texts, trigger (reverse-engineer pass, 2026-10-28; read-only)

Saved by the main session from the agent's report. Labels: CONFIRMED / INFERRED / UNKNOWN. Scripts and outputs: `scripts\`
(scan.py, orphans.py, namehits.py, texts.py, rawbytes.py, fit.py) and `out\` (scan.txt, orphans.txt, namehits.txt, texts.txt,
dump\fe_mainmenu.txt, dump\dialogs.txt).

## A. Two definitions nothing posts (CONFIRMED)
| Definition | Array (shipped module; stock) | Items | Use |
|---|---|---|---|
| 9DC5CA23 | 0x1624 (0x1608) | title 1BFFB5E2, body EB89A815 (console "DLC packages ... corrupt ... {0}"), OK 0A9F2DCC | "a new version is ready" |
| 75CF8F27 | 0x13C4 (0x13A8) | title 1BFFB5E2, body C47A8040 (no text in any table), OK 0A9F2DCC | "was updated" |

- Our shipped dialogs module rebuilt in memory (`dialogs_with_mute(dialogs_with_session_messages(preview5))`) = `menus\out\dialogs.smosbin`
  byte for byte; the installed DLL embeds it.
- Zero hits for both ids and both body keys in: every module of every game `Scripts\*.sbn` (SuMemcardScripts included), Metal Sonic's
  three .sbn, our built modules, the decrypted exe and the game-folder DLLs, every file of every installed and parked mod (raw and
  inside zlib streams), our `src`/`tools` text, our DLL (its embedded dialogs module blanked first). No id is the name hash of a string
  found there.
- `dialogs` (ddb2e0e7…) and `fe_mainmenu` (885d11da…) are the same in the stock, Metal and Roster Pack packages.
- Both arrays: 3 items, one OK, not shared: an in-place body edit is enough (no repointing).
- Rejected: the console's online error dialogs (61764521, 949D796E, 937CBD41, 93DE36EE, 71C55ED8, 61D13C08: the 1:1 port may need
  them), three Wii-only character dialogs (75042CC8, AA586AE1, D49F76E7), SuMemcardScripts' 19 ids, the storage dialogs.

## B. Texts (CONFIRMED unless marked)
- The PC dialogs module never reads a definition's first item (title).
- Bodies: point each definition at a Wii-only key nothing uses and rewrite it: 484F1E46 ("Nintendo Wi-Fi Connection service ... has
  been discontinued", EN 172 / FR 196 / GE 185 / IT 200 / SP 213 bytes) and 74DF3842 ("Communication error ... disconnected from
  Nintendo Wi-Fi Connection", EN 135 / FR 167 / GE 184 / IT 161 / SP 174). Each appears once per table; tables stay sorted.
- `DIALOG_EDITS` additions: `(0x163c, 0xeb89a815, 0x484f1e46)` (9DC5CA23 body), `(0x13dc, 0xc47a8040, 0x74df3842)` (75CF8F27 body).
- TEXT spare bytes (stock / ours now / after both edits): EN 44/42/230, FR 10/0/211, GE 17/20/236, IT 38/32/255, SP 25/15/264.
- The tables are UTF-8 (all 641 strings x 5 languages decode); `tools\text_tables.py` encoded cp1252: fix before accented texts.
- JP and US tables: "Empty" placeholders, never loaded on PC (the language field is only set to EN/FR/GE/IT/SP at 0x49792B-0x4979A8).
- Proposed texts (the agent's translations; INFERRED, not checked by a native speaker): see tools\text_tables.py.

## C. The trigger on the main menu (CONFIRMED in bytes unless marked)
- Our native 7A1C is called in our fe_mainmenu at 0x1C12 (PC after the call 0x1C1A), reached only when: the menu is idle (g3),
  `!1c03(g7)`, no dialog is up (`!1607(dialogs, 53335ccb)` = the dialogs busy flag, +0x116), `!0009()`, a license picked
  (`g16 != 0x6CB83A85`).
- Hook: natives.cpp `Dispatch`, `id == 0x7a1c` at `ScriptSite(vm, 0x1c1a, -19, 57 16 02 85 3a b8 6c 4b 14 3b 00 32 00 1c 7a 01 00 fe 7f)`
  (7A1C is also called by the search screen: fe_network_game 0x7390, the stock-roster lobby 0x6D18, the Metal lobby 0x6EB6).
- Rules: a waiting Steam join first (7A1C returning 1 leaves the main menu that frame); the notices keep their own state (never
  `g_dialog`); retry a failed post (128 queued messages or dialogs not running); never post an id not in the map (dialogs busy
  forever); space the two notices (>= ~1 s or until the close at dialogs PC 0x751; INFERRED); input handled by the game (OK hides the
  box, a frame, busy cleared, `043f(0,0)`; INFERRED, strong). Another mod's FrontEndScripts without our fe_mainmenu: no notice (log it).
- Residual risks: a one-frame input window (INFERRED, small); the achievement popup or a memory-card error replacing the notice;
  re-scan the ids when any mod updates; drawing and input in game UNKNOWN until tested.
