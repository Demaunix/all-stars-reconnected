# Items and All-Star moves on both screens (M5 groundwork)

## Built: step 1 (0.5.0-m5g, 2026-10-21; facts in `re\m5-items2\FINDINGS.md`)
- Contract flag `kContractItems` (needs the host's AI; config `[Race] ItemSync=1`), wire version 4, message
  `Event` (reliable): the game event's bytes from +8, the owner's sim tick, the car slot.
- Post hook 0x6b9c40 (kinds 0, 1, 2 only): a car this game owns (our player; on the host also the AI) runs
  and is sent, captured before it runs (sent before running, so a nested fire follows its give); a
  puppet's event is freed unrun; the event we run for another game passes unchanged (`g.injecting`).
- Receiver: queue per sender; run when the sender's car is shown at that tick (`Playout::ShownTick`; the
  host's AI use the host car's clock) or 25 ticks after arrival; only while the window has < 12 posts.
  Checks: the sender owns the car, body slot = car, slot < racer count, car is a puppet here, gadget in
  the item table (+0xc01ccf24, never the roulette), projectile type known (0x4c0fc0). Fresh event from
  0x516760, bytes copied except head and quantizer vtables; a give first clears another held item; a
  fire lends the owner's controller +0xb8 for the call.
- Item boxes: racer slot 8 hook (0x6afbd0) returns 1 for puppets on the box message. No more item drops
  for network cars while items are mirrored.
- All-Star: owner sends phase changes (pseudo kind 0x40); a copy gets a 60 s length at its start and ends
  with the owner's through S+0x4b (phase 3/4 only).
- Hits (5, 6, 10, 11) stay local: each game decides hits on its own view (victim decides for its own car;
  puppet hits are cosmetic). Mirroring them (with the bit-30 pool-leak fix for dropped kind 5) is step 2,
  only if the in-game test shows it is needed.
- Item hooks are their own install group: if another mod holds those slots, races run without items.

Static analysis 2026-10-21 (reverse-engineer subagent, read-only, `re\bin\SSASR-unpacked.exe`; scratch
`re\m5-items\`). Verified by a second, independent pass the same night (see the end); nothing run in game. C = confirmed, I = inferred.
Companion: `notes/NPC-SYNC.md` (same event pipeline).

## Finding (C)
The PC game still runs the console's item system as events, offline too: every item give, use, hit and
homing lock is an event posted to the event manager `[scene+0x1374]`, whose post runs it at once through
the console handler 0x5c0260. The console split the work by ownership: the owner's machine created the
event (gated by 0x4e8910 "racer is local"), every machine executed it. On PC 0x4e8910 is a stub
(`b0 01 c3`, always local), so each PC rolls, picks up, fires and decides hits for every car, the
remote one included: hence a bomb on one screen and speed shoes on the other for the same car.

## Item lifecycle (C)
- Racer fields: held item +0xc1c (gadget hash), +0xc20 ammo, +0xc24 used; roulette +0x9cc/+0x9cd,
  +0xc0c..+0xc14; +0xc50 allows replacing; effects +0x1d08/+0x1d10; All-Star struct +0x1ff0, character
  All-Star object +0x2030; held-item model via 0x504130. Object id +0x2b4 = (serial<<5)|owner; racers 0xffe0|i.
- Pickup: item box sends message 0x06cd52a4 to racer vtable 0x6afbb0 slot 8 (data slot 0x6afbd0 →
  0x4f4f20, thiscall, ret 4) → 0x539c10. The roulette (0x85d1e53c) is added directly (0x539680, no
  event); other items post a kind-0 event (+8 racer byte, +0xc gadget, +0x10 count).
- Roll: 0x5393e0 (in gadget update 0x5398c0, from racer slot 4 0x4f3990) ends the roulette → 0x538bf0 →
  roll 0x537af0 (shared MT19937 0x4be040; position from lap record +0x80; weights 0xd12778/0xd126f8/
  0xd12738; All-Star only if 0x536100: items == 2, +0x2030 exists, nobody else in an All-Star, scene
  cooldown +0x1170) → kind-0 event.
- Use: 0x5398c0 reads the input block (buttons 0/1 = item) → 0x538bf0(0, back, flag) builds a 0x80-byte
  launch frame (projectile type, serial [0xcdd048], target, flags, pos +0x20, rot +0x30, dir +0x40, vel
  +0x50, speeds) → kind 1 (kind 2 when aimed) → executor FireGadget 0x539df0 spawns projectiles / boosts.
- Projectiles: pooled (vtable 0x6b2980, 0x5b0 bytes, pool [0xcdd034]/[0xcdd038]); spawn 0x5391a0 →
  0x538310; owner +0x310, homing targets +0x318/+0x31c; lookup by id 0x49d2e0.
- Hits: collision pass 0x51d060 asks 0x51cb30 who decides: racer vs projectile → the racer (victim);
  projectile vs projectile → the lower owner index. Decider local → kind-5 event (ids, shapes, the victim's
  shield state, a random byte) → executor 0x49d400 → projectile script → racer slot 8: FlipOut 0x4f1fe0,
  spin 0x4f1f90, effects 0x4eee30. Kind 6 (latch, 0x49d380) has no null check.
- All-Star: a kind-1 event with gadget 0xb678e022 → 0x4e8530 → 0x585b10 (racer+0x1ff0) state 2; each tick
  0x586980 runs for every car: 3 transform, 4 active (timer vs duration, which depends on race position,
  0x585de0), 0x586240 ends (cooldown scene +0x1160..+0x1170) → 5 / 1. Network21's message handler ignores
  item, hit and autopilot messages: items never change a puppet's controller state.

## Event manager (C)
Vtable 0x6b9c30. Allocate 0x516760 thiscall(ECX = [scene+0x1378], kind) ret 4. **Post = slot 4, data slot
0x6b9c40 → 0x5c0820**, thiscall(mgr, ev) ret 4: runs the event synchronously, keeps it; every 15 ticks
0x5c0890 runs the delayed pass (kinds 5/6) and frees. **Cap 16 posts per 15-tick window** (`[mgr+0x12] <
0x10`), beyond that the event is freed unrun (stock). Free 0x516900.

| Kind | Size | Executor |
|---|---|---|
| 0 | 0x14 | 0x539680 AddGadget |
| 1, 2 | 0xe0 | 0x5b7530 unpack → 0x539df0 FireGadget |
| 3, 4 | 0xe0 | sub-projectile 0x51ef10, 0x538310 |
| 5 | 0x14 | 0x49d400 collision |
| 6 | 0xc | 0x49d380 latch |
| 7 | 0x20 | 0x5365c0 (empty) |
| 8, 9 | 0xc | doors |
| 10, 11 | 0x10 | homing target set/clear 0x5253a0/0x525480 |
| 0xc | 0x14 | nav line 0x4e8880 (never runs on PC) |
| 0xd-0xf | 0xc | 0x4f2de0 FlipOut/SpinOut/4 s blink (only 0xf produced on PC: respot 0x5ad580) |
| 0x10, 0x11 | 0x14, 0x40 | results |

Kinds 1-4: vtable 0x6b9a84, quantizer vtables at +0xa8, +0xb8, +0xc4, +0xd0: never overwrite them.
Correction to `notes/LAPS.md`: 0x4e8910 is the ownership predicate, not the kind-0xc handler.

## Recommended design (the console's ownership model)
- Owner of an event = its racer: kinds 0-3 and 0xf byte +8; kind 4 word +0x10 & 0x1f; kinds 10/11 word +8 & 0x1f.
- Hook the post slot 0x6b9c40: our car's event → pass, and (if the window has room) send its bytes
  (+0x08..end, minus the quantizer vtables) reliably; the remote human's own events (not injected by us) →
  drop with 0x516900; AI → pass, never send (until NPC sync makes the host own the AI).
- Hits (kind 5, also 6/10/11): the victim's PC decides; decider = remote human → drop (the projectile
  flies through the copy, as on the console); decider = our car → pass, send only if every object
  involved is human-owned (AI ids are per PC and could alias).
- Receiver: at the start of our Racing / WaitingForFinishers updates (never in holds or pause):
  `ev = 0x516760(kind)`, copy the fields, set an "injecting" flag, call the original 0x5c0820; window full
  → retry next tick. Kind 0xf: `0x4f2de0(racer, 0xf)`. Events posted while an injected one runs are local
  side effects for the remote car: drop them (the owner's own copies arrive separately).
- Item boxes: filter racer slot 8 (data slot 0x6afbd0): remote human + message 0x06cd52a4 → return 1 (as the
  stock early-outs). Otherwise the local roulette blocks the owner's real give (AddGadget refuses while
  +0xc1c != 0 && +0xc50 == 0) and then clears the item.
- Both slots are .rdata, on-disk = decrypted bytes, outside the Roster Pack's fingerprint ranges:
  0x6b9c40 = `20 08 5c 00`, 0x6afbd0 = `20 4f 4f 00`. Check the values before swapping (Metal Sonic's
  33 patches are unknown).

## Risks
The 16-per-15-tick cap (injected events count); never rely on matching RNG (send values); AI items stay
per PC (a remote car can spin with no visible cause until NPC sync); projectiles spawn one network delay
late (advance by velocity x delay?); Giant Rocket steering not synced (detonation moves the copy); held
items can drift (some clears post no event: add held gadget/count to the Pose, correct with 0x537880 /
0x539680); All-Star length depends on position (add +0x1ff0 state to the Pose, end with 0x586240, I);
re-entrant post hook (injecting flag); the HUD shows held items for local players only (0x535770), so the
other player's item shows only as the model on the car (I); trophy/stat hooks (0x4a2610) run for the
remote car (I). Not items: Network21 slot 10 0x5a4aa0 may switch a slow car's controller to state 2 (unverified).

## First in-game step
Post hook for kinds 0, 1, 2 plus the box filter; log every give, use, drop and inject with the gadget
name. Test: car A takes a box and fires (rocket, speed shoes): window B shows the projectile or boost
from A's car and the item model on A's car. Then an All-Star (items ON): B shows A transform. Hits (5,
6, 10, 11), kind 0xf and Pose state fields are step 2.

## Order with NPC sync
Both use ownership. Doing NPC sync first (host owns the AI) lets the same post filter treat AI as
host-owned; doing items first is smaller and fixes the most visible problem. Either order works.

## Independent verification (2026-10-21, second reverse-engineer pass, `re\m5-verify\`)
CONFIRMED: post slot 0x6b9c40 = `20 08 5c 00` in both exes (vtable 0x6b9c30 slot 4; the only reference;
11 virtual call sites, none uses the event after posting; one manager class); cap check 0x5c084c; kind 0
built at 0x539cfa, kinds 1/2 posted at 0x5390dd; kind 5 only after 0x51cb30 returns 1 (victim decides);
racer slot 0x6afbd0 = `20 4f 4f 00` (only reference), all returns `ret 4`, early-outs return 1.
Corrections and missed risks:
- 0x516760 takes the kind on the stack and ignores ECX (operator new + constructor; kind at [ev+4]);
  0x516900 ignores ECX too (deleting destructor). Events with [ev+4] == -1 go to a special slot.
- The delayed pass runs every 15th manager update on the previous window's events: an event lives 15-30
  updates. The window stops rolling while the update at 0x49df80 is skipped (0x4cc410() == 0x31 or
  0x4cc130() == 5).
- Not every give/fire is an event: the roulette fires FireGadget directly and frees its event unposted
  (0x53901a), and 0x539c10 adds 0xd045a8ca directly (handed out by FireGadget at 0x53b2ec).
- Kinds 0xc and 0xd-0xf run only when 0x4e8910() == 0, so never on PC: injecting them does nothing, and
  calling 0x4f2de0 directly runs code the PC build never executes (avoid). Kind 0xf is still posted by
  respot and uses window slots.
- The original post frees a new event at once when the window is full: the hook must check
  `[mgr+0x12] < 0x10` and copy the bytes before passing it on.
- Validate before injecting: 0x49cb50 returns a pointer for any index < 8 without checking the racer
  count; kind 6's 0x49d380 has no null check.
- Other mods hooking these slots: UNVERIFIED (their DLLs not scanned); check the slot values at install
  (refuse if not stock), as for every race hook.
- Smallest safe first step: a pass-through post hook that only logs kind [ev+4], racer [ev+8], gadget
  [ev+0xc] and the window count [mgr+0x12], with HookEventPost `void __fastcall(void* mgr, void*, void* ev)`
  and HookRacerMessage `int __fastcall(void* racer, void*, void* arg)`.
