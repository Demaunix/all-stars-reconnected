# M7 session messages: dialogs, in-race display, C++ post, return paths (reverse-engineer report, 2026-10-21)

Saved by the main session. Static analysis only; the game was never run. A read-only `-process` of a copy of the loader
project (`re\m7-messages\ghidra`, not committed), capstone and script dumps. **C** = confirmed in bytes or decompile;
**I** = inferred. Module offsets include the module header. "Shipped" = our substituted `dialogs` module
(`menus\out\dialogs.smosbin` = `menus\base\preview5-dialogs.smosbin`, SHA d0aacdab...), 0x1c bytes longer than stock
`dialogs` (ddb2e0e7...) because of Codex's code insert, so its typed-constant offsets are stock + 0x1c. `dialogs`, `game`,
`pause_menu`, `results` and `ingame_unlocks` are byte-identical in the stock and Roster Pack FrontEndScripts (C).
Helper scripts: `re\m7-messages\scripts\` (stz.py, defs.py / deftable.py, dumpmod.py, idcount.py, langscan.py,
posters.py, macnames.py); raw output in `re\m7-messages\out\` (game data; not committed).

## Bottom line
- **The texts are there; the dialogs that use them are not.** No dialog definition, script or exe code ever uses CDA264D3,
  07B1DE19, 5F50399B or 21A53CCF (RETURN TO LOBBY) (C). Existing definitions: 168056C7 = 65A73F51 "The host has ended the
  match..." (OK); CB5D217E = 08D1C242 "There are no other players..." (OK); 8374A07B = C4C2F0A1 "There was an error and the
  connection to the host was lost." (OK).
- **The definition decides the body text and the button labels** (C). Editing 3 dwords in place in our shipped `dialogs`
  module gives exactly the user's three dialogs, localized in EN/FR/GE/IT/SP. No code changes or relocation needed.
- **Our DLL can post the dialog itself from C++** (C): the exe does this for its achievement popup (0x40e410). Payload
  `[definition, NONE, NONE]`, the shape the stock `ingame_unlocks` uses. No callback needed.
- **The `dialogs` module keeps running during a race**: never stopped; the scheduler runs every live instance each frame
  (C). The stock game shows dialogs during the race session (GP and unlock popups at the results screen, which waits on
  them) (C). Not checkable statically: whether it draws over the in-race HUD and whether its input works mid-race (I).
- **The return path is already built** (C, race.cpp): QUIT_GAME plus the existing front_main 1b01 hook picks the lobby
  when the player is still in the Steam lobby (0xb0a0d459), otherwise the search screen (0x7ab1c9a7).
- **The console's own flow survives in the Mac scripts** (`fe_network_error_handler`), section 4.

## Q1. The `dialogs` definitions
**Layout (C).** Header +0x18 points to the typed constants at stock +0x9ec (shipped +0xa08): `count=1, {value 8, type
0x89}` = a map at stock +0x9f8 with header `{76, 0, 9, 0xffff}`, then 76 x `{key, 0x83}{offset, 0x88}`, keys sorted
ascending. Each value offset is self-relative to its own value field (28BC494F: value 0x5b8 at +0xab0 -> +0x1068). Each
definition is an array `{count, cap, 9, 0xffff}` followed by count x `{hash, 0x83}`. ONLINE-STRINGS.md's
"dialogs+0xfb0..0x1b38 references" are items inside these arrays (data), not code.

**The show handler** (stock code +0x1cb..0x4d6; C): checks message id 0xdfea0f93; payload P = message element [i*5+3]
(inbox layout `[p5, target, msg, payload, sender]`, 0x4bb2a0). If a dialog is open (g2 busy) it first calls the old
callback with `[-1]` and destroys scene 424b961f. Then creates scene 424b961f (0205), g2 = 1. DEF = MAP[P[0]]; callback
module g4 = P[1], export g5 = P[2]. Buttons g3 = len(DEF) - 2, labels `021f("POPUP\STRING_BTN0n" / "POPUP\STRING_BORDER_BTN0n",
NODE(DEF[2+n]))`. Body `0222(424b961f, NODE(DEF[1]))` = the localized text; 1614 (0x5fd280) formats `{0}` with P[3] into a
0x400 buffer; the first size SMALL/MEDIUM/BIG whose fit test `1636(..., e2be7156)` passes is used, written with 0248.
**DEF[0] (the title) is never read on PC.** Shipped-only branch (p5 +0x364..0x37d): the body format becomes `"{0}"` (free
text) only when `g4 == fe_network_game 0x8398d0bc && g5 == 0x9fc14701` (our CHOICE). Any other callback, NONE included,
shows the definition's own text. **Hazard:** an unknown P[0] gives DEF = NONE, skips all display, but leaves g2 = 1: busy
forever. Never post an id that is not in the map.

| Definition | Body key, English text | Buttons |
|---|---|---|
| 168056C7 | 65A73F51 "The host has ended the match. Returning to the lobby." | OK |
| CB5D217E | 08D1C242 "There are no other players in the match. Returning to the lobby." | OK |
| 8374A07B | C4C2F0A1 "There was an error and the connection to the host was lost." | OK |
| 93DE36EE | 71B23DB1 "There was an error and the match was closed." | OK |
| 71C55ED8 | B665A8E1 "An error has occurred and the match was closed. Exiting." | OK |
| 14A011B9 | 53D4C286 kicked | OK |
| DD492AF5 | 5040F97A quit warning | NO / YES |
| 61D13C08 | 0A29C876 "You are the game host." | OK |
| 2E61EFAA | C3942EC5 "The game session is no longer available." | OK |

- CDA264D3, 07B1DE19, 5F50399B and 21A53CCF appear as raw dwords in none of: the PC exe, stock and Roster Pack
  FrontEndScripts, Behaviours, SetupObjects, race.sbn, our built modules, the Mac FrontEndScripts and Mac binary. They exist
  only in the text tables, translated in EN/FR/GE/IT/SP (9421f31b EN, 9ac5fb72 FR, f08557e0 GE, 91ef54fd IT, 55cb9117 SP)
  and "Empty" in dialogs_US (d4b24208). On Mac, 21A53CCF was used only in a Mac-only definition 39A06F99
  (["Select which action...", RETURN TO LOBBY, KICK]).
- PC loads `<name>_<LANG>.stz` (0x4a7f60, `"%s_%s.stz"`) with LANG = table 0x6eca3c[[0xc28fb8]+0x6380] (EN, DA, DU, FI,
  FR, GE, IT, NO, PO, RU, SP, SW, US, JP); default 0 = EN (`mov [esi+0x6380],0` at 0x49792b); only FR, GE, IT, SP are set
  from the system language (0x497953..0x4979a8). **English loads _EN** (C).
- The definition chooses the button label (DEF[2..]) (C). Every 3-element definition is a one-button dialog.
- Free text with the console's exact wording works only with the CHOICE callback (C), which routes the answer into the
  lobby's ACTION logic: wrong mid-race. The definition route is better: localized; length no issue (the 80-83 character
  texts fit; stock bodies of ~185 characters exist) (I).

**Recommended data edit** (shipped module, same size, no relocation; bytes verified with xxd):
- CB5D217E body at **+0x1844**: `42 c2 d1 08` (08D1C242) -> `d3 64 a2 cd` (CDA264D3).
- CB5D217E button at **+0x184c**: `cc 2d 9f 0a` (OK) -> `cf 3c a5 21` (21A53CCF, RETURN TO LOBBY).
- 8374A07B body at **+0x143c**: `a1 f0 c2 c4` (C4C2F0A1) -> `19 de b1 07` (07B1DE19).
- 168056C7 stays (body at +0xfcc = `51 3f a7 65`, 65A73F51, OK). Stock offsets: -0x1c (+0x1828, +0x1830, +0x1420, +0xfb0).
- Keep the map keys unchanged (sorted). Assert the old bytes in build_menus.py and re-pin the output hash.
- Fallback without editing the module: swap the 0222 and 021f key arguments in RaceNativeBefore at the shipped dialogs
  sites (the body call is p5 +0x38b, PC 0x393, bytes from PC-19 `02 1f 96 4b 42 07 05 51 01 23 3c 32 02 22 02 01 00 e9 00`).

All 76 definitions (stock offsets, title / body / buttons) are in `re\m7-messages\out\deftable.md` (from
`scripts\deftable.py`). Network-relevant ones besides the table above: 0133bf1a 7357c4b2 "Not all players are ready." OK;
204e0232 12680470 "Unable to create match" OK; 28bc494f da3e45c7 "No matches were found!..." REFRESH/CREATE/CANCEL;
28fdd773 5ce65322 "Unable to join the match because it is full..." OK; 61764521 5fb221bb "The selected match is no
longer available" OK; 937cbd41 616e0c36 "...already in it." OK; 949d796e c68145b6 "This match is no longer available." OK;
f1898138 49209e84 "Are you sure you want to leave the lobby?" NO/YES; f2bee196 3756af52 "There must be at least two
players..." OK; 853ab2e9 "ARE YOU SURE YOU WANT TO EXIT?..." / RESULTS.

## Q2. Can the dialog show during a race?
**The module keeps running (C).** front_main starts `dialogs` if not running (front_main +0x8b..0xb9: `1603(dialogs)`,
`1601(dialogs)`). No script stops it (1602 targets: front_main by fe_game_bridge; hud, loading, results, pause_menu,
gp_podium by game; car_viewer modules). Instances die only through 1602 (0x5fd150 -> 0x4bafe0) or a package unload
(0x4bb6e0). The scheduler 0x4bb0a0 (from 0x4967c0) moves queued posts into the inbox (0x4badb0), then runs every live
instance in the 48-slot table 0xce5498. An inbox entry stays until its target reads it; broadcasts are cleared each frame
(0x4bae60(0)). `dialogs` has its own message loop (stock +0x17e..0x811, `160e()` plus a one-frame yield).
`fe_network_game` is likewise never stopped (C); 1607 (0x5fd850) finds the live instance (0x4bb070) and export (0x48b4b0)
and runs it synchronously.

**The stock game uses dialogs within the race session (C).** `ingame_unlocks` posts `160c(dialogs, dfea0f93, [49c532d2,
NONE, NONE])` (+0x246..0x266) and `[6bd6cae1, NONE, NONE, text]`. gp_podium (+0x0b76) and results (+0x2007, +0x25cb) loop
on `1607(dialogs, 53335ccb)` until the dialog closes (I, strong: closable in the race session). The exe's achievement
update posts CHALLENGE_COMPLETE to `dialogs` from the main loop (0x40292d -> 0x4a2980 -> 0x40e580 -> 0x40e410), same
scene 424b961f.

**Drawing over the HUD (I).** 0205 = SceneManager::CreateScene (0x47fdc0); the dialog creates scene 424b961f at show
time, no priority argument. pause_menu creates its scenes the same way at pause time (`0205/1` at pause_menu +0x422,
+0x4f9...) and those draw over the HUD. By analogy the dialog should too. Not verifiable statically.

**Input (C for the reads; I mid-race).** `dialogs` reads the primary pad `[ctrl+0xc+[ctrl+0x960]*4]` (ctrl =
[0xc2a1b4]+0x14): 043c(0) (0x600c90) select = byte +0x125 bit 4; 043d (0x600f20) bytes +0x119 / +0x10f; 043e
(0x600fe0) bytes +0x11a / +0x110. 043f(1, 0) (0x6015c0) sets ctrl+0x959 = -1 and input-mode bit 1 (ctrl+0x971,
0x4a8970), recomputing the mode (0x4a87b0): modes 0 and 1 use the same mapping; the switch runs 0x4ae2e0(1) on the primary
pad (held buttons marked "release first"). Opening the dialog does not lock the race's controls; a held accelerator may
read as released until pressed again (I).

**Fallbacks** if the in-game test shows no dialog or no input: at the results screen (stock popups do this); in our lobby or
search screen after QUIT (needs a definition-based notice with a non-CHOICE callback; the lobby frame gates on busy now).

## Q3. Posting from C++
The exe's own pattern at 0x40e410..0x40e4f5 (C):
```
ScriptValue arr = {0, 0};                                   // {u32 value, u32 type}
0x48ed30 thiscall(ecx=0xc28b20, &arr, count) ret 8          -> al=1 (creates the array; type = [0xc28b20+4], runtime-registered)
0x48cb00 thiscall(ecx=0xc28b20, &arr, index, &elem) ret 0xc (copies with addref; int3 if arr's type != [ecx+4])
0x4bb220 cdecl(sender, target, msg, &arr, 0) -> al          (copies with addref into queue slot 0xce5568+i*0x18)
0x48c8b0 thiscall(ecx=&arr) ret                             (releases our reference)
```
- Array object (C): value = pointer to `{+0 count, +4 ?, +8 elements*}`, 8-byte ScriptValue elements.
- Ownership (C): refcounting through the per-type handler 0xc28920[type] (vtbl +0xc addref, +0x10 release; strings type 5
  with a short refcount at ptr-2). 160c's handler 0x5fd9f0 wraps `{values[2], types[2]}` with addref (0x48c110), posts,
  releases (0x48c8b0). The queue slot is released when 0x4badb0 delivers it; the inbox slot when 160e reads it (0x4bb2a0).
- 386 call sites use 0xc28b20 as the array helper's `this`; 0xc28b20 and 0xc28b34 are .bss, filled at runtime: never
  hardcode the array type id.
- Element types (C): definition `{def, 3}` (hash); NONE = the game's canonical none `*(ScriptValue*)0xc28b34` ({0, type
  0}); payload 3 elements `[def, NONE, NONE]` (the `ingame_unlocks` shape). A non-array payload breaks the INDEX ops.
- The post (C): 0x4bb220 returns 0 when 128 posts are queued ([0xce5488] >= 0x80) or `dialogs` isn't running (0x4bb1d0):
  retry next frame.
- The only exe-side poster to `dialogs` (C, all 78 direct 0x4bb220 sites classified, `out\posters.txt`): 0x40e4cb
  (Achievements -> CHALLENGE_COMPLETE, payload `[int index]`). The exe has no 0x694fe697 dword and no NETWORK_ERROR_* strings.
- To close a dialog (C): post 0x3cb781ef with the NONE payload (front_main does at +0x707; dialogs +0x4d6..0x533 handles
  it with callback -1 and a scene destroy).

## Q4. After OK, and where the console went
**Detecting OK without a callback (C).** With NONE in P[1] and P[2] the select path skips the callback (+0x6db..0x6e9
shipped). Close site in the shipped module: `02 1f 96 4b 42 | 32 01 06 02 01 00 5e 01` = HASH 424b961f, then `0206/1` at
+0x749, PC after = **0x751** (compare from PC-13); stock +0x728, PC 0x735. One frame later busy is cleared, then `51 00 51
00 32 02 3f 04 01 00 61 01` (`043f(0,0)` at +0x762, PC **0x76a**; stock PC 0x74e). The `5e 01` / `61 01` line fields make
these sites unique. A dialog can also be replaced (new dfea0f93) or closed (3cb781ef) through other sites: then no answer
arrives; reset the pending state on EndRace.

**Ending the race (C).** As `LeaveRaceForJoin` does: the clean-ups 2116 (0x5fab70), 210a (0x5fa0f0), 093f (0x58e820);
post `0x4bb220(0, 0xbed107e7, 0x682e3dfe QUIT_GAME, &{0,2}, 0)` (payload 1 on results_retry_quit). The PC game module
also accepts QUIT_GAME_TO_LOBBY 0x95a26f75 (game +0x062e); both lead to the same exit routine (+0x835); on PC its argument
is never read. Keep `g_returnToLobby` set: front_main reads key 0x72d1c72b at +0x438 (our Hook1b01, PC 0x440) and gets
0xb0a0d459 when the Steam state is InLobby (-> `fe_network_game.45b79703`, g22 = 1, lobby re-entry ACTION -7), else
0x7ab1c9a7 (-> `fe_network_game.c5cccce2`, g22 = 0, the search screen, ACTION -2); front_main bytes verified at +0x52b and
+0x556. Cases: 1 (host alone, still in the lobby) -> the lobby; 2 (a guest that saw the host leave; with host migration
it stays in the lobby, which becomes the new host's) -> the lobby or the search screen as the Steam state says; 3 (the
host still in the lobby) -> the lobby.

**The console flow** (Mac FrontEndScripts `fe_network_error_handler`, package +0x01ef6c; C): a map from error id to
`[dialog export, flag, flag]` (+0x76..0x13a), ids named by hashing the Mac binary's strings:
NETWORK_ERROR_PLAYERS_LEFT b3995cda -> 06d589d4 -> **CB5D217E**, callback 881ec44f; NETWORK_ERROR_PLAYER_RETURNED_TO_LOBBY
eb05396b -> cf577e43 -> **168056C7**, callback 881ec44f; NETWORK_ERROR_HOST_LEFT 78d13212 -> 1f89a99e -> **8374A07B**,
callback afddedd6; NETWORK_ERROR_HOST_KICK 508508eb -> 14A011B9; NETWORK_ERROR_LOBBY -> 93DE36EE; NETWORK_ERROR_GAME ->
71C55ED8. Callback 881ec44f (+0x658): in NETWORK_GAME `1b03(0x72d1c72b, 0x7ab1c9a7)`, then `160c(<module in blackboard
0x4a8c2b95>, QUIT_GAME_TO_LOBBY 0x95a26f75, 0)`: back to the network lobby. Callback afddedd6 (+0x4fe): on host-left
`1b03(0x72d1c72b, 0xee8a2362 fe_mainmenu)`, then QUIT_GAME(0): back to the **main menu**. Before showing an in-game error
(+0x15e): `if 0918() >= 0: 0926()` (pauses only if a pause was being requested). For host-left and kicked, 095f sets
scene+0x1350 = 1 (gates a stats call 0x4bbec0). The console never showed CDA264D3 or 07B1DE19 from these scripts; the
user's texts and buttons are new combinations. The PC `fe_network_error_handler` is byte-identical to the PC
fe_network_game stub; its A363A411 values are name_hash("LOBBY") scene/node uses.

## Q5. When the last other human leaves
PC (C): no exe or script path detects it or shows a dialog; our race code marks the peer gone, gives its car to the AI,
stops waiting for it and races on. Console (C from the Mac scripts, I for the C++ side): the session layer raised
NETWORK_ERROR_PLAYERS_LEFT; network_game +0x829 switched to state "NetworkErrorPlayersLeft"; the error handler showed
CB5D217E; on OK it quit the race with QUIT_GAME_TO_LOBBY. The race was not ended by the race logic.

## Smallest safe implementation
1. **Build:** the 3-dword in-place edit of the shipped `dialogs` module (Q1), old bytes asserted in build_menus.py.
2. **DLL `ShowNetDialog(def)`** on the game thread, posting only while not paused (`[0xc2a015]==0 && !g_netPause`): the Q3
   sequence with target 0x694fe697, message 0xdfea0f93, `[def, none, none]`; retry next frame if the post returns 0. Place:
   RaceNativeBefore at the game inbox site (game +0x048e, PC 0x496, `51 01 32 01 0e 16 02 00 2f 00`), every frame through
   the race and the results.
3. **Triggers in Pump**, mid-race only (phase Running, `!RaceOver()`); store the per-peer abort reason (today only logged).
   Case 1, CB5D217E: host, every peer gone. Case 2, 8374A07B: guest, host absent from the Steam lobby. Case 3, 168056C7:
   guest, the host's kAbortQuit while the host is still in the Steam lobby (the host's 090a sends kAbortQuit after every
   race: never act on it at the results screen).
4. **Answer:** RaceNativeBefore on 0x0206 at the dialogs PC 0x751 (0x735 stock) ScriptSite while our dialog is pending; then
   the LeaveRaceForJoin quit sequence; Hook1b01 picks the lobby or the search screen.
5. **Optional:** while our dialog is up, 0918's return -1 (int) in RaceNativeAfter at pause_menu PC **0x3e0** (bytes from
   PC-8: `32 00 18 09 01 00 7a 00`, then `27 17 00`): stops Esc opening the pause menu while Enter could confirm a pause item.
6. **Cleanup:** on EndRace with our dialog still open, post 0x3cb781ef with the none payload.

## Risks
- Not yet seen in game (I): drawing over the HUD and Enter/A input mid-race. The first in-game check.
- The race keeps running behind the dialog (no world pause, as on the console). Held buttons on the primary pad reset
  when the dialog opens.
- A wrong definition id leaves `dialogs` busy forever (C): post only the 3 ids, asserted at build time.
- A dialog left open after the race blocks our lobby's busy-gated screens until dismissed: close it on EndRace.
- The 128-message queue is shared: a post can fail and must be retried.
