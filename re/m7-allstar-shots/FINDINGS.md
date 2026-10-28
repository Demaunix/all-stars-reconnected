# All-Star shots on copies: the input slot, every class, the exact-moment sync (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`; Ghidra project copy `ghidra\`, 45 MB, don't commit); saved
from its report by the main session (session "Night Shift"). C = confirmed in bytes/decompile, I = inferred. Every vtable slot
named below holds the same value in the on-disk `re\bin\SSASR.exe` (0 differences across slots 6, 7, 11, 12, 13, 15 of all 25
vtables). Each class's slot-7 function is referenced only by its own vtable slot (C, dword scan). None of these slots is near
the Roster Pack's 12 fingerprinted ranges. Background: `re\m7-opa\FINDINGS.md` (§6), `notes\ITEMS-SYNC.md`.

## Short answer
- **A copy's fire decision comes only from slot 7.** `0x5a0080` is the only caller of slot +0x1c in the All-Star and racer
  code (C). The 12-byte `out` struct is: steer, vertical, "button down", "button pressed".
- **14 of the 24 classes have their own random or deterministic slot 7**: those copies diverge, Opa-Opa among them. The other 10
  use the base `0x5a00d0` (outputs zeros): their computer racers never fire and their copies follow time and position.
- **No All-Star shot goes through the event manager** (C): nothing to mirror as is.
- **Recommended fix (a):** the owner detects its own shot from a class field after the update and sends a new pseudo-event; on
  the copy a slot-7 hook outputs "fire" only while a shot is due, with the class's gate timer forced open for that tick. For
  Opa-Opa: one new data-slot hook (`0x6bb3f4`); the rest in the existing pre-/post-update steps.

## 1. The base input (C)
- **The chooser `0x5a0080`**: `thiscall(obj, out*)`, `ret 4`. Bytes `56 57 8b 7c 24 0c 85 ff 8b f1 74 2e 8b 4e 10 83 b9 f0 09 00
  00 00 74 18 e8 f3 8e f4 ff 84 c0 74 0f 8b 06 8b 50 18 57 8b ce ff d2 5f 5e c2 04 00 8b 06 8b 50 1c 57 8b ce ff d2`. A null
  `out` does nothing; the racer is `[obj+0x10]`; if `[racer+0x9f0] != 0` and `0x4e8f90` (`[ctrl+0x18]==1`, a local human) ->
  slot 6 (`+0x18`), else slot 7 (`+0x1c`). A Network21 puppet (category 3) and the computer racers (category 2) both use slot 7.
  Callers: 37 sites in 22 classes.
- **`out` is 12 bytes** (callers zero +0, +4, +8, +9; slots 6/7 zero three dwords). Slot 6 `0x5a0000` reads `CS=[ctrl+0x1c]`
  (layout as `notes\KEYBOARD-DRIFT.md` §1): `+0` steer `CS+0x40` (`0x540790`), `+4` vertical `CS+0x3c` (`0x540770`), `+8` button 0
  state 2 or 3 = "down" (`0x540540(0)`), `+9` button 0 state == 2 = "pressed this tick" (`0x540520(0)`); button 0 = item (I). No
  throttle or brake.
- **The phase-4 tick is slot 12 (`+0x30`)**: `0x5867a0` calls it at `0x586859` with `&dt` (float4), `ret 4`. Phase 5 calls slot
  15 (`+0x3c`, `0x585ca9`); every slot 15 that reads input uses only `+0` (steer).
- **Opa slot 7 `0x5ea2e0`** (data `0x6bb3f4`): `56 8b 74 24 08 85 f6 74 28 33 c0 89 06 89 46 04 b9 d1 8c c2 00 89 46 08 e8 73 3e
  ed ff 99 b9 50 00 00 00 f7 f9 85 d2 75 08 b0 01 88 46 08 88 46 09 5e c2 04 00` (MT % 80 == 0 -> out+8 = out+9 = 1).
- **Opa calls slot 7 twice per phase-4 tick**: Opa slot 12 `0x5ea580` (`ret 4` at `0x5ea5b0`) runs only while `[obj+0x1ac]==2`
  (set by slot 11 at `0x5e9b32`). First call `0x5ea3c0(1)` through `0x5a0080` at `0x5ea3e4`: uses only `+0` as the vehicle
  steering `[[racer+0x9f4]+0x984]`. Second call `0x5e9ee0` through `0x5a0080` at `0x5e9f0d`: the fire. Phase 5 (slot 15 `0x5ea5c0`
  = `mov [esp+4],1; jmp 0x5ea3c0`) calls it once, steer only.
- **The fire gate (C, `0x5e9f12..0x5e9f4a`)**: `+0x160 += dt[0xc2a01c]`; if `+0x160 > [obj+0x188+12*w]` and `out+8 != 0` ->
  `+0x160 = 0` (`f3 0f 11 86 60 01 00 00` at `0x5e9f4a`), then a shot. `out+8` is the "down" byte: holding the button auto-fires.
- Slot 4 runs `0x586980` unconditionally at `0x4f3bcb`, so slot 7 is called every tick for puppets in phase 4 (C).

## 2. The classes (factory `0x5863b0`, switch on vehicle data `+0x1718`, set from data key `0x63c05ddd` at `0x55fafc`)
Names (I, strong): PC vtable override patterns matched against the Mac build's RTTI `Revenge_*` classes (the Mac slot = the PC
slot + 1); 21 of 24 match exactly; the 3 left settled by PC strings (`LOD1_Gun` -> Ulala, `Big_Hit/Small_Hit` -> Amy,
`BonanzaBlimp` -> BoostSteal). Opa-Opa = 0x12 checks out.

**Classes whose copies diverge** (slot-7 data slot = stock function; phase-4 tick slot 12 in brackets). "Proof" = the field the
owner can check after its update; "force" = what the copy writes before the update while a shot is due.

| id | character (I) | slot 7 (the computer's rule) | what fire does | owner's proof (after the update) | copy: force, then send |
|---|---|---|---|---|---|
| 0x01 | Sonic (SuperSonic) | `0x6b7c6c=0x5de860` MT%80 | +9 -> pulse `0x5ddb80` if `+0x178<=0.001`: `+0x178=2.5`, anim 5 [`0x6b7c80=0x5deef0`] | `+0x178==2.5f` | `+0x178=0` |
| 0x02 | Eggman | `0x6bb52c=0x5eacc0` MT%40 | +9, `+0x134>=+0x138`, anim<6 -> rocket `0x63a9d61e` (`0x5ead00`); side from `+0x130` [`0x6bb540=0x5eb050`] | `+0x134==0.0f` | `+0x134=+0x138`; send `+0x130` |
| 0x04 | Amy | `0x6b782c=0x5d86c0` `+0x140!=0` or MT%80 | +9 & anim 2/7 -> hammer anim (`+0x140` combo or 4/3 via `+0x13a`), speed 1.5 [`0x6b7840=0x5d8870`] | `+0x13a` flip / anim (I) | send `+0x140` |
| 0x05 | Alex Kidd (JanKenPon) | `0x6b771c=0x5d5af0` MT%40 | +9 & `+0x130<=0` -> `0xc33889e7` (`0x5d5710`); aim = the vehicle steering `[[racer+0x9f4]+0x984]` [`0x6b7730=0x5d5e20`] | `+0x130` rose | `+0x130=0`; send `+0x984` |
| 0x06 | Bonanza Bros (BoostSteal) | `0x6b793c=0x5bc4a0` aim=2*rand-1, MT%10 | read in slot 23 `0x6b797c=0x5bd470`, from the blimp's update (`0x52992d`, owner in phase 4): +0 turns aim `+0x150`; +9 & `+0x1a4==0` -> "Attack", `+0x1a4=3`; bomb `0x733eebe3` thrown at anim>=14 (`0x5bc4f0`, `+0x186=1`) | `+0x1a4` 0->3 | send `+0x150` |
| 0x08 | Beat (JetSetRadio) | `0x6b7ad4=0x5dade0` always held | +8 level -> spray anim 2<->3, `+0x15c` [`0x6b7ae8=0x5db390`] | level | send level edges |
| 0x09 | Shadow (ChaosWarp) | `0x6b7be4=0x5dcf80` MT%30 | +9 (C, `mov edx,[esp+0x29]` at `0x5dd973`) & `+0x174<=0` -> `0xd0d38b75` from RightHand; aim from +0 and +4, plus 2 local random draws (spread) [`0x6b7bf8=0x5dd7f0`] | `+0x174==0.5f` | `+0x174=0`; send `out+0`, `out+4` |
| 0x0a | Knuckles | `0x6b7b5c=0x5dba40` held until `+0x140>+0x13c`; `+0x140` counts only for computer racers (`0x4e8fb0`, category 2) | +8 held charges `+0x138` (levels `+0x130`); release -> punch `0x5dbd50` on every car placed ahead (`0x5db890`) [`0x6b7b70=0x5dc160`] | release: `+0x134==0.5f` | `+0x134=0`; send level edges + `+0x138` |
| 0x0d | Ulala (MaxGroovy) | `0x6b7d7c=0x5e07c0` MT%40 | +9 & anim 2 -> homing `0x67fa5939` at the car one place ahead (`0x5e0220`) [`0x6b7d90=0x5e09e0`] | anim 2->6/7/8 (I) | the target is the copy's own race order (I) |
| 0x0f | AiAi (MonkeyBall) | `0x6b839c=0x590e30` MT%50 | +9 -> pulse `+0x1b0=1.5` (`0x5906f0`); balls `0x590a60` are on a timer [`0x6b83b0=0x5910b0`] | `+0x1b0==1.5f` | `+0x1b0=0` |
| 0x11 | Zobio (ZombieCharge) | `0x6b7e04=0x5e1390` MT%30 | +9 & anim 2 & `+0x138<=0` -> anim 6 [`0x6b7e18=0x5e14c0`] | anim->6 (I) | `+0x138=0` |
| 0x12 | Opa-Opa | `0x6bb3f4=0x5ea2e0` MT%80 | §3 | §3 | §3 |
| 0x13 | B.D. Joe (CrazyTaxi) | `0x6b8024=0x5e4810` MT%10 | +9, `+0x150<=0.001`, anim!=3 -> pulse `+0x150=1.5`, anim 3 [`0x6b8038=0x5e49e0`] | `+0x150==1.5f` | `+0x150=0` |
| 0x14/0x15 | Ryo (both classes; ctor `0x5857b0(1/0)`) | `0x6b7a4c=0x5da420` MT%50 | +9 & anim!=3 -> anim 3, `+0x131=0` [`0x6b7a60=0x5da570`] | anim->3 (I) | - |

MT = the shared local generator `0x4be170` (ECX `0xc28cd1`). Anim = `0x59fe90`: `([[racer+0x2044]+0xcc]+0xe >> 10) & 0xf`.

**Classes using the base slot 7 `0x5a00d0`** (zeros; its 11 data slots `0x6b7694`, `0x6b77a4`, `0x6b78b4`, `0x6b79c4`,
`0x6b7cf4`, `0x6b7e8c`, `0x6b7f14`, `0x6b7f9c`, `0x6b80ac`, `0x6b8134`, `0x6b81bc` include the base vtable's own): 0x03 Big
(Frogs), 0x07 Tails (Tornado), 0x0b ChuChu, 0x0c Billy, 0x0e Amigo (SambaParty), 0x10 Jacky/Akira, 0x16-0x19 the console-only racers
(0x17 Ratchet among them). Their copies follow only time and position. Exceptions: Big's `+0` moves `+0x378` and class 0x18's `+9`
inflates, so a human owner's input isn't mirrored there. 0x16 (0.5 s shots `0x67bdc430` at a random car) and 0x17 (12 randomly
aimed objects) diverge through local random draws, not slot 7. 0x16 and 0x18/0x19 are probably console-only racers (I).

## 3. Opa-Opa: firing exactly when the owner did
**One shot (C):** weapon 0: in one tick 4x `0x538170(0x84aa17c2,-1,0,2,frame)` + `0x5391a0` (`0x5e9fb9` / `0x5ea00e` ...
`0x5ea296`), projectiles `+0x398` = 0.2/0.4/0.6/0.8, interval `[+0x188]` = 3.0. Weapon 2: 1x `0x9ba6856b` (`+0x398` = 0.5),
interval `[+0x1a0]` = 4.0. Weapon 1: interval 3.4e37, never "shoots"; the fire byte goes into `0x5e9890`: if `+0x154<=0.001` ->
`+0x154=2.5` (`0x40200000`), a 2.5 s grow pulse. **No ammo counter**: `+0x18c`, `+0x190` etc. are written only at slot 11 and
never read (the "4 shots" in `re\m7-opa\FINDINGS.md` is a 4-projectile volley per trigger).

**What a shot reads (C):** the launch frame `0x536860` from the racer's pose (locator `0x54c9b0`), its velocity (`racer+0x100`
x dt via `0x4dca40`), a direction flag from `[ctrl+0x80]` (`0x5365d0`), the local serial `[0xcdd048]` (`0x4b4e90`, bumped by
`0x4b8830`). No aim input: on a copy at the owner's shown tick the puppet's pose stands in; the message needs only {slot, owner
tick, weapon}.

**(b) No existing event kind fits (C):** in the All-Star code (`0x585000-0x587000`, `0x590000-0x591600`, `0x59f000-0x5a1000`,
`0x5bb000-0x5bda00`, `0x5d5000-0x5eb400`) no call to `0x516760`, no access to `scene+0x1374/+0x1378`; `0x538170`, `0x5391a0`,
`0x538310` create the projectile directly through `0x4b8830`. The nearest kind is 3 (`0x5c036e..0x5c03ed` = `0x538310(frame)` +
`0x50f7e0`, posted only by sub-projectiles `0x51f3c6`); it skips `0x5391a0`'s extras (flag `+0xbc` bit 19, `0x550920`, `0x4c4260`,
`0x522120`) and the per-class writes (Opa's `+0x398`): not recommended. Spawned All-Star projectiles still post kinds 10/11
(homing, `0x52031c..0x520361`) and 5 (hits) like any projectile; those stay local.

**(a) Recommended design (one new hook):**
1. Hook data slot `0x6bb3f4` (stock `0x5ea2e0`), beside `0x6bb404`: `void __fastcall HookOpaInput(uint8_t* obj, void*, uint8_t*
   out)`. Call the original first (the local random draws stay as stock). Then, if `racer=[obj+0x10]` is a remote car and `out`
   is set: zero `out[0..11]`; if a shot is due, `out[8]=out[9]=1`. Both calls in the tick see the flag; only `0x5e9ee0` uses it.
   Copies then never fire on their own: no phantom shots.
2. New pseudo-event `kEventAllStarShot` on the reliable Event path (per-sender queue, run at `ShownTick`). Body (4 bytes) =
   `(w+1) | counter<<8`.
3. Owner (post-update, right after `SendAllStarPhases`, for `SendsItems(k)`; `obj=OpaObject`, phase 4, `w=[obj+0x184]`): send when
   `w` is 0 or 2 and `[obj+0x160]==0.0f` (only `0x5e9f4a` writes 0; the other writers: `0x5e9e0c` (100000), `0x5e9f2b` (+= dt)), or
   `w==1` and `[obj+0x154]==2.5f` with the previous tick's value != 2.5.
4. Copy, in `RunQueuedEvents`: `RunAllStarShot` sets due, the weapon and the start tick.
5. Copy, each pre-update (after `RunQueuedEvents`) while due, phase==4 and `[obj+0x184]` == the due weapon: `w` 0 or 2 ->
   `[obj+0x160]=100000.0f` (`[0x6bb4b4]`), so local dt drift can't refuse the shot; `w==1` -> if `[obj+0x154]>0.001f` write
   `0.0f` (`0x5a0900` shrinks it, then the fire sets 2.5).
6. Copy, post-update: the shot was used if the same proof shows (`+0x160==0` / `+0x154==2.5f`) -> clear due, log. Otherwise
   drop after ~90 ticks, or when the All-Star ends, a peer leaves, or the race ends.
7. While the copy's `+0x184` is 4 (weapon not yet known), keep the shot pending and never force (with weapon 4 the gate would
   reset `+0x160` with no spawn and fake the proof).
8. Call `SendAllStarShots` after `SendAllStarPhases`, so the phase-4 message (with the weapon) always comes first in the queue.

**The same pattern for the other 13 classes:** one hook function over each slot-7 data slot from §2's table, plus each class's
proof and force fields. Beat and Knuckles need the "down" level mirrored (edges), not single shots. Mirroring raw button presses
alone would be wrong: a press the owner's own gate refused would fire on the copy.

## 4. Risks
- **Steering in `out`**: it carries steering only; the class code writes it into the puppet's `+0x984`, at most visual for posed
  cars (I). Real aim only for Bonanza (`+0x150`) and Shadow (`+0`/`+4`); Alex Kidd's shot reads `+0x984` itself. Opa-Opa has none.
- **Opa state**: `+0x160` is forced on due ticks, so drift doesn't matter; no ammo; the weapon must be 0..2 first (pending while
  4); `+0x1ac` must be 2 (set by slot 11; slot 12 does nothing before that).
- **Shot timing**: a computer racer's shot comes at the first tick after the interval in which MT%80==0 (~80 ticks on average).
  An owner's shot can land in its very first phase-4 tick, before the copy's transform ends: the pending window must cover that.
- **Knuckles copies today** hold forever (category 3 isn't counted as a computer racer) and punch once when the All-Star's end
  forces a release (`0x5dc160` end test): a phantom hit on every car ahead (C for the logic; I for the hit effect).
- **Projectile ids** come from each PC's own serial and differ, as before; only local hits use them.
- **Other mods** may hold these slots: install as its own group and refuse any non-stock value; the Roster Pack's 33 patches unknown.

## Confidence and files
High (C): §1, the slot table, the Opa shot gate and proofs, the absence of event posts. Inferred (I): the character names; the
anim-based proofs (Amy, Ulala, Zobio, Ryo); whether the puppet's steering matters; the tick rate; console-only classes;
Bonanza's throw timing. Files (`re\m7-allstar-shots\`): `scripts\` (run.sh, Decomp.java, l1.txt, l2.txt, vtables.py,
classstrs.py, macnames.py (reads the Mac build's names), slotcalls.py, dispscan.py), `out\` (d1.txt,
d2.txt, vtables.txt, classstrs.txt, macnames.txt), `ghidra\` (the project copy, 45 MB, don't commit).
