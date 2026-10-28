# m8-offvoice: Metal Sonic's voice at the player's finish in an offline race (2026-10-24, reverse-engineer pass, saved by the main session)

The user's report (1.0.0-rc1): offline single race as Sonic, Metal Sonic's robot voice at the finish; one A/B race with our mod
parked had none (finishing orders not noted). C = confirmed in bytes / source, I = inferred.

**Verdict: stock behaviour; our mod cleared (C).** The race-finished enter 0x5b23b0 always calls **0x4bce30** at 0x5b24e9, which
(unless `[0xc29fdb] != 0` or 1 racer) plays the TOP THREE finishers' driver lines in **2D** (full volume, not positional): place 0
POS (0x54d330(0)), places 1-2 NEG (0x54d350(0)), each through 0x54d0a0 mode 0 (no distance test; the car's cooldown >= 20.0 gate),
plus the announcer's RESULT1-3 cue (Metal Sonic has none); the loop stops after the first human car; a human outside the top 3
gets NEG + RESULT4/FINFAIL. So an AI Metal Sonic finishing 1st-3rd ahead of Sonic says his line as Sonic crosses. The A/B
difference is most likely the finishing order (or Metal's 20 s cooldown).

**Our hooks offline (C, compiled DLL 07c4b3d1... read):** every hook passes straight to the original when no online race is
active (`Active()` false, phase kIdle, no peers): 0905/0900/2003/0912/0913/090a/1304/0916/1b01, the race-phase updates, racer slots
3/4/8/13, catch-up, the listener (only 0x28 while running), the speech update 0x6a4650 (phase gate -> original), the take select /
source, the race-finished enter (tail jump to 0x5b23b0), the Opa/pulse/gate All-Star slots, the event post, the dispatcher's
Before/After (read-only offline), background running (focus/input only), voice chat (no peers, no XAudio2). No voice state is
kept across races. Other mods' DLLs reference none of our hooked slots (I for computed addresses).

**If it ever needs proof in game:** a pass-through log at the race end of places 0-3: car, human, RESULT1 cue `[V+0x76c]`, the
POS/NEG cooldowns before/after (a drop from >= 20 to 0 = its 2D line played). Zero-code check: note whether Metal finished ahead
of the player in the top three.

Scripts: `scripts\` (callers.py, dlldis.py, gameaddrs.py, scan_dwords.py, strs.py).
