# The race clock and real time (2026-10-29; reverse-engineer pass, two rounds)

Question: players' finish times disagree with what they saw (a player crossing behind gets a better time). Exe:
`re\bin\SSASR-unpacked.exe` (3c0c9f01...9973), `.data` defaults checked in the Steam exe (c110b4d3...1b03). Static analysis only.

## Evidence from a player's log (1.1.0, 25 online races, 2026-10-24..29)

`scripts\clockdrift.py <log>`: real time from the countdown's release + 3.0 s to the "you finished" line, minus the
reported time. This player's game lost 0-2.7 s in almost every race (e.g. 06c877a0: 2.74 s of 236 s); the other players'
games mostly 0.01-0.05 s (the network delay); one other PC lost 7.77 s (f9dc5418) and 8.32 s (5850418e), another 10.9 s
(261ef092). The game that loses time reports a better time while its car arrives later: f9dc5418 put a player 1st with
5'00"672 whose Finished arrived 5.7 s after the host's 5'02"727 finish.

## 1. The main loop's step timer (CONFIRMED)

- Clock: `timeGetTime` (app vtable 0x67df48 slot 4 = 0x402ee0, `jmp [timeGetTime]`), whole ms; no `timeBeginPeriod`.
  The pump 0x402890 calls 0x4a23c0 once per frame: `app+0xc` = now, `app+8` = ms since the last call. App object =
  `[0xc2a1b4]` (only writer the base constructor 0x4a2360; non-null for the whole run).
- 0x4a05f0, once per frame (from 0x4967d7): `acc = [0xc2a000] + elapsed` (float ms); `n = trunc(acc / 20)`;
  `[0xc2a000] = acc - 20n` with the UNCAPPED n; `[0x6ec868] = n`, then capped: flag `[0x6ec864]` (1 in the exe's data)
  gives `cap = [0x6ec86c]` = 4 (nothing writes it; the flag only at start-up, 0x4a0700, from a blackboard key
  "updatelimit"). The steps over the cap are dropped for good: **at most 4 steps (80 ms) a frame**.
- Step loop 0x4967c0: `for i < 0x4a06e0()` (`[0x6ec868]`); the sim block only while pause flags 14/15/16 are clear
  (0x496885); 0x4a0710 per step: `[0xc2a004]++` always, `[0xc2a008]++` when unpaused. Step length 1/50 s
  (`[0xc2a018]`, `[0xc2a01c]`, from 0x4a0740(50)); `[0xc2a020]` = 50.
- A frame loses time once leftover + frame >= 100 ms: one hitch of H ms loses about H - 80 ms; a steady frame rate under
  12.5 fps runs the simulation at (80 ms / frame time) of real speed. Faster frame rates keep exact real time.
- No time scale: `[0x6ec860]` is a constant 1.0 (three reads). Vsync (`Settings.ini VSYNC`, app+0x78) and the
  "cmdline_30Hz" present interval don't cause drift. Lost-device frames (fullscreen Alt+Tab, 0x497487 -> 0x403680),
  long main-thread stalls (window drag, a stuck input read) and the stock pause (0926, `[0xc2a015]`) lose time too.

## 2. The race clock (CONFIRMED)

- `[0xcf7a80]` in a timer object at 0xcf7a7c (+0 start limit, +4 clock, +8 limit; 0x4cdd60 sets it). +1 per call of
  0x4cbb90 while `[0x6ecd50]`, called only from 0x5b2110 (the lap step) in the Racing (0x5b2161) and WaitingForFinishers
  (0x5b238a) updates. Resets only before GO (0x4cc9b2 in 0x4cc930; 0x4cca30 -> 0x4cdd60 from the SetupRace enter
  0x5b0cd0 and 0x5b0d70); nothing writes it at GO or later in a Single Race (mission writers 0x4cc230, 0x4ccf80).
- Lap and finish times: 0x51a500 (via 0x4cbbd0, the per-record lap loop) = `(clock << 12) - trunc(f * 4096)`, f from
  0x4f8880 (the crossing's fraction within the step) -> 0x51a090 writes +0x68. At a crossing `+0x78 = time - old +0x68`,
  best lap +0x60 the minimum, then `+0x68 = time`; every tick the running lap `laps[+0x74] = (clock << 12) - +0x68`.
  A lap time is always the clock minus a stored stamp; finished records are frozen (0x51a09c, 0x51a538).
- Every reader (all raw references checked): 0x51a500 (laps), the race end 0x4cdbf0 and the Racing update's time-up
  0x5b2160 (only with a time limit `[0xcf7a64]` > 0, `>=` tests), the overtake watcher 0x4c4840 (`clock > 8`), the HUD
  native 0x58f260 (keys 7 TIME, 20 remaining, 2 running lap), the Time Trial ghost (0x5336f0 / 0x5335b0 / 0x532a30 /
  0x533740: sample index = clock / interval, capped), time-limit beeps 0x4ff880 (timed modes), race mode 2 (0x4ed320),
  mission code. **No equality test, per-tick delta or clock-indexed table in a Single Race: a forward jump is safe.**
- Commentary does not read it (its own float `[0xce7ba8]`, + step dt per race tick, 0x5b213c). Items, AI, catch-up,
  the replay player don't either. The race-end timer `[0xcf7a8c]` / `[0xcf7a90]` (RACE ENDS IN, the intros' and the
  countdown's phase timer) is separate: +1 per tick in 0x4cbbb0.
- The race end's estimates 0x4cd290 don't read the clock: a reference finisher's `T_ref = +0x68` and progress
  (0x519f40) give `+0x68 = trunc(T_ref / (P / P_ref))` (stretched below 0.75); fallbacks +0xa000 per place, a 30 s
  window. A corrected clock corrects them (the reference may be a remote car's written time: consistent when every
  PC corrects).
- Small quirk: splits use `clock - (1 - f)`, lap/finish times `clock - f` (up to one step, the same on every PC).

## 3. Measuring the dropped time (CONFIRMED)

`D = T - acc - 20 * ([0xc2a004] - j + n)` with T = `[[0xc2a1b4]+0xc]` (uint32 ms), acc = `[0xc2a000]` (float ms,
0..20, constant during the steps), n = `[0x6ec868]` (constant during the steps), j = the step within the frame. D stays
the same while the simulation keeps real time and grows by exactly the ms dropped (by the cap, skipped lost-device
frames, pauses if `[0xc2a008]` is used). Take it at a frame's first step (j = 0): the first race tick whose T changed,
with no step missed since the last one seen. The Countdown (0x6b2148 -> 0x5b1ac0), Racing (0x6b216c -> 0x5b2160) and
WaitingForFinishers (0x6b2190 -> 0x5b2380) updates run only inside the step loop (0x4968ad -> 0x49ade0 -> 0x4cbb80
-> 0x50e0e0), after the frame's 0x4a05f0, before the step's 0x4a0710.

## What the mod does (1.1.1, race.cpp ClockObserve / ClockStart / ClockCatchUp)

D is read at every frame start from hold B on; its value at the countdown's release is the base; each Racing /
WaitingForFinishers tick (before the stock update) adds the whole 20 ms ticks dropped since to the race clock, until
the race end. Every game does it, so every finish time and estimate is real time since the shared release + the 3 s
countdown. Not changed: the race-end timer (RACE ENDS IN lasts longer in real time on a PC that drops time) and the
best-lap record check (it gets the real lap time).
