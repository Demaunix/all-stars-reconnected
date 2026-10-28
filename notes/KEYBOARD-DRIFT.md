# Keyboard vs controller drifting (research for a future fairness option)

Written 2026-10-21, read-only research. Nothing was run in game and no source was changed.
Labels: **C** = confirmed in bytes or Ghidra decompile of `re\bin\SSASR-unpacked.exe`; **I** = inferred (reasoning, not proven).
Raw decompiles and scripts: `re\kbd-drift\` (`out\d*.txt`, `scripts\`). The Ghidra project was a scratch copy outside the repo.

## In plain words

- The game has **no separate "controller drift" feature**. I found no code that treats keyboard and controller differently, and the name "Drift+" is not in the exe. The whole difference is how the steering number is made.
- **Keyboard steering is only ever -1, 0 or +1** (left key, nothing, right key). A **stick gives every value in between** (but nothing below about a quarter of the way, because of the pad dead zone).
- While you drift, the **amount of steering feeds straight into the drift angle of the car**. Full steering = tightest arc, no steering = the angle melts away and the car straightens. A stick can sit at "half" and keep a steady arc; a key can only be full or off, so players tap the key to fake "half". That is the effect you saw.
- To start a drift the game wants: **gas held + brake held + steering past a per-car threshold** (so "brake and drift share one key", as players say). Letting go of the **brake** key is what ends the drift on purpose; letting go of **steering** only removes the angle (I found no "steering is zero, stop drifting" test).
- If the keyboard fix is not wanted, the fallback (section 3b) is also possible: while a keyboard player is in the lobby, snap controller steering to full/none so a controller drift auto-aligns the same way. Same hook, host decides at Start, no code bytes.
- Smallest keyboard fix I'd suggest: a small wrapper that, **only for a keyboard-driven local player, while a drift is running and brake is held**, keeps steering at a moderate value in the last direction when the key is released. It is a lobby rule (host decides), changes no code bytes, and controller players are untouched.

## 1. Input path: key -> steering number -> drift

Objects: **CS** (control state) = `controller + 0x1c`, `controller = [racer + 0x9f0]` (C: `lea ecx,[eax+0x1c]` at 0x5af2ba). **Vehicle** = `[racer + 0x9f4]` (C by matching fields: `0x4e90f0` reads `[[racer+0x9f4]+0x2d0]` and `0x5cb4b0` reaches the racer through `[vehicle+0x3b0]`). Other vehicle pointers: `+0x3b8` = tuning block A (CanStartDrift reads its `+0x170`), `+0x3bc` = tuning block B (the drift update reads `+0x1d0`, `+0x1e0`, `+0x230`, `+0x2a0` from it), `+0x3c0` = CS (C: 0x54e03f `mov ecx,[esi+0x3c0]` right before the brake getter `0x5407d0`).

| Step | Address | What | |
|---|---|---|---|
| 1 | `0x406e00` | Input setup: 4 XInput pads (ctor `0x405890`), keyboard (ctor `0x405f00`, size 0x7f8), mouse. Keyboard default bindings are written right after the ctor: id->scan code S=id1 (`0x1f`), D=id3 (`0x20`), UP=id23, DOWN=id24, LEFT=id25 (`0xcb`), RIGHT=id26 (`0xcd`), SPACE=id31 (`0x39`), A=id32, ENTER, ESC. | C bytes; whether the saved/user bindings differ: I |
| 2 | `0x406230` (keyboard vtable `0x67e444` slot 4) | Reads DirectInput state (`0x406030`, `GetDeviceState`), then `0x4060f0` turns each key's bit 7 into a float channel `1.0` or `0.0`. **Pure on/off, no ramp.** | C |
| 3 | `0x405dc0` -> `0x405ab0` | XInput pad: left stick X short / 32768 as an analog channel, **0 unless |value| > 7849 (0.24)**; D-pad, triggers (byte/255), buttons 1.0/0.0. | C |
| 4 | device tables | Every device maps a logical **id** to a channel (`device + 0x18 + 4*id`) and has a "bound" flag (`+0xd4 + id`). Pad: id1..4 = LB, LT, RB, RT; id9/10 = left stick X/Y; id23..26 = D-pad up/down/left/right; id31 = A. Keyboard ids as in step 1. | C (pad ctor 0x405890) |
| 5 | `0x6b3390` row 0 (picked by `CS[+4]`, row 0 is the only filled row) | Action -> id: `[4,3,2,1, 9,10, 32,34, 23,24, 31, 0,0,0,0, 25,26, 23,24]` | C bytes; `CS[+4]` = 0: I |
| 6 | `0x5af150` (Human state 0x11, vtable `0x6b1b80`, **slot 3 at `0x6b1b8c` = `50 f1 5a 00`**) | Builds the CS each tick: **steer (`CS+0x40`) = val(id26) + val(id9) - val(id25)** (keyboard: RIGHT - LEFT; pad: D-pad right + stick X - D-pad left), vertical (`CS+0x3c`) = id10 + id23 - id24, **accelerate (`CS+0x44`) = max(id4, id3)**, **brake (`CS+0x48`) = max(id2, id1)**, buttons 0.. from id31 etc. Setters `0x540580/590/5a0/5b0` store to `CS+0x3c/0x40/0x44/0x48`. | C |
| 7 | `0x5a47d0`, `0x5a48d0` (called from `0x5a4b60`) | These are **not** steering: they turn the stick/D-pad into menu-style button edges (buttons 3, 6..10, hysteresis 0.4/0.6). | C |
| 8 | `0x4f3990` (racer tick, entry at `0x6afbc0`) | Calls `0x4e7da0(1)` (steering filter) and then `0x55aab0` (vehicle step). | C |
| 9 | `0x4e7da0` **steering filter** | `target = -raw*|raw|` (**signed square**, so a stick at 0.5 gives 0.25). Filtered value is stored in `racer+0xa90..0xa9c`. Normal case: `C += (target - C) * (0.5r + 1/32 + 0.25r^2)`, `r = p0 + (1 - f) + 1/16`, `f = clamp((racer.speed(+0xa70) + 32)/128, 0..1)`, `p0 = [racer+0x1f0] + 0x6c0` (per-car data). **Return-to-centre branch**: when `|target| < 1/16` (i.e. `|raw| < 0.25`) it does `C = C * p1 * f` each 20 ms tick (`p1 = [racer+0x1f0] + 0x6d0`, per-car data) and snaps to 0 below 1/32. Clamped to +-0.5 while `racer+0x1ee8 != 0`. | C (constants read from a crash dump, see Method) |
| 10 | `0x54e010` (called from `0x55aab0`) | **Brake-hold timer** `vehicle+0x310`: `+= 0.02` each tick while `CS+0x48 > 0`, else 0. | C |
| 11 | `0x551940` **CanStartDrift** (called from the drift update) | True when: racer flags `+0x1ee0/+0x1ee1/+0x1ee8` clear and no All-Star (`+0x1ff0` is 0 or 1); then either `vehicle+0x460 != 0` (set by the boost code `0x4e9780`, I: "boosting") or all of: speed above a per-car minimum, `[CS+0x50] & 2`, drift state != 2, **`CS+0x44` (gas) > 0**, **`vehicle+0x310` (brake held) > 0**, **`|raw steer| > tuning A (+0x170)`**, (already drifting or `|filtered| <= |raw|`), and speed vector > 0.001. | C |
| 12 | `0x5cb4b0` **drift update** (called from `0x5568c0` and `0x582e00`, both in the vehicle step) | Starts with the tweak name "DisableDrifting". Drift state `vehicle+0x2d0`: 0 none, 1 entry/hop, 2 drifting, 3 releasing, 4 end (names I, transitions C). `if state != 1 and CanStartDrift` -> state 1, latch cleared. | C |

Other things seen: `0x508170` and `0x508180` are 3-byte stubs that always return 0. Their callers (steer filter, CanStartDrift, drift update, `0x5af150`'s "steer * 3, clamp" line) switch to different thresholds when they return true. This looks like compiled-out digital-pad handling from a console build (I). They take no arguments, so they could not be per player.

## 2. What ends or straightens a drift, and why a stick avoids it

Drift state transitions in `0x5cb4b0` (all C unless marked):

- **0 -> 1** (or 3/4 -> 1, never from 2): `CanStartDrift` (gas + brake + steering past threshold).
- **State 1 (entry)**: reads the **raw** steer, keeps a latch at `vehicle+0x2a0` of the **largest same-direction magnitude** (the opposite direction never flips it in this build because the stubs return 0). After the hop timer `vehicle+0x270 >= tuning B (+0x230)` -> **state 2**.
- **State 2 (drifting)**: the car's body angle is `vehicle+0x250 + vehicle+0x210` (rotation built by `0x673d90`), and **`vehicle+0x250 = (tuningB(+0x1d0) * filteredSteer + clamp(vehicle+0x230, +-1) * tuningB(+0x1e0) + vehicle+0x250) * k`**. So the filtered steering number is a direct drive on the drift angle. No steering -> that term is gone and the angle relaxes: **this is the "auto-align" you see.**
- **2 -> 3** when any of: `racer+0xa60` (I: speed) below tuning B `+0x2a0` (I: minimum speed); All-Star active; **brake not held while the global byte `0x6ec732` is non-zero** (the byte is 1 in the game's memory dump, so "release brake = end the drift", i.e. the boost release); a stunt timer (`racer+0xad0`) running with a small slip angle.
- **3 -> 0** (`0x54dd30` resets every drift field): when the slip angle `|vehicle+0x260| < 1/16` and the drift angle `|vehicle+0x210| < 1/32`, i.e. the car is nearly straight again. Also reset by stunts, hits and respawns (`0x4ee436`, `0x4f0774`, `0x4f08b9`, `0x4f0b7c`, `0x4f2032`, `0x4f2938`, `0x4f2f05`).

So with **brake still held**, releasing the steering key does **not** by itself leave state 2 (no such test found): the car straightens because its drift angle loses its steering drive, and it is "redone" by pressing steering again (state 3 or 0 -> 1 restarts the hop). If the player lets go of both, state 3 then exits as soon as the car is straight. **This reading is partly inferred; the live check is open question 1.**

Why a stick avoids it: a stick at, say, 0.5 keeps `raw = 0.5`, so `target = 0.25` is steady, the lag filter holds `C ~ 0.25`, and the drift angle keeps a steady middle value. The return-to-centre branch (`|raw| < 0.25`) only triggers when the stick is nearly centred. A key can only give 0 (return-to-centre every tick, so `C` falls by `p1*f` per 20 ms) or 1.0.

## 3. Candidate fixes (project style: no code bytes, nothing the Roster Pack fingerprints)

The Roster Pack fingerprints 12 ranges (NATIVE-BRIDGE.md section 1: `0x4d4c14`, `0x4ba870`, `0x5b09a0`, `0x4d3d10`, `0x4d3ba0`, `0x4d4ea0`, `0x54bb00`, `0x4900bd`, `0x5fe690`, `0x6f6a88`, `0x6f6620`, `0x6f661c`). Every address below is outside them.

### Option A (recommended): "Keyboard drift hold", wrapper on the Human state update

- **Hook**: replace the data slot `0x6b1b8c` (`50 f1 5a 00`, the Human state's update, only a data reference) with our thiscall wrapper. Same slot-swap method as the M2/M3 hooks; check the 4 original bytes first.
- **Wrapper** (after calling the original `0x5af150`, so the CS is complete):
  1. Only when the rule is on.
  2. Is this player on the keyboard? `CS[0]` (`[controller + 0x1c]`) is the player's **device index**: the racer's `+0x9ec`, copied there when the controller is made (C: `0x4e6110`, `[controller+0x1c] = [racer+0x9ec]`), i.e. the third argument of native `0905` (I). The device list is 4 XInput pads (index 0..3) then the keyboard (**index 4**), with the mouse kept apart (C: `0x406e00` appends in that order and stores the keyboard at `mgr+0x4c`). So **keyboard player = `CS[0] == 4`** (the keyboard half is C; "the lobby's pad index is the same number" is I, see section 3b). A belt-and-braces check is the device's vtable: `dev = [mgr + 0xc + 4*CS[0]]`, `mgr = [[0xc2a1b4] + 0x14]` (C: the expression `0x5af150` uses); keyboard when `[dev] == 0x67e444`. I found no code path that changes `CS[0]` during a race (I), so there is no mid-race device switch to handle.
  3. `vehicle = [racer + 0x9f4]`; if `[vehicle+0x2d0]` is 1 or 2 **and** `[vehicle+0x310] > 0` (brake held):
     - while `|CS.steer| >= 0.5`, remember its sign;
     - if `|CS.steer| < 0.5` (key released) and a sign is remembered, write `CS+0x40 = sign * H`.
  4. Forget the remembered sign when the state is not 1/2, brake is released, or the opposite key is pressed (that passes through unchanged and re-latches).
- **H** (hold level): config/rule value, start with **0.7** (target `0.7^2 = 0.49`, about half the drift drive). Tune with a controller side by side.
- **Why it is safe for the rest**: it only runs in drift states 1-2 with the brake held, so it cannot start a drift (that is state 0/3) or stop one; it changes one float in the CS that the game already treats as analog.
- **Risks**:
  - Order inside a tick: the **controller/state update runs before the racer physics** (C: `notes/RACE-BRIDGE.md` "Sim step order"), so the wrapper's value is the one `0x4e7da0` and the drift update read in the same tick.
  - Other readers of the same steer value: lean/wheelie `0x4e90f0`, the replay/ghost recorder `0x504420`, camera `0x551940`. They would see the held value too (probably wanted).
  - Other mods that hook drift code (Ten Stage Drift, Honest Gauges) are not touched, since we only change the input number. Ten Stage Drift is currently disabled.
  - Held 0.7 could feel "sticky" in a tight hairpin. The opposite key always overrides.
  - Remote cars are pose puppets (M3/M4): nothing to sync for the effect itself.

### Option B (simpler, wider): "Keyboard analog smoothing", same hook

- Same wrapper and keyboard check, but always (not only in a drift): ramp `CS.steer` toward the key value (-1/0/+1) at a limited speed (for example attack 6/s, release 4/s) so a short tap gives a partial value and a release fades instead of dropping to 0.
- **Pro**: no vehicle fields read, so nothing to misread; helps normal steering too.
- **Con**: changes the feel of every corner and slightly delays the drift start (the drift needs `|raw| > threshold`); does not truly "hold" a drift, it only softens taps.

### Not recommended: flip the two digital-pad stubs (`0x508170`/`0x508180`)

They are code and global (no player argument), so they would affect controller players too, and the effect of the alternate paths was not worked out. Listed only as a lead.

### As a lobby rule (the same for everyone)

- New host option **"Keyboard drift hold"**: Off (vanilla) / On (level H). The host's value is what every PC uses (like the `HostRunsAI` contract flag and the host's items/AI rules from M5), and it is shown in the lobby Settings.
- Each PC applies it **only to its own local keyboard-driven car**; controller players keep their analog stick as is. Result: every keyboard player gets a stick-like drift hold, every controller player already has it. Nothing needs to be sent for the simulation (each PC drives its own car); only the agreed setting travels in the lobby data / contract flag (a wire version bump if it goes into the race contract, like `HostRunsAI`).
- Codex's idea of "disable controller Drift+ when a keyboard racer is present" is **not possible as a switch**: there is nothing to turn off, only a stick that is naturally analog. The closest honest version is the fallback in section 3b below (make controller steering digital while a keyboard player is in the lobby).

## 3b. Fallback rule: controllers steer like a keyboard when a keyboard player is in the lobby

Asked for by the user (2026-10-21): if the keyboard cannot be given a sustained drift, make controller players feel the keyboard way instead. This is feasible with the same hook and no code bytes. All C unless marked.

**What "like a keyboard" means in the game's numbers.** The keyboard puts exactly `-1`, `0` or `+1` into `CS.steer` (`0x5af150`: RIGHT - LEFT, each key 0.0 or 1.0). Everything after that (signed square, lag filter, return-to-centre when `|raw| < 0.25`, drift start/latch/yaw drive) is shared. So if a pad player's `CS.steer` is snapped to the same three values, **the rest of the game behaves for them exactly as for a keyboard player**: release = 0 = return-to-centre every tick = the drift angle melts, hold = full lock.

**Where to hook** (best first):
1. **Same wrapper as Option A** (Human update, data slot `0x6b1b8c`), after the original runs. For a local human whose `CS[0] != 4` (pad, wheel, joystick) and with the rule active, rewrite `CS+0x40`:
   - snap with hysteresis: switch to `+-1` when `|x| >= 0.55`, back to 0 when `|x| < 0.35` (a keyboard has no in-between, so any threshold is a design choice; the pad code already zeroes `|x| < 0.24`);
   - two strengths: **drift only** (only while `[vehicle+0x2d0]` is 1 or 2 and `[vehicle+0x310] > 0`, so normal steering stays analog; this matches the complaint and is the suggested default) or **all steering** (always snapped; heavier, although a keyboard player's normal steering is also softened by the lag filter, so a snapped pad is not as harsh as it sounds);
   - one wrapper serves both rules (Option A for keyboards, this one for pads), selected by the rule value.
2. **Pad poll wrapper** (data slot `0x67e408` = XInput pad poll `0x405dc0`; the DirectInput joystick class has its own vtable `0x67e39c`): after the original, snap the stick-X channel (channel 11, `+0x2c` of the device buffer) before the Human update reads it. Also simple, but it cannot be "drift only", and it snaps the stick in menus too (menus use the stick through the 0.6 thresholds of `0x5a48d0`, so mostly harmless). The D-pad channels are already digital.
3. *Not recommended*: code patches such as the `0x508170/0x508180` stubs (global, no player argument).

**How the game knows each player's input device (lobby member data).**
- Each PC already receives its **primary pad index** from the lobby script every frame (native `0400`, `PRIMARY_PAD` in `tools\netmenu\steam.py`; stored as `g_localPad` in `src/natives.cpp`; it is also the number the race start sends `fe_game_bridge` as that player's device, which `0905` then receives; it reads hash 0 until a pad is assigned). Index `4` = keyboard, `0..3` = pads (keyboard half C, see Option A step 2).
- Publish it like `racer`/`ready`/`net`: one more member datum, e.g. `inp` = `k` (index 4) or `p` (anything else), written in `WriteLocalData()` (`src/steam_mp.cpp`) and read in `ReadLobbyData()`; add `inp` to `LobbyMember` (`src/lobby.h`). The race-time check needs no lobby data at all: it is `CS[0] != 4`.
- **Host decides once, at Start.** `anyKeyboard` = any member (host included) with `inp == k`. The host puts the resolved rule into the race contract (a flag in `src/race_wire.h` next to `HostRunsAI`, which means a wire version bump) so every PC applies the same rule for the whole race. A member leaving or changing device mid-race does not change it (this answers Codex's "do not let a device switch silently remove the restriction"); the next race re-evaluates.
- Rule values for the host: `Off`, `Keyboard drift hold` (Option A), `Pads steer digitally when a keyboard is present (drift only)`, `... (all steering)`. A new lobby datum (e.g. `inprule`) avoids touching the 6-field `rules` string. The Settings screen would need one more row, and the lobby frame script chunk is near its 16-bit jump limit (`notes/RACE-BRIDGE.md` section 7: 440 of 594 free bytes used), so a first version can be `config.ini` on the host plus a lobby text line.

**Risks.**
- **Feel and consent**: it is a real handicap for pad players. Show it clearly in the lobby ("pads steer digitally: a keyboard player is in this lobby"), keep it host-controlled and off by default.
- **Threshold choice**: 0.55 / 0.35 is a guess. Too low turns light touches into full lock; too high under-steers. A stick also cannot copy the keyboard's instant 0-to-1 edge (it takes a moment to cross the threshold), so a snapped pad is slightly more forgiving than a key. Needs a side-by-side test with a keyboard player.
- **Device identification**: `4 = keyboard` and "the lobby's pad index equals `0905`'s device" come from reading code and from Codex's trace ("device argument is 4"); neither is verified in a live run (I). A user with a pad connected who plays on the keyboard is classified by the primary index, not by what they press. A wheel or joystick counts as a pad (snapped too).
- **Trust**: devices are self-reported member data; a modified client can lie. Same trust level as the rest of the mod.
- **Overlap with Option A**: they target different devices (A: keyboard only, 3b: non-keyboard only), so a lobby uses one or the other, not both.
- **Menus and other screens** are untouched with hook 1; hook 2 touches the stick in menus.
- **Other readers of `CS.steer`** (lean `0x4e90f0`, replay recorder `0x504420`, camera) see the snapped value, as they do for a keyboard.
- **Remote cars are pose puppets**, so nothing about the rule needs to travel during the race.

## 4. Open questions

1. **What the user sees when the key is released**: does the state really stay at 2 (brake held) and only the angle collapse, or does something else end the drift? Check with a read-only debug log at 50 Hz (state `vehicle+0x2d0`, `racer+0xa90`, `CS+0x40`, `vehicle+0x310`, `vehicle+0x210`) during a keyboard drift, then the same on a pad. Needs one in-game test by the user.
2. **Per-car numbers are unknown**: `[racer+0x1f0]+0x6c0/0x6d0` (filter lag and return), tuning A `+0x170` (drift start steering threshold), tuning B `+0x1d0/+0x1e0` (yaw gains), `+0x230` (hop time), `+0x2a0` (min speed). They come from the Racers data; the same debug log can print them from live memory. This decides the best H.
3. **Which keys the user really uses to drift**: the defaults written at `0x406e00` put brake on S and gas on D, item on SPACE, while a forum post says SPACE is brake/drift (I, web). Bindings may be changed in the game's own settings or stored in the save. The wrapper does not depend on the key (it reads gas/brake/steer values), so this only matters for test instructions.
4. ~~Update order inside a tick~~: answered from `notes/RACE-BRIDGE.md` (the controller update runs before the racer physics).
5. **`CS+0x50` bit 1** (required by CanStartDrift): unknown meaning, probably "driving enabled"; check it does not differ for keyboard.
6. **Wheel pads**: the pad code removes the dead zone when XInput reports a wheel (`GetCapabilities` type 1, subtype 2, flag at pad `+0x2f4`), so wheels get raw values (C). Out of scope.
7. Mixed-input detection (Codex's requirement: "active input device, not just a connected pad"): the race-time check `CS[0] == 4` is exact for the local player; the lobby-time value comes from the primary pad index (section 3b). Verify once in a live lobby that the logged pad index is 4 on keyboard and 0..3 on a pad (`g_localPad` can simply be added to an existing log line).
8. Does anything change `CS[0]` after the controller is created (device unplugged, another pad takes over)? The controller-drop path `0x4f3a9a` turns a human into AI (I); no keyboard fallback found.
9. The snap thresholds for rule 3b (0.55 / 0.35) and the hold level H of Option A (0.7) need a side-by-side test with one keyboard and one pad on the same car.

## Method and sources

- Ghidra 12.1.4 headless on a scratch copy of the project (`re\kbd-drift\scripts\run.sh`, `Decomp.java`), plus `tools\disasm.py` byte checks for the setters/getters and hook slot.
- The unpacked exe's `.data` does not hold the SSE constants the code compares against (they are filled in at startup), so they were read from the game's crash dump `%LOCALAPPDATA%\CrashDumps\Sonic _ SEGA All-Stars Racing.exe.117304.dmp` with `re\kbd-drift\scripts\dmp.py`: `0xc284a0` = 0, `0xc284e0` = 1/32, `0xc284f0` = 1/16, `0xc28520` = 0.25, `0xc28550` = 0.5, `0xc28570` = 1, `0xc28610` = 32, `0xc286c0` = 0.001, `0xc2a030` (tick) = 0.02, sign mask `0xc28900`, abs mask `0xc28910`; byte `0x6ec732` = 1 in both the unpacked file and the dump. Heap data (racer/vehicle objects, per-car numbers) is not in those dumps.
- Codex's notes (read-only): `MULTIPLAYER-FEATURE-QUEUE.md` "Mixed-input Drift+ fairness" and `NATIVE-SETUP-TRACE.md` say the native Drift+ path was never mapped; this note maps the steering/drift path but finds no "Drift+" switch.
- Web (data, not instructions): a forum search suggests SPACE brake/drift as one function on PC. Used only for open question 3.
- Section 3b was added the same day after the user's request relayed by the main session. Sources: `0x4e6110` (re-decompiled: `CS[0]` = racer `+0x9ec`), `0x406e00` (device order), `src/natives.cpp` (`g_localPad`, native `0400`), `src/steam_mp.cpp` and `src/lobby.h` (member data), `tools\netmenu\steam.py` (race start sends the device array) and `notes/RACE-BRIDGE.md` (sim order).
