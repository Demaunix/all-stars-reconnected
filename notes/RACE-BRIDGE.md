# Race bridge (M3): lobby → one race on every PC → the other cars moving

Design written 2026-10-20 (session "M3 · Two Cars, One Track"). Facts: C = confirmed in the decrypted exe
(`re\bin\SSASR-unpacked.exe`) by the reverse-engineer analyses of 2026-10-20 (raw outputs in that session's
scratchpad), I = inferred, X = Codex's live-test record (not re-run by us). Status of each part is at the end.

## What we learned (the facts the design rests on)

**How a stock Single Race starts (C).** `fe_charselscreen` sends `fe_game_bridge` per-slot arrays
(character hashes `0xb27af30e`, devices `0x71612751` with -1 = AI/empty, two per-player appearance arrays, setup object
`0x692fcf3b` = `0xf68e21eb` for a normal race); `fe_trackselect` sends the course (`0x8af64da9`), then start
(`0x9b3cf8f9`). `fe_game_bridge`'s race routine (script offset `0x1e9b`) then, in order: picks AI characters
with script randoms (`1d02`, the shared MT19937), creates the 8 racers in slot order with native `0905
(character, stateName, device)` (humans "STATE_Control_StartingGridHuman" + `0908(localIdx++, racer)` to bind
the local player; AI "STATE_Control_StartingGridAI"), sets rules `0900(1 /*type*/, 0, laps, 0)`, loads the
course `0904(name)`, shows the load screen and switches front_main → game. Items and hazards are not passed:
`0x4c9dc0` reads them from the active profile's settings record.

**Network racers exist natively (C).** `0905(char, "STATE_Control_Network", dev)` makes a category-3 Network21
racer; `setControlState 0x507f00` (thiscall, ECX = controller) switches a racer's state later. Network21's
update (`0x5afd20` → shared tail `0x5a4b60`) reads no local input and **never places the actor**. Its input
block CS (controller+0x1c: steer +0x40, throttle +0x44, brake +0x48, buttons +8) is zeroed each tick by
`0x540620` and nothing fills it: the console network code would have. Game type 5 (`kGameType_NetworkRace`)
crashes (`0x574490` has no phase table for it): keep type 1.

**Race phases (C).** Machine `0xcf7a98`, one step per 50 Hz sim tick: Loading (50, waits for the loader) →
SetupObjectManager (51) → SetupRace (52, grid placement) → **WaitingToStart (55, update slot `0x6b20b8` →
`0x5b0f50`: music, REMOVE_LOADING_SCREEN, advance)** → TrackIntro (57, skippable) → CharacterIntro (58,
skippable) → **Countdown (59, update slot `0x6b2148` → `0x5b1ac0`)** → Racing (60). Restart path: RestartRace
(53) → WaitingToStartNoIntro (56, slot `0x6b20dc`).

**Grid (C).** Mode 1 shuffles the non-StartingGridHuman racers with the shared RNG, then appends the
StartingGridHuman racers **in creation order** at the back (placement loop `0x49f899`, "GridPosition%02i").
So the grid is the same on every PC only if the creation order, the states and the RNG state match. Codex's
start jump (R14, X) came from each PC putting its own human last.

**Randomness (C).** One global MT19937 (state `0xce7c58`, index `0x6ecb80`), seeded once at start-up from the
clock; `0x4be160` reseeds it (stdcall, seed). Script randoms, the grid shuffle and AI all draw from it.

**Codex's evidence (X).** The hold-then-countdown idea worked between two real PCs ("countdowns match", R14-R18).
Its one lobby-started race (Kit39, Test028) crashed during loading with a null course graph (`[0xd12560]`,
read without a check by the HUD at `0x4e81a0`); that start replaced the countdown update and sent the start
messages itself. Remote cars as Network21 puppets moved smoothly only once playback was paced by game ticks.

## The design

### 1. Lobby: what everyone agrees on before Start
- Each member publishes Steam lobby member data: `racer` (identity hash of the chosen racer) and `ready`.
- The host publishes lobby data: course (ordinal + name hash), laps, AI setting, items, catch-up.
- The host's Start is enabled when every member has a racer and is ready (at least 2 humans).
- This closes M2's "settings and racer choices are not shared" gap.

### 2. Start: the host's contract
On Start the host builds a contract and sends it to every guest (reliable P2P message `Contract`, `src/race_wire.h`):
race id, RNG seed, course, laps and rules, and the **full 8-entry roster in creation order**: humans first, in
lobby order (host first), then AI characters chosen by the host (excluding the humans' racers). Every PC
creates exactly this roster in exactly this order, so every PC builds the same grid.

### 3. Each PC starts an ordinary Single Race
- Our lobby script sends `fe_game_bridge` the same messages as the stock Single Race path (one local human,
  devices -1 elsewhere, setup object `0xf68e21eb`, the contract's course), so the stock race routine runs.
  **Exact message formats: pending RE follow-up (see Status).** This is the step that crashed Codex, so before
  releasing the hold we also check that the course graph `[0xd12560]` is non-null.
- **0905 hook** (native table slot `0x6f6004`, armed only for a contract): call k creates `roster[k]`;
  humans (local and remote) as StartingGridHuman, AI as StartingGridAI. The remote humans get a device that
  can't read local input (to confirm). The script's `0908` binds whichever racer it just created as player
  `localIdx`; a **0908 hook** (slot `0x6f6010`) makes sure only our local racer is bound, as player 0.
- **SetupRace hook** (update slot `0x6b204c`): reseed the RNG with the contract's seed once, then run the
  original. The grid shuffle then matches on every PC.

### 4. Two holds, never a replacement
Every hook here is a 4-byte data-slot swap in `.rdata` that calls the original unchanged, or simply doesn't
call it yet. No code bytes are patched (the Roster Pack fingerprints code ranges).
- **Hold A, WaitingToStart** (slots `0x6b20b8`, `0x6b20dc`): until released, return without calling the
  original. Loading is done, racers exist and are placed, the loading screen is still up. On the first held
  tick, switch each remote racer to Network21 (`0x507f00(controller, 0x15)`), and send `Loaded`. The host
  sends `Go` when everyone is loaded; guests release on arrival; the host releases half a round trip later.
- **Hold B, Countdown** (slot `0x6b2148`, first call only): the intros are skippable per player, so the PCs can
  drift by a few seconds. Hold before the first countdown step until everyone has reached it, then release
  the same way. After that the stock countdown runs untouched on each PC (1-second timers on each side).
- **The intros (M6, `re\m6-intro\FINDINGS.md`)**: between the holds, a press at the course or racer intro is a
  vote (`IntroVote`, reliable) instead of a local skip (the stock skip is held off through the intro state's gate
  byte); the course intro's CONTINUE reads "CONTINUE (X/Y)"; once every player still in the race has voted,
  every game advances the stock flow (`0x4cce60`) to the countdown; without a full vote the course intro
  ends at its record's 5 s (on PC it would loop its fly-by forever) and the racer intro at its own 5 s.
- Timeouts: never quit or cancel from inside a hold (Codex's cancel path is the prime suspect for its crash).
  After 60 s, release and race on; a remote car that never connected just stays on the grid.

### 5. Moving the other cars
- Each PC sends its own car's pose every sim tick (`Pose`, unreliable, ~4 KB/s per peer): position,
  rotation, velocity and angular velocity, stamped with the game's sim tick `[0xc2a008]`.
- Each PC plays the other cars back with `src/playout.h`: paced by sim ticks, a small adaptive delay
  (≈ network delay + 60 ms on a clean link), blending between poses and briefly extrapolating.
- **Pose facts (C, analysis 2026-10-20):** the racer object (what `0905` returns) *is* the car body.
  Position +0xc0 (x, y, z, metres, Y up), rotation +0xd0 (quaternion x, y, z, w), velocity +0x100 (m/s;
  integrated as pos += 0.02 × vel), angular velocity +0x120, matrix-dirty byte +0x1e0, life state +0xa1c
  (2 = active), controller `[+0x9f0]`, vehicle physics `[+0x9f4]`. `0x4f8440` (racer vtable slot 9) sets the
  position and the dirty byte. `0x4f1870` is a full teleport (resets velocities, snaps the scene nodes): for
  respawns only.
- **Sim step order (C):** phase machine (our phase hooks) → per racer controller/state update → per racer
  physics (`0x4f3990`, saves the previous pose) → per racer **end-of-tick commit, racer vtable slot 13
  (`0x6afbe4` → `0x4f3c50`)**: render pose, checkpoint sweep, scene nodes. The nodes keep the previous and
  current transform per tick and the renderer blends them, so one pose per tick looks smooth.
- **Hook (C):** slot 13. For a remote car (active, holds B onwards): write the played-back pose *before* the
  original; for our own car: read and send the pose *after* it. Physics still runs on the remote car (it
  pushes local cars like a heavy object), but its result is replaced each tick.
- **Remote car creation:** made as a StartingGridAI racer (a network racer gets no engine sound, `0x559f90`),
  device 15 (no pad), `+0x9e4 = 0x17` so the grid sort places it like a human; at hold A,
  `0x507f00(controller, 0x15)` and `+0x9e4 = 0x15` (racer reset reads it).
- Not done (later): CS inputs (steer, throttle) for wheel/drift visuals; item pick-ups by the remote car on
  the local PC (the checkpoint/trigger sweep runs for it); laps (M4: Network21 has no nav recovery).

### 6. Threads and transport
- Steam calls stay on our worker thread (M1 rule). During a race the worker wakes on an event as well as every
  few milliseconds, and moves P2P messages between Steam and two small locked queues.
- The game thread drains the inbound queue once per sim tick and stamps each pose with the tick (`Playout::Push`),
  and queues its own pose and the race messages for sending.
- ISteamNetworkingMessages channel 1 (`kRaceChannel`), sessions accepted only from members of our lobby.
- Before `SteamAPI_Shutdown`: close all P2P sessions and stop pumping (Codex's helper crashed in steamclient
  on shutdown races).

### 7. The lobby script and the start sequence (C)
- One native per lobby frame (`kNatFrame`) takes the racer, popup modes, rules, the Start key and the primary
  pad; the DLL posts member data and rules (only when changed), polls for an armed race and runs the host's
  Start. The lobby frame chunk is near the script's 16-bit jump limit (594 bytes were free; this uses 440).
- **Start key: F2 on the keyboard, Y on a pad** (native `041f`, pressed this frame). `0418` would read +0xf0,
  which is Enter on the keyboard (START on a pad), the same key as "change racer".
- Start sequence, in one frame: `160c(fe_game_bridge, 0xb27af30e, [8 character hashes])`,
  `160c(.., 0x71612751, [8 pads: ours at our creation index, -1 = AI])`, `160c(.., 0x8af64da9, "<course name>")`,
  `160c(.., 0x9b3cf8f9, none)`. Laps come from the `0900` hook (no profile write). The bridge sets catch-up 0
  for a single local player.
- **Why Codex's lobby start crashed (I, strong):** the bridge turns the course payload into text (`%u` for a
  hash), so a course hash became the track name "1641522085"; `0904` fell back to a generic track, the course
  graph `[0xd12560]` stayed null and the HUD crashed. We send the name, and hold A refuses to release while
  the course graph is null.
- Course table: the stock lobby's 24 courses in `build/race_tables.h` (generated and checked against
  `stock-fe_network_game.smosbin`). Egg Hangar's `doomeggzone_dlc` isn't in the lobby list yet.
- The two racers that need per-player appearance data are refused as online picks for now (the bridge would have to send that data separately).
- After the race the game returns to the main menu. Coming back into our lobby (return key `0x7ab1c9a7`,
  `fe_network_game` exports `0xc5cccce2`/`0x45b79703`) is for later.

## Status (2026-10-20, 0.3.0-m3a, commit `3262a2b`)
| Part | State |
|---|---|
| Race message format (`src/race_wire.h`) | done, offline-tested (4656 checks with playout) |
| Remote-car playback (`src/playout.h`) | done: clean link jerk 0.050 (= the path's own), rough link 0.125 with ≤ 13 ticks delay |
| Lobby member data, rules, Start (F2), start sequence | done, offline-tested (natives test, menu build checks) |
| Hooks 0905 / 0900 / 090a / SetupRace / holds / slot 13 | done, offline-tested against fakes (`tests/test_race.cpp`, 103 checks) |
| Independent review | first pass NOT YET (11 findings, incl. the bridge passing 0905's device and 0900's args as hashes, and host hold B never released); all fixed in 0.3.0-m3b (`7e0dd5b`); re-check PASS; its 3 minor leftovers fixed after |
| Rules not applied yet | items, catch-up and AI difficulty come from each player's own profile (only laps are shared) |
| In-game test | needs the user (two game windows) |
