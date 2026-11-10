# Drift release: why a keyboard straightens (reverse-engineer pass 2026-11-10; saved by the main session)
Read-only. Scripts re\drift-release\scripts\ (heldcurve*.py, s3exit.py, s3zero.py, s3equil.py, swing.py, edges.py,
overlap*.py, drive_summary.py, nostart*.py, run.sh); outputs in out\ (local); Ghidra copy in ghidra\.
Data: re\drift-parity\drift-log-20261108.csv. "own" = the drift's own side (steer sign = -latch), "other" = the opposite.

## 1. No edge, latch or device path in steering (CONFIRMED)
- 0x5af2f6-0x5af392: steer = RIGHT(id26, row[16]) + (stickX(row[4]) - LEFT(id25, row[15])) (subss 0x5af31e,
  addss 0x5af340), stored by 0x540590 (f3 0f 11 41 40 = CS+0x40) every tick from live channel values. Both arrows -> 0.
- Log: held arrow -> steer +-1.000 and racer+0xa90 +-1.0000 through every release (6008-6055: 48 ticks; 9100: 119).
- Drift starts from state 0 with an arrow held 67/81 ticks (7093, 7805): CanStartDrift's |filtered|<=|raw| never blocks.
- The release boost (0x553590 -> 0x587cf0 -> 0x4e9a00: racer +0x380..+0x45c) touches no steering field.

## 2. Release-phase mechanics, device-blind (CONFIRMED, bytes re-checked)
- Normal steering 0x578bf0 runs only in drift state 0 (0x578c00 cmp [esi+0x2d0],0 / jne). States 1-3 turn the car only by
  the drift yaw +0x250 and correction +0x210 (0x673d90).
- State 3 slip target = 0.5 * (+-tuneB+0x180 = +-0.9) * C^2; C = racer+0xa90 only when it has the slip's sign
  (0x5cc3ab-0x5cc3e6 maxps/minps with 0): only own-side steering holds the slide.
- State 3 -> 0 only when |+0x260| < 1/16 and |+0x210| < 1/32 (0x5cca99-0x5ccb63, byte 0x6ec732 = 1).
- Hop side = -CS.steer on the S-down tick (0x5cbdaf call 0x540790; xorps 0xc28900; 0x5cbdb7).

## 3. Same input, same result (CONFIRMED, log)
Sum of latch-signed (yaw+angle) over the first 4 release ticks:
- own side held: kb +0.060 (n=14), pad +0.051 (17)
- other side held: kb -0.071 (26), pad -0.069 (14)
At full own-side lock: identical 40-tick curves (kb 3, pad 8). The slip settles at 0.15-0.45, so the release phase never
ends while full lock is held (16 runs of 11-154 ticks, both devices; none ended by the slip rule).
This matches the user's dp1 results: pad snapped to +-1 "stayed in the position"; keyboard at 0.6 swung out
(-0.58 rad over 20 ticks).

## 4. Keyboard-only causes (log CONFIRMED; link to the complaint INFERRED, medium)
- Both arrows down = 0. Concentrated at the S release:
  - at the release tick: 9/63 keyboard releases; within +-5 ticks: 21/64; versus 2.4% of all state-2 ticks
  - pattern other -> both -> own (e.g. 11052, 12311, 12482)
  - those releases turn -0.015 rad in 4 ticks instead of +0.060: "straightens regardless of whether Right is held"
  - 6495-6525: both down through the release, then the own arrow lifted -> full straighten
  - A stick cannot point both ways.
- A key's counter-steer is always full strength: "other" at the release costs kb -0.064 vs pad -0.040 rad (4 ticks).
- Side held at S-up: kb own 33 / other 48 / both 14 / none 5 %; pad own 31 / other 35 / none 35 %.

## 5. Fix options (this game's own car; Human-update wrapper, slot 0x6b1b8c = 50 f1 5a 00 re-checked;
## after the original; write CS+0x40 only; apply the racer+0x1d0c bit-3 flip like DriftPlusOff)
- A1 keyboard -> pad (recommended): in drift states 1-3, when both arrows are down, the more recently pressed arrow wins
  (+-1; same-tick tie stays 0). Track dl/dr per tick via DeviceValue(row[15]/row[16]).
  The deliberate side switch keeps working and gets faster: the newest arrow is the new side, and a both-down re-tap
  now hops instead of waiting.
- A2 optional: in state 3 while S is up, cap a held other-side arrow at 0.6. Pass the raw value on the S-down tick,
  so the re-hop still sees 1.0 > 0.65 and the side switch is untouched.
- B pad -> keyboard: snap the stick to -1/0/+1 (hysteresis 0.55/0.35) in drift states 1-3 (dp1 did state 3 only).
- Not recommended: "held stick stops counting until re-centred" (no such keyboard rule exists).
  If ordered: fresh press = |x| < 0.24 (pad dead zone, 0x405ab0) then >= 0.65 (tuning A +0x170).

## Unknown
- Whether the user's described "release Right, re-press" is really lifting the counter arrow.
  Settle with an A1 test build ([Debug] toggle) or a labelled run.
- Whether a part-pushed own-side stick ends state 3 by itself: one sample, 10338 at 0.898 -> state 0 after 16 ticks.
- An S re-press while both arrows are down never occurred in this log.
