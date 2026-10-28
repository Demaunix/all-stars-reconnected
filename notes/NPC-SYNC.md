# AI racers in the same places on both screens (M5 groundwork)

Static analysis 2026-10-21 (reverse-engineer subagent, read-only: Ghidra headless, disasm, capstone scans
of `re\bin\SSASR-unpacked.exe`; scratch `re\m5-npc\`). Verified by a second, independent pass the same night (see the end); nothing
run in game. C = confirmed in bytes/decompile, I = inferred.

## Headline: the console's model was "the host owns the AI" (C)
- Message table (0x4d2130, jump table 0x4d228c): 5 `MSG_SWITCH_NETWORK_TO_AI_PACIFIST_CONTROL`, 6
  `..._TO_LOCAL_AI_CONTROL`, 7 `MSG_SWITCH_AI_CONTROL_TO_NETWORK`, 8 `..._TO_AI_PLAYER_LEFT`.
- Base state handler 0x5a4500: msg 7 → state 0x15 (Network21) (0x5a45ac `push 0x15` → 0x50d400/0x50e2b0);
  AIRacing 0x5a6e90 and StartingGridAI 0x5a5180 reach it. Network21's handler 0x5afd50 (vtable
  0x6b1c3c slot 1): msg 5 → state 6 (pacifist AI), 6 → state 3 (AIRacing) or 0x18 on the grid, 8 → 3/0x18.
- No PC code sends 5/6/7 (all 105 call sites of the poster 0x4d2bf0 checked); msg 8 only at 0x4f3a8f
  (controller dropped → AI). The deciding network layer was stripped from the PC build.
- Ownership check stubbed: 0x4e8910 = `b0 01 c3` (always "simulated here"); callers include item give
  0x539c10, item fire 0x538bf0, targeting, collision outcome 0x51cb30, receive handler 0x5c0260. Can't
  be changed without code patches: on PC every racer counts as local.

## Recommended design (option A, the console's)
1. The host sends its human and the 6 AI cars' poses (one batch per tick; AI may be 25 Hz).
2. At hold A the guest switches each AI slot (k >= humanCount) like the remote human (race.cpp HoldA):
   `0x507f00(ctrl, 0x15)` and racer `+0x9e4 = 0x15` (the reset 0x4efa90 re-reads it).
3. The guest's slot-13 hook writes each AI pose and applies its nav path (0x4e8880): closes Codex's
   "slot-9 gap" (state vtable slot 9 = 0x5a4ca0, stuck-cursor recovery, which Network21 never calls) for AI too.
4. AI results from the host: a Finished per AI slot through the existing ApplyFinishes path (0x4ccbb0 has
   no category check). Estimates (0x4cd290) use each PC's own distances, so the guest holds its race end
   (up to ~5 s, like the existing grace) until every AI has the host's result. Never re-sort after the end:
   0x4cc790 → 0x4d3db0 adds GP points and a profile stat (double count).
5. Host lost: hand the AI back with `0x507f00(ctrl, 3)` (or msg 6 via `0x4d2bf0(6,0,ctrl,0,1)`).
6. Respots arrive as pose jumps: set kPoseSnap (SendPose never sets it today).

Why not "simulate on both and correct" (B): the AI's internal state (item decisions, trapped recovery,
wrong-way, respot timers, RNG draws) diverges anyway, and each PC's AI would fire items the other never sees.

## What changes for the guest under A (C unless noted)
- AI behaviour is all in the states (AI update 0x5a6f60 calls state slots incl. UpdateGadgets 0x5a83d0);
  Network21 only runs 0x5a4b60 (no AI decisions, no item fire). So **guest-side AI fire no items: until
  item sync exists, host AI can hit the host's player but never the guest's** (fairness regression).
- Puppets still pick up item boxes locally (harmless, I) and react to local hits (FlipOut 0x4f1fe0,
  SpinOut 0x4f0730/0x4f0880, collision spin 0x4f2490): cosmetic only, the pose write overrides movement,
  and the host never sees the hit (I).
- Lap records and positions work for puppets (proven for the remote human, M4).
- Correction to LAPS.md: racer +0x20fc is not an "AI flag" but set when vehicle physics
  `[racer+0x9f4]+0x10 == 2` (0x4e633d); puppets update their cursor every tick (I, strong).
- Category 3 for AI puppets is fine (IsNetwork checks 0x4e8fd0 are "local human" tests); +0x9dc stays 8.

## Console event pipeline (for item sync later)
Events go through scene+0x1374 (vtable 0x6b9c30); its post method is data slot **0x6b9c40** (→ 0x5c0820,
which on PC dispatches straight back into 0x535090 → 0x5c0260). Kinds (factory 0x516760): 0 AddGadget,
1-4 gadget fire (posted by 0x538bf0), 5/6 collisions, 8/9 doors, 10/11 target set/clear, 0xc nav line,
0xd/0xe/0xf FlipOut/SpinOut/0x4e66e0, 0x10 one result, 0x11 all results + sort. Racers addressed by slot
(object id u16 at +0x2b4, low 5 bits = racer index, 0x49d2e0). Hooking 0x6b9c40 on the owner's PC could
capture every gadget event for replay elsewhere (I).

## Bandwidth
Pose = 72 bytes; 6 AI at 50 Hz = 21.6 KB/s and ~300 packets/s (sends are UnreliableNoNagle, one packet
each). Batch one message per tick (~353 B; ~170 B quantized) → 8.8 KB/s at 25 Hz (4.2 quantized).
`kRaceMsgMax` (lobby.h) and `kWireMax` (128) must grow. Playout: 25 Hz works with the existing buffer
(margin 4 ticks); give all of the host's cars one shared delay/cursor, or the AI shift against the host's car.

## Smallest first in-game step
Host: after the stock commit of each AI racer (Running/HoldB), send the existing Pose with slot = k.
Guest: accept slots >= humanCount from the lobby host only, one Playout per AI slot (shared delay
preferred), switch AI to Network21 at hold A, write poses. Logs: "AI k follows the host", pose counts.
Test: timer-ending race on one PC, Win+PrtScn of both windows at once mid-race: same AI order on both;
same AI order in both results tables (times differ ~0.1-0.2 s until the results step).

## Risks
Fairness until item sync; cosmetic local hits on puppets; guest POS flicker near close AI (latency); host
AI paces off the host's player; message size/queue limits; respot snaps; AI freeze if the host goes quiet.

## Independent verification (2026-10-21, second reverse-engineer pass, `re\m5-verify\`)
All load-bearing claims CONFIRMED in the bytes (0x4e8910 stub, 17 call sites; message names 5-8 and the
handlers; Network21 update = `jmp 0x5a4b60`, which never presses item buttons; 0x4cc790 → 0x4d3db0;
+0x20fc). Corrections and missed risks:
- The poster 0x4d2bf0 has 109 call sites (not 105); none sends 5/6/7. Not ruled out: handlers called
  directly with a stack-built message.
- `0x507f00(ctrl, 0x15)` sets controller category 3, so AI puppets also stop counting as AI at the 13 call
  sites of IsAI 0x4e8fb0 (category == 2): item-roll weights 0x537b4e, first-AI lookup 0x49ba10, AI
  counters 0x531056/0x5310a2/0x531102, 0x5daae0, 0x5dac40, 0x5dc160, 0x4fd220. Effect UNVERIFIED.
- Host lost: use `0x507f00(ctrl, 3)` (sets category 2 itself); the msg-6 route doesn't write +0x18.
- 0x4d3db0's points/stat run only when the standings table `[0xd124f0] != 0`; callers 0x5b2399 (our
  WaitingForFinishers hook) and 0x5c0780 (kind 0x11).
- The msg-8 path 0x4f3a8f also lowers scene+0x1104 and sets +0x9dc = 8 (fields M4 uses).
