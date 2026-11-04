# Fire events from another player's game (2026-10-30; deep-analyst pass, static: re\bin\SSASR-unpacked.exe)

For notes\SAFETY.md F6. Nothing was run in game; "always finite" for honest events is INFERRED from the code.

## How every fire event is built (CONFIRMED)
One producer, 0x538bf0 (kinds 1 and 2 posted at 0x5390dd): a local 0x80-byte launch frame gets defaults (0x5351e0), a projectile
type fills it (0x538170 -> 0x536860), kind 2 replaces the position with a raycast hit (0x538f61), d045a8ca copies its target
object's +0xc0..+0xdf into frame +0x20..+0x3f (0x539036); then the new event gets +8 / +0xc / +0x12 / +0x13, its own frame
reset (0x538fca), the local frame copied in whole (0x5b7190, 0x20 dwords) and packed (0x5b72e0: writes only +0xa0..+0xdf).
0x516760's heap doesn't zero: unwritten bytes are leftovers.

## Layout of event +0x20..+0xdf (body = event - 8)
| Event | Content |
|---|---|
| +0x20 | u32 projectile type hash (0 = none) |
| +0x24 | u16 projectile serial; +0x26..+0x27 unset |
| +0x28 | i32 target racer (-1 none; bounds-checked unsigned at 0x538345) |
| +0x2c..+0x2f | four 0/1 flag bytes (aimed, 0x5365d0, direction valid, for 0x527200) |
| +0x30 | u8 from racer+0xc70; +0x31..+0x3f unset |
| +0x40 / +0x50 / +0x60 / +0x70 | float4 position (default 0) / rotation quaternion (default 0,0,0,1) / direction / velocity |
| +0x80 | float4: 1.3 x max(\|racer velocity\|, 0.2), scaled per projectile type |
| +0x90 | float4: one value in 4 lanes, 0.2778 x (type +0x194, or 550) x sim dt |
| +0xa0..+0xdf | packed fields (24-bit direction, 29-bit quaternion, velocity, position) and quantizer vtables 0x6b9a88 / 0x6b9aa4; a field whose flag bit is off for the gadget is unset: never check these as floats |

**The 24 dwords at +0x40..+0x9f are written for every fired gadget** (defaults or the car's state; the aim normalization is
masked at 0x538e83; the one unmasked one, 0x536ac4 for types 831875d1 / a0a0c5c8, can't be zero for a real car).

## Per gadget (flag table 0x5b71b0, bits 0x400..0x8000 read by pack/unpack only)
| Flags | Gadgets | The receiver's use of the launch floats |
|---|---|---|
| 0xffff | 84418b5f, 5f724da9, 7711877d, 11a9bc48, 9a67cb42, 0053f395, eec55d13, 3ebcdded, 805c3c3b, 0f6a6e94, 84917c21, f005c29d | all six vectors rebuilt from packed bits before FireGadget (direction.w +0x6c stays as sent) |
| 0xffff | 94749a0f | never fired (the builder returns at 0x538c3c) |
| 0xc00 | d045a8ca | position and rotation rebuilt; FireGadget (0x53b32f) only moves racer+0x1cf8's object to them |
| 0xc | 953d2512, d8ae59cb | nothing rebuilt: all 24 raw dwords go into the spawned projectile (0x53b09e, 0x53b3e1 -> 0x538310: position -> vtable+0x24 setter; rotation -> +0xd0; direction -> +0x420 (and the turn axis at 0x5384e3); velocity -> 0x4b97d0; +0x80 -> +0x380; +0x90 -> +0x370) |
| 0x200 / 0 | 26e96da5, 53eb2f26, ba8e1dca, b678e022 (All-Star), 734481a5, c01ccf24 | no launch floats read |

## The rebuild (for the open part)
Quantizers: 0x6b9a88 8-bit over [0, 10]; 0x6b9aa4 12-bit over [0, 4000] (0x5bda30). Rebuilt position = unit vector x <= 4000,
velocity <= 10, +0x80 / +0x90 in [0, 10], direction a unit vector. The 24-bit vector decode 0x5be330 clamps before its square
roots (never NaN). The quaternion decode 0x5c00b0 ends with an unclamped `fsqrt(1 - a^2 - b^2 - c^2)` (0x5c0119): honest bits
leave >= 0.25 under the root, crafted bits can give NaN (INFERRED; 0x5bdb90's component range not traced). The 3-lane decodes
leave the 4th lanes of position / velocity as leftovers (d045a8ca's position.w is uninitialized even in honest games): never
check 4th lanes after the rebuild.

A check for the 13 gadgets with flag 0x800: a scratch event from 0x516760(kind), the body copied as RunEvent does, 0x5b7530 on
it (thiscall, no args; writes only that event), then event +0x50..+0x5c finite with norm^2 in [0.81, 1.21], freed with
0x516900. Never unpack the real event twice (0x5b7530 shifts and byte-swaps +0xdc in place).
