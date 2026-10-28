# Item commentary lines: do they favour the host? (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`; own Ghidra copy `ghidra\`, 45 MB, don't commit); saved from
its report by the main session (session "Echo Chamber"). C = confirmed in bytes/decompile, I = inferred. Every slot named below
holds the same value in the on-disk `re\bin\SSASR.exe` (checked); none is near the Roster Pack's 12 fingerprinted ranges.

## Short answer
None of these lines tests for the local player in its gates (the near test `0x4bbe10` is always true when there is a local
player: C, `s-l <= 250` or `l-s <= 80`, constants `0x6aa8cc`/`0x6aa8d0`). Instead each line follows **the host's simulation**:
- **HIT: favours the host's view.** The host decides its own car's and the AI's hits (right). A guest's car: only the host's
  cosmetic copy of a hit or crash can trigger it; the guest's real spins never reach the host.
- **ATTACK/ATTFAIL: fair as to who can be named** (every owner's projectiles exist on the host); hits on guest cars are judged by
  the host's cosmetic view. Low priority.
- **ALLST/ALLSTU: fair** (the replayed give/fire events run the stock trigger for guests, at the owner's shown tick).
- **ALLSTM: mostly fair** (names the All-Star user; fires on any car touched in the host's view; puppet victims cosmetic). Low.
- **COM_ALL_T_4x (Opa-Opa's All-Star weapon): wrong for guests.** A copy's weapon is a random draw (C): the line, and the weapon
  the copy fires, can differ from the guest's real choice. This is an item-sync bug too, not only commentary.

## 1. Triggers, subjects, gates (C)
Every line: the 25% generic cue from the shared generator (`0x4be180` -> `0x4be040`, ECX `0xc28cd1`); the last gate before the
request `0x4bbec0` is `[0xc29fdb]==0`; on guests every line is muted later at `0x4bbec0` (scene+0x1350).
- **HIT `0x4bc390`.** Chain: racer slot 4 (data `0x6afbc0` = `0x4f3990`; life `+0xa1c==2`) -> `0x55aab0` (at `0x4f3b64`) ->
  per car `0x4bd3b0(idx)` (at `0x55ab11`) -> `0x4bc390` (at `0x4bd4e0`). `0x4bd3b0`: idx < scene+0x1100, race clock
  `[0xce7ba8] >= 7.0` (`0x4bd3c6`: `f3 0f 10 05 1c 43 6a 00 0f 2f 05 a8 7b ce 00 0f 87`). Then callouts live `[0xcf7a51]`, racer
  byte `+0xba0` set (`0x4bc3ab`: `80 b8 a0 0b 00 00 00`), excitement +0.25, `[0xce7bb4] >= 45.0` (`0x6ad4d0`), near (always);
  then `[0xce7bb4]=0` (`0x4bc3f6`), cue (generic `[0x6f50f8]` or vehicle+0x72c[6]), `0x4bbec0(cue,2500,0)` at `0x4bc445`. No
  finished check. Each car is tested in its own update: the first spinning car in update order wins once the cooldown is ready
  (I: index 0 = the host's car). `[0xce7bb4]` is read only at `0x4bc3db`, reset only at `0x4bc3fa`, ticked at `0x4bc136..0x4bc147`
  in `0x4bbf60` (Racing tick `0x5b2110` at `0x5b213c`, outside the racer updates).
- **Racer `+0xba0` = spin/flip flag.** Set (`c6 86 a0 0b 00 00 01` or a mov) by SpinOut `0x4f0730` (`0x4f07b8`), FlipOut `0x4f1fe0`
  (`0x4f2076`), failed stunt `0x4ee410` (`0x4ee46a`), crash spin `0x4f2490` (`0x4f27fd`, from end-of-tick `0x4f3c50`),
  `0x4f08fd`/`0x4f0baf`; cleared when the spin timer ends in `0x4f3c50` (`0x4f4539`: `c6 86 a0 0b 00 00 00`). The racer message
  handler `0x4f4f20` calls FlipOut for message `0x6481c4e5` (at `0x4f5882`) with no controller/category check; the weapon script
  sends it to the victim (Behaviours.sbn `Basic_Weapon_Behaviour` +0x1d99, from HandleCollision).
- **ATTACK `0x4bd0a0` / ATTFAIL `0x4bd150`.** Only caller: the projectile message handler `0x526140` (message `0xc6320496` ->
  sound slot 7 -> ATTACK at `0x5265fd`; `0x50b97c51` -> slot 8 -> ATTFAIL at `0x5263c2`; both need `0x5200a0`: projectile +0x448
  sound set or `0x4b8fb0`, which only loads audio data; no camera test). `0x526140` is reached from the projectile message slot
  (vtable `0x6b2980` slot 8, data **`0x6b29a0` = `0x52c9a0`**, thiscall(proj, msg) `ret 4` at `0x52e501`; hash `[[msg+0xc]+4]` at
  `0x52c9b4`) and from the projectile's audio timeline (+0x44c, vtable `0x6b28bc` slot 1 = `0x529640`, data `0x6b28c0`). Subject:
  the projectile's owner `[[proj+0x310]+0x9d8]`. Gates: owner not finished (`0x51a010`), live, `[0xce7bb8] > 46.0` (`0x6ad4cc`;
  `0x4bd0de`: `f3 0f 10 05 cc d4 6a 00 0f 2f 05 b8 7b ce 00 73`), 25% generic (`[0x6f5114]`/`[0x6f5118]`, else vehicle cue 13/14),
  `0x4bbec0(cue,5000,0)` at `0x4bd13b`/`0x4bd1eb`; the cooldown is reset only inside the `[0xc29fdb]==0` branch (`0x4bd146`).
  Senders of `0xc6320496` (bytes `96 04 32 c6` in Behaviours.sbn): `Basic_Shockwave_Behaviour` HandleCollision (+0x14ca),
  `MonkeyBallAssist_Behaviour` HandleCollision (+0x124c, +0x12bc), `Basic_Weapon_Behaviour` **Rebound_Weapon** (+0x1a2f: a wall or
  ground bounce, no victim; called by the projectile's world collision `0x527260`, string `0x6b29d8`, reference `0x527762`).
  `0x50b97c51` is in no script: ATTFAIL comes only from the timeline data (I).
- **The hit run is synchronous inside the post (C):** post `0x5c0820` -> `call [mgr+8]` (phase 0) -> `0x535090` -> `0x5c0260` ->
  kind 5 at `0x5c03fb` -> `0x49d400` -> each object's vt+0x14 -> projectile `0x51f920` -> script call `0x531e50` -> HandleCollision
  (I). The kind-5 ids are the words at ev+8 and ev+0xa (`0x5c0403`: `66 8b 43 08`; `0x5c040b`: `66 8b 4b 0a`); racer ids `0xffe0|i`
  (ITEMS-SYNC). The delayed pass (phase 1, `0x5c01d0`) doesn't rerun the response.
- **ALLST `0x4bc850`**: from AddGadget `0x539680` only for gadget `0xb678e022` (`0x53981a`: `52 e8 31 30 f8 ff`); gates finished,
  live; 25% generic `[0x6f5108]`; wait 3000.
- **ALLSTU `0x4bc8d0`**: from FireGadget's All-Star branch (`0x53b382`: `8b 96 d8 09 00 00 52 e8 42 15 f8 ff`), then `0x4e8530`;
  same gates; generic `[0x6f510c]`; wait 5000.
- **ALLSTM `0x4bcd90`**: on listener message **0x28 = MSG_HIT_BY_REVENGE**, data slot **`0x6afba8` = `0x4ee020`** (vtable
  `0x6afba4` slot 1, ECX = racer+0x2c0, thiscall `ret 4` at `0x4ee0ba`; sender `[msg+8]` at `0x4ee0bd`); sent
  `0x4d2bf0(0x28, attacker, victim+0x2c0, 0, 1)` from ~26 sites in the per-character All-Star classes (`0x590db8`,
  `0x5d5b72`..`0x5eacb1`). Subject: the All-Star user. Gates finished, live, near; no generic (always vehicle cue 12: `0x4bce00`
  `6a 0c`); mute check `0x4bce07`; no cooldown variable (only the entry's 30 s).
- **COM_ALL_T_4x `0x4bc970(idx, cue)`**: from `0x5e9e45` in Opa-Opa's All-Star class ("ALLSTAR_OPA\..."; class id 0x12 at vehicle
  data +0x1718; vtable `0x6bb3d8`), slot 11, data **`0x6bb404` = `0x5e9b10`** (thiscall, no args, `ret` at `0x5e9edb`), called once
  when the transform becomes active (`0x585de0`, `0x585fc9`: `8b 42 2c ff d0`). Weapon `[obj+0x184]`: 0 -> `0x131930b8`
  (COM_ALL_T_41), 1 -> `0xefc87eef` (40), 2 -> `0xf6a28420` (42). Its choice (slot 9, `0x6bb3fc` = `0x5e9a60`): the car's own human
  (category 1, `0x4e8f90`) picks from a menu by input; otherwise, when it is 4: shared generator % 3 (`0x5e9aed`:
  `b9 d1 8c c2 00 e8 79 46 ed ff 33 d2 b9 03 00 00 00 f7 f1`). The class object is `[racer+0x1ff0+0x40]`, its racer `[obj+0x10]`.

## 2. On the host, for a guest's car (a category-3 Network21 puppet, `g.remote[k]`; adopted cars are not remote, `DriveHere`)
- **HIT:** the test runs (slot 4 runs for puppets). The puppet's `+0xba0` is set only by the host's own cosmetic hits (HookEventPost
  passes kind 5; the stock FlipOut runs on the puppet), crash physics in slot 13, and All-Star touches in the host's view. The
  Pose carries no spin state: the guest's real hits make no line; hits the guest dodged still can.
- **ATTACK/ATTFAIL:** the guest's fires run on the host (same projectile ids, the owner's launch frame), so the guest can be
  named. Hit, bounce and timeline outcomes come from the host's simulation: authoritative when the victim is the host's car or
  AI, cosmetic when it is a guest; the host's own projectiles hitting a guest puppet are cosmetic too.
- **ALLST/ALLSTU:** the replayed kind 0/1 events run AddGadget (`0x5c02b9`) and FireGadget: the stock lines play as for the host's
  own car, about a playout delay late.
- **ALLSTM:** a guest's All-Star copy on the host sends 0x28 to every car it touches in the host's view (host/AI victims
  authoritative, puppets cosmetic); the host's own All-Star touching puppets is cosmetic as well.
- **COM_ALL_T:** the copy's weapon is a random draw on every non-owner screen: on the host this line about a guest is wrong 2
  times in 3, and the copy fires a different weapon (C).

## 3. Recommended design (data slots only; each installed only over its stock value; game thread only; the WrongWay pattern)
New message `kMsgItemLine {race, kind, slot}`, guest -> host, reliable.
- **HIT (first).** Guest, each race tick (beside `ReportWrongWay`): `racer[g.local]+0xba0` 0 -> 1: send HIT for its own slot.
  Host: hook racer slot 4, data `0x6afbc0` (stock `0x4f3990`): for a car with `g.remote[k]` while `FairCallsOn()`, save
  `[0xce7bb4]`, write 0.0f, call the original, restore (silences only the stock HIT for that car; excitement still accrues).
  Never touch `+0xba0` (SpinOut/FlipOut read it). On a guest's report: `CallHit(k)` modelled on `FairAlert` (not `0x4bc390`: it
  draws the shared generator and needs the puppet's `+0xba0`): gates `AnnouncedPlayer(k)`, `[0xcf7a51]`, `[0xce7ba8] >= 7.0`,
  k < scene+0x1100, `[0xce7bb4] >= 45.0`, `[0xc29fdb]==0`; cue `[0x6f50f8]` if `AnnounceRoll() < 0.25` (and non-zero), else
  `PlayerCue(k,6)`; `0x4bbec0(cue,2500,0)`, `[0xce7bb4]=0`, `NoteMention(k)`. Optional ("players before AI"): hold `[0xce7bb4]` for
  every car in that hook and pick each tick from host-owned rising edges plus reports, players first, with a ~0.5 s window.
- **ALLSTM.** Hook data `0x6afba8` (stock `0x4ee020`): `int __fastcall(uint8_t* listener, void*, int32_t* msg)`, racer =
  listener - 0x2c0. Host, `msg[0]==0x28` with a remote receiver: `[0xc29fdb]=1` around the original, then restore (mutes only the
  line; the trophy stat `0x4a2610` and the controller forward still run). Guest, `msg[0]==0x28` to its own car: send ALLSTM with
  the sender's record index. Host on the report: the stock `0x4bcd90(index)` directly (cdecl, no RNG, no cooldown; prologue
  `56 8b 74 24 08 56 e8 35 f1 00 00`).
- **Opa-Opa weapon (also fixes the copies).** Owner: put the weapon into the existing All-Star phase pseudo-event when its phase
  becomes 4 (read `obj=[racer+0x1ff0+0x40]` if `*(u32*)obj==0x6bb3d8`; weapon `[obj+0x184]`), e.g. byte 1 of the 4-byte body, or a
  wire version bump. Every screen: hook data `0x6bb404` (stock `0x5e9b10`); for a remote `[obj+0x10]`: weapon known -> write
  `[obj+0x184]=w`, then the original; unknown (host) -> the original with `[0xc29fdb]=1`, and when w arrives
  `0x4bc970(index, cue[w])` (cdecl, no RNG; prologue `8b 44 24 04 50 e8 56 f5 00 00`). The owner's phase message is applied before
  the stock update at the owner's shown tick, so it usually arrives in time (I).
- **ATTACK/ATTFAIL (optional, low value).** Host: in HookEventPost for kind 5 where an id is a remote racer, hold `[0xce7bb8]` at
  0.0f around the original (its other users: ALERT `0x4bca50`/`0x4bcb89`, init `0x4bbcf6`). Guest: hook data `0x6b29a0` (stock
  `0x52c9a0`); while its own car's kind 5 runs, report hash `0xc6320496`/`0x50b97c51` with the owner. Host on the report: a
  `FairAlert`-style line (cooldown > 46, generic `[0x6f5114]`/`[0x6f5118]` at 25%, wait 5000, then reset `[0xce7bb8]`). Misses
  ATTACKs from bounces and timelines (I); leaving the stock as is = the game.
- **ALLST/ALLSTU:** no change.

## 4. Risks
Slot 4 runs 8 times per tick per car: keep the hook cheap. `[0xc29fdb]` also gates the countdown voice: set and restore it only
around one synchronous call. Commentary latency: a guest's events are said on the host after a playout delay plus one hop. Another
mod may hold `0x6afbc0`/`0x6afba8`/`0x6bb404`: install them as their own group and refuse a non-stock slot. Roster Pack racers
have no cue entries (silent everywhere). Racer update order ("index 0 wins ties"): I. What exactly sends `0xc6320496` in
HandleCollision and the ATTFAIL timeline data: I.

## 5. Cue tables (C)
`0x54d550` (ECX = vehicle) matches the vehicle's name against the table `0x6f1000` (30 x 0x224, key at +0) and sets
`vehicle+0x72c[i] = hash("%s%s", prefix[i] (0x6f5088), name at +0x1fe)`: the same hash as `re\m5-items\scripts\hashmap.py`
`name_hash` (HIT_SONIC = `212bb3ec`; others match the entry table). Generic table `0x6f50e0`: 1 GAIN `704da8e2`, 2 LOST
`1183b3ab`, 3 CONT `0c23d443`, 6 HIT `222a21b1`, 7 OFFTK `4f407630`, 8 WNGWY `255f6f16`, 9 ALERT `7b318993`, 10 ALLST `c3bb437e`,
11 ALLSTU `049331ca`, 12 ALLSTM `d715075e` (unused), 13 ATTACK `1c75b527`, 14 ATTFAIL `00bd4778`; the others 0. Characters named
"xxx" (the per-player-appearance racers, the JSR cars) get cues with no entry (silent); both Ryo racers share "RYO". All characters x 21 cues:
`out\cues.txt` (item lines only: `out\cues-items.txt`; `scripts\chars.py all` regenerates).

## Confidence and files
High (C): the chains, gates, slots and bytes above, the shared cooldowns, the Opa-Opa random draw, the synchronous hit run.
Medium (I): script semantics (Rebound, HandleCollision), the ATTFAIL source, tie order, the timing of the Opa-Opa phase message.
Nothing tested in game. Files: `scripts\` (run.sh, Decomp.java, l1-l18.txt, callscan.py, callargs.py, dispscan.py, immscan.py,
vcall.py, rangestrs.py, bytesearch.py, chars.py, guess.py, peek.py), `out\` (d1-d18.txt, cues.txt, cues-items.txt,
behaviours-modules.txt, Basic_Weapon_Behaviour.txt/.smosbin, Basic_Shockwave_Behaviour.txt/.smosbin,
MonkeyBallAssist_Behaviour.smosbin).
