# Drift+ and each player's input device (2026-10-27)

Reverse-engineer pass on `re\bin\SSASR-unpacked.exe` (3c0c9f01…; data slots and tables also checked in `re\bin\SSASR.exe`
c110b4d3…), saved here by the main session. Scripts `scripts\`, raw decompiles `out\d1..d14.txt` (`ghidra\` is a scratch copy).
C = confirmed in bytes or decompile, I = inferred, U = unknown. Nothing here was run in game.

The user's definition: "Drift+ is a community made name for a controller ability to 'steer twice' using both its control
stick and d-pad". The user's rule: while a keyboard player is in an ONLINE lobby, a controller steers with the stick OR the
d-pad, never both; when both are used, the first one keeps control until it is let go.

## Corrections to notes\KEYBOARD-DRIFT.md
- **"Keyboard = device 4" is not reliable (C).** `0x406e00` sets the device count `[mgr+4]` = 0 at 0x406e3d, then
  `EnumDevices(GAMECTRL, 0x406c70)` BEFORE the XInput loop; the callback skips XInput devices (`0x406910`, the WMI
  `Win32_PNPEntity` "IG_" check), so every other DirectInput controller gets indices 0..k-1, then the 4 XInput slots (always
  created; `dev+0x1cc` = connected), then the keyboard at k+4 (0x406f99, also `mgr+0x4c`). The mouse is apart at `mgr+0x50`.
- **Classify by the device's vtable (C):** XInput pad `0x67e3f8` (0x40589f), keyboard `0x67e444` (0x405f10), DirectInput
  controller `0x67e39c` (0x405277); or `dev == [mgr+0x4c]` for the keyboard. `mgr = [[0xc2a1b4]+0x14]`,
  `dev = [mgr+0xc+4*idx]`, 16 slots.
- **The Human slot alone misses two states (C):** 0x12 Human_Erratic (vtable 0x6b1bb0, slot 0x6b1bbc = `a0 f6 5a 00`) calls
  `0x5af150` directly (E8 at 0x5af6ac); 0x17 StartingGridHuman (vtable 0x6b1c94, slot 0x6b1ca0 = `60 4f 5a 00`) has its own
  copy of the stick + d-pad sum in `0x5a4f60`. 0x13 Human_MonkeyBall (`0x5af880`, AiAi's All-Star) uses the stick only (no
  d-pad term): leave it.

## What Drift+ does (C unless marked)
- Steer `CS+0x40` = stick X (id9) - d-pad left (id25) + d-pad right (id26), unclamped (the x3-and-clamp path at 0x5af34a is
  behind the stub `0x508180`, which returns 0). At the end, if `[racer+0x1d0c]` bit 3 is set, the steer is negated.
- The steering filter `0x4e7da0` (target = -raw*|raw|, lag by speed and per-car data only) has no clamp (only +-0.5 while
  `racer+0x1ee8 != 0`, meaning U): with raw 2.0 the filtered value C converges to 4.0.
- x4 (linear in C): normal turning `0x578bf0` (yaw = C x speed gain `0x551910`) and its speed scrub `0x5521a0` (drift state 0
  only: Drift+ costs speed outside drifts); drift hop (state 1) angle rate `tB+0x200 * 0.8 * C` (0x5cbadf); drifting (state 2)
  target slip `+-(tB180 + |C| tB1a0)` (counter-steer: minus `|C| tB190`) and the yaw drive term `tB1d0 * C`.
- x16 (C squared): the release (state 3) slip target `0.5 * local_140 * C'^2` (0x5cc275..) and the counter-steer angle rate.
  **This probably explains "a controller keeps its direction through the release" (I): a plain stick at full lock is C = 1,
  the same as a key; only Drift+ goes beyond.**
- No difference: the drift latch `+0x2a0` (sign only), air steering `0x5cb0e0` (clamped), the steer quantizer `0x55b060`
  (+-31/31, feeds the record built in 0x504420), lean / items / All-Star code (thresholds on the vertical).
- Drift+ can block a drift start: CanStartDrift `0x551940` in state 0 requires |C| <= |raw| (chains 3 -> 1 skip it).
- Visual / camera (unclamped, I): wheel angle `0x59c5b0`, held-item swing `0x4c4af0` (C*pi/8), camera swing `0x4ee1d0`.
- Absolute sizes depend on per-car tuning (not in the dumps, U).

## Other ways past +-1
- Vertical `CS+0x3c` = stick Y + d-pad up - d-pad down also reaches +-2, but every reader thresholds it (no gain).
- Gas / brake use max(), so <= 1. XInput: stick = short/32768 (dead zone 0.24), d-pad / buttons 0/1, wheels lose only the
  dead zone. DirectInput: axes +-1000 split to 0..1 with a 0.3 dead zone, the POV hat as 4 digital channels (`0x405460`).
  The same 3-input sum applies everywhere: +-2 is the ceiling.

## Which device a racer uses (C)
- Every driving read uses `[mgr+0xc+4*CS[0]]` only (`0x5af150`, `0x5aeee0`, `0x5a4f60`, `0x5af880`); `CS[0]` = racer `+0x9ec`
  (0x4e6360 arg 3), copied to `ctrl+0x1c` once (0x4e6110). A player can't switch devices in a race.
- The primary pad: `fe_startscreen` clears it (`0402` at 0x075d); natives `0437`/`0436` (`0x4a8a70` with id 35 / 28) return the
  first device in list order whose press bit is set this frame; `0403(idx)` at 0x07b5 stores `[mgr+0x960]` (flag
  `[mgr+0x95c]`). Native `0400` returns it; `kNatRaceSlot` (natives.cpp) passes it to `0905` as the device. So the lobby's
  input type = the class of `[mgr+0xc+4*[mgr+0x960]]` (changes only by going back to the title screen).

## Hook plan (C)
- Order per tick: scene pre-pass 0x49dd47..0x49dd7a -> 0x4eff50 -> controller slot `[vt+8]` = 0x6b0364 -> `0x5097a0`:
  `0x540620` zeroes CS, the state machine `0x50e0e0` runs the top state `[[ctrl+0x228]+0xc]`, then the mask `0x5406d0`; only
  then racer slot 4 `0x4f3990` (filter at 0x4f3b59). A value written right after the state update is this tick's.
- Human states run only for local humans (category `[ctrl+0x18] == 1`, table 0x507edc; Network21 0x15 = category 3; AI 2).
- State layout (enter 0x5a46c0): +0x10 racer, +0x14 ctrl, +0x18 CS (= ctrl+0x1c); steer = `[[state+0x18]+0x40]`.
- Values: `row = 0x6b3390 + 0x4c*[ctrl+0x20]` (row 0 = `04 03 02 01 09 0a 20 22 17 18 1f 00 00 00 00 19 1a 17 18` dwords;
  `[ctrl+0x20]` = 0); `val(id) = ([dev+0xd4+id] & 1) ? *(float*)([dev+0x1bc] + 4*[dev+0x18+4*id]) : 0`; stick X =
  val(row[4] = 9) (XInput channel 11, buf+0x2c), d-pad left = val(row[15] = 25) (buf+0x24), right = val(row[16] = 26) (buf+0x28).
- Slots (thiscall(state), no args, plain ret; none in the Roster Pack's fingerprints; no installed mod DLL references them):

| Slot | Bytes | Stock | State |
|---|---|---|---|
| 0x6b1b8c | 50 f1 5a 00 | 0x5af150 | Human |
| 0x6b1bbc | a0 f6 5a 00 | 0x5af6a0 | Human_Erratic |
| 0x6b1ca0 | 60 4f 5a 00 | 0x5a4f60 | StartingGridHuman |

- Existing keyboard/pad differences: only the dead zones (stick 0.24, DirectInput 0.3) and on/off keys; the stubs
  `0x508170`/`0x508180` (`32 c0 c3`) keep their digital-pad paths dead. Side note (I): MonkeyBall reads only id 9, which the
  keyboard has no binding for.
