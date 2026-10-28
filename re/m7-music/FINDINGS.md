# Race music choice in online races (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`, own Ghidra copy in `ghidra\`). C = confirmed in
bytes/decompile, I = inferred. Not run in game. Do not commit `data\Shopping.dat` (game data) or `ghidra\`.
Question (user): do online races let each player choose the course's song from what they unlocked, as single player does?
The lobby music row the user first asked for was dropped (2026-10-22).

**Short answer: yes.** Each player's own choice works on their own PC; nothing forces a default track. One sync risk:
whether the picker draws from the shared RNG depends on how many of that zone's tracks this PC owns.

## 1. Where the choice happens (C)
No save or settings field holds a music choice: it is picked during the course intro, in the `hud` script.
- Trigger: the HUD_TRACKINTRO handler (hud 0x4e04, posted on the course intro's first tick, `re\m6-intro`) maps the
  course family to a shop zone id (hud global 2, e.g. seasidehill -> 5636b74f).
- List: native 200e (`0x5f8470`, call at hud 0x4e76: `32 01 0e 20 00 00 78 07`) builds `[count, (CardText, UnlockID) x
  up to 8]` from the zone's Music rows of the shop table (Shopping.dat in Base.xpac, sheet "Sheet1"), in shop order,
  owned rows only.
- Owned: `0x547af0`; profile = `0xc2a520 + [0xcdb530]*0x3af0`; music table at profile+0x33f0, 8-byte entries {flags,
  UnlockID}, count `[0xd21480]`; bit 0x08 = owned; `-unlock 1/5` or byte `[0xd214a4]` unlocks everything.
- 5 tracks per zone (MUSIC_xx_MAIN_A..E); one per zone is free and owned from the start (profile setup `0x546df0`, flags
  0x0C): count >= 1.
- Count 1, or RestartRace (state 53) on the state stack (`2108(53)`, `0x5fa050` -> `0x4c90d0`): track 0 at once, no
  banner, no random draw.
- Count >= 2: the music-title banner (scene ab2ae52f); the first track from native 1d01 (`0x5fcc80`, call at hud 0x4ee6:
  `32 02 01 1d 00 00 84 07`) = one shared-RNG draw (`0x4be170` -> `0x4be040`, rand % count); Left/Right (natives
  0423/0424, frame code hud 0x4b18..0x4dff) cycle the owned tracks while the banner is up; the racer intro hides it.
- A pick: native 1e0b (`0x5fcaa0`, call at hud 0x4b08: `32 01 0b 1e 00 00 ff 06`) -> `0x4dc730` sets
  `[[0xc29fc0]+0x1338]` to the track's sound (`0x577c60(UnlockID)`), plus `0x4a2610(0x13, id, 0, 0)` (achievement
  tracker; no RNG: I). Racing enter (`0x54a370`) plays whatever +0x1338 holds.
- Stock and Roster Pack hud modules are byte-identical here.

## 2. Our race start goes through it (C for the path, I in game)
Game type 1 and the normal TrackIntro, which posts HUD_TRACKINTRO. The mod never touches +0x1338, 200e, 1d01 or 1e0b.
Each PC builds its list from its own profile; Left/Right work during the (now longer) course intro. Local only, never sent.

## 3. 0x54a540 draws every time (C)
`0x54a540` draws MAIN_<A..E> at every TrackIntro enter (`0x54a5f5`: `b9 d1 8c c2 00 e8 76 3b f7 ff`); it redraws only
on a negative signed remainder (the same on every PC from the same state). The hud overwrites +0x1338 afterwards.
Equal draw count on every PC.

## 4. Desync risk (C in code; effect in game I)
The contract reseeds the shared RNG at SetupRace (race.cpp HookSetupRace, `0x4be160(g.c.seed)`), before TrackIntro.
A PC owning only the zone's free track (count 1, no draw) and one owning 2+ (one draw) are one draw apart from the first
course-intro tick on. Two PCs both owning 2+ draw equally (they may start on different tracks if their lists differ).
Left/Right never draw. How much a one-draw offset matters after the intro depends on what still uses the shared RNG
locally (the host runs the AI, items are owner-driven: maybe minor, I).

**Smallest safe fix (I):** reseed again at one logical point every PC passes after the intros, e.g. the countdown hold's
release, `0x4be160(g.c.seed ^ salt)` as the loading-tips reseed does. Keeps the players' local music choice.

Confidence: high on paths, addresses, table layout; medium (I) on the in-game effect of a one-draw offset.
Files: `scripts\` (Decomp.java, run.sh, natives.py, dumpmods.py, xfind.py, ssrt.py: Shopping.dat decoder);
`out\` (d1-d6.txt, natives.txt, shopping.txt: all 40 music rows, st-hud.txt).
