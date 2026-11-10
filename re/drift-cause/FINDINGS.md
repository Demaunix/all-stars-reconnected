# Drift release: keyboard vs controller (reverse-engineer pass, 2026-11-09; saved by the main session)

Read-only. Scripts `re\drift-cause\scripts\` (releases.py, s3decay.py, yawstats.py, repress.py, s2steer.py, boostspeed.py, run.sh); outputs and decompiles in `out\` (local only); Ghidra copy in `ghidra\`. Data: `re\drift-parity\drift-log-20261108.csv`.

## Answer
No keyboard-vs-controller difference exists in the drift-release code or the logged physics: the same steering sign and size and the same re-press timing give the same result on both devices. The "swing across" is a NEW drift started on the other side: S is re-pressed within a few ticks while the counter-steer arrow is still down, and the new hop takes its side from the steering on that tick. The keyboard matters only through how it is driven: this player taps S very fast, and a key's counter-steer is always 1.0, above the 0.65 drift-start threshold. KEYBOARD-DRIFT.md §3e's "same value, different result" was a sign error in the grouping.

## Evidence (CONFIRMED unless marked)
1. **New drift side = steering on the re-press tick.** 0x5cbda1 `mov ecx,[esi+0x3c0]; call 0x540790` (CS+0x40) then `xorps xmm0,[0xc28900]`: latch +0x2a0 = -steer on the first state-1 tick (store 0x5cbf7d); the stubs 0x508170/0x508180 (`32 c0 c3`, always 0) stop a later flip; the latch sign picks the base angle +-tuningB+0x180 (0x5cbffc). Log row 3544->3545: steer +1.000 -> latch -1.000. Re-press within 8 ticks with the new latch on the opposite side: slip flips keyboard 24/24, pad 16/16. Same side: keyboard 0/18, pad 0/7. No quick re-press: keyboard 3/17, pad 2/23.
2. **§3e's "held INTO the turn" was the counter-steer.** `drift_dir.py` grouped by sign(steer)*sign(latch) = +1, but the latch is -steer, so +1 = the arrow opposite the hop's. State-2 yaw drive (+0x250, latch-signed, full lock): hop arrow kb +0.0220 / pad +0.0219; opposite +0.0015 / +0.0015; none +0.0113 / +0.0121.
3. **State 3 holds the angle only for steering toward the drift's own side.** 0x5cc3ab-0x5cc3e6: slip +0x260 > 0 -> C = maxps(0, racer+0xa90) else minps(0, ...); target = 0.5*(+-tB+0x180)*C^2. Counter arrow = C 0 = no steering. |slip| change per tick, first 4 state-3 ticks, full lock: hop side kb -0.0044 vs pad -0.0041/-0.0065/-0.0083; counter kb -0.0171 vs pad -0.0182/-0.0184; none -0.0138 vs -0.0150.
4. **The dp1 forced tests follow from 3.** Controller style on a keyboard (0.6): C^2 ~0.13 on the hop side -> straightens at -0.017..-0.019/tick; `DriftReleasing` counts state 3 while S is held, so every re-press stayed under 0.65: no hop (5/5). Keyboard style on a pad (-0.92 -> -1.0) strengthened the hop-side hold ("stayed in position"); the same run still swung across 5/5 on counter-arrow quick re-presses.
5. **Driving differs (log CONFIRMED; the link to the feel INFERRED).** State-2 ticks counter/hop/none: kb 3445/649/1702, pad 1968/518/1721. Quick re-press (<= 8 ticks): kb 42/59 releases, pad 11/34 (the trigger ramps 2-3 ticks). Counter arrow down at the re-press: kb 29/53. Tuning A +0x170 = 0.65 (read at 0x551a6b).

## Leads checked
- Drift button: no partial-value path. Every caller of the brake getter 0x5407d0: 0x54e010 hold timer (> 0 only), 0x4e7c40 air-trick smoother (racer+0xad0 > 0 only), 0x5910b0 clamp(gas-brake) for an anim/sound object (vtable 0x6b83b0), 0x59e7d0 lights. 0x5af150 builds no button bit from brake. (No direct `[x+0x48]` CS reads found: INFERRED.)
- Digital flags: none. `dl`/`dr` are our logger's channel values (ids 25/26); the only digital switches are the two always-0 stubs.
- Steering history: no lasting effect (latch magnitude only sign-tested, INFERRED from 0x5cbffc/0x5cbaf1).
- Boost: vehicle+0x460 is 0 in all 21,429 rows (not the drift boost); speed after release changes alike on both devices.

## Fix options (write only CS+0x40 in the Human-update wrapper, slot 0x6b1b8c = `50 f1 5a 00` re-checked; outside the Roster Pack ranges)
1. **Same-side tap:** on the S-down tick in state 3 (prev CS+0x48 = 0, now > 0, <= 10 ticks after the release), if the counter arrow has been held without a break since state 2, write CS+0x40 = -sign(latch) for that one tick: the hop keeps the old side. A fresh counter press during the gap passes through (a deliberate side change still works).
2. **Counter arrow under the hop threshold** (what a half-pushed stick gives): in state 3 only, cap a held-through counter arrow at 0.6; never touch the hop side.
Either way drop dp1's release-window remap (its 0.6 weakens the hop-side hold and blocks every re-hop in state 3).

## Unknown
Which releases the user FEELS as "auto-straighten" (medium confidence it is the counter-arrow quick tap). Settle with a labelled 2x2 run (keyboard / pad x quick tap with the hop arrow / the counter arrow held), ideally with a read-only marker key that writes `# MARK` into drift-log.csv.
