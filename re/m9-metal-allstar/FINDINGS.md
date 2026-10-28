# Metal Sonic's All-Star online (reverse-engineer pass, 2026-10-24, session "M9 · Metal Times Two")

Saved from the main session. Covers the standalone Metal Sonic 1.1.2 (`MetalSonicMod.dll` db9ce65b…) and the Roster Pack Metal
(wrapper fee9cece…): the class bytes are identical, only the core helper addresses differ (CONFIRMED, generated plans). Scratch
(not in the repo): the session scratchpad `metalallstar\` (`scripts\plan.py`, `mdis.py`, `calls.py`; `out\region0.bin`,
`out\patches.json`, `out\allstar_machine.txt`).

## Summary
Metal's copies make no hidden decisions (no random draws, no input, no target choice; the beam follows the synced pose and fixed
per-tick steps), but OUR copy handling breaks its end: the 60 s length every copy gets (race.cpp, the All-Star copy start) keeps a
copy's plates/beam open, because they retract only when elapsed >= length. On other screens the full-width beam stays open while
the owner's retracts, ends open, and stays frozen through phase 5 until slot 16 despawns it (CONFIRMED). Hitting local cars while
frozen: INFERRED. Phase-5 length: UNKNOWN.

## The class (CONFIRMED, bytes)
- Not inside `MetalMaximumOverdrive.dll`: `MetalSonicMod.dll` copies it into a read/execute block allocated at start-up (vtable at
  the block start); the core DLL provides helpers. All-Star class id 0x1a; the patch at 0x5863ea (`83 f8 18 0f 87 28 03 00 00` ->
  jmp block+0xb20) builds a 0x220-byte object. Phase-4 start: the patch at 0x585fe2 (`c7 06 04 00 00 00` -> block+0xae0) runs
  `metal_finish_activation`.
- Slots: 7 = stock `0x5a00d0` (zeros), 6 = stock `0x5a0000`; overridden 1 +0xd0 init, 4 +0x310 keep-alive while a plate is open
  (+0x129/+0x12a), 5 +0x330 message 0x27 -> 0x28, 8 +0x380 prepare, 11 +0x3e0 `metal_activate_tracked`, 12 +0x400 phase-4 tick,
  13 +0x5e0 cleanup, 14 +0x610 hide, 15 +0x630 phase 5 (steering only), 16 +0x730 restore (despawns the LaserBar), 19 +0x7a0,
  24 +0x7e0, 25 +0x800 animation events.

## Decisions on a copy
- Random: none (no 0x4be170/0x4be180, no CRT rand; the core imports KERNEL32 only) (CONFIRMED).
- Input: the tick calls 0x5a0080; a copy gets zeros (CONFIRMED). A human owner's item button (out+9), with the 1.5 s timer `+0x1e0`
  idle and the animation not 3: `+0x1e0 = 1.5f`, animation 3, a 1.5 s speed boost (vehicle +0x990), a local camera kick
  (`0x4a0c00`). The speed reaches copies through the pose; animation 3 is cosmetic; the owner's lean/bank (`render+0x30`,
  `racer+0x1f70..0x1f90`) isn't shown on copies (cosmetic, INFERRED).
- Targets: none: open plates at the track's edges 30 units ahead of `ctrl+0x80`; closed plates fixed offsets from the pose
  (CONFIRMED, source); the cursor following a remote car's synced pose INFERRED.
- Timing: 2.0 units per tick at 50 ticks/s (INFERRED).

## Hits (CONFIRMED)
The LaserBar is spawned locally on each screen (`0x4b8b00` -> `0x4b8830`), never through the event manager. A car hit is flipped
by its own game: the patch at 0x4f4f20 -> block+0xb40 checks protection (`0x4f8220`), sends 0x27 and calls FlipOut `0x4f1fe0(1)`:
"victim decides" already holds; our listener hook handles 0x28 generically.

## Our code (CONFIRMED)
Our Opa / pulse / gate hooks compare stock vtables (Metal never matches); our slot-8 hook chains into Metal's 0x4f4f20; ending a copy
through `S+0x4b` uses the stock 0x586240. Activation can fail on one screen only if Metal's memory checks fail there (that screen
resets to phase 1 via `0x586050` or runs with no beam: INFERRED, rare).

## The fix (no hooks into Metal)
- Don't hook Metal's code or vtable (its block is read/execute; its activation tracking and renderer require `vtable == expected`).
- Recognise Metal: `GetProcAddress(GetModuleHandleW(L"MetalMaximumOverdrive.dll"), "metal_runtime_config")`: magic 0x5452534d,
  version 1, size 64, armed 1; the vtable at +0x10; compare with `[racer+0x2030]`'s vtable (`[S+0x40]`); any check failing: change
  nothing.
- Owner (after the race update, beside SendAllStarPhases): phase 4, the object is Metal, and `S+0x20[0] >= S+0x30[0]` for the first
  time this All-Star: resend phase 4 with a body flag bit 16 ("time up"); reset at each All-Star's start.
- Copy (the pre-step beside ForcePulseShots): at the owner's shown tick, phase 4 and Metal: copy all four lanes of `S+0x20` into
  `S+0x30` (closing starts in this tick's update). A copy still in phase 3: pending, applied on its first phase-4 tick; dropped if
  the phase leaves 3/4 or after ~250 ticks. Keep the 60 s length and the `S+0x4b` end as the fallback.
- Optional cosmetic press mirror: the owner's proof `obj+0x1e0 == 1.5f` right after the press tick; on the copy, only if
  `+0x1e0 <= 0.001f` and the animation (`0x59fe90`) != 3: `+0x1e0 = 1.5f`, `0x59fe70(obj, 3)` (thiscall, ret 4).

## Test fakes
Racer state: phase +0, total +0x10, elapsed +0x20, length +0x30 (four floats each), object +0x40, end byte +0x4b; a 0x220-byte,
16-aligned Metal object whose vtable equals a fake config's. Fake tick copied from 0x5867a0: time-up check, slot 4 (plate open
flags, else the base check), then the `S+0x4b` / slot-12 branch; plate closing uses the elapsed from before the update, afterwards
`elapsed = min(elapsed + dt, length)`. Cases: exactly one "time up" per All-Star; non-Metal / no config -> no write; a copy in
phase 3 keeps it pending; the regression (60 s length, no message: plates still open when `S+0x4b` ends the copy). Press fake:
decrement the timer (0x5a0900) before checking the press.
