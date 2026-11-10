# Final lap fever: more ambient course life (2026-11-09)

Reverse-engineer pass, static only, on `re\bin\SSASR-unpacked.exe` (its own Ghidra copy `re\m10-ambient\ghidra\`, don't commit), capstone, and read-only parses of Tracks.xpac scene members. Saved from its report by the main session. C = confirmed in bytes/decompile, I = inferred, U = unknown. Nothing was tested in game.

**Correction to `re\m10-lap-fever\FINDINGS.md` §3 (C):** `0x625f50` / `0x6263a0` are node-matrix updates for a model instance, not emitter updates. The emitter layout there is right (+0x50 array, count `[[obj+0x34]+0x38]`, +0x54 active).

## 1. Per-emitter fields and how to reach every ambient emitter
- **Emitter instance** (0xF0 bytes; constructor `0x618050`, update `0x619ca0`). Five run-time multipliers, all 1.0f after construction (C): +0x0C opacity, +0x10 size, **+0x14 spawn rate**, +0x18 lifespan, +0x1C speed (the FXDATA names Opacity / Scale / EmitterRate / Lifespan / Speed).
- **Rate read (C):** `0x619d1d` `f3 0f 10 5f 14`: rate = (def+0xE4 ? `[[inst+0x58]+chan*4]` : def+0xE8) * [E+0x14]; accumulator +0x3C += rate*dt; floor() particles spawn per frame. Other fields: +0x30 def, +0x44 owner, +0x34 alive count, +0x54 emitting.
- **The engine already writes them at run time (C):** the effect tick `0x5492a0` sets all five every tick on its own effects (rate store `0x54937e`), clamping opacity 0..1 and **rate 0..3.0**.
- **Track ambience (C):** track.Forest trees are instanced by `0x4e3b70` into the static object **`0xD12630`**: +0x04 instance count, +0x08 instance array (stride 0x78), +0x0C controller count, +0x10 controller array (stride 0x34; +0x24 = instance). Instance +0x34 tree def (def+0x38 emitter count, def+0x3C emitter defs), **+0x50 emitter-instance pointer array** (null when 0, `0x6273b0`). Teardown `0x4e3f40` zeroes +0x08 / +0x10, so count 0 or pointer 0 = no track. These emitters run from `0x4e44a0` (skipped while paused), not from the global list `0xDEFD34` (walker `0x62e480`: cars, items, effects; its multipliers are rewritten every tick: don't use it). No other writer of track-instance multipliers found (I).

## 2. MODEL_DETAIL
No effect on particles (C): stored at `[0x7f9ff0]+0x7C` (`0x403a71`), read only by `0x4031b0` (from `0x55b993` / `0x55bc6d`) to choose `%s_4p` low-detail assets at load. No settings.ini value scales particles.

## 3. Butterflies
- **Animated mesh, not an emitter (C):** Whale Lagoon `c67b2f09` tree 28 `Butterfly_Anim_500` (0xac56d1c6): 242 nodes, 0 emitters, 60 keyframed `BF_MoveN|bobN` groups. More of them would need new draw registration (I).
- **Cheap option (I): faster flapping / flight:** anim controller +0x0C = per-tick frame step (written only at play `0x630c50`: dt*speed*60; advanced by `0x630b10`: +4 += +0xC, C). Scale only instances whose node[0] name starts "Butterfly_Anim" (other track animations can be hazards).
- `leafFallMat` is in item.Forest (breakable trackside items): an impact effect, not ambience (I).

## 4. RNG and crash safety
- **No shared race RNG (C):** particles use a private LCG at `0x6f802c` (41 references, all particle code); no path from `0x62e480` / `0x619ca0` reaches `0x4be040` / `0x4be170` / CRT rand `0x45f3e0` (`scripts\reach.py`, depth 6).
- **No overrun (C):** per-emitter cap = (int)(def+0x18 * 3.0) or 100 (`0x61a08f`); vertex buffer sized cap*6 (`0x6153e0`); spawns also clamped to the free count `[0xdeaa64]` of a global 9000-particle ring (`0x403d9f`). Zero lifespan / negative accumulator guarded. Values are read fresh each update, so mid-emission changes are safe.
- **Limits:** opacity > 1.0 overflows the alpha byte (C): leave +0x0C at 1. The 9000 pool is shared with car effects; a big multiplier on burst-heavy courses (Casino Park: 90 firework emitters) could starve boost / drift particles on that PC (I, visual only).

## 5. Smallest safe recipe (existing per-tick race hook, game thread)
```
// m = 1 + 2*hype (max 3.0 = the engine's own clamp)
u32 n = *(u32*)0xD12634; u8* arr = *(u8**)0xD12638;
if (n && arr) for (i < n) { inst = arr + i*0x78; def = *(inst+0x34); if (!def) continue;
  ne = *(u32*)(def+0x38); em = *(u8***)(inst+0x50); if (!ne || !em) continue;
  for (j < ne) if (em[j]) { *(float*)(em[j]+0x14) = m;              // spawn rate
                            *(float*)(em[j]+0x18) = 1 + 0.5f*hype;  // optional: longer life
                            *(float*)(em[j]+0x10) = 1 + 0.25f*hype; } }  // optional: bigger; leave +0x0C, +0x1C at 1
```
Write only when m changes; write 1.0 back when hype returns to 0 or the race ends. Pool guard (I): if `*(int*)0xDEAA64 < ~1500`, hold m. What changes (C, data): Whale Lagoon's 25 sea-sparkle glints up to 3x as frequent, denser whale / wave splashes; other courses' braziers, steam, torch sparks, waterfall spray, snow, cannon smoke, fireworks (`out\allemit.txt`). **28 of 56 track members have no track.Forest emitters: nothing changes there.**

## Confidence
High (C): the multiplier fields, the clamps, RNG isolation, the 0xD12630 layout and teardown, MODEL_DETAIL. Medium (I): that every ambient emitter is reached this way, how visible it is, pool starvation, the butterfly speed-up. Unknown: def+0x18's exact meaning, the in-game look.

## Files
`scripts\`: run.sh, Decomp.java, a1-a23.txt, reach.py, mulwrite.py, member.py, emitdefs.py, allemit.py. `out\` (game-derived, don't commit): a*.txt, allemit.txt, op-7f9ff0.txt, mulwrite.txt.
