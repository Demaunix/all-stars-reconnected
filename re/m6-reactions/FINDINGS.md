# Driver reactions and announcer on network cars (reverse-engineer report, 2026-10-21)

Saved by the main session. Read-only static analysis of `re\bin\SSASR-unpacked.exe` (Ghidra headless on a scratch
copy + capstone); nothing run in game. C = confirmed in bytes/decompile, I = inferred. RC = reaction controller
`[racer+0x2040]`, CA = character-animation object `[racer+0x2044]`, CS = input block `[racer+0x9f0]+0x1c`,
B = `[[racer+0x9f4]+0x3c4]`.

## 1. How a driver's animation is chosen
**Reactions (RC, 0xc0 bytes, C).**
- Created in the reset 0x4ef820 only when racer+0x20fc == 0 (0x4ef8b5 `80 be fc 20 00 00 00 75 25 ... e8 57 41 fd ff`,
  ctor 0x4c3a30(racer,1,1)); same for every controller kind.
- Tick 0x4c5310 from racer slot 3 (data slot 0x6afbbc = 0x4f1eb0); runs only when racer+0xa1c == 2. Scene update
  0x49dca0 calls slot 3 then 4 for every visible racer (racer+0x2104 & 0x1f).
- RC fields: +0x98 racer; +4 queue count (max 8), entries at +0x18, 16 bytes {def*, target racer, flags, lifetime};
  +8 flags (1 ready, 2 look-at, 4 enabled, 0x10 finished); +0xb0..+0xbc pop timer (4 floats); +0 last position.
- Queue: 0x4c4080 thiscall(RC, u32 key, racer* target, float lifetime) `ret 0xc`; racer wrapper 0x4e7850. Keys in
  the "Reactions" table (0x24-byte rows at [0xcf77d4], sorted by key +0xc): +0 type (Win, Lose, Positive, Negative,
  Taunt, Showboat, FiredForwards, FiredBackwards; names at 0x6ecbcc), +4 BackupType, +8 Priority, +0x10 LinkedTo node,
  +0x14 LifeTime, +0x18 BackupRadius, +0x20 Probability.
- Pop 0x4c4410 (timer 0 and ready): node per type: Win 0x46770a3a, Lose 0x9db1d546 (both need racer+0x9f8),
  Positive 0xa7a4b00f, Negative 0x7cad4e8c, Taunt -> 0x4c34e0 (NodeTauntSelectFwd/Back/Left/Right relative to the
  target, queues "Taunted" on the target), Showboat 0x1b13cf01, FiredForwards 0x68bd232b, FiredBackwards 0x7e91a0e2.
- Play 0x502f60 thiscall(CA, node hash) `ret 4`, al = started; sets CA bit 0x40 (reaction playing), its only setter.
  Race-time callers 0x4c34e0 and 0x4c4410 (0x4bfff0 = garage scene).
- Ready gate 0x4c4750: racer+0x20fd == 0; not spinning (+0xad0/+0xb8c); +0x1ec0 == 0; speed +0xa70 not below the
  minimum; All-Star state 0/1, +0x1d0c == 0, +0x1ee8 == 0; not airborne (0x4e8ff0 -> 0x54e800); CA can play (0x502f90:
  bit 0x40 clear and `[[CA+0xc4]] == 4`).
- Key sources: FiredWeapon/Forwards/Backwards projectile spawn 0x5391a0 -> 0x4c4260; "Boost" FireGadget 0x4e8480 /
  0x4e84e0; item received AddGadget 0x5398a7; "TriggerMiniBoost" vehicle step 0x553787; SpinOut/FlipOut 0x4f0730,
  0x4f0880, 0x4f09c0, 0x4f1fe0, 0x4f2490; Respawn 0x4f2c10, 0x4f2d60; Stunt/StuntFailed/WheelieStunt 0x4ee410 (CS
  button 2); Overtake/OvertakeRival 0x4c4840 (local positions); finish keys by position 0x4c5150 (racer messages
  10/0xb/0xc: 0x4ed290 / 0x4e7880 / 0x4e78e0); post-finish Happy/WinTaunt/Lost 0x4c4980; **Showboat** 0x4c5360 only
  for controller category 1 when CS button 0 is just pressed (0x540520(0)) with no item held; "Win" = enter of
  controller state 0x28 (0x5b05f0, vtable 0x6b1eac).

**Base driving animation (CA, 0xe0 bytes, ctor 0x503770, C).** Tick 0x504ae0 -> 0x504420 from slot 13 at 0x4f4619.
State from 0x5042b0 into `[CA+0xcc]+0xe & 0x1f`: 0x12 All-Star; 0xf hit (+0xad0/+0xba0/+0xba4); 0x13/0x14 grabbed;
0xb trick (+0x1ee2); 0xc wheelie (+0x1ee8); 9 airborne; 7 drift (B+0x189); 8 boost (CA+0xd0 bit 1, from B+0x195...);
2/3 stopped; 4/5 driving. Parameters: speed racer+0xa60, steer CS+0x40 (0x540790 at 0x5044df), lean +0xb80/+0xac0.

**What drives it.** Mostly racer/vehicle/boost state; Showboat, stunts, wheelie, drift, mini-boost and steering need
the input block; Win needs a controller-state change. Network21's controller update 0x5097a0 (vtable 0x6b0364) zeroes
CS +0x3c..+0x48 (0x540620 at 0x50983b) and nothing writes it again, so these never happen on a puppet (C).
Network21's handler 0x5afd50 swallows messages 7/10/0xb/0xc/0x13, passes others to 0x5a4500 (message 0x35 -> state
0x28; only poster found: mode-gated 0x567870).

**Does a puppet play reactions?** Only its own, locally triggered ones (I, unverified in game): overtakes in this PC's
order, this PC's cosmetic hits, replayed item gives/fires (M5), finish by position. Never: Showboat, stunts, wheelie,
mini-boost, Win, drift/boost/trick poses, steering. The owner's reactions are never sent (event kinds 0..0x11, C):
the "not shared" the user saw. Unverified whether a puppet's RC ever gets ready (base state 4 via B+0x370 in
0x4f0c60, "not airborne" from the receiver's physics).

## 2. Announcer and driver voice
- Announcer = CommentMan 0x4bbec0(cue, priority) -> 0x45ffb0 -> 0x46db60. Cue = vehicle+0x72c[idx] (0x54d530, 21
  per-character slots) or 25% a generic one (0x6f50e0); per-PC cooldowns 0xce7bb0..0xce7bdc (mostly 45 s), random.
- Triggers for any racer (C): item get 0x4bc850 (AddGadget); item fire 0x4bc8d0 (FireGadget); overtake 0x4bc180
  (via 0x4c3410); car hit 0x4bc390 (racer+0xba0, near view); projectile hit by owner 0x4bd0a0 / 0x4bd150; homing lock
  0x4bc9d0 (near view); lap 0x4bcd90 (message 0x28, near view); gap/pack 0x4bc6d0; race-end top 3 0x4bce30.
  Near view (0x4bbe10) = within a race-distance window of a local player's car.
- Never for a puppet (C): position callouts 0x4bc290 (local single player), intro 0x4bc510 (local player's
  character), wrong way 0x4bcba0 (Human state 0x5af150 only).
- Category-1 tests that treat the remote human like an AI (C): overtake lines and reactions 0x4c3410 (`bb 03 00 00
  00 ... e8 69 5b 02 00`: a human can be called out in any place, other cars only in the top 3/4); race end callouts
  stop at the first local human.
- Driver voice (VO) 0x54d0a0 "Triggering VO index %i": 3D within 2000 units, 20 s cooldown per index. Any racer:
  overtake VO 0/1 (0x4c3460..0x4c348d), hit VO 1, fired weapon VO 4 (0x54d490). Never for a puppet: trick VO
  0x54f720, boost VO 0x54fe10 (category 1), lost-control VO 0x54f6b0 (category 1).
- Console (I): the same shared code ran on every machine; the remote human was a category-3 car, talked about like
  an AI, from events that ran on that machine. The screens never said the same lines. Console-correct on PC: keep
  commentary local, as now. No announcer-specific fix needed (remaining gap: hits, ITEMS-SYNC step 2).

## 3. Feasibility and the smallest safe fix
**Step 0 (log only):** per puppet once a second: RC != 0, RC+8, RC+4, CA bit 0x40, `[[CA+0xc4]]`,
`[[CA+0xcc]+0xe]&0x1f`, vehicle +0xb2; plus the key (def+0xc) of each new queue entry, every car.

**Fix A: mirror the owner's played reaction node (no new hooks).**
- Owner, in HookRacerCommit after the original, for cars it owns (host: its AI too): CA = [racer+0x2044]; if bit 0x40
  of [CA] rose since the last tick, find its node with 0x502fb0 thiscall(CA, hash) `ret 4` (bytes `8b 89 c4 00 00 00
  85 c9 74 05 e9 11 7a 09 00`; runner = [CA+0xc4], +0x20 head, +0x8c next, +0x88 node, node+0xc hash; 0x59a9d0, C),
  testing the 7 fixed hashes above plus the 4 NodeTauntSelect* names hashed with 0x4947e0(str, len, 0x4c11db7)
  (cdecl). Send a reliable Event pseudo kind 0x41 {slot, owner tick, u32 hash}; wire v6 + a contract flag,
  independent of the item group.
- Receiver: at Playout::ShownTick, if the car is a puppet (StillNetworkCar), CA != 0, [CA]&0x40 == 0 and
  `[[CA+0xc4]]` is 4 or 5, call 0x502f60(CA, hash) before the original slot 13 (bytes `8b 44 24 04 56 8b f1 8b 8e c4
  00 00 00 50 e8 bd 7e 09 00`). Retry up to 25 ticks, then drop; an unknown hash returns 0 safely (0x59a4e0). Suppress
  the puppet's own pops: write 1.0f to RC+0xb0..+0xbc every tick (0x4c5310 pops only when it reaches 0); its overtake
  detection, voice and announcer still run.
- Risks: a base-state change on the receiver may cut a reaction short (cosmetic); "Taunted" on the other player's car
  is lost unless the target slot is sent too; local hit reactions on puppets go away (hit spins still play).
**Fix B (optional): steering.** Owner sends CS+0x40 as int8 per Pose/Car (kPoseSize 72, kCarSize 58: CarsSize(7) =
423 > kWireMax 416, raise kWireMax/kRaceMsgMax); receiver writes it into the puppet's CS+0x40 just before slot 13
(the next controller update zeroes it before slot 3/4 read it). Unverified: other CS+0x40 readers in slot 13 and
the renderer.
**Not feasible in a short session:** drift/boost/trick body poses and Showboat/stunts as base states (0x5042b0
recomputes them inside slot 13 from physics fields); owner-consistent hits (ITEMS-SYNC step 2).
In-game check for Fix A: on the host, press the item button with no item held (Showboat); the guest's screen should
show the host's driver doing the same taunt, and the log the pseudo kind 0x41 hash.
