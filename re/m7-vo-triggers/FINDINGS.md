# Driver voice (VO) triggers: who fires what, on which PC (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`; Ghidra copy `ghidra\`, don't commit); saved from its report by
the main session. C = confirmed in bytes/decompile, I = inferred. Builds on `re\m7-announcer2\FINDINGS.md` §3, §4.

**Finding: driver VO can't be "the host decides".** Five kinds fire only for the PC's own local human: drift POS, stunt POS,
respot NEG, the intro line, the race-end "4th or worse" NEG. The host never makes them for a guest's car, and a guest's hit lines
on the host come only from the host's cosmetic copy of the hit. Each car's owner decides its VO, as with items (the host owns the
AI).

## Q1. Every trigger (V = racer+0x9f4; 20 s cooldown per car per index, shared by every caller of that index)
Common (C): the cooldown is reset to 0 at 0x54d113 before the 3D range test, so a 3D trigger farther than 2000 m from the
listener (`[0x6b4928]` = 4e6) resets it and queues nothing. No VO path uses 0x4bbe10 or a random skip; CRT rand only orders the
two overtake lines and picks the take.

| # | Site | Event | Index, mode, wait | Which cars, which PC |
|---|---|---|---|---|
| 1 | 0x4c3410 (0x4c3466/73/80/8d) | overtake, race time > 5 s (`[0x6a4324]`) | POS overtaker + NEG overtaken; 3D, 3000 | any car with a tracker, every PC, from its own places (C; network cars I) |
| 2 | 0x4bce30 (0x4bce98/b9/f98) | race-finished enter | top-3 POS/NEG, then the first cat-1 car at place >= 3 NEG; 2D, 3000 | every PC; blocked on the host by HookRaceEnd's `[0xc29fdb]`; FinishLines calls 0x54d330/0x54d350(0) itself |
| 3 | 0x4ebe00 (slot 6, `0x6afbc8`) at 0x4ec133 | car contact | NEG on racer `[param2+0x10]`; 3D | every PC, any car incl. puppets (I) |
| 4 | 0x4f2490 (slot 13 when +0xbb9) at 0x4f25b3 | hard hit on racer +0xbc0 | NEG; 3D | as #3 |
| 5 | SpinOut 0x4f1f90, FlipOut 0x4f1fe0 (NEG at the end) | item/hazard hits (slot 8 handler, 0x4f2490, gadget 0x590cb0, 0x5d6xxx-0x5eaxxx) | NEG; 3D | the victim, on every PC where its copy registers the hit (owner real, others cosmetic) |
| 6 | slot 8 (0x4f4f20) | msg 0xb446dcd0 | NEG on the receiver (0x4f6033); 3D | as #5 |
| 6 | slot 8 | msgs 0x32b4127c (0x4f52f0), 0x89d4dfa0 and one more (0x4f6ac2) | ATTACK on the receiver (victim); 3D | as #5 |
| 6 | slot 8 | msg 0xe16400fe (to the attacker) | ATTACK on the attacker (0x4f6b07); NEG on the victim `[msg+8]` (0x4f6b1f), 2D if the attacker is cat 1, else 3D | as #5 |
| 7 | 0x4c4260 <- projectile spawn 0x5391a0 (FireGadget 0x539df0) | 8 projectile gadgets fired | ATTACK on the firer; 3D | every PC running the fire: owner + item replays |
| 8 | 0x4e8530 <- FireGadget All-Star 0x53b39a (missions 0x5c2dd0, 0x5c4be0) | All-Star starts | ALLSTAR; 2D, 10000 | owner + replays |
| 9 | 0x4e7430 <- 0x534f00 (Podium enter 0x5b2b00 = slot 0x6b21f8; Mission results 0x5b3960) | podium | slot 0 RCE_WON 0x54d370 (V+0x4bc); slots 1, 2 RCE_LST 0x54d400; 2D, 5000 | every PC at its own podium (corrects `re\m6-finish\FINDINGS.md`, which swaps them; bytes 0x4e7483-0x4e74aa) |
| 10 | 0x4e7880 / 0x4e78e0 (listener `0x6afba8`, msgs 0xb/0xc from 0x4ccbb0 reasons 3/2) | elimination / time-limit end | RCE_LST; 2D | not in our Single Race flow (0x4cd290 gives reason 1, we give 6; both msg 10) (I, strong) |
| 11 | 0x54f720 (0x556621) | stunt start (racer+0x1ee2) | POS; 3D | +0x1ee2 set only by controller button 2 in the air (0x4ee5db-0x4ee5f9); Network21 input is zero: never network cars (C); AI unknown (I); inside 0x555d90, which returns early for non-cat-1 cars far from the local car |
| 12 | 0x54fe10 (two calls) | 0.6 s into a drift, each drift-level change | POS; 3D | cat-1 gate 0x54fe31 + V+0xb0==0: local human only |
| 13 | 0x550e50 <- 0x4e71c0 <- respot states 0x5ad850/0x5adae0/0x5adeb0 | respot in STATE_Racing | NEG; 3D | needs a local viewport (0x4a0c40 != -1): local human only |
| 14 | 0x54d2a0 <- 0x4bbda0 <- CharacterIntro enter 0x5b14ff | racer intro | INTRO; 2D, 3000 | one local player, RetryCount 0, player 0: local only |
| - | 0x54f6b0 | - | - | dead code (no call, jump or pointer) |

Cooldowns tick +dt at 0x555dcd (inside 0x555d90 <- 0x55aab0 <- physics slot 4, every car). The VO groups exist for every vehicle
(stored at 0x55a163 before any category test) (C).

## Q2. Which PC fires what
Host's own car: all kinds, on the host. A guest's car: #11-14 and the race-end 4th+ NEG only on the guest; #1, #3-6 on each PC
from its own view; #7-8 wherever the item event runs. AI: on the host for #1, #3-8 and maybe #11; never #12-14. Host-only capture
would miss a guest's drift POS (probably its most frequent), stunts, respots and intro; its hits would come from the host's
cosmetic view; 3D lines out of the host's 2000 m would be lost. Owner-decided is required.

## Q3. The voice queue (§4 confirmed and corrected)
- `[0xbdbf04]` is created with cap 8 (`6a 08` at 0x460512); the music queue `[0xbdbf08]` cap 12 (0x460547). Commentary shares
  `[0xbdbf04]` (CommentMan +0x2c): 8 slots in all, the playing line included (C).
- Single-line start 0x46e6e7 (`c6 81 ac 00 00 00 01 8b 0b 8b 11 8b 42 38 ff d0`), return address **0x46e6f7**; VO lines never take
  the paired start at 0x46e72a (serial 0). Play slot `0x6a4718` = 0x470bf0; update slot `0x6a4650` = 0x46e430 (C).
- +0x10 points to an array of 8 slot POINTERS, each to a 0x14-byte record (0x46dc70); removal (0x46e000) copies record contents
  down, so a record address is no line id; the head is always `[[q+0x10]][0]`. Record (0x46de90): +0 sounder, +4 wait s32 (-1
  never expires), +8 prio, +9 pause-after, +0xa serial, +0xc link, +0xe VO flag, +0x10 user; VO lines use 0,0,0,0,1,0. A full
  queue drops the line silently. Waits count down only for slots after the head group (removed at <= 0); the head goes when Play
  returns non-zero, when it stops playing, or on a flush. The update does nothing unless +0x24 == 1, or while the pause timer
  +0x44 runs (C).

## Q4. Take pick and forcing
- All 147 VO groups (21 characters x 7) in Audio.xpac are `RANDOM type="shuffle" norepeat="1"`, children 2D (C; `vogroups.py`).
  Roster Pack racers not checked. 0x46f080(1) sets +0xcc=1, +0xc0=1 (shuffle), +0xc8=1, +0xc9=100 (no probability roll) (C);
  that the XML keeps these defaults: I.
- The shuffle pick (C): if every item has item+4 set, clear them all; draw k = round(rand/32767 * count) (k == count redraws);
  accept an unplayed item, or a played one on the 5th try; item+4 = 1; push the child, ECX = S, `call 0x46f1c0` (ret 4); jump to
  child->vt[0x38] (0x46fe58-0x46fe71). 0x55afb0(ECX = S+0xb0, k) uses the same indexing and only moves the iterator +0xb8/+0xbc;
  0x476cb0 always returns 0.
- Forcing is safe when filtered on `_ReturnAddress() == 0x46e6f7`, a pending entry for ECX, and `[S+0xcc]==1 && [S+0xc0]==1 &&
  [S+0xb0] > 0`: 0x476cb0(S); reset the bag if exhausted; item = 0x55afb0(S+0xb0, k); item+4 = 1; 0x46f1c0(S, [item+0x28]);
  return `[item+0x28]->vt[0x38]()`; otherwise the original. CRT rand is per PC: skipping draws harms nothing.

## Recommended design
1. Capture on every PC for its own cars (its player's; the host also the AI): in HookSpeechUpdate (`ECX == [0xbdbf04]`), before
   the original, once per frame: a cooldown below its last sample = that index just fired (the only writers: the triggers, the
   tick, setup 0x55a683-0x55a6d7; C in vehicle code, rest I). Catches 2D, 3D and out-of-range 3D lines with no trigger hooks.
   Wait by index: 0/1/4 -> 3000 (3D in race), 2/3 -> 5000, 5 -> 10000, 6 -> §3. The owner picks the child at capture (its own
   bag) and arms `pending[S]` before the original can start the line. Send `{race, tick, slot, idx, mode, wait, child index +
   count}` reliably; receivers accept it only if the sender owns that car.
2. Mute the cars this PC doesn't own, from hold B to race-finished: 0.0f every frame to V+0x4d0, +0x4d4, +0x4e0, +0x4e4
   (indexes 0, 1, 4, 5). Leave 2/3 (only the podium: local on every PC, take from seed ^ slot ^ idx); 6 per §3. FinishLines: 20.0f
   into the cooldown just before each 0x54d330/0x54d350 on a car the host doesn't own, and send those lines too.
3. Replay: w = wait - lateness (drop if <= 0); null-check S; §4's recipes (2D, or the 3D steps against this PC's listener); then
   arm `pending[S]`.
4. New slot hook `0x6a4718` (stock 0x470bf0, the same on disk): the forced take above; a FIFO per S; drop entries older than
   wait + 0.5 s.
First in-game step: log only (no mute, nothing sent): count captured lines per car and index on each PC, to show the gap.

Confidence: high (C) for the trigger table, gates, modes, waits, queue layout, cap and pick; medium (I) for slots 3/6 and hits
running on network cars, AI stunts, the XML-to-field mapping, the frame order (sim ticks, then audio update), and nothing outside
vehicle code writing the cooldowns.

Files: `scripts\` (run.sh, Decomp.java, l1-l4.txt, callscan.py, dispfind.py, msgpost.py, hashmatch.py, vogroups.py, peek.py,
peekdisk.py), `out\` (d1-d4.txt, f4f20.txt, f55d90.txt), `data\` (Audio.xpac extracts: don't commit).
