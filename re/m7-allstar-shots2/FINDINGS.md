# Six more All-Star classes: gates, proofs, forces, payloads (static pass, 2026-10-23; reverse-engineer subagent, saved by the main session)

Basis: every claim checked against the bytes of `re\bin\SSASR-unpacked.exe` (SHA-256 3c0c9f01…9973) with `tools\disasm.py` and
Ghidra decompiles (project copy `ghidra\`, 45 MB, don't commit). **C** = confirmed in the bytes; **I** = inferred. Slots 6, 7, 11,
12 and 15 of all six vtables are identical in the unpacked exe and the on-disk `SSASR.exe`. Builds on `re\m7-allstar-shots\FINDINGS.md`.

Slot-7 (input) / slot-12 (phase-4 tick) data addresses (vtable = slot 7 - 0x1c):
- Eggman `0x6bb52c` / `0x6bb540` (vtable 0x6bb510; stock 0x5eacc0 / 0x5eb050)
- Shadow `0x6b7be4` / `0x6b7bf8` (0x6b7bc8; 0x5dcf80 / 0x5dd7f0)
- Alex Kidd `0x6b771c` / `0x6b7730` (0x6b7700; 0x5d5af0 / 0x5d5e20)
- Zobio `0x6b7e04` / `0x6b7e18` (0x6b7de8; 0x5e1390 / 0x5e14c0)
- Ryo `0x6b7a4c` / `0x6b7a60` (0x6b7a30, classes 0x14/0x15 share it; 0x5da420 / 0x5da570)
- Knuckles `0x6b7b5c` / `0x6b7b70` (0x6b7b40; 0x5dba40 / 0x5dc160)

**Proofs are exact when the owner reads them right around the stock slot-12 call** (a wrapper on that slot), because slot 11 at
the All-Star start also writes some of these fields. Slot 12 is `thiscall(obj, float* dt)`, ret 4; the racer is `[obj+0x10]`.

## 0. How the owner reads its own input (C)
- `ctrl = [racer+0x9f0]`; the chooser `0x5a0080` takes slot 6 (human) when that pointer is non-null and `0x4e8f90` passes
  (`[ctrl+0x18]==1`). CS is the embedded object `ctrl + 0x1c` (NOT `[ctrl+0x1c]`, which is the device index): slot 6 at
  `0x5a0035..0x5a003f` does `mov esi,[eax+0x9f0]; add esi,0x1c`.
- Slot 6 returns: out+0 = float `[ctrl+0x5c]` (steer), out+4 = float `[ctrl+0x58]` (vertical), out+8 = (`[ctrl+0x24]` is 2 or 3),
  out+9 = (`[ctrl+0x24]==2`). The floats are copied exactly.
- Same-frame order: the scene update `0x49dca0` first runs `0x4eff50` for every racer (pre-pass `0x49dd47..0x49dd7a`: controller
  slot 2 `0x5097a0` -> the state machine at `ctrl+0x200` -> Human `0x5af150` -> the CS setters `0x540590` steer / `0x540580`
  vertical), and only then the update loop `0x49de01..0x49de4d` calls racer slot 4 `0x4f3990` -> the All-Star tick `0x586980` at
  `0x4f3bcb`. No setter is reached after the tick by direct calls (3 indirect calls in `0x4e6570` on racer+0x1eb4 objects: I).
  **So reading `ctrl+0x5c` / `ctrl+0x58` after the tick gives what slot 6 returned in that tick.**

## 1. Eggman (tick `0x5eb050`, slot 7 = MT%40) — all C
- Gate: chooser at `0x5eb07b`; `+0x134 += dt` (`0x5eb087`, stored `0x5eb096`) BEFORE the test; `comiss` against `+0x138`
  (`0x5eb08f`): needs `+0x134 >= +0x138`. Inside the open gate anim 8 becomes 7 (`0x5eb0a2..0x5eb0b7`). **out+9** (any nonzero) is
  read at `0x5eb0bc`; out+8 unused. The anim must be below 6 (unsigned, `0x5eb0ca`); slot 25 `0x5ea6c0` turns a finished anim 6
  into 8 and finished 3/4/5/7 into 2 (6/7/8 = one sequence, probably a hit reaction: I).
- Fire: `+0x130 -= 1` (`0x5eb0cf`), negative -> 7 (`0x5eb0d8`); rocket `0x5ead00` (`0x5eb0e6`); `+0x138 = +0x148` (0.75, stored by
  the ctor at `0x5ea76a`, nothing else writes it) (`0x5eb0eb`/`0x5eb0f4`); **`+0x134 = 0.0f`** at `0x5eb0fa`.
- `+0x134` right after a fire tick is exactly +0.0f (dt was added before the reset); after any other tick it is prev + dt > 0.
  Slot 11 `0x5ea8e0` writes `+0x130 = 0`, `+0x134 = 0` (`0x5eaa22`, `0x5eaa2c`), `+0x138 = 0.75` (`0x5eaa19`); slot 13 `0x5eab20`
  writes `+0x130 = 0`, `+0x134 = 0`.
- `+0x130` is a rocket pattern index 0..7 (7, 6, ... 0, 7 from 0), read by `0x5ead00` AFTER the decrement: bit 0 picks anim 4 +
  launch locator 3 or anim 5 + locator 4 (`0x5ead44..0x5ead7f`); rocket +0x390 = A[i]*4.712389, +0x394 = B[i]*2 + 2.5
  (`0x5eaeb8..0x5eaee4`; A = {-3, -1.72, 1, 2.647, -2.98, 2.1, 2.71, -3.765}, B = {1.5, 2, 2.5, 3, 2.26, 1.756, 1.134, 2.78}); +0x398
  = 0.83 (locator 3) or 0.17 (locator 4). `0x5ead00` makes no random draw; the rest comes from the car's pose and vehicle `+0x990`.
- Proof: `+0x134 == 0.0f` after the stock call (end of tick: previous > 0 and now 0). Payload: P = the owner's post-tick `+0x130`.
- Force (copy, before the tick): `+0x134 = +0x138` (or larger); **`+0x130 = P + 1`** (the copy decrements before spawning; 1..8
  never wraps); out+9 = 1. A copy whose anim is 6 or higher refuses the shot: keep it pending, force again next tick. The rocket
  parameters then match (C); the flight follows the copy's own pose (I).

## 2. Shadow (tick `0x5dd7f0`, shot `0x5dd1b0`, slot 7 = MT%30)
- Tick (C): 16 per-car contact cooldowns `+0x130..+0x16c` count down (the Chaos contact hit `0x5dcd70`, 2.0 s per victim, local).
  Chooser at `0x5dd95a` (out at `[esp+0x20]`); `0x5dd090(out+0, dt)` at `0x5dd96a` is steering only; `0x5dd1b0(byte out+9, out+0,
  out+4, dt)` at `0x5dd989`.
- Gate (C): `+0x174 -= dt` (`0x5dd1d4`) every tick before the press test, no lower bound. Fire only if **out+9 == 1 exactly**
  (`cmp byte [ebp+8],1` at `0x5dd1bc`) and the new value <= 0 (`0x5dd1e5`); then `+0x174 = [0x6f6bc0] = 0.5` (`0x5dd1f9`). One
  shot per 0.5 s of summed phase-4 dt. Slot 11 `0x5dc7e0` sets `+0x174 = 0` (`0x5dca75`).
- Proof: `+0x174 == 0.5f` after the tick (only the fire writes it). Force: `+0x174 = 0`, out+9 = 1, out+0/out+4 = the owner's
  values (§0). Payload: the owner's `[ctrl+0x5c]`, `[ctrl+0x58]` after the tick.
- Aim formula (C unless marked): s = vehicle `+0x990` * `[0xc285c0]` (projectile +0x370); P = `0x4c0fc0(0xd0d38b75)` (missing: e =
  2.45); r1 = `0x4be180` at `0x5dd277`; x = (50*out4 + 20*(r1 - 0.5) + 150) * P.f110 * dt / s.x^2 clamped +-0.99999; e =
  sin(0.5 * `0x64c430`(x)) (CRT, probably asin: I); a track sample 100 units ahead `0x4fb9c0` at `0x5dd339` (ECX `ctrl+0x80`); r2 at
  `0x5dd38f`; lat = clamp(0.5*out0 + 0.25*(r2 - 0.5), +-0.9); target = `0x573200`(curve, 0.5*lat + 0.5) (cross-track point: I); the
  direction mixes normalize(target - car) with e * the up vector `0x572e20`. Spawn `0x538170(0xd0d38b75,-1,3,1)` then `0x5391a0` at
  `0x5dd64d` from the RightHand bone plus velocity*dt; +0xe0 = 3.0; anim 5 (direction dot `racer+0x220` > 0) or 6; `0x5a01d0(2.0)`.
- The two random draws (C): one global MT19937, state `0xce7c58` (624 dwords), index `[0x6ecb80]`, tempering `0x4be120..0x4be154`,
  float = u32 * 2^-32 (the ECX `0xc28cd1` is ignored). r1 and r2 are consecutive draws; nothing between them draws. Unsynced, the
  spread is +-10 on the 150 range term and +-0.0625 of the width on the target. Exact option (I, design): the owner peeks U1 =
  temper(state[idx]), U2 = temper(state[idx+1]) after its chooser call (idx <= 622) and sends them; the copy's slot-7 hook writes
  untemper(U1), untemper(U2) into state[idx], state[idx+1] and restores the originals after the stock slot 12 returns.

## 3. Alex Kidd (tick `0x5d5e20`, shot `0x5d5710`, slot 7 = MT%40) — C
- Order in the tick: (1) chooser `0x5d5e4b`; (2) `0x5d5b80(out+0, dt)` at `0x5d5e5e` BEFORE the gate: if `[veh+0x980] != 0`: out0 != 0
  -> `+0x984 = clamp(+0x984 + 0.9*dt*out0, +-0.9)`; out0 == 0 -> `+0x984 *= (1 - 0.9*dt)` (`[0x6f6b50]` = 0.9); (3) C = (1 - I)*1.5 +
  I*0.5 with I = `+0x134` at the tick start (`0x5d5e63..0x5d5e91`); (4) `+0x130 -= dt` (`0x5d5e9f`/`0x5d5ea6`); above 0: no shot;
  (5) else if out+9 != 0 (`0x5d5eb0`): fire `0x5d5eb9`, then **`+0x130 = C`** in [0.5, 1.5] (`0x5d5ecc`); (6) mash meter `+0x138`:
  +0.33 per pressed tick, cap 1.0 (`0x5d5edb`), else *0.95; (7) `+0x134 = (1 - 2dt)*+0x134 + 2dt*+0x138` (the arm speed `0x5a01d0`).
- Proof: `+0x130` after > before (exact, = C); end of tick: now > 0 and now > previous. Slot 11 `0x5d58a0` writes 0 (`0x5d59e8`).
- Aim: `0x5d5710` reads the vehicle `+0x984` AFTER this tick's integration (`0x5d5776`): `+0x394 = 0.5*v + 0.5`; `+0x398 =
  [ctrl+0xb8]` (own lateral track position); `+0x370 = +0x380 = racer+0xa60 + [0xc28570]`; `+0x390 = 1`. Slot 12 integrates out+0,
  it doesn't copy it.
- Payload: V = the owner's vehicle `+0x984` after the tick (the value the shot used). Force: `+0x130 = 0`, out+9 = 1; in the slot-7
  hook write vehicle `+0x984 = V / (1 - 0.9*dt)` and out+0 = 0, dt = `[0xc2a030]` (what `0x5867a0` passes); if `[veh+0x980] == 0`
  write V as is. Optional visual: `+0x134` for the arm speed.

## 4. Zobio and Ryo — no field written only at the fire: the anim edge (C)
The anim is read by `0x59fe90` as bits 10-13 of the word at `[[racer+0x2044]+0xcc]+0xe`; the request `0x59fe70` -> `0x4e8760` ->
`0x502d00` writes those bits at once (visible right after the stock call).
- **Zobio** (tick `0x5e14c0`, slot 7 = MT%30). Gate (`0x5e1562..0x5e15a0`): `+0x138 -= dt` every tick before the press test; if out+9
  != 0, anim == 2 and `+0x138 <= 0`: `0x59fe80` and anim 6 (`0x5e15a0`). A branch before can turn anim 3/4 into 5 or set 4 while
  airborne (`0x4e8ff0` -> `0x54e800`; "airborne": I); either blocks the fire. The zombie comes later: slot 25 `0x5e1210` (data
  `0x6b7e4c`), when anim 6 finishes: anim 7 and the spawn `0x5e1010` at `0x5e12b1` = `0x4e9be0(0x13c5c35c, racer+0x220, racer, 5.0)`
  40 units ahead along the path (`0x4fbc70`) at lateral `[ctrl+0xb8]`, no direct random draw. When anim 7 finishes: anim 2 and
  `+0x138 = 0.5` (`0x5e12d0`). Slot 11: `+0x138 = 0` (`0x5e0f15`). Proof: anim 2 before -> 6 after the stock call (`0x5e15a0` is the
  class's only request for 6; the others: `0x5e1254`(2), `0x5e1278`(3), `0x5e1291`(2), `0x5e12a3`(7), `0x5e12c3`(2), `0x5e155d`(4/5);
  nothing outside the class requesting 6: I). Force: `+0x138 = 0`, out+9 = 1; the copy's anim must be 2 (retry while due). Payload:
  none (the zombie appears when the copy's own anim 6 ends).
- **Ryo** (tick `0x5da570`, slot 7 = MT%50). Gate: if out+9 != 0 (`0x5da59f`) and anim != 3 (`0x5da5ad`): `+0x131 = 0` (`0x5da5b6`),
  anim 3 (`0x5da5bd`), `0x59fe80`, `[racer+0x1e94]` slot 3 (0). No timer, no projectile: the attack is the anim-3 lift window;
  contacts during it go through slot 5 `0x5da260` (message kinds 0x1d/0x30 send event 0x28; in anim 3 also `0x4f1fe0(1)`, the
  `0x4eee30` launch, `+0x131 = 1`; else `0x4f1f90(1)`): local hits. When anim 3 ends (slot 25 `0x5d9e00`, case 3): anim 4 if `+0x131`,
  else 2; `+0x131 = 0`. The byte `+0x130` is the ctor variant (clips only). Proof: anim != 3 before -> 3 after (`0x5da5bd` is the
  class's only request for 3). Force: nothing to write; out+9 = 1; the copy's anim must not be 3. Payload: none.

## 5. Knuckles (tick `0x5dc160`) — C
- Slot 7 `0x5dba40`: out+8 = 1 unless `+0x140 > +0x13c` (`0x5dba52..0x5dba67`); never out+9; no random draw. Slot 11 `0x5dbab0`:
  `+0x13c = 3.0` (`[0x6f6bac]`); `+0x130`, `+0x134`, `+0x138`, `+0x140` = 0.
- Tick: held = **out+8** (`mov bl,[esp+0x24]` at `0x5dc19b`). Only for computer cars (`0x4e8fb0`, `[ctrl+0x18]==2`): `+0x140 += dt`,
  reset to 0 when not held (a Network21 copy never counts: the stock copy holds forever). Near the All-Star end
  (`0x5dc1cd..0x5dc22f`) held is forced to 0. If `+0x134 > 0`: only the cooldown counts down (`0x5dc28d..0x5dc29e`). Else if held:
  `+0x138 += dt` (`0x5dc255`), then the levels in `0x5dbe90`: 0->1 as soon as the charge > 0 (anim 3 wind-up, `0x5dbefc`/`0x5dbf01`),
  1->2 at >= 1.25 (`0x5dbed6`), 2->3 at >= 3.0 (`0x5dbeb9`). Else if released with charge > 0: below 0.25 the charge keeps growing
  (`0x5dc276`); at >= 0.25 the punch `0x5dbd50` fires (`0x5dc286`).
- Punch `0x5dbd50`: every other car with place <= the puncher's (`0x4cbef0(r)+0x80`, ahead of it) gets `0x5db890(r, +0x130)` (the
  level read at `0x5dbd96`); then anim 6 (level 1) or 5 (levels 2/3), `+0x130 = 0` (`0x5dbe68`), `+0x138 = 0` (`0x5dbe72`),
  **`+0x134 = 0.5`** (`0x5dbe82`).
- Proof: `+0x134 == 0.5f` after the tick (nothing else writes 0.5). Payload: L = `+0x130` read BEFORE the stock tick (1..3; the
  release path never changes it). Force at the punch tick: `+0x134 = 0`, `+0x138 = 0.25f`, `+0x130 = L`, out+8 = 0. On every other
  tick of a copy: out+8 = 0 and, unless mirroring a hold, `+0x138 = 0` before the tick (no forced-release phantom punch at the end).
  Optional visual: the owner sends a HoldStart (its `+0x130` 0->1 edge = anim 3); the copy outputs out+8 = 1 until the punch is due.
- Charging again: yes, after each punch a 0.5 s cooldown, then a new hold: any number of punches per All-Star (mirror every hold
  and release). A tap shorter than 0.25 s still punches 0.25 s after it.

## Smallest safe hooks
1. One data-slot wrapper per class over slot 12: the copy's pre-writes before the stock call, the owner's proof right after it.
2. The slot-7 hook outputs the fire byte only while a shot is due: out+9 = 1 (exactly 1 for Shadow), or out+8 for Knuckles' hold.
3. Their own group; refuse any non-stock slot value.
4. Shadow's exact aim also needs the generator-word plant (§2); without it a small spread stays.

## Confidence and files
High (C): every gate, proof value and force; Eggman's index tables; Shadow's formula and the generator layout; the controller read
timing. Inferred (I): anim 6-8 as a hit sequence; asin and the cross-track point in Shadow's aim; nothing outside the classes
requesting anim 6 (Zobio) / 3 (Ryo); the indirect calls in `0x4e6570`; random draws inside `0x5391a0` being sound/voice picks.
Nothing was tested in game. Files: `scripts\` (run.sh, run2.sh, Decomp.java, CallTree.java, slots.py, ctrlcall.py, the input lists
l1..l11.txt, t1.txt, t2.txt), `out\` (decompiles d1..d11.txt, call trees t1.txt, t2.txt), `ghidra\` (don't commit).
