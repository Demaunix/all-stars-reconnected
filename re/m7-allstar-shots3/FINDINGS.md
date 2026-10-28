# The last four All-Star classes: gates, proofs, forces, payloads (static pass, 2026-10-23; reverse-engineer subagent, saved by the main session)

Basis: checked against the bytes of `re\bin\SSASR-unpacked.exe` with `tools\disasm.py` and Ghidra decompiles (project copy `ghidra\`, 45 MB,
don't commit). **C** = confirmed in the bytes, **I** = inferred. Vtable slots 0-30 of all four classes are identical to the on-disk `SSASR.exe`.
Nothing tested in game. Builds on `re\m7-allstar-shots\FINDINGS.md` and `re\m7-allstar-shots2\FINDINGS.md`.

## Slots (C)
| class | vtable | slot 7 data = stock | tick data = stock | phase-5 slot 15 | chooser `0x5a0080` call sites |
|---|---|---|---|---|---|
| Amy 0x04 | 0x6b7810 | `0x6b782c`=0x5d86c0 | slot 12 `0x6b7840`=0x5d8870 | `0x6b784c`=0x5d8820 | 0x5d88e0 (tick), 0x5d8843 (slot 15, steer only) |
| Ulala 0x0d | 0x6b7d60 | `0x6b7d7c`=0x5e07c0 | slot 12 `0x6b7d90`=0x5e09e0 | `0x6b7d9c`=0x5e0cd0 | 0x5e0a0c (tick), 0x5e0cf3 (slot 15, steer only) |
| Beat 0x08 | 0x6b7ab8 | `0x6b7ad4`=0x5dade0 | slot 12 `0x6b7ae8`=0x5db390 | `0x6b7af4`=0x5db650 | 0x5db3c2 (tick), 0x5db674 (slot 15 passes held = 0, steer only) |
| Bonanza 0x06 | 0x6b7920 | `0x6b793c`=0x5bc4a0 | **slot 23 `0x6b797c`=0x5bd470** (slot 12 `0x6b7950`=0x5bbd60 only checks the end) | `0x6b795c`=0x404090 (`ret 4`) | 0x5bd4b5 only |

Slot 7 runs once per phase-4 tick (once per slot-23 call for Bonanza), plus once in phase 5 for steering only. Every proof below can be
read right after the stock tick returns; an anim request (`0x59fe70` -> `0x502d00`) shows at once.

## 1. Amy (C unless marked)
- Slot 7 `0x5d86c0`: out+8 = out+9 = 1 if `+0x140 != 0` (`39 81 40 01 00 00` at 0x5d86d3) or MT%80 == 0 (the draw always happens).
- `+0x140` is NOT a combo counter: slot 11 `0x5d7e60` (0x5d7e73..0x5d7e9b) turns on three zones on the racer (`0x511870(0xfbec1d39 /
  0x53a4cf86 / 0x68c72b2e, 1)`, ECX = the racer); another object inside a zone sends message 0x23 / 0x24 / 0x25 to Amy's listener
  (0x4f5f9c..0x4f6004, via `0x4d2bf0`); slot 5 `0x5d83d0` (phase 4 only; jump table 0x5d84e0) sets `+0x140` = 4 / 3 / 5 (0x5d8410 /
  0x5d842b / 0x5d8437) and `+0x144` = 1.5 (`[0x6a4264]`): the swing that matches where the target is (which zone is which side: I).
  Reset: at the tick's start, if `+0x144 > 0` it subtracts dt; below 0 -> `+0x140 = 0`, `+0x144 = 0` (0x5d888c..0x5d88bb). Slot 11
  clears `+0x13c`, `+0x140`, `+0x144`, `+0x13a` (0x5d8003..0x5d8017). Nothing else writes `+0x140`; the swing never clears it.
- Fire gate: out+9 != 0 (`80 7c 24 11 00` at 0x5d88f5) and the anim is 2 or 7 (0x5d8907 / 0x5d8913). No timer.
- The swing: anim speed 1.5 (`0x5a01d0`, `racer+0x1f2c`); if `+0x140 != 0` the anim becomes `+0x140` (0x5d896d); else 3 if `+0x13a` is
  set, else 4, and `+0x13a` flips (0x5d8986..0x5d89a5). Anims 3/4/5 are requested only at these two sites (`0x5d8500` is dead code).
- The hit is local contact, not a projectile: message 0x27 from the victim's collision code to the owner of the touching object
  (`[obj+0x310]`); Amy's slot 5 handles it (0x5d8443): `+0x138 = 1`, message 0x28 to the victim, `0x4c8570(victim, 9, 0)`; it doesn't
  check the anim (which object carries the hit volume: I). At the swing's end slot 25 `0x5d8280` goes to anim 6 (hit) or 2.
- **Proof:** anim before the stock call 2 or 7, after it 3, 4 or 5. **Payload:** A = the anim after.
- **Force (copy, before the stock call):** `+0x140 = A`, `+0x144 = 1.5f`; the slot-7 hook outputs out+9 = 1. The copy's anim must be 2
  or 7 (else keep the shot pending). Leaving `+0x140` set is harmless while the hook blocks the copy's own presses.

## 2. Ulala (C unless marked)
- Slot 7 `0x5e07c0`: MT%40 == 0 sets out+8 = out+9 = 1. Gate: out+9 (`80 7c 24 21 00` at 0x5e0a24) and the anim exactly 2 (0x5e0a36). No timer.
- Target: `place = 0x4cbc40([racer+0x9d8])` (0-based index in the race-order table `0xcf79d0`, 8 bytes per entry, count `[0xcf7a56]`);
  place 0 or no entry: T = -1, anim 6; else T = the byte at `0xcf79d0 + 8*(place-1)` (the car one place ahead, 0x5e0a70). If that racer
  exists and its `+0x20fd` is 0: failing the forward-cone (`[0xc28520]`) or range (`[0xd47f90]`) test -> T = -1, anim 6; else the side
  dot product picks the anim: < -0.25 -> 8, > 0.25 -> 7, else 6. If that racer is null or `+0x20fd != 0`: jump to 0x5e0bf6, fire at T
  with NO anim change (anim stays 2).
- The shot spawns in this tick: 0x5e0bf1..0x5e0c08 requests the anim, `0x59fe80`, then `0x5e0220(T, anim)` (thiscall, `ret 8`) ->
  `0x538170(0x67fa5939, T, 3, 2)` (T = Ulala's own index becomes -1). The anim sets the lane: 7 -> 0.0 / `+0x398` 0.2; 8 -> 1.0 / 0.8; 6 or 2
  -> 0.5 / 0.5. The homing target goes into the projectile's `+0x318` (`0x520240`); no random draw in `0x5e0220`.
- No class field is written at the fire. Slot 25 `0x5e0640` turns a finished anim 3..8 back into 2. Anims 6/7/8 are requested only at
  0x5e0bf1; the contact handler (slot 5, message 0x27) requests anim 3 + MT%3 (a local draw) and sends message 0x28 to the victim.
- **Proof:** anim 2 before, and (a) 6/7/8 after, or (b) for the dead-target path the projectile serial `[0xcdd048]` changed during the
  call (`0x4b8830` bumps it on every object creation, 0x4b8962; that the shot is the only object created in the tick: I).
- **Payload:** A (6/7/8, or 2) and T. **Force, simple:** the slot-7 hook outputs out+9 = 1 while the copy's anim is 2 (retry while due);
  the copy picks its own target from its own race order and the synced poses (I: the same unless the screens disagree near the +-0.25,
  cone or range limits). **Force, exact (not built):** never output fire; right after the stock call, if the anim is 2: `0x59fe70(A)` (skip
  when A = 2), `0x59fe80()`, `0x5e0220(T', A)` with T' the owner's T mapped to this PC's index.

## 3. Beat (C unless marked)
- Slot 7 `0x5dade0`: always held (out+8 = out+9 = 1), no random draw: copies of a computer Beat already match. The tick uses only out+8
  (0x5db3df, 0x5db4e7; `0x5db1d0` the low byte); out+9 unused. On/off, no levels.
- Held: anim 2 -> 3 with `+0x15c = 0` (0x5db3fe / 0x5db406, the class's only request for 3); the effect `[obj+0xd0]` slot 6 gets 0; the
  steering `+0x984` is frozen; the spray aim `+0x15c = clamp(0.5*sin(+0x170) + out0, +-1)` goes into the anim blend; the phase `+0x170`
  grows by dt*pi each tick (slot 11 resets it); **`+0x130 = 0.9f`**. Released: anim 3 -> 2 (0x5db41d); the effect gets 10000; `+0x984`
  follows out0 (clamped +-0.2); **`+0x130 = 1.1f`**. `+0x130` is written at 0x5db32c (`f3 0f 11 86 30 01 00 00`) only while vehicle `+0x980
  != 0` (slot 11 sets it to 1 through `0x550b40`); no other phase-4 write (slot 16 writes 1.0 at the end).
- The spray object `[obj+0x158]` is the projectile 0x8dcdc54b made in slot 8; `0x511870(0x556f6d98, 1)` turns it on at the start of anim 3
  (slot 25 `0x5db090` -> `0x5dada0`), off when anim 3 ends or anim 2 starts (`0x5dadc0`). Hits are local: message 0x27 -> slot 5 `0x5db190`
  -> `0x5daae0(victim)` tags it (`+0x134[i]`, `+0x154++`; a computer car gets message 0x19; a local human gets the graffiti on screen
  `0x4bb220(…, 0x96930381, 0xdd761549)`; message 0x28 to the victim); `0x5dad50` untags cars for which `0x4e8330` is true each tick.
- **Proof (owner, right after the tick):** H = `[obj+0x130] == 0.9f`. Send H's edges (and H at the start of phase 4). **Copy:** the slot-7
  hook outputs out+8 = the mirrored H; for the spray aim also out+0 = the owner's `[ctrl+0x5c]` while held (the phase `+0x170` still
  drifts with each PC's own dt: visual, I).

## 4. Bonanza Bros (BoostSteal) (C unless marked) — NOT built yet
- Who calls slot 23: `obj = [racer+0x2030]`; the blimp `[obj+0x174]` is a projectile spawned by `0x5bc780` from slot 25 at the start of
  anim 0, with `[blimp+0x5a4] |= 0x80` (0x5bc939). The generic projectile vtable 0x6b2980 slot 13 (`0x6b29b4` = 0x529830) calls
  `[[blimp+0x310]+0x2030]->slot23(blimp)` (`8b 42 5c … ff d0` at 0x529929 / 0x52992d) only when bit 7 of `+0x5a4` is set and `[racer+0x1ff0]
  == 4`. No other slot-23 call. Slot 23 returns at once unless its argument equals `[obj+0x174]`.
- Order inside slot 23 `0x5bd470`: (1) the chooser; (2) the aim: if `+0x1a4 != 5` and out0 != 0: `+0x150 -= [0xc2a01c] * 10.0 * out0`,
  clamped +-0.3490659 (20 degrees) (0x5bd4ba..0x5bd539), no decay; (3) the bomb position `+0x160` from the bone `Hud_Status_Ticking_Bomb`
  (or `RightHand`); (4) the state machine `0x5bd040` (0x5bd65e): in states 2/3/4, if the layer-3 clip position (slot 15 of
  `[[obj+0x1a0]+0x34]`) >= 14.0 (`[0x6f5a78]`; unit: I) and `+0x186 == 0` (0x5bd38d): the throw `0x5bc4f0(normalize(+0x130))` (0x5bd41b):
  `0x538170(0x733eebe3, -1, 0, 3)` at `+0x160`, orientation = the current `+0x150` about blimp `+0x230`, velocity `0x4dca40() + dir*1.3 -
  blimp+0x230*0.35`; then **`+0x186 = 1`** (`c6 86 86 01 00 00 01` at 0x5bc768, the only write of 1); when the clip has ended `+0x1a4 = 0`;
  the next call's state 0 sets `+0x186 = 0` (0x5bd2f7) and restarts "RevengeReady"; (5) `+0x130` rebuilt from `+0x150` (0x5bd89d /
  0x5bd908): a throw uses the `+0x130` built in the PREVIOUS call; (6) the attack: if out+9 (`80 7c 24 2d 00` at 0x5bd7cd), `+0x1a4 == 0`
  and `+0x1a0` set: play "Attack" at speed 2.0 if `+0x186 == 0` (0x5bd954), then `+0x1a4 = 3` either way (0x5bd98f). `0x5bc220` has no refs.
- **Proofs (a wrapper on slot 23):** attack: `+0x1a4` 0 or 1 before, 3 after with `+0x186 == 0`; throw: `+0x186` 0 -> 1 during the call.
- **Payload:** the aim at the attack is not what the bomb uses (out0 keeps turning `+0x150` until the throw): send `+0x150` after every
  call (or when it changes) plus Attack and Throw messages; the bomb needs A_prev (after the call before the throw) and A_throw.
- **Force (copy):** the slot-7 hook outputs out0 = 0 (and out+9 = 1 while an attack is due); before each stock call write `+0x150` = the
  owner's value for that shown tick (then `+0x130` and the orientation match); hold the throw: in states 2/3/4 while the owner's Throw
  isn't due, write `+0x186 = 1` before the call (read only at 0x5bd38d and 0x5bd954); at the due call write `+0x186 = 0`. If the owner
  never throws, the clip ends and state 0 clears the hold: no phantom bomb. Writing the aim only before the attack tick is not enough.
- Slot 23 is `thiscall(obj, blimp)`, `ret 4`: the same shape as slot 12's `(obj, float* dt)`, but the argument is the blimp.

## Smallest safe hooks
1. A slot-7 data-slot hook per class (the table). Call the original first (the random draws stay as stock); for remote cars zero the
   output and then output only the mirrored press or level: Amy and Ulala out+9; Beat out+8 plus the steer; Bonanza out+9 only, out0 = 0.
2. Tick wrappers on `0x6b7840`, `0x6b7d90`, `0x6b7ae8` (slot 12) and on `0x6b797c` (Bonanza slot 23). The copy's force before the stock
   call, the owner's proof right after it.
3. Their own install group; refuse any slot value that isn't stock.

Files: `scripts\` (run.sh, Decomp.java, CallTree.java, slots.py, fields.py, dwscan.py, msgscan.py, pushscan.py, l1-l3.txt), `out\`
(d1-d3.txt, push2325.txt, chooser_callers.txt), `ghidra\` (don't commit).
