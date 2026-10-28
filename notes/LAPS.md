# Remote laps and shared results (M4)

Static analysis of `re\bin\SSASR-unpacked.exe`, 2026-10-20: a first reverse-engineer pass (M3 session),
re-checked and corrected by a second one (M4 session; scripts and raw dumps in `re\m4-laps\`), with the
key bytes re-read by the main session. C = confirmed in bytes/decompile, I = inferred. Built in
0.4.0-m4a (`src/race.cpp`, "laps and results"); not yet tested in game.

## Why the remote car's laps go wrong (C)
- Every tick, every active racer (life `+0xa1c == 2`) runs the controller update `0x5097a0` (slot
  `0x6b0364`, one controller class for all categories), which calls `0x507f60`: matrix rebuild when
  `+0x1e0` is dirty, then the track-cursor update `tracker.0x4fd880(car+0x250)` (call at `0x507fb2`).
  Tracker = controller + 0x80.
- The cursor walks along its **current nav path** only. The path changes through a forced line
  (`tracker+0xac`) or a full search. Human17 recovers a stuck cursor through state slot 9 (`0x5a4ca0`);
  Network21 never calls it, so a remote car that starts on the "wrong" path can stay there all race:
  wrong lap distance, position and crossings (Codex's Kit09-Kit12 symptom).
- AI throttle: at `0x507f89` the cursor update is skipped 7 ticks in 8 if byte `car+0x20fc` is set (set
  once at creation for AI-driven racers, `0x4e6250`) and the node's `+0x7c` is nonzero. Our remote is
  created as AI, so it may have it (logged at hold A). Cost: crossings seen up to 7 ticks late.

## The fix: send the nav path, apply it with the console's own function (C)
- **Sender** (slot-13 after-hook, own car): `t = [car+0x9f0]+0x80`; path `p = [t+0x30]`, else
  `[t+0x80]` (last in-range path; the game's own fallback, `0x4fcab0`); `g = [0xd12560]`;
  `nav = (p - [g+0x48]) / 0x20` if aligned and `< [g+0x1c]`, else 0xFFFF. Sent as a u16 in each Pose.
  The path array is the course file's own, relocated in place (`0x4dbf50`): same index on every PC
  that has the same course files. The count is per course (logged at hold A).
- **Receiver** (slot-13 hook, remote car, right after writing the played-back pose, before the
  original commit): if `[g+0x48] + nav*0x20 != [t+0xac]`, call `0x4e8880` (thiscall, ECX = car, push
  nav, `ret 4`). It returns quietly if `g` is null, nav is out of range or the controller is null;
  calls `0x4fb3a0(t, path)` (sets `+0xac`, clears `+0x30/+0x34` only if the line changed) and
  `0x4fd880(t, car+0xc0)`: the snap uses the raw position we just wrote (no matrix timing issue). No
  events, allocations or category checks. Compared every tick, so a forced line set by the AI before
  hold A or by a local respawn is put back. Never clear `+0xac`.
- Then the stock lap chain counts laps for every racer (no category filter): Racing/WFF update →
  `0x5b2110` (skipped while paused) → `0x4cbb90` clock++, `0x4cbbd0` → `0x51a500` / `0x51a090`,
  `0x4cd090` (positions, finish list), `0x4cdbf0` (race end).
- The PC stub `0x4e8910` (`b0 01 c3`) is the console's network event 0x0c handler (nav line change).

## Lap records and the finish list (C)
- Record i (`[0xcf7a68] + i*0x90`) belongs to racer creation index i (`racer+0x9d8 = i`, `0x49fa2e`;
  checked at hold A). `+0x60` best lap, `+0x68` time of the last counted crossing (frozen once
  finished, `0x51a09c jae`), `+0x74` counted crossings (finished when `>= +0x84` = laps + 1), `+0x78`
  last lap, `+0x7c` lap shown ("LAP n"), `+0x80` position (0-based), `+0x8c` the racer.
- Times: `rec+0x68 = ([0xcf7a80] << 12) - round(f*4096)`, i.e. 1/4096 sim tick since GO. Ticks per
  second `[0xc2a020]`. The clock resets at SetupRace and counts only in the Racing and WFF updates.
- Finish list: `[0xcf7950 + 8k]` racer index (byte), `[+4]` reason (dword: 1 finished, 2 time up,
  6 estimated), count byte `[0xcf7a57]` (room for 16). Add: `0x4ccbb0` (cdecl index, reason; once per
  racer; posts finish events and clears the controller's input buffers `0x508420`). Set reason:
  `0x4cc050`/`0x4cc090`. Race over: `[0xcf7a6c]` (`0x4cc400`).

## Race end and results (C)
- Phases: Racing `0x3c` (update slot `0x6b216c` → `0x5b2160`) → WaitingForFinishers `0x3d` as soon
  as the finish list is non-empty (vtable `0x6b2184`: enter `0x5b2220` sets `+0x10 = -1`, update slot
  `0x6b2190` → `0x5b2380`, exit `0x5b2230`) → RaceOver `0x3e` (3 s) → Race_Results `0x46`.
- WFF update `0x5b2380`: `0x5b2250` (RACE ENDS IN, ECX = phase object), `0x5b2110` (lap chain); if the
  race is over: sort `0x4cc790` (qsort by unsigned `rec+0x68`, writes `+0x80`, adds points) and the
  phase switch. A race that ends in a Racing update is sorted in the next WFF update.
- `0x4cdbf0` reads `scene+0x1104` (dword) at entry. For a Single Race (no mission): ends when the
  phase timer `[0xcf7a8c] >= [0xcf7a90]` has run out (type 1 or 5), or when `0x1104` = finished
  category-1 humans (`0x4cbc70`). Then `0x4cd290` estimates the unfinished racers and sets `0xcf7a6c`. Its estimate (race mode 1/5, one category-1 human: path `0x4cd6d1`): a car whose record `+0x74` equals the lap count (byte `[0xcf7a55]`, i.e. on its last lap) is appended with reason **1** at its projected time (`0x4cd71b`-`0x4cd71e`), any other with reason 6 (`0x4cd7d2`, the results row's "DNF") (C, independent check 2026-10-22). Online, players who hadn't finished are marked 6 afterwards (race.cpp MarkPlayersDnf, 0.7.0-m7h).
- `scene+0x1104`: +1 at creation for categories 1 and 3 (`0x49b2ea`), category map 0x17→1, 0x18→2,
  0x15→3. Our remote is created as AI, so `0x1104 = 1`: each PC would end its race when its own
  player finishes, with the remote estimated.
- `0x5b2250` (only when `0x1104 > 1`, type 1 or 5): if `+0x10 == -1` and a category-1 human finished
  with reason 1 (`0x4ccad0`), starts the 30 s timer (`0x4ccf40(30)`, cdecl; timer object `0xcf7a88`:
  +0 ticks, +4 elapsed, +8 limit) and sets `+0x10 = 0`; afterwards shows RACE_ENDS_IN to the local
  players not yet in the finish list.
- With `0x1104 > 1` outside the updates: Race_Results exits into Mission_League (`0x5b25e7`), scenic
  camera instead of the podium (`0x5b3985`), lap-complete popup suppressed (`0x4ed555`). So the count
  is raised only inside the two updates.
- Console precedent, network event 0x10 (`0x5c0670`): `rec+0x68 = time; rec+0x80 = pos;` then
  `0x4cc380(racer) ? 0x4cc090(racer, reason) : 0x4ccbb0(index, reason)`.

## What 0.4.0-m4a does (design as built)
1. Pose carries the sender's nav path (wire version 2); the receiver applies it as above.
2. When our car enters the finish list, a reliable `Finished {slot, reason, rec+0x68, rec+0x60}` goes to
   the others (once, any reason).
3. On receipt, before the next Racing/WFF update (and only before this game's race is over): the
   remote's record gets that time and best lap, `+0x74 = +0x84` (finished: a later local crossing can't
   overwrite the time), and its finish-list reason is set, or it is added with `0x4ccbb0`.
4. While another player is still racing (not finished, not gone), or has finished while ours hasn't,
   `scene+0x1104` is raised by one around the original Racing/WFF update only, as in 2-player split
   screen: the race waits for the other player; after our finish the stock 30 s RACE ENDS IN covers a
   player far behind. If the other player finished first, our WFF hook starts the same timer itself
   (`0x4ccf40(30)`, `+0x10 = 0`), so the slower player also sees RACE ENDS IN.
5. Timer running out with a player still racing (independent review, finding 1): the first finisher's
   timer always ends first (the late game starts its own when our Finished arrives). So the first
   finisher's WFF hook holds the timer two ticks short (`[0xcf7a8c] = [0xcf7a90] - 2`; elapsed is
   counted only by `0x4cbbb0` inside `0x5b2250`, the end test is `0x4cdd1f`) for up to 5 s, until the
   late game's own estimate arrives as its Finished. Both screens then show that same estimate.
6. Our car counts as finished when it is in the finish list or its record says so (`+0x74 >= +0x84`,
   what the race end counts); a Finished that can't be queued is retried every tick.
7. Log: each human's lap changes, both finishes, "this game's race is over", the sorted results table,
   nav-path change counts.

## Known limits and runtime checks
- AI cars run separately on each PC (M5), so AI places and times differ between the screens; the
  humans' times and order match.
- Timeout (a player more than 30 s behind): the late game's estimate is used on both screens if it
  arrives within the 5 s grace; otherwise each PC shows its own estimate. RaceOver's enter (`0x5b247b`) reads `0x1104` and may skip its finish
  effect `0x49db20` when the race ends inside a raised update (minor).
- The start skew from hold B (about half a round trip) can flip a photo finish.
- Check in game: nav values and the course's path count on both PCs; whether the remote has the AI
  throttle; that the lap-complete popup still shows for our own laps.
- Pause is not shared (desyncs the clocks; "safe pause" feature).

## Addresses
| VA | What | Tag |
|---|---|---|
| 0x6b0364 → 0x5097a0 → 0x507f60 | controller update, cursor update call 0x507fb2 | C |
| 0x4e8880 / 0x4fb3a0 / 0x4fd880 / 0x4e8910 | apply nav line / set forced line / snap / PC stub | C |
| 0x5b2110 → 0x4cbbd0 → 0x51a500 / 0x51a090; 0x4cd090; 0x4cdbf0 | lap chain, positions, race end | C |
| 0xcf7a68 / 0xcf7950 / 0xcf7a57 / 0xcf7a6c | records / finish list / count / race over | C |
| 0x4ccbb0 / 0x4cc050 / 0x4cc380 / 0x4cc400 | add finish / set reason / in list / race over | C |
| 0x6b216c → 0x5b2160 / 0x6b2190 → 0x5b2380 | Racing / WFF update slots | C |
| 0x5b2250 / 0x4ccf40 / 0xcf7a88 | RACE ENDS IN / start timer / timer | C |
| 0x4cd290 / 0x4cc790 / 0x58d890 | estimate / final sort / results row native | C |
| 0x49b2ea..0x49b2fc / 0x507e90 | scene+0x1104 at creation / category map | C |
| 0x5c0670 | console network finish handler (precedent) | C |
