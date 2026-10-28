# Opa-Opa's All-Star weapon on copies: phases, slots, the safe sync (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`; own Ghidra copy `ghidra\`, 45 MB, don't commit); saved from
its report by the main session (session "Night Shift"). C = confirmed in bytes/decompile, I = inferred. Every slot named below
holds the same value in the on-disk `re\bin\SSASR.exe` (checked): `0x6bb3f4 = e0 a2 5e 00`, `0x6bb3fc = 60 9a 5e 00`,
`0x6bb400 = 70 93 5e 00`, `0x6bb404 = 10 9b 5e 00`, `0x6bb408 = 80 a5 5e 00`, `0x6bb3ec = 20 a3 5e 00`, `0x6afbc0 = 90 39 4f 00`,
`0x6afba8 = 20 e0 4e 00`. None is near the Roster Pack's 12 fingerprinted ranges. Background: `re\m7-item-lines\FINDINGS.md` §1, §3.

## Short answer
Opa-Opa's weapon menu runs in **phase 3**, not phase 2. Phase 2 lasts at most one All-Star tick. Phase 3 ends only when the
transform animation's end event fires. Slot 9's return value is ignored, so skipping it cannot hold anything. The weapon
`[obj+0x184]` is final only at the 3->4 change: only the phase-4 message can carry it. On a copy the random draw has no visible
effect until slots 10 and 11 run, back to back, at the 3->4 change; slot 10's only output is `+0x120`, HUD-only for local
players. So writing `+0x184` just before slot 11 is enough. The weapon can arrive after the copy's slot 11: then write 4 ("none",
fires nothing) at slot 11 and apply the real weapon late (see §2).

## 1. The phase machine
**Dispatcher 0x586980** (C): `thiscall(S = racer+0x1ff0)`, called only from racer slot 4 at `0x4f3bcb`
(`8d 8e f0 1f 00 00 e8 b0 2d 09 00`), inside the `[racer+0xa1c]==2` block; one call per racer update.
Phase 1 -> `0x585b60`, 2 -> `0x585bb0`, 3 -> `0x585c00`, 4 -> `0x5867a0`, 5 -> `0x585c70`.

**Opa vtable 0x6bb3d8, slots used** (C): 3 (+0xc) `0x4d1ec0` always false (`32 c0 c3`); 5 (+0x14) `0x5ea320` messages;
7 (+0x1c) `0x5ea2e0` AI input; 8 (+0x20) `0x5e8e10` animation begin; 9 (+0x24) `0x5e9a60` phase-3 tick; 10 (+0x28) `0x5e9370`
animation end; 11 (+0x2c) `0x5e9b10` phase-4 start; 12 (+0x30) `0x5ea580` phase-4 tick; 13 (+0x34) `0x5e9410` end;
28 (+0x70) `0x5e9a40` scale.

**Phase 2 end** (C, humans and non-humans alike). `0x585bb0` calls obj slot 3 (always 0 for Opa) and tests `S+0x20 == [0xc284a0]`
(the zero vector; `0x585b10` zeroes `S+0x20` on entering phase 2), so at the first phase-2 tick it jumps to `0x4e8820`
(`add ecx,0x1ff0; jmp 0x586160`). `0x586160` clears `S+0x4b`; if vehicle `+0x1770 > 0` it sets `S=3` (`c7 01 03 00 00 00`) and
calls `0x4e8760(racer,0)` (= `ecx=[racer+0x2044]`, `0x502d00`: transform animation selector 0); otherwise straight to `0x585de0`
(phase 4, skipping phase 3 and slot 10). The fire happens in `0x5398c0` (called at `0x4f3b3b`) earlier in the same slot-4 call:
the owner's phase goes 1->3 inside one update and is never observed as 2 (C for the order; I for what the mod samples). A copy
whose kind-1 event is injected before the update behaves the same.

**Phase 3 tick `0x585c00`** (C): calls slot 9 every tick (`8b 11 8b 52 24 8d 44 24 10 50 ff d2` at `0x585c3b..45`:
`thiscall(obj, &dt4)`), then adds dt to `S+0x20` and `S+0x10`; never changes the phase.

**Phase 3 end** (C): only through the transform animation's events (`0x503170` -> racer callback `0x4f0dc0`, `thiscall(racer, ev)`
`ret 4`, when `ev[1]==0x12`), never through slot 9 or `+0x120`:
- flag bit 2 -> `0x5869d0(ev[3]=0, 1)` at `0x4f0deb` -> `0x585d50` -> slot 8 at `0x585d5f`: `+0x184=4`
  (`c7 87 84 01 00 00 04 00 00 00` at `0x5e8e3c`) plus the human's menu setup;
- flag bit 4 -> `0x5869d0(0, 0)` at `0x4f0e18`; needs `[S]==3` (`83 3e 03` at `0x586a0c`) and runs `0x5868d0`: slot 10 at
  `0x586927..33` (`8b 4e 40 85 c9 74 07 8b 01 8b 50 28 ff d2`, no args); then, if `S+0x4b==0`, `0x585de0`, which calls slot 11 at
  `0x585fc0..cc` (`8b 4e 40 85 c9 74 07 8b 11 8b 42 2c ff d0`), then `cmp [obj+0x120],0 / jbe` at `0x585fd1`, `0x535b70(racer)`,
  then `S=4` at `0x585fe2` (`c7 06 04 00 00 00`). With `S+0x4b` set it calls `0x586240` (end) instead.
- Phase 3 can be aborted to phase 1 by `0x586050` (respot/reset callers): slots 10 and 11 never run then.
- The phase-3 length is the animation clip's length (data), not a code constant (I).

**Slot 9 `0x5e9a60`** (C): `thiscall(obj, const float4* dt)`, `ret 4` at `0x5e9ae1` and `0x5e9b0a`; its only caller ignores EAX
(`0x585c47` goes straight to `movaps`); no other virtual call through +0x24/+0x28/+0x2c on the All-Star object. Non-human branch:
`if [obj+0x184]==4: [obj+0x184] = MT % 3` (`0x5e9ae4`, `0x5e9aed..0x5e9b00`): the draw happens once, at the first phase-3 tick
after the animation-begin event. The generator `0x4be170` is a global MT19937 with ~100 callers, not shared between PCs.

## 2. Copy behaviour and the safe sync
- **Skipping slot 9 does not hold the copy** (C): phase 3 still ends on its local animation end. With `+0x184` still 4: slot 10
  leaves `+0x120` unchanged (`0x5e93e2`: only 0 writes 2 at `0x5e9403`, 1/2 write 4 at `0x5e93f7`); slot 11's switch skips both
  the line and `0x5e9580` (`0x5e9e14..0x5e9e1e`; 4 jumps to `0x5e9e50`); the phase-4 tick `0x5e9ee0` reads `[obj+0x188+4*0xc] =
  +0x1b8` (an effect pointer read as a float, in bounds: the object is 0x1d0 bytes, `0x404460(0x1d0)` for case 0x12) and its
  switch returns without firing (`0x5e9f52..0x5e9f8f`); slot 28 returns `fld1`; slot 5 sends `0x28` only for weapon 1. A harmless
  "blank" All-Star.
- **Readers of +0x184 between the draw and slot 11** (C): only slot 10 (`+0x120`). Slot 28 (`0x4e0635`) and slot 5 are gated on
  phase 4 (`0x4e0621`, `0x5ea33b`).
- **`+0x120` is HUD-only and does nothing for a copy** (C): `0x535b70` -> racer `+0xc30` -> `0x535a10` picks item icons, and
  `0x535770` posts `HUD_GADGET_ON` only if `0x4a0c40(racer) < [0xc2a044]`; `0x535be0` and `0x4ff060` need `index < [0xc2a140]`;
  `0x4a0c40` returns -1 for any non-local car.
- **Writing `+0x184` at slot 11 (`0x6bb404`) is enough** (C): it is also the only point both paths into phase 4 reach
  (`0x586160` can skip phase 3).
- **No way to hold through data slots**: the animation end is a direct `E8` call chain (`0x503170` -> `0x4f0dc0` -> `0x5869d0`) (C).
- **Safe approach.** In the slot-11 hook for a remote car (players' cars, and the host's AI as seen on guests): weapon known ->
  write it (0..2, or 4), then the original; unknown -> write **4**, the original, mark pending. When the weapon arrives while
  `S==4`: write `+0x184 = w`; if `w==1` call `0x5e9580(obj)` (`thiscall`, no args, `ret` at `0x5e96fc`; what slot 11 does for
  weapon 1); on the host say the line `0x4bc970([racer+0x9d8], cue[w])`. From the next tick the copy fires that weapon
  (`+0x160` was set to 100000.0 (`[0x6bb4b4]`) at slot 11, so the first shot comes as soon as the fire flag is set). `+0x120`
  can be skipped.
- **Arrival order (I).** The phase-4 message is due at the copy's shown tick of the owner's 3->4 change, just before the copy's own
  animation end in the ideal case; shown-tick stalls, jitter or a late kind-1 can reverse it, so both orders must work.

## 3. The owner
Order of values in `+0x184` during phase 3 (C): (1) the previous All-Star's weapon: nothing resets it at the end (slot 13 doesn't
touch it, the constructor never sets it); (2) 4, at the animation-begin event (slot 8); (3) a human: 1 (the default) once the
phase-3 time `S+0x20 > [0xd480a0]` = `splat(2.5f)` (static init `0x679350`: `f3 0f 10 05 a4 f2 6a 00 0f c6 c0 00 0f 29 05 a0 80
d4 00`; `[0x6af2a4]` = 2.5), written at `0x5e91ce`; then cursor moves 1->0 / 1->2 / 0->1 / 2->1 in `0x5e91f0` every tick (writes
at `0x5e935c`, `0x5e92e8`, `0x5e932e`); (4) AI and copies: the draw.
Final value (C): at the transform animation end = the 3->4 change (slots 10 and 11 read it while `S==3`; `S=4` is written after
at `0x585fe2`); no writer can run after that (all are phase-3 paths). So it is **not** final at the 1->3 change: only the phase-4
message can carry it. A human whose animation is shorter than 2.5 s keeps 4 and fires nothing: the wire must be able to say
"4 = none" (I).

## 4. Calling conventions (C)
- **Racer slot 4, `0x4f3990`**: `thiscall(racer)`, no stack args; prologue `53 56 8b f1`; exits through a tail jump at `0x4f3c45`
  (`8b ce 5e 5b e9 22 b7 01 00`) to `0x50f370` = `c7 81 e4 01 00 00 00 00 00 00 c3`: plain `ret`. Hook as `void __fastcall(void*,
  void*)`. `0x55aab0` is called at `0x4f3b64` (`8b 8e f4 09 00 00 e8 47 6f 06 00`) with ECX = `[racer+0x9f4]`, unconditionally
  inside the `[racer+0xa1c]==2` block (`8b 86 1c 0a 00 00 83 e8 02 0f 85 10 01 00 00` at `0x4f3abf`).
- **Listener slot 1, `0x4ee020`**: `thiscall(listener = racer+0x2c0, msg*)`, `ret 4` on every exit. Message (builder
  `0x4d2bf0(kind, sender, receiver, p4, delay)`, cdecl): `[0]` kind (0x28), `[1]` delivery time (`[0xd1244c]` + delay), `[2]` = `+8`
  the sender's racer **pointer**, `[3]` receiver, `[4]` p4. Before its switch it forwards `msg` to `[racer+0x9f0]` vt+4, and, if the
  receiver's own phase `[racer+0x1ff0]` is 3, 4 or 5, to `[racer+0x2030]` vt+0x14 (slot 5). Case 0x28 (`0x4ee0bd`): `edi =
  [msg+8]`; if non-null `push [edi+0x9d8]` (the record **index**) -> `call 0x4bcd90` -> `add esp,4`; then `0x4a2610(8, attacker,
  listener-0x2c0, 0)` with ECX = `0xc2a1b8`.
- **`0x4bc970(index, cue)`**: cdecl, plain `ret` (`0x4bc9ca`); returns at once if the record from `0x4cbed0(index)` is finished
  (`0x51a010`) or callouts are not live (`[0xcf7a51]`); if `[0xc29fdb]==0` calls `0x4bbec0(cue, 5000, 0)`; excitement `[0xce7bb0]
  += 1.0` either way; no near test, no random draw, no cooldown variable.
- **`0x4bcd90(index)`**: cdecl, plain `ret` (`0x4bce21`); finished -> return; live -> excitement +1; `0x49cb50(ECX=[0xc29fc0],
  index)` must be non-null; the near test `0x4bbe10(index)` must pass; cue = `0x54d530(ECX=[[record+0x8c]+0x9f4], 12)`; if
  `[0xc29fdb]==0` calls `0x4bbec0(cue, 5000, 0)`; no cooldown variable.
- **`0x4bbec0` gates**: `[0xc29fdb]`; game type not 1 or 5 -> requires the option check `0x4c90d0(0x2c)`; `!0x4c90d0(0x35)`;
  `scene+0x1350 == 0`; then `0x45ffb0` -> `0x46db60` -> `0x46d5e0` (entry by cue) -> `0x46daf0`, which holds the entry's own
  cooldown (`+0x20`/`+0x30`) and a chance check using the C runtime's `rand()` against entry `+0x1d`.

## 5. Writing +0x184 from outside
- All accesses in the class range `0x5e8b00..0x5eb000` (C, scan): writes `5e8e3c`, `5e91ce`, `5e92e8`, `5e932e`, `5e935c`,
  `5e9b00`; reads `5e92b2`, `5e92fb` (human menu), `5e93e2` (slot 10), `5e9a40` (slot 28), `5e9ae4` (slot 9), `5e9dbc` (slot 11),
  `5e9f1a` (phase-4 fire), `5ea38e` (slot 5).
- Shared base methods cannot touch it (I, strong: the smallest All-Star class is 0x130 bytes, case 0x19). Outside the class the
  only virtual reader through `racer+0x2030` is slot 28 at `0x4e0635`, gated on phase 4 (I, heuristic scan).
- **Per-weapon table** (C): entries of 0xc bytes at `+0x188`; the shot interval is the first float (`lea edx,[eax+eax*2]` /
  `comiss xmm0,[esi+edx*4+0x188]` at `0x5e9f28`/`0x5e9f35`). Slot 11 fills it before its switch: weapon 0 `+0x188/18c/190` =
  3.0 (`[0x683cd8]`), 4.0, 0 -> 4 shots of `0x84aa17c2`; weapon 1 `+0x194/198/19c` = 3.4e37 (`[0x6bb3d0]`), 3.4e37, 0 -> the
  grow/ram `0x5e9890` plus contact `0x28`; weapon 2 `+0x1a0/1a4/1a8` = 4.0, 5.0 (`[0x6a4324]`), 0 -> `0x9ba6856b`.
- **Valid values: 0..2, and 4 (none).** 3 or 4 read in-bounds junk and fire nothing; 6 and up read past the object. Clamp to
  {0,1,2,4}. The slots are referenced only by vtable `0x6bb3d8`; call everything from the game thread.

## 6. Other findings that affect the plan
- **Copies still fire at random times** (C). In phase 4 the fire flag for non-humans comes from slot 7 `0x5ea2e0` (data
  `0x6bb3f4`, `thiscall(obj, out*)`, `ret 4`): it zeroes `out` and sets `out+8/+9 = 1` when `MT % 80 == 0` (`0x5ea2f0..0x5ea30e`).
  The choice between slot 6 and slot 7 is made by `0x5a0080` (category 1 -> slot 6, which reads input). The shots are direct
  local spawns (`0x538170` -> `0x5391a0` -> `0x538310`, no event post), so they are not mirrored. Even with the right weapon,
  copies make "phantom" shots; under victim-decides they can really hit the local car (I). Possible next step: hook `0x6bb3f4`
  for remote cars and output no fire, or fire at the owner's shot ticks (the owner's shot is visible when `+0x160` resets to 0 at
  `0x5e9f4a`).
- The animation event dispatcher `0x503170` also draws the MT (case 0xb) (C).

## What this meant for 0.7.0-m7ze as first written (fixed in the main session afterwards)
1. `OpaWeaponChosen` assumed phase 2 is the menu; in phase 3 it would send the previous use's stale weapon, then cursor
   positions. Send the weapon only once `phase==4`.
2. `HookOpaStart`'s unknown path let the copy fire its random draw for the whole All-Star. Write 4 instead, apply the real
   weapon late.
3. `RunAllStarPhase` never wrote the weapon into a copy already in phase 4. Add the late-apply step from §2.
4. `AllStarPhaseBody` could not express 4 ("none"): encode it (w+1 = 5), so copies can tell "not known yet" from "no weapon".

## Confidence and files
High for §1-§5 (bytes plus decompile). Inferred: animation timing and which callback fires on Network21 cars; how the arrival
order compares with shown ticks; that the base class cannot write `+0x184`; the phantom-shot consequence. Nothing run in game.
Files: `scripts\` (run.sh, Decomp.java, l1.txt..l6.txt, dispscan.py, vcallscan.py), `out\` (d1.txt..d6.txt, scan-c30.txt),
`ghidra\` (the project copy, ~45 MB, don't commit).
