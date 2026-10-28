# M6 online pause (reverse-engineer report, 2026-10-21)

Read-only; capstone disassembly of `re\bin\SSASR-unpacked.exe` + the script decodes; nothing run in game.
C = confirmed in bytes, I = inferred. Helper scripts: `re\m6-pause\scripts\`, raw xrefs `re\m6-pause\xref*.txt`.

## Main result
The PC exe and the PC `pause_menu` script still have the console's online pause. In a race of game type 5
(native 0916 == 5) the pause menu calls native **0928 instead of 0926**: no world pause; the pausing
player's own car is handed to `STATE_Control_AIRacing_DisconnectedController` (state 0x21) and the HUD stays.
CONTINUE calls **0929 instead of 0927** (hands the car back). The menu closes itself when the results start.

## How a pause stops the race on PC (C)
- `game` starts `pause_menu` (+0x041f `1601(0x791427eb)`); its loop calls 0918 (slot 0x6f6050 -> 0x58ed50)
  every frame: the creation index of the local human asking to pause (Start bit pad+0xf0 & 0x10, pad
  disconnected pad+0x1cc == 0, keyboard [pads+0x4c]->vt+0x28(0xC5)), else -1; -1 if any pause flag
  (0xc2a014/15/16) is set, or unless [0xcf7a51] && [0xcf7a54] (or scene+0x1117). [0xcf7a54] is set at Racing
  enter (0x5b2032), cleared at WaitingForFinishers exit (0x5b2237): pause only between GO and the race end.
- The pause = native 0926 (slot 0x6f6088 -> 0x58ee70 -> 0x49bfe0): rumble off and [0xc2a015] = 1 (if
  [0x6ec865], 1 on disk). No phase change, no time scale.
- Gate: main frame loop 0x4967c0 at 0x496885 (`call 0x495480` = any of 14/15/16; jne 0x496900) skips the sim
  block (telegrams 0x4d2a80, race phases 0x49ade0 -> 0x4cbb80, racers, physics). 0x4a0710 adds to the tick
  counter 0xc2a008 only when 14/15/16 are all clear. The script VM is gated only by 14 and 16 (0x496854).
- CONTINUE: native 0927 (0x49c000): rumble on, [0xc2a015] = 0. 0xc2a016 = lost focus (WndProc), 0xc2a014 = debug.

## pause_menu's 0916 sites (module sha ba215a415643…, same in stock and Roster Pack; PC = VM+4 after the call)
| Site | Call / PC | Script | Type 5 does |
|---|---|---|---|
| open | 0x04bc / 0x04c4 | `NAT0916; INT8 5; NE; JF 0x4eb` | 0928 (0x04eb) instead of 0926 + `160c(hud, HUD_PAUSE 0x84b33a36)` |
| race-end auto-close | 0x0676 / 0x067e | `0916==5 && 1b01(0x86d2420e)` | G17 = 0 (closes the menu), `1b03(0x86d2420e, 0)` |
| quit destination | 0x06fc / 0x0704 | the return key | leave (tested M5 path) |
| HUD_UNPAUSE | 0x083a/0x0842, 0x0892/0x089a, 0x08ee/0x08f6 | `0916 != 5 -> 160c(hud, 0x26f4c14f)` | skips HUD_UNPAUSE |
| unpause | 0x0985 / 0x098d | `0916 != 5 -> 0927 else 0929` | 0929 (0x099f) |
| controls page race-end | 0x0a99 / 0x0aa1 | `0916==5 && 1b01(0x86d2420e)` | CALL 0x14b3 (back to the main page) |
| item list | 0x0cdd / 0x0ce5 | (hooked since M5) | no RESTART |
- `results` writes `1b03(0x86d2420e, 1)` at RESULTS_START (+0x1ade); nothing on PC clears it except the type-5
  pause menu (1b07 is a bare ret at 0x6021f0): after any race the key stays 1.
- `network_pause_menu` / `network_game` are debug modules no script starts.
- Behaviours.sbn track objects also call 0916: never return 5 globally.

## 0928 / 0929 (C)
- 0928 (slot 0x6f6090 -> 0x49d1e0, thiscall on the scene): the first racer with [ctrl+0x18] == 1 (human) not
  category 3 = the local car; 0x4eaa10(1): racer+0x1fc8 and telegram 0x32 to racer+0x2c0, next sim tick ->
  controller -> Human state handler 0x5ae1e0 -> 0x5a4500: 0x32 sets ctrl+0x24c = 1 (0x5a4622), 0x33 clears it.
  The Human update (0x5af15d) then pushes state 0x21 = AIRacing_DisconnectedController; only the top state
  updates, so the pad no longer drives the car; 0x21's update 0x5a8180 runs the AI racing update 0x5a6f60 and
  pops back to Human once ctrl+0x24c == 0. 0929 (0x49d260) = the same with 0x33. A finished car: no-ops.
- While paused (I): the AI drives the car along the racing line (enter 0x5ac270 speed band); its vtable
  0x6b1e18 makes slot 19 a no-op (AIRacing reads racer+0xc1c there): likely no item use; still picks up boxes,
  can finish; ignores pad telegrams 0x20/0x22. The sim, our hooks, poses, items and finishes keep running; the
  HUD stays live (RACE ENDS IN keeps counting); title "PAUSED 1P/2P" from 0f0f(p) = racer+0x9dc.

## Risks
1. Race end with the menu open: the 0x067e/0x0aa1 overrides close it; a stale key 1 from an earlier race
   would close the next online pause at once: filter 1b01(0x86d2420e) at those two sites to 0 unless RaceOver().
2. RETURN TO LOBBY (0x06d4) never unpauses; the sim ticks a few frames until 090a. Low.
3. CHANGE CONTROLS writes 2004(7/8) and 0f08 (pad config), not our rules fields.
4. 0929 must follow 0928 (else the car stays AI): latch at the open site.
5. Unverified: an All-Star running while pausing; whether CONTROLS shows in the type-5 list (local +0x0d64).
- Rejected: clearing [0x6ec865] (pad still drives, HUD hides, no auto-close); re-pointing 0926/0927 slots.

## Recipe
- Latch g_netPause (outside g), reset at the race's end.
- Hook0916 open site PC 0x04c4, 16 bytes from PC-8 `32 00 16 09 01 00 b8 00 51 05 4b 14 21 00 32 00`:
  g_netPause = g_returnToLobby && running; if set, 5 (type kept).
- Hook0916 while g_netPause, 5 at (16 bytes from PC-8):
  0x067e `32 00 16 09 01 00 f4 00 51 05 4a 02 0e 42 d2 86`; 0x0842 `32 00 16 09 01 00 61 01 51 05 4b 14 15 00 02 81`;
  0x089a `32 00 16 09 01 00 6e 01 51 05 4b 14 15 00 02 81`; 0x08f6 `32 00 16 09 01 00 7d 01 51 05 4b 14 15 00 02 81`;
  0x098d `32 00 16 09 01 00 94 01 51 05 4b 14 0c 00 32 00`; 0x0aa1 `32 00 16 09 01 00 cb 01 51 05 4a 02 0e 42 d2 86`.
  Don't clear the latch at 0x098d (0x0842 runs after `CALL 0x985` on one path).
- Hook1b01 while g_netPause, arg0 = {0x86d2420e, type 3}, at PC 0x068e `02 0e 42 d2 86 32 01 01 1b 01 00 f4 00 4f`
  or PC 0x0ab1 `02 0e 42 d2 86 32 01 01 1b 01 00 cb 01 4f` (14 bytes from PC-13): if !RaceOver(), value 0.
- In-game test: pause mid-race (HUD stays, the paused car drives itself on both screens, games in sync);
  CONTINUE (control back); stay paused to the race end (menu closes, normal results); a second race (the pause
  stays open); RETURN TO LOBBY from the pause; an offline race still freezes.
