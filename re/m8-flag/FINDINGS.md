# m8-flag: the game list's flag cell (audit A3) and the catch-up flip (audit C2)

Reverse-engineer pass 2026-10-23 (session "Chao Goes Public"), saved by the main session. C = checked in bytes this pass, I = inferred.

## Q1: the host's nationality flag in each game-list row
- **Nationality** = dword at **profile + 0x18** (profiles `0xc2a520 + idx*0x3af0`, active index `[0xcdb530]`; race.cpp's
  kProfiles / kActiveProfile) (C). Read: native `2003(24[, idx])` -> 0x5f9620 -> 0x5f8630, case 0x18 at 0x5f8740 -> 0x545ee0
  thiscall(profile), whose platform override (`[[0xc28ce0]+0x10]` vtable 0x67df48 slot 20 = 0x402f10) is a PC stub: always
  `[profile+0x18]`. Write: `2004(24, code)` -> ... -> 0x545760 `mov [ecx+0x18], eax` (fe_license_nationality 0x0978). The index
  is clipped only by a signed `cmp edi,4; jl`: guard 0..3 yourself.
- **Code -> sprite table** (30 pairs; byte-identical in PC fe_mainmenu 0x07b6..0x0888 -> g18, PC stock fe_network_game
  0x0bd9..0x0cab -> g15 (export a53c1dbc), PS3 fe_network_game 0x0bc6..0x0c9f -> g14) (C); country names from the atlas (I),
  codes = Xbox XC_COUNTRY (I): 103 7103b4d4 US, 6 03d7659e AU, 5 9edb3b89 AT, 8 c8f9043e BE, 13 a4b0616c BR, 35 d9d21c5e GB,
  16 46eeafc8 CA, 20 39ecf13c CN, 25 eb609853 DK, 32 b97d42e9 FI, 34 239f1c4a FR, 24 24398b27 DE, 39 7a1b0b7d HK, 46 9e15fc1b IN,
  99 0b95be26 TR, 50 bc74bbe7 IT, 53 7b85947a JP, 56 9764b9e4 KR, 71 3537e3b3 MX, 74 b4698c95 NL, 76 04f2a447 NZ, 75 b8c40e16 NO,
  84 006c260c PT, 88 8081c0e3 RU, 91 57d73ff3 SG, 31 2669a16a ES, 90 23015fd9 SE, 18 4cb915a0 CH, 97 4adeb129 TH, 0 47ace633 SEGA.
- **The flags are in the search screen's own scene** (a363a411, in fe_net_lobby's containers, EN E01CF5DC; one PTEX with 110
  sprites): all 30 flags and the 8 flag cells in all 7 stock languages and Metal Sonic's 7 replacements (`scripts\flagcheck.py`) (C).
- **Row node 1 is the flag cell** (TXTR, default sprite a4e21032 = a French-flag-looking placeholder, 50x50, left of the name;
  per row 4fe590fb, 4f95faf3, fe1fd0fb, cb969389, 8a533a8d, 5b34e35d, 6001dfc7, 0c710d38): hidden when unknown. Node 5 is the
  SIGNAL cell (37d7e2e7 for row 0, default 7bf528a4 = four green bars; untouched by our script) (C). Console: PS3 0x1fdd..0x1fef
  `0228(row[1], g14[r[0]])`, r[0] = the host's nationality code (C).
- **Build:** the host publishes lobby data "flag" (read on the game thread into an atomic); rows read it; native 0x7a27 (row) ->
  the flag's art hash or 0; show_results sets node 1 with 0228 and shows it, else hides it (~60 bytes a row).

## Q2: catch-up flips for one tick at lap crossings (PROGRESS: race 1a04099c)
- race.cpp `HookCatchup` ranks by `0x4e8740` = `0x519f40(rec)` = `(lap - 1) * L + wrap(live cursor - start line)`; live cursor
  `0x4e80f0(racer)`, L `[[0xd12560]+0x64]`, start line `[[[0xc29fc0]+0x10e4]+0x2f8]`, lap `rec+0x7c` (C).
- The lap (`rec+0x7c`) changes only in the race update (0x5b2110 -> 0x4cbbd0 -> 0x51a500, +1 at 0x51a6d2), which stores the
  cursor it counted with at **racer+0xb84** (0x4e8250) (C); the cursor moves first in each car's controller update (0x5097a0 ->
  0x507f60 tracker update, then the state machine -> Human 0x5af150 -> slot 11 0x5ae2b0, our hook) (C). So in the tick a car's
  cursor passes the line, the hook reads it one lap short (own car always; cars updated before ours too; I for the order): the leader
  ranks last for one tick (the logged flip).
- **Fix:** measure every car as of the last race update: `d - wrap(live cursor) + wrap([racer+0xb84])` (when the record has
  crossings); fallback: keep each car's last distance and add/subtract L on a jump of more than L/2. Log when they differ by more
  than L/2. Test: car A past the line, lap not counted (+0xb84 = L-1), car B at L-50: A stays leading.

Files: `scripts\` (run.sh, Decomp.java, DispWrites.java, sec3.py, flagcheck.py); `out\` (gitignored); `ghidra\` a 45 MB project
copy (not for git).
