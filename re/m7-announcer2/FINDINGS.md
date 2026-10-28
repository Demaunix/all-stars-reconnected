# Announcer pass 2: fair overtakes, race-end lines, every racer's intro line, VO mirror (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`, own Ghidra copy in `ghidra\` (don't commit), capstone; one XML
from Audio.xpac in `data\` (don't commit)). C = confirmed in bytes/decompile, I = inferred. Builds on `re\m7-announcer\FINDINGS.md`
(stages A/B built: race.cpp "the announcer"). User's decisions (2026-10-22): overtake call-outs fair for every player; at the
finish the same top-3 lines then ONE line about a random player; every racer's intro line at its car (3D) as an experiment;
racer voice shouts the same on every screen (host decides).
Proposed slots hold the same values in the on-disk `re\bin\SSASR.exe`, none in the Roster Pack's 12 fingerprinted ranges:
`0x6afbbc`=`0x4f1eb0`, `0x6b21b0`=`0x5b23b0`, `0x6b2120`=`0x5b1390`, `0x6a4718`=`0x470bf0` (plus `0x6a4650`, `0x6afbe4`).

## 0. Corrections to pass 1 (C)
Race-flow state vtables are 9 slots (0x24): Racing `0x6b2164` (enter `0x5b2020`, update `0x5b2160`); WFF `0x6b2188` (enter
`0x5b2220` only sets the timer state `[this+0x10]=-1`, update `0x5b2380`); "race finished" `0x6b21ac` (enter `0x5b23b0` at slot
`0x6b21b0`, update `0x5b2530`). So `0x4bce30` (call at `0x5b24e9`) runs at the race end, after WFF's update saw race over
(`[0xcf7a6c]`), sorted (`0x4cc790`) and queued the advance (`0x4cce60`). WFF starts at the first finish of any car (types 1/2/5;
`0x5b21b1`: `call 0x4cc1a0` (finish count `[0xcf7a57]`), `jbe`, `jmp 0x4cce60`).

## 1. Fair overtake call-outs
Stock (C): racer vtable slot 3, data slot **`0x6afbbc`** (`0x4f1eb0`), runs when `racer+0xa1c==2` and tracker `[racer+0x2040]`
!= 0 -> `0x4c5310` (call `0x4f1f13`) -> `0x4c4840` (call `0x4c531d`) -> `0x4c3410` (call `0x4c4929`), the only callers. Every car
has a tracker unless vehicle `+0x10==2` (`racer+0x20fc` at `0x4e6344`; tracker made in `0x4ef820` at `0x4ef8b5`). Slot 3 runs for
every car (scene loop `0x49de2c` when `racer+0x2104 & 0x1f`; slot 13 ORs 0xf every tick at `0x4f3c6f`; network cars: I).
`0x4c4840`: skips if `racer+0x20fc` (`80 b8 fc 20 00 00 00 0f 85`), `[0xcf7a80] <= 8`, or `prev==-1 || prev<=cur` (prev =
`[tracker+0]`, cur = `[record(racer+0x9d8)+0x80]`); overtaken `O = 0x49cb50(ECX=[0xc29fc0], byte [0xcf79d0+prev*8])`; posts the
reaction (`0x4c4080`), calls `0x4c3410(R, cur, O, prev)` cdecl, always writes `[tracker]=cur`.
Bias in `0x4c3410`: limits `ebx=3` (8 if R is category 1), `ebp=4` (8 if O is category 1) (`0x4c3417`:
`bb 03 00 00 00 57 8b ce 8d 6b 01 e8 69 5b 02 00`); test `0x4e8f90` = `[[racer+0x9f0]+0x18]==1` (direct call, ~100 callers, no
slot). Coin `rand()&1`: 0 -> if `cur<ebx`, `0x4bc180(R+0x9d8, 1, cur)` (GAIN, LEAD at place 0); 1 -> if `cur<ebp` (the
overtaker's NEW place; `39 6c 24 18 7d 16` at `0x4c34ba`), `0x4bc180(O+0x9d8, 0, prev)` (LOST, TRAIL at last place). The two VO
lines (after 5 s) have no bias.
`0x4bc180` cdecl: gates `[0xce7ba8]>=7`, subject not finished (`0x51a010`), `[0xcf7a51]`, `scene+0x1100>4`; `ce7bb0+=1`; if
`[0xce7bd4]>=45.0`: cue (25% generic via shared MT), `0x4bbec0(cue,2500,0)`, `[0xce7bd4]=0`. Timers rise by dt to 46 in `0x4bbf60`.
No data slot between the category tests and the line; a temporary category flip is unsafe (R's slot 3 reads it again at
`0x4c48bc`, `0x4c5368`: the human-only Showboat path).
**Smallest fix (host, while AnnounceSending): hook `0x6afbbc` and add the missed line afterwards.** Before the original: if R's
tracker exists, `R[0xa1c]==2`, `!R[0x20fc]`, `[0xcf7a80]>8`, `prev!=-1`, `prev>cur`: compute O, save `cd0=[0xce7bd4]`. After: only
if a non-local human (`g.racers[k<humanCount]`, not gone) is R or O, `cd0>=45` and `[0xce7bd4]==cd0` (stock didn't speak).
Stock windows `okS_G = cur < (cat1(R)?8:3)`, `okS_L = cur < (cat1(O)?8:4)`; fair windows `okF_*` with "is a human" for
"is category 1". Side: the single side with `!okS` (the stock coin landed there), or own coin if both `!okS`. If
`okF(side) && !okS(side)`: `0x4bc180(side==G ? R[0x9d8] : O[0x9d8], side==G, side==G ? cur : prev)` (re-checks every gate; our
capture sends the line). Private RNG for the coin (not CRT rand, not the shared MT). Exact alternative: set `[tracker]=cur` before
the original (stock skips) and redo the block (`0x4c4080`, a fair `0x4c3410`, `0x4c5ad0` twice).
Other biases: **ALERT `0x4bc9d0`** (C): local = `[[0xc2a068]+0x590]` via `0x4dd010` (`8b 0d 68 a0 c2 00 e8 ab 05 02 00`); silent
with no local player, if R's place is worse than the local's (`cmp [rec+0x80],edi; ja`), or within 50 (`[0x6a42a8]`) of the target;
else 50% MT skip unless R is local; cooldown `[0xce7bb8]>46` (shared with ATTACK/ATTFAIL); reached only from `0x536d10`
(`racer+0x1cfc` change, homing lock) called directly by gadget code (`0x520240`, `0x520470`, `0x521dc0`, `0x5253a0`, `0x525830`,
`0x529970`): no slot. Option: in HookRacerCommit watch `racer+0x1cfc` becoming non-null; if `[0xce7bb8]` stayed >46 and R is a
non-local human, re-check not finished, `[0xcf7a51]`, distance >= 50; cue 25% `[0x6f5104]` else `[V+0x72c+9*4]`;
`0x4bbec0(cue,1000,0)`; `[0xce7bb8]=0`. Or leave ALERT. Near view `0x4bbe10` re-verified always true with a local viewport
(250 `[0x6aa8cc]`, then `viewer-car <= 80` `[0x6aa8d0]`).

## 2. Race-end lines
`0x4bce30` runs once per PC at its race end (race-finished enter, slot `0x6b21b0`). On the host: every player finished or RACE
ENDS IN ran out (`0x4cdbf0` compares the category-1 finished count `0x4cbc70` with `scene+0x1104`, raised by the mod while others
race), then estimates (`0x4cd290`), `[0xcf7a6c]=1`; WFF's update sorts and queues the advance; MarkPlayersDnf/DnfLast run after
that update, before the enter (I: at the next flow flush). Finish list `0xcf7950` 8-byte `{u8 index, u32 reason}`, count
`[0xcf7a57]`, reset `{0xff,0}` at race start (`0x4cbdf0`); full and sorted here.
What it says (C): top-3 loop i=0..2: `racer=0x49cb50(ECX=[0xc29fc0], idx)`; VO `0x54d330(0)` (VO 0 SG_<CHAR>_POS, 2D, 3000) for
i=0, `0x54d350(0)` (VO 1 _NEG) for i=1,2 (thiscall ECX=`[racer+0x9f4]`, ret 4); cue `0x54d530(0x10+i)` (thiscall ret 4 ->
`[V+0x72c+4*idx]`); gates inlined; `0x45ffb0(cue,25000)`. The loop stops after a category-1 car's line (`0x4bcf29`:
`8b ce e8 60 c0 02 00 84 c0 0f 85`). Then the first category-1 car at place >= 3: VO 1 and RESULT4 (`0x13`, place <= 7 and reason
== 1) or FINFAIL (`0x14`) via `0x45ffb0(cue,35000)` (`83 fe 07 7f 0b 83 7d 04 01 be 13 00 00 00`). RESULT1-3 = top three;
RESULT4 = finished 4th or lower; FINFAIL = did not finish (reason != 1). Take names don't reveal the words (I).
Cue convention (C): `cue = hash("<PREFIX><CHAR>")`, `"%s%s"` at `0x6a46b4`, prefixes `0x6f5088[0..20]` (RESULT1_ `[0x6f50c8]` ..
RESULT4_ `[0x6f50d4]`, FINFAIL_ `[0x6f50d8]`), `0x4bbf30` -> `0x4947e0(str,len,0x4c11db7)`; per vehicle by `0x54d550` into
`V+0x72c..+0x77c`. RESULT/FINFAIL entries: probability 255, 30 s cooldown.
**Design:** hook `0x6b21b0`; before the original save `[0xc29fdb]` and set 1 (0x4bce30 exits at once, lines and VO:
`80 3d db 9f c2 00 00 ... 0f 85`; nothing else in `0x5b23b0` reads it); restore after. Then host only, while AnnounceSending:
optionally flush first (as LeaderLapCalls); `0x54d330(0)` on the winner's vehicle; for i=0..2 `0x4bbec0([V_i+0x72c+(0x10+i)*4],
25000, 0)`; then one random human outside the top 3 (host's private RNG): cue index `0x13` if place<=7 && reason==1 else `0x14`;
`0x4bbec0(cue, 35000, 0)` (optionally `0x54d350(0)` on that player's vehicle first). Every human in the top 3: no extra line
(confirm with the user). Queue cap 8 (the playing line keeps one): keep to ~6 entries. Guests: set `[0xc29fdb]` too (their
commentary is muted by +0x1350 until teardown; this also silences their local race-end VO) once the VO mirror exists. Nothing
flushes the queue between race end and results (`0x46d590` only at `0x5b0db5` and the TrackIntro/CharacterIntro/Countdown enters;
`0x45faf0` only at scene setup/teardown); GEN_END at results enter `0x5b30c0` queues behind (C).

## 3. Every racer's intro line, 3D at its car
VO sounders are named SounderGroups `SG_%s_POS|NEG|RCE_WON|RCE_LST|ATTACK|ALLSTAR|INTRO` (formats `0x6a8e10`, `0x6b51a8`,
`0x6b5198`, `0x6b5188`, `0x6b5178`, `0x6b5168`, `0x6b515c`); vehicle setup `0x559f90` fetches each with `0x45fbb0(hash)` (one
shared registry object per name, type `[0xbdbff4]`) into `V+0x4b4+4*idx`; VO 6 = `[V+0x4cc]`; cooldowns `V+0x4d0+4*idx` start at
20.0 (`[0x6a4ae4]`, ~`0x55a62b`-`0x55a6d5`). Sonic's intro group (Audio.xpac `00fd8b63`): E_SONIC_GAME_LOST_02, E_SONIC_OPEN_01,
E_SONIC_OPEN_02, E_SONIC_TOUR_WON_00; RANDOM shuffle norepeat; children 2D, streamed `WAV\CHVO\SONIC\*.str`.
3D without the queue (C, as `0x54d0a0`'s 3D path `0x54d1e3`-`0x54d23c`), S = the intro group: (1) `0x474a00(S,1)` (thiscall
ret 4: +0x28=1 3D, +0x34=1.0f); (2) `S->vt[0x34](0)` (`0x470600` ret 4: loop off); (3) `S->vt[0x2c](750.0f, 2000.0f)`
(`0x470490` ret 8; constants `[0x6b4924]`, `[0x6a46dc]`); (4) `S->vt[0x20](&racer[0xc0], &zero3)` (`0x46f910` ret 8: position at
+0xc/+0x38, forwarded to every child; skips moves < 0.5 only if S+0xd0); (5) `S+0xac=1` (as the queue at `0x46e6e7`; meaning I);
(6) play a forced take (C, the RANDOM pick's tail `0x46fe58`-`0x46fe71`): `0x476cb0(S)` (thiscall no args: +0x80=1, +0x87=1,
owner `[S+0x78]->vt[0x9c]`); `item = 0x55afb0(ECX=S+0xb0, k)` (ret 4; children at +0xb0: count, sentinel +0xb4); `item[+4]=1`;
`0x46f1c0(S, child=[item+0x28])` (thiscall ret 4: copies mode, position, min/max); `child->vt[0x38]()` (0 = started). Only when
`[S+0xcc]==1` (RANDOM), else stock Play `S->vt[0x38]()` (`0x470bf0`, no args).
Same take on every PC: child k minimising `Mix32(seed ^ slot*0x9e3779b9 ^ [child+4])` (child+4 = its name hash). Driver VO looks
language-independent (one CHVO set, I). 8 at once is fine: each character its own group/child; each child stream
(`iSAL1_Streamer_XAudio2`, vtable `0x6a2300`) has its own 0x30000 buffer (`0x466cc0`) and reader thread on Play (`0x467630` ->
`_beginthreadex(0x467210)`); no shared pool (C); XAudio2 voice limits I. Two cars with one character share S: play once.
Listener `[0xbdbedc]+0xd4..0xdc`, set every frame by `0x45fa30` -> `0x464270` (call `0x496b38`, before the audio update
`0x496b6f`); grid cars are inside min distance 750: all 8 full volume, only panned; pass a smaller min/max to `vt[0x2c]` for
audible proximity (tune in game). No position updates on a static grid; for moving cars `vt[0x20]` each tick while `vt[0x6c]()`
(`0x46f840`) says playing.
Timing: hook the CharacterIntro enter slot `0x6b2120` (`0x5b1390`) on every PC, play the 8 after the original returns (the enter
has flushed the queue at `0x5b14dd`). Silence the stock 2D line (only player 0's: `0x5b14e2`: `[0xc2a044]==1 && RetryCount==0` ->
`0x4bbda0(0)` -> `0x54d2a0`): before the original write 0.0f to the local vehicle's `[V+0x4e8]` (`0x54d2a0` needs >= 20). Enters are
not simultaneous across PCs (each ends its own intros; hold B lines them up): playing at each PC's own enter keeps the lines in step
with its own racer intro (I). Countdown enter flushes only the queue (`0x5b18c3`; audio manager `vt[0x24]` `0x4627a0` is a bare
ret): to cut lines still playing at the countdown, `S->vt[0x3c]()` (Stop `0x46f790`) when `vt[0x6c]` says playing.

## 4. VO mirror (C unless noted)
Queue `[0xbdbf04]`: slots +0x10, cap u16 +0x14 (8), count u16 +0x16, own sounders +0x18, run state +0x24, current sounder +0x28.
Slot 0x14: +0 sounder, +4 wait ms, +8 prio, +9 pause-after, +0xa serial, +0xc link, +0xe VO flag, +0x10 user. VO lines:
`0x45fef0(S,wait,prio,1)` cdecl -> `0x46de90(S,wait,prio,0,vo,0,0,0)` (appends at the tail; nothing when full; serial/link 0).
VO lines start only through the single-line start (`c6 81 ac 00 00 00 01 8b 0b 8b 11 8b 42 38 ff d0` at `0x46e6e7`; return
address **`0x46e6f7`**). Slots go when they stop playing, their wait runs out (the head never counts down), or a flush.
Host: in the queue Update hook (`ECX==[0xbdbf04]`) before the original: a slot with +0xe==1 whose sounder wasn't present at the end
of the previous update is new (appends only happen outside the update; a sounder can't be re-queued within its 20 s cooldown).
Car/idx: scan each car's `V+0x4b4..+0x4cc` (two cars with one character: ambiguous). 3D or 2D: `[S+0x28]` (set by `0x474a00` just
before queueing). Wait: slot+4. Send `{tick, car, idx, mode, wait, serial}`. Optional: 3D triggers out of the host's 2000 range
(cooldown reset, nothing queued): watch `V+0x4d0+4*idx` fall in HookRacerCommit, send as 3D with wait 3000.
Guest: mute local VO by writing 0.0f to `V+0x4d0..+0x4e8` (7 floats) for every car every tick (every trigger gates on
`20.0 <= cooldown`, e.g. `0x54d0ff`; ticked at `0x555dcd`: `f3 0f 10 83 d0 04 00 00 f3 0f 58 05 1c a0 c2 00`). Replay in the
Update hook before the original: `w = wait - lateness` (drop if <= 0); 2D: `0x474a00(S,0)`, `0x45fef0(S,w,0,1)`; 3D: if
`|racer[0xc0..] - [0xbdbedc]+0xd4..|^2 <= 4e6` (`[0x6b4928]`), steps 1-4 of §3, then `0x45fef0`; else skip as stock.
Forcing the take: hook `0x6a4718` (SounderGroup::Play, thiscall no args, the direct target of the `call eax`); filter
`_ReturnAddress()==0x46e6f7` and S has a pending forced take; on both PCs `child = argmin Mix32(seed ^ serial ^ [child+4])`
(serial = the number the host gives the line when it detects it, before its update can start it); play with the §3 emulation.
Without it the takes differ (CRT rand in `0x46fc40`).
Risks: game thread only; the queue vtable is shared with the music queue (filter ECX); 8 slots; a VO slot added and flushed
between two updates is never mirrored (rare).

## Confidence
High (C): the state-vtable correction; race-end timing and list; the overtake chain, windows and the `cur<ebp` detail; gates and
conventions; sounder layouts, 3D recipe, forced-take emulation; slot values in both exes; queue layout and start site; Countdown not
stopping direct plays. Medium (I): network cars' slot 3 every tick; DNF fix-ups before the finish enter; what +0xac controls; 8
streams together, grid attenuation, VO the same in every language; the ALERT reading. Nothing tested in game.
Files: `scripts\` (run.sh, Decomp.java, l1-l12.txt, peek.py, peekdisk.py, callscan.py, dispscan.py, off2va.py, xpacxml.py),
`out\` (d1-d12.txt).
