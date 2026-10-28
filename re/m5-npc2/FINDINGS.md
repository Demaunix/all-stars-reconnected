# M5 AI puppets: reverse-engineering checks (2026-10-21)

Reverse-engineer subagent, read-only, on `re\bin\SSASR-unpacked.exe` (scripts and raw output in this
folder: `scripts\`, `out\d1..d19.txt`, `statemap.txt`, `statenames.txt`, `gadgets.txt`, `inlinecat.txt`,
`xref1/2.txt`). Saved by the main session from the agent's report (the agent could not write files).
C = confirmed in bytes/decompile, I = inferred. Nothing run in game.

**Headline:** no category-2 racer on the guest (local human category 1, the other 7 Network21 = 3) has
no crash path. Five additions: a pose guard, item-free puppets, a correct hand-back, a tie-proof
results order, and never creating the AI as Network.

## Reference facts (C)
- Category = controller +0x18, written only by 0x507f00 (0x507f1e) and the controller-drop path 0x4f3a9a
  (category-1 only). State -> category map 0x507e90 (table 0x507edc): 3, 1, 9, 0x18 -> 2; 0x11, 0x17,
  0xf, 0x14, 0x23 -> 1; 0x15 -> 3.
- 0x507f00: 0x50d060 runs each stacked state's exit (slot 6) and deletes it synchronously; sets +0x18 and
  +0x134 (2 for cat 1, 1 for cat 2; cat 3 leaves it); 0x50e140 queues the new state (enter at the next
  state-machine pass).
- States (names 0x50c550, factory 0x50d420): 2 TrappedRecovery, 3 AIRacing (vt 0x6b8cb0), 7
  AIRacing_Finish, 0xf Respot, 0x11 Human, 0x15 Network (vt 0x6b1c3c), 0x17 StartingGridHuman, 0x18
  StartingGridAI (vt 0x6b1cc0), 0x1a Podium, 0x1c Parked, 0x24 RollingStart_Human_KeepDistance. Slots:
  1 msg handler, 2 enter, 3 update, 6 exit, 7 resume, 9 cursor recovery, 10 trapped timer.
- Top state id: `[[racer+0x9f0]+0x228]+4`.
- AI-behaviour table 0xd12c00 (count 0xd12bf0): added by state enters via 0x530e60 (0x5a53f0, 0x5abe20,
  0x5a6e60, 0x5ae0f0), removed by exits via 0x530f00 (StartingGridAI exit 0x5a52c0). Network21 enter
  0x5afd10 (`jmp 0x5a46c0`) / exit 0x5a4700 (empty) don't touch it: the hold-A switch removes puppets.

## Q1. Crash risk with zero category-2 racers: none (C)
All 13 IsAI (0x4e8fb0) call sites: 0x49ba29 (first-AI 0x49ba10, only caller 0x5ac540 = state 0x24
enter; null -> pops the state), 0x4e9da8/0x4eefc6 (status effect 5; cat 3 gets nothing), 0x4ec2da
(script property "isAI" 0x0ee27249, used only in Behaviours.sbn Basic_Shockwave/Basic_Weapon: hit
reaction tuning, cosmetic on a puppet, I), 0x4fd259 (cursor search, all paths, like M4's remote),
0x531056 (dead code), 0x5310a2 (callers 0x531260, 0x5aa5e0 null-check), 0x531102 (caller checks > 0),
0x537b4e (item roll: local human unaffected; puppets roll from the human table), 0x55a1da (audio, at
creation only: puppets keep the AI engine sound), 0x5dab41/0x5dac9b (zones: AI-only messages),
0x5dc194 (gadget hold timer, see Q5). No division by an AI count; 0x531890 divides by the (guarded)
human count. 21 IsNetwork sites are "IsHuman and not IsNetwork" or "not IsNetwork" before a pad read.
18 inline category reads compare with 1, an AI-only parameter, or `!=0 && !=4`.

**Trap:** never create the AI as STATE_Control_Network in 0905: scene+0x1104 counts category 1 or 3 at
creation (0x49b2cf-0x49b2fc; other writers 0x49e649, 0x49fef9, 0x4f3a7b). 7 network AI would break the
race end, the results flow, lap popups and the item table (0x537b65: 0x1100==8 && 0x1104>4 -> AI table).
The hold-A switch leaves 0x1104 alone.

Local player effects: (1) AI pace off the host's player only (rubber band 0x5aa5e0 in AIRacing,
0x49bb60(1) = category-1 racers); human catch-up 0x5ae2b0 needs 2+ local humans. (2) **All-Star
lockout** (mechanism C, frequency I): puppets still collect boxes (0x539c10/0x539680 have no category
test; ownership stub 0x4e8910 = 1), may roll the All-Star (0xb678e022) and keep it; while anyone holds it
at +0xc1c (or holder state +0x1ff0 not 0/1), 0x49d7b0 refuses an All-Star to everyone (0x49d7fb). A
puppet holding any item can't take another box (0x539680, +0xc1c != 0).

## Q2. Race end and results (C)
- 0x4cd290 estimates only racers not in the finish list (inline test 0x4cd390-0x4cd39a against
  racer+0x9d8; 0x4cd970 and 0x4cd090 use the same test). For each it writes +0x68 and appends with
  reason 6 after posting msg 10 / 0x1e and clearing input (0x508420): the same as 0x4ccbb0.
- 0x4cc790 qsorts [0xcf7a56] (racer count) entries, comparator 0x4cc670: unsigned rec+0x68 only, a tie
  returns 0 (CRT qsort not stable). Then rec+0x80 = rank; 0x4d3db0 points (GP only, standings table).
- Results row 0x58d890 reads rec+0x80, +0x68, +0x60, rec+4/+0 (mission only, I), racer+0x9dc, the
  finish-list reason (0x4cc2f0), the character hash. Rows follow the sorted finish list (0x4cc2d0).
- Differences left with equal times/reasons: (a) tied times reorder rows -> send ranks, or order the
  list; (b) reason 4 posts msg 0xd and 0x4e7940 sets +0x68 = 0xFFFFFFFF a tick later: use 1/2/6 (3
  eliminated) only; (c) 0x4cd810 (time limit) rewrites every +0x68 when [0xcf7a64] != 0 (not Single
  Race, I); (d) the guest's lap chain may add a puppet with reason 1 and its own time: overwrite
  +0x68/+0x60, set the reason (0x4cc050/0x4cc090, no messages), +0x74 = +0x84 freezes the record
  (0x51a090 entry test); (e) 0x4ccbb0 posts msg 10 and clears input: for the local human it starts the
  finish autopilot (the stock estimate path does the same).

## Q3. Finishing keeps a puppet in Network21; the podium does not (C)
Finish paths post msg 10/0xc/0xb/0xd to racer+0x2c0 and broadcast 0x1e; the racer receiver 0x4ee020
(vtable 0x6afba4) forwards to the controller, then HUD for local players (0x4ed290), clears held items
(0x537880), stops the All-Star holder (0x586210). Network21's handler 0x5afd50 (table 0x5afe54) ignores
7, 10, 11, 12, 0x13; the base handler 0x5a4500 has no case for 9, 0xd, 0x11, 0x1e, 0x1f. The host's
finished AI go to AIRacing_Finish (7) and keep driving. **Podium:** 0x5b2b00 / 0x5b3960 call 0x534f00,
which posts MSG_RACE_PODIUM (0xe) to every controller (0x534f59); the base handler switches to Podium
0x1a (0x5a45a0), category stays 3: pose writes would drag podium cars.

## Q4. Hand-back (C)
0x507f00(ctrl, 3): category 2, +0x134 = 1, as at creation. AIRacing enter 0x5abe20 rebuilds its state
(0x5a46c0 binds racer/controller/tracker; behaviour blocks from controller +0x250..+0x25c persist;
0x5aa0c0 lane offset from the current pose; re-registers in the AI table 0x5abf18). Console rule
(Network21 msg 6/8): `0x49d720() ? 0x18 : 3` (any racer's top state 0x17/0x18 = still on the grid);
StartingGridAI waits for GO (msg 4), so 3 after GO, 0x18 before. Restore racer+0x9e4 = 0x18 (soft reset
0x4efa90(1) -> 0x507f00([racer+0x9e4])). Unchanged by the switch: 0x1104, +0x134, +0x9dc, +0x9e8,
+0x20fc. Don't use the msg-6 route (leaves +0x18 at 3).

## Q5. What still runs for a Network21 puppet
Network21 update = `jmp 0x5a4b60` (0x5afd20): no UpdateGadgets 0x5a83d0, no slot 9 (0x5a4ca0), no slot 10
(0x5a4aa0 -> TrappedRecovery; called only from other states' updates 0x5a5f66, 0x5a6fd5, 0x5aca83,
0x5af186), no rubber band. RESPOT (0x13, posted by 0x5a4b60, 0x4f3a29, 0x4f3d16/0x4f3d94) is ignored;
the physics respawn 0x4f3270 needs CS button 0xb: no local respots (the host flags its respawns as
snaps). Item firing needs CS button edges (0x540520/0x540540/0x540560 in 0x5398c0); nothing writes a
puppet's buttons (0x5a47d0 skips network, 0x5a48d0 needs category 1, 0x540620 only moves 2->3/4->1, base
handler 0x2b/0x2c touch button 0xc only). Latent: All-Star gadgets run from the racer physics update for
any category (holder racer+0x1ff0: 0x586980 -> 0x5867a0 -> gadget +0x30); input via 0x5a0080 uses the AI
path for categories 2 and 3, and 10 of 20 gadget classes fire on rand or a hold timer (0x5dba40,
0x5dc160). A puppet can't start one (needs a button), but a replayed All-Star on a puppet would fire
locally (item sync).

## Crash risks to guard / fixes (as built in 0.5.0-m5e)
1. 0x507f00 only from a phase or commit hook, never inside that controller's callbacks; re-read
   [racer+0x9f0] every time (hard reset 0x4ef820 -> 0x4e6110 allocates a new controller).
2. Never create the AI as Network at 0905.
3. Results: reasons 1/2/3/6 only; the finish list stays at 8 (0x4ccbb0 de-duplicates).
Fixes: pose guard (write a puppet's pose/nav only while its top state is 0x15); item-free puppets
(0x537880(racer, 1), thiscall, ret 4, when +0xc1c != 0 and +0x1ff0 is 0 or 1); hand-back as above;
results ranks (+0x80) so tied times show in the host's order.
