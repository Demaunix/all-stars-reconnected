# Lobby rules: AI RACERS = OFF and CATCH-UP = ON (reverse-engineer report, 2026-10-21)

Saved by the main session. C = confirmed in code/bytes, U = unverified; nothing run in game. Ghidra headless on
`re\bin\SSASR-unpacked.exe` (project copy `re\m6-rules\ghidra`, not for git), capstone, script decodes; slot bytes
re-read in `re\bin\SSASR.exe`. Scripts and outputs: `re\m6-rules\scripts\`, `re\m6-rules\out\`.

## A. AI RACERS = OFF
### A1. How the stock game decides the car count
- 2003(i) via jump table 0x5f91d4 (C): 1 racers (+0), 2 laps (+4), 3 difficulty (+8), 4 items (+0xc), 5 hazards
  (+0x10 byte), 6 catch-up (+0x11 byte).
- fe_game_bridge (C; LDL/STL are top-of-stack relative, handlers 0x48fabb/0x48fff2): stock 0x20f7, Roster Pack
  0x2199, identical bytes `51 01 32 01 03 20 01 00 cb 03` then `07 03 43`: aiLeft = 2003(1) - H (H = slots with
  device != -1); difficulty == -1 and H > 1 -> aiLeft = 0. Slot loop (stock 0x211a-0x21ee, RP 0x21bc-0x2290):
  human -> 0905 GridHuman + 0908; other -> 0905 GridAI only while aiLeft > 0, aiLeft--; else no racer.
  Our race has H = 1: 2003(1) = N gives N creation calls, k = 0..N-1 (our humans).
- Not reachable in the stock PC menus (C): only fe_game_bridge reads setting 1; only defaults 0x4a9f90 and
  0x5f9250 (2004 index 1) write it; the PC Rules page has no racers row. Engine support: dev script
  `Scripts\2p_split_screen.sbn` runs `0900(1,0,3,0)` on smb_easy with 2 cars; the console's AI OFF showed POS 2/2.
- Engine paths bounded by the real count (C): SetupRace 0x5b0cd8 -> 0x4cca30([scene+0x1100]) sets [0xcf7a56];
  results module loops (0x4cbbd0-0x4cdd5f, race end 0x4cdbf0, estimates 0x4cd290) use [0xcf7a56]/[0xcf7a57];
  grid 0x49f6e0-0x49f8f0 and progress bar (0x501bc2/0x501c86, 0x501567/0x5017c4) by scene+0x1100; results rows
  `i < 090c(0,1)` (= [scene+0x1100], results 0x41d9, native 0x58f28b); HUD 0937 (0x58ef50) / 090c(0,1).
  0x49cb50(i) has no count check for i < 8: its 9 fixed-8 call sites are count-bounded/derived/guarded.
- U: cars on grid places 0..N-1; item/All-Star rolls by position (2nd-place row, not "last", in 2 cars); POS x/N.

### A2. Bridge changes (race.cpp)
1. `Cars() = g.c.ai == 0 ? g.c.humanCount : kGridSlots` (the contract carries ai).
2. Hook2003 racers site: PC 0x2101 (stock) / 0x21a3 (RP), code from PC-10 `51 01 32 01 03 20 01 00 cb 03`; return
   Cars() always during our start (also guards a save with racers != 8); int type (0x48c520). Drop the "AI off is
   not supported yet" log.
3. Hook0905: created at k == Cars()-1; Fail if k >= Cars().
4. HoldA: count != Cars(); **the guest's AI-puppet loop (k < kGridSlots) reads *(racer+0x9f0) on a null racer:
   crash**: bound by Cars(), don't arm aiPuppets without AI; the lap-record check loop too.
5. TrySendResults: k >= N has no record -> placesOk false, all ranks dropped: loop k < Cars(), rest reason 0 /
   kRankNone.
6. MatchHostOrder: returns early on kRankNone at k = N: Cars() for slotAt, the kFinishCount test, the r loops,
   memcpy Cars()*8 bytes only. With item 5.
7. Logs: LogPlaces, "8 racers" messages. LogResults already guarded.
8. RosterKnown: AI entries only for k < Cars(). RaceHostStart: keep 8 roster entries (ContractValid needs 8
   distinct). CarLeft: still makes a departed car an AI (decide: keep or park). Safe already: HandBackAi,
   ApplyHostResults, DropNetworkCarItems, item code (Runnable checks the scene count), progress bar / tags.
9. Tests: fakes with N racers and scene count N; 2003 pushed as int.

## B. CATCH-UP = ON
- 0x5ae2b0 (C) is slot 11 of the Human states (0x11 Human, 0x12 Human_Erratic, 0x13 Human_MonkeyBall; vtable
  slots 0x6b1bac, 0x6b1bdc, 0x6b1c0c, each `b0 e2 5a 00`), called by the Human update 0x5af2b0 (`call [edx+0x2c]`),
  thiscall(state), ret. State: +0x10 racer, +0x14 controller, +0x18 CS (= controller+0x1c), +0x1c tracker
  (= controller+0x80). +0x59 set at enter (0x5ae0f0) to GameSetup.Catchup == 1 (hash 0x9ab6eb36; readers
  0x5ae173/0x5ae199). The bridge writes 0x9ab6eb36 = 2003(6) only with >1 local human (stock 0x21f1): online 0.
- Each tick (C): return if [0xd127f2] (arena course) or +0x59 == 0; reset racer+0xb00..0xb0c = [0xc28570]
  {1,1,1,1}, CS+0x4c = 0; needs 0x531130() >= 2 (category-1 racers in table 0xd12c00, sorted farthest first each
  tick: 0x531260 qsort, compare 0x530d70 -> 0x519f40) and [0xd12bf0] >= 2; if 0x4fb650(tracker, 350.0) keep
  defaults; p = [controller+0x250] (parser 0x55cffc-0x55d074).
  Leader: gap = d(2nd) - d(self), d = 0x4e8740 (record distance, 0 before the first crossing): gap < p+0xa4
  (HumanSlowDownRangeMax) -> sd = p+0xa8 (HumanSlowDownMin); else if gap < p+0xa0 (RangeMin) t = (gap-a0)/(a4-a0),
  sd = a8*t + 1 - t; racer+0xb00 = {sd x4}.
  Last: gap = d(ahead) - d(self): gap > p+0x98 (HumanCatchupRangeMax) -> cu = p+0x9c (HumanCatchupMax); else if
  gap > p+0x94 cu = (gap-94)/(98-94)*9c; CS+0x4c = cu. Middle humans: defaults.
- Outputs feed the car's own physics: 0x4f3b50 -> 0x4ed5e0 reads CS+0x4c (vehicle +0x340); 0x4e7c2c multiplies
  racer+0xaa0 by racer+0xb00. Humans only; the AI pace (0x5aa5e0) is separate, always on (record +0x11 read only
  by 2003(6) at 0x5f90ee).
- "Host counts remote humans" won't work (C): a remote car is Network21 (no Human state); runs on each owner.
- Design: install group of the 3 data slots (original 0x5ae2b0); the hook calls the original (inert online),
  then for our race (running, racing, contract catch-up on, not RaceOver, state racer == our car) redoes the steps
  with humans = cars k < humanCount that exist, not adopted, player connected, not finished (excluding finished
  cars is a choice; stock after a finish U). Distances 0x4e8740 (thiscall(racer) float, read-only; guard null
  racer/record: 0x4cbed0 null at/past the count). 0x4fb650 read-only. Slots far from the RP fingerprints.

## Plan and risks
1. AI off: A2.1-A2.9 as one change; fakes first, then one 2-PC race with AI off. Risks: never run from PC menus
   (U); grid/item-roll/position wording, results stats 2004(83/84,...) (U); the guest crash in A2.4 if missed.
2. Catch-up: 3-slot group + ~50 lines; log rank/cu/sd on change; test: one car far ahead, the trailing car's
   cu > 0 in the log. Risks: remote distances 60-100 ms late (negligible); parameter values from the "FlatOut"
   human behaviour (strength U); 3-4 players: only first and last affected, as stock.
