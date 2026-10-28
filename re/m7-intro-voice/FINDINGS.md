# The course intro's announcer line and music: when they end (2026-10-22)

Reverse-engineer pass, static only (`re\bin\SSASR-unpacked.exe`, own Ghidra copy in `ghidra\`; Audio.xpac read only).
C = confirmed in bytes/decompile, I = inferred. Not run in game. Do not commit `data\` (sound XMLs from the game) or `ghidra\`.

Why: our online course intro ended after a fixed 250 ticks (5 s) without a full vote, and the user heard the announcer's
course line cut off (0.7.0-m7a, 2026-10-22). The user's design: a majority of players pressed CONTINUE -> go on once the
announcer has finished; otherwise -> once the intro music stops; either way the racer intro still plays.

## Short answer
- **What cuts the voice (C):** CharacterIntro enter flushes the commentary queue at `0x5b14d5`
  (`8b 0d 0c bf bd 00 6a 01 e8 ae c0 eb ff` = `0x46d590(1)`), stopping every queued or playing line. Any advance into the
  racer intro cuts the line.
- **Voice query (C):** the announcer queue's item count, word `[[0xbdbf04]+0x16]`: non-zero from TrackIntro enter until the
  last line has finished.
- **Music query (C):** the intro music object's own "is playing" (`0x471790`), the same call the game makes on its race music.
- **The intro music does not loop and ends by itself (C)**, but on 3 of 8 zones it is shorter than the announcer (House of
  the Dead 4.2 s, Monkey Ball 8.4 s, Jet Set Radio 10.0 s vs 9.5-17 s of announcer): "music stopped" must also wait for the voice.

## 1. How TrackIntro enter starts the voice, and what cuts it (C)
- TrackIntro enter `0x5b0f60`: flushes the queue at `0x5b0f95` (`0x46d590(1)`, ECX `[0xbdbf0c]`); at `0x5b0fa2` checks the
  game type and queues the lines (`e8 89 b1 f1 ff 83 f8 05 74 05 e8 5f b5 f0 ff`: type 5 skips; our races are type 1, so
  `0x4bc510` runs); then `0x5b1063` sets up and starts the music (section 5).
- `0x4bc510` (cdecl, no args): gates byte `[0xc29fdb] == 0` and a course record at `[[0xc29fc0]+0x10e4]+0x1f0`; matches the
  course name (+0x1ec) against the table at `0x6ed260` (27 records of 0x1fe bytes, letter code +0xff..+0x101); builds
  `INT1_<2 letters>` (zone; samba SA -> SD) and `INT2_<3 letters>` (track); hashes each (`0x4947e0`), finds the commentary
  entry (`0x46d5e0`, ECX `[0xbdbf0c]`); one shared group id (`0x46d450`) in word +0x28; then `0x4bbec0(hash, 20000, 0)` each.
- `0x4bbec0` gates: not mode 0x35, `[[0xc29fc0]+0x1350] == 0`; game types other than 1/5 also need mode 0x2c. Chain
  `0x45ffb0` -> `0x46db60` -> `0x46daf0` -> `0x46d6b0`; at `0x46d784` `mov ecx,[ebp+0x2c]; call 0x46e2e0`. That ECX is the
  queue object at `[0xbdbf04]` ("SpeechCue_iSAudioInterface", vtable `0x6a464c`); `[0xbdbf0c]` is CommentMan, whose +0x2c
  is the same queue (`0x460576..0x460590`, `0x46d978`). `0x46de90` appends a 0x14-byte slot and adds 1 to the count at +0x16.
  No handle is returned: the queue slot is the only record of the line.
- Queue fields: slots +0x10, capacity +0x14 (8), count +0x16, run state byte +0x24 (0 stopped / 1 running / 2 paused),
  current sound +0x28, gap after a line +0x44.
- Its update `0x46e430` (vtable +4) runs every frame from the audio update at `0x45f993`
  (`8b 0d 04 bf bd 00 85 c9 74 08 8b 01 8b 50 04 53 ff d2`; called from `0x496b6f` with the frame time in ms): starts the
  first line, waits until it no longer plays, removes it, then plays INT2.
- The cut: CharacterIntro enter's `0x46d590(1)` resets every commentary entry, then `0x46de70(1)` on the queue (`0x46de30`
  stops every queued sound, vtable +0x3c) and zeroes the count and current sound.
- Not the cutter: `0x4bbda0(0)` only queues the player character's own voice line later, into the same queue. The
  `Mix_PreRace` (`0x598e9ee2`) mix at `0x5b14b4` is the one TrackIntro enter applies too (I: not a cut).
- Countdown enter `0x5b18bb` flushes the queue again and stops the intro music:
  `8b 0d 0c bf bd 00 6a 01 e8 c8 bc eb ff e8 83 8a f9 ff`.

## 2. Voice query, side-effect free (C)
`cue = *(u8**)0xbdbf04`; finished when cue is null, or the vtable check fails (`*(u32*)cue == 0x6a464c` and
`[0x6a464c+4] == 0x46e430`), or the count `*(u16*)(cue+0x16) == 0`, or the run state `*(u8*)(cue+0x24) == 0` (a stopped
queue never drains). No stock getter; plain reads on the game thread (I).
Byte-check: `0x46e502` `0f b7 5e 16 66 85 db 75 0d 5f 5d 5b b8 01 00 00 00 5e 59 c2 04 00` (the update's "queue empty,
return 1"), plus the `0x45f993` pattern above.
The count stays above 0 the whole time (C): both lines are queued in the enter, before our first tick; the current line
is removed only when its sound reports "not playing", and its stream reports "playing" from the play call on (state 4 in
`0x467630`), so loading time doesn't end the wait. Run state 2 (paused) is set while the game's pause flags
`[0xc2a014..16]` are on (`0x495e10` -> `0x45fe00(2,0)`): the queue doesn't advance; the hard cap bounds it.

## 3. Recommended rule (once per tick, after the stock update)
```
t = [0xcf7a8c]                                   // course-intro ticks, 50 per second
voiceDone = (section 2)
m = *(void**)(*(u8**)0xc29fc0 + 0x1334)
musicDone = !m || *(u32*)m != 0x6a47e8 || !((bool(__thiscall*)(void*))0x471790)(m)
end = (majority && voiceDone) || (voiceDone && musicDone) || t >= 1250
if (end) 0x4cce60()                               // on to the racer intro
```
Hard cap 1250 ticks (25 s): the longest natural end is ~20.7 s (Seaside Hill music), the announcer's worst 17.7 s; covers a
paused game, a course missing from the table, stream start-up delay. The two PCs end at different times (each picks its own
recorded take, maybe another language: the voice end can differ by seconds; the music end is nearly identical); hold B
evens it out. Nothing here touches the shared RNG.

## 4. Line lengths (C: sample counts; MS-ADPCM 48 kHz mono, 32 samples per 22-byte block)
Each line is one of 3-6 recorded takes, picked at random (I: by the sound library). INT1 + INT2 per course, English:
shortest pair 8.6 s, longest 17.1 s (Jet Set Radio hard); worst over all PC languages 17.7 s (Spanish, Jet Set Radio
hard). Seaside Hill easy (English): 11.4-14.6 s, so the old 5 s cut lands inside the first line. Speech ends ~0.2-0.35 s
before each file's end (I). Japanese: `com_jap.xml` lists `wav\COM_JAP` files not in the archive (language id 0xd only).
Audio.xpac keys (C): `0x498060`'s hash of `".\Resource/Audio/" + location`, last character to first,
h = h*0x83 + toupper(c), '/' -> '\'.

## 5. Course-intro music
- Start (C): `0x5b1063` (`e8 d8 94 f9 ff e8 b3 92 f9 ff`) calls `0x54a540` (sets `[[0xc29fc0]+0x1334]` =
  `MUSIC_<2 letters>_INTRO` (samba stays SA), +0x133c `_ENDING`, +0x1338 `_MAIN_<A..E>`) and `0x54a320` (vtable +0x18(0),
  then +0x38). `0x54a540` draws once from the shared RNG (`0x4be040`), missing from the earlier RNG list (same count on
  every PC: once per race).
- Stop (C): `0x54a350` (vtable +0x3c) only in Countdown enter (`0x5b18c8`) and `0x4f9546`. CharacterIntro doesn't touch
  it: the music runs on into the racer intro. The race music starts at Racing enter (`0x5b2048`, `0x54a370`).
- "Is playing" (C): the music is a plain sound object (vtable `0x6a47e8`; `sounder` element created at `0x4784a0` ->
  `0x471890`); slot +0x6c = `0x471790`, thiscall, no args, AL. Reads only (byte-check `8b 81 d8 00 00 00 85 c0 74 60 83
  b9 80 00 00 00 01 75 5a 80 b9 85 00 00 00 01 74 51`). 0 when nothing is assigned or the stream is in state 0/1/2/10;
  1 while starting (state 4) and while paused (pause sets +0x80 = 2 at `0x476d34`). The game calls it at `0x54a3b0`
  (`8b 42 6c 8b ce ff d0 84 c0`).
- A course missing from the table: `0x54a540` leaves +0x1334 holding the previous race's intro music and stock code
  replays it; 0 = treat as stopped.

## 6. Does the music loop? No (C)
All eight `MUSIC_xx_INTRO` have `loop value="0"` in `music/music.xml` (archive key 80e201cb); the stream's non-loop path
ends in state 2 (`0x466f40`) and "is playing" turns false. Lengths (xWMA decoded sizes): Seaside Hill 20.74 s, Billy
Hatcher 18.13, Final Fortress 16.73, Samba 15.23, Casino Park 15.15, Jet Set Radio 10.03, Monkey Ball 8.45, House of the
Dead 4.22. With the rule above the natural course-intro end is ~15-21 s; on JSR, Monkey Ball and HotD the announcer decides.

## 7. Settled by the user
Always on to the racer intro. Consequence: when the course intro ends because the music stopped, the racer intro plays
without music (as in the stock game when a player waits); in the majority case the music keeps playing into the racer
intro until Countdown enter stops it.

## Confidence
High: code paths, call sites, the cut, field meanings, stream lengths, loop flags. Medium (I): real-time behaviour
(stream start delay, ~1 frame + 10 ms end detection lag), how the take is picked, that nothing else queues commentary
during the course intro. Scripts: `scripts\` (tables, durations, xkey, xwma, wavhdr, speech_span, envelope, introsounds,
xpaclist, strhash, opscan, Decomp.java). Results: `out\` (durations.txt, music-intro.txt, introsounds.txt, audio-list.txt,
decompiles d1-d26.txt).
