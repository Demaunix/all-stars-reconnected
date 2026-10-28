# m8-pause: the online pause's first pause left the car standing (2026-10-23, reverse-engineer pass, saved by the main session)

C = confirmed in bytes, I = inferred.

**Cause (C):** the "AI holds this car" latch is a byte at **racer+0x1fc8**. 0928 (handler 0x49d1e0, call at 0x49d253) and 0929
(0x49d260 / 0x49d2d3) are the only callers of **0x4eaa10(on)**, which returns at once when the byte already equals `on`
(0x4eaa24 `cmp byte [ecx+0x1fc8],al; je`), else stores it (0x4eaa2c) and posts telegram 0x32 / 0x33 to racer+0x2c0 (delay 1).
0x32 -> listener 0x4ee020 -> controller slot 1 0x507e70 -> Human handler 0x5ae1e0 -> 0x5a4500 (index 0x0b -> 0x5a4622
ctrl+0x24c = 1); the Human update 0x5af15d pushes state 0x21 (factory 0x50d86f, vtable 0x6b1e18, update 0x5a8180 -> AI racing
0x5a6f60) and pops it when +0x24c returns to 0. Only 0x4eaa10 touches +0x1fc8 (`scripts\ScalarScan.java`, `out\scalar1fc8.txt`).
The 8 racers are built once at start-up (scene constructor 0x49fed0 from 0x4971f0) and reused; the racer constructor sets
+0x1fc0/+0x1fc4/+0x1fc9 but not +0x1fc8; each race's controller is new (+0x24c = 0 at 0x507d3d).
**So:** a race left from the open online pause (RETURN TO LOBBY / EXIT: pause_menu's quit branch 0x06df-0x080e never calls the
unpause subroutine 0x985 that picks 0929) leaves the byte 1; the next race's first 0928 does nothing (Human state, idle pad: the
car stands); CONTINUE (0929) clears it, so the second pause works. Not a mod regression: a latent M6 issue; hits real players.

**Logs (C facts, I reading):** first pause in a fresh run worked (host 20:55:20, guest 20:56:39); the first pause after a race
left from the pause failed (host 20:56:39, 21:04:34; guest 21:04:26, whose Human-only catch-up hook still logged at 21:04:27),
the repause worked. Mod hooks (listener, 0928/0929 not hooked, catch-up, slots 4/7/8/13, background running) don't cause it.

**Fix (0.7.0-m8o):** clear racer+0x1fc8 at each racer's creation (Hook0905); log "the online pause's AI flag was left set by an
earlier race; cleared". Test: tests\test_race.cpp TestPauseLatch.
