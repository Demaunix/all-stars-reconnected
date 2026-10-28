# Racer shouts: distance fading and extra lines (2026-10-23, reverse-engineer pass, session "Key Party")

The user's report (first 8-Sonic online race, 2 games on one PC + 6 test players, 0.7.0-m8h): "a bunch of Sonic grunts and other
sounds one after the other ... locally instead of in proximity?" C = confirmed, I = inferred.

## Answer
Mostly the game's own behaviour; the one-PC test setup about doubled it.
- Stock: every racer (computer ones too) says lines when bumped, hit, firing or overtaking (20 s cooldown per car and kind, a
  "within 2000 m" gate), all through one speech queue per game (one after another). The lines ARE positional: placed where the car
  was when it shouted (frozen), panned to the camera, fading with distance: full within ~20-40 m, silent beyond ~65-400 m.
- In this test: all 8 Sonic (one voice); both games on one PC's speakers (each plays its own copy of every computer racer's line,
  ~0.1 s apart, plus each player's own replayed): ~22 lines/min through the speakers vs ~11 for a single player; 16 intro lines at
  the start (8 per game, the user's m7zb choice; stock 1); a tight 1-lap pack.
- The mod plays no doubled lines in a game; its replays go through the stock line function (same gate, same 3D placement). One
  small extra: another player's race-end "4th or worse" NEG is replayed on the other screen as 2D (stock plays it for the local
  player only).

## Evidence
- 0x54d0a0: cooldown gate 20.0 `[0x6a4ae4]`; reset at 0x54d113; range test car `+0xc0..c8` vs listener `[0xbdbedc]+0xd4..dc`
  against `[0x6b4928]` = 4.0e6 (2000 m) at 0x54d1ca; then 3D mode (0x474a00(1)), no loop, `vt+0x2c(750, 2000)`, position fixed
  at the trigger, queued 3000 ms. The 2D branch (0x54d258) has no range test.
- The 750/2000 are lost: VO children are streamed; the streamer (vtable 0x6a2300) keeps min/max for itself (0x474970) and forwards
  only volume/position/pan/base volume to its XAudio2 voice `[+0x98]` (0x467630, 0x4670a0, 0x467040). The inner voice comes from
  the shared pool (`0x4651d0` at 0x466da6; list `[0xbdbedc]+0xc`), so it fades with the min/max of the last 3D sample that used it,
  or the constructor's 10/100 (0x47470e / 0x474758). Audio.xpac 3D ranges: mostly 20/65 (1056), 20/125 (720), 20/100 (433),
  30/200 (295), largest 100/600 and 70/700.
- 3D apply 0x466400 (slot 22, only if +0x28 == 1): X3DAudioCalculate (flags 0x21; CurveDistanceScaler FLT_MAX: pan only) then
  the game's linear gain (1 at <= min, linear to 0 at max; 0x4665e7-0x466633) into SetOutputMatrix (0x4666fc); listener per frame
  at 0x496b38 from `[0xc2a100]+0xa0..a8` (the camera, I). Units metres (I, strong).
- Logs (races e3d02e97 12:18:14-12:19:40, 0c593ce2 12:21:43-12:23:02 UTC): no doubles (the other player's car's shared kinds held
  at 0 every frame; "0 of the others' (counted here)"); replays "0 not queued here"; test players: per-car counts identical on host
  and guest (their own stock triggers on the guest); race 1: 16 lines per game in 86 s, race 2: 11 in 79 s; an item burst at
  12:19:08 gave ATTACK + 3 NEG on each game plus 2 announcer lines in ~3 s; the extra: "said the owner's: car 0 NEG" (2D) at
  12:19:40.258 and 12:23:02.401 on the guest.

## Suggestions
1. Release: no change needed. Tests on one PC: turn one game down in the Windows volume mixer, use mixed racers.
2. Optional (console-true): in `CountVoices` don't send `kind == 1 && group[kGroupMode] == 0` (only the race-end lines produce it),
   or replay those lines in 3D.
3. Leave the min/max quirk (stock PC sound layer, same on every PC).

Scripts: `scripts\` (run.sh, Decomp.java, minmax.py, peek.py, xpacxml.py, imports.py); outputs `out\` and `ghidra\` (gitignored).
