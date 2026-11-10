# Final lap fever: music, crowd, ambience, confetti, lap line (2026-11-09)

Reverse-engineer pass, static only, on `re\bin\SSASR-unpacked.exe` (its own Ghidra copy `re\m10-lap-fever\ghidra\`, don't commit), capstone, plus read-only scans of the game's xpac/sbn files (the extracted members are only in the session scratchpad). Saved from its report by the main session. C = confirmed in bytes/decompile, I = inferred. Nothing was tested in game.

Volume units everywhere are u16 attenuation: 0 = full, 10000 = silent (C from use: All-Star and master fades go to 10000, then stop or pause; "1/100 dB" is I).

## 1. Music switch mid-race
**All-Star (Revenge) music (C).**
- **Start** (All-Star base start `0x5a0580`). Gate: `0x4cc4e0()!=0` and states 0x11/0x13 not on the stack. There is no local-car check, so any car's All-Star triggers it.
  - `g=0x577e80(0xa980d1bf SG_RevengeMusic)`, then `0x475550(g, [as+0xe0]=500 ms, 0, 0, 0, 0x2711)` fades it in.
  - The revenge sounder `[as+0xdc]` (`MUSIC_REV_<char>`, fetched in `0x5a0360`) gets `vt+0x38` Play unless `[0xc29fd9]`.
  - `0x4754d0(SG_GameMusic 0x40ce3f41, 500, cb 0x59fec0, 0, 10000, 0x2711)` fades the race music out (bytes at `0x5a0666`: `68 11 27 00 00 68 10 27 00 00 6a 00 68 c0 fe 59 00`).
  - The callback `0x59fec0` pauses **`[[0xc29fc0]+0x1338]`** (`vt+0x40`).
- **End** (`0x5a07b0`): SG_RevengeMusic fades to 10000 with callback `0x59fee0` (Stop revenge sounder), SG_GameMusic fades to 0, and `[[0xc29fc0]+0x1338]->vt+0x44` Resume.
- If the All-Star owner is human, the game also ducks SG_TrackAmb/SG_TrackSpot by 600 (`0x463690(0x6b72c8,2,10,0/1)`).
- So the All-Star always pauses and resumes **whatever +0x1338 holds**. The mod must store its new song there, or the old song comes back after an All-Star.

**Other +0x1338 users (C).**
- Racing enter `0x54a370`: vt+0x18(0), then Play if not playing.
- Race end `0x54a450` (call at `0x5b2541`): Stop +0x1338, queue ENDING `+0x133c`.
- `0x54a3b0`: Stop.
- Pause `0x495e10`: pauses and resumes every sounder (`0x464940`/`0x4649b0`).

**Sounder API (iSAL2_Sounder vtable `0x6a47e8`, C).**
- +0x18 SetVolume(u16) `0x471280`: stores +0x50 and forwards base +0x52 plus the user volume to the stream channel. It is applied by the channel's dirty update (timing I).
- +0x1c SetPitch(float) `0x4712e0`; +0x38 Play `0x471a60`; +0x3c Stop `0x471070`; +0x40 Pause `0x4710f0`; +0x44 Resume `0x471140`; +0x6c nonzero = playing.
- Lookup: `0x45fbb0(hash)` cdecl (a sounder or a SounderGroup). The object's name hash is at +4.

**Playback rate.** The pitch path is C: sounder +0x2c -> streamer `0x467010` -> voice `0x465920` -> XAudio2 `SetFrequencyRatio` (`call edx` at `0x465963`, ratio = +0x64 * +0x30 * +0x2c * clamp). It resamples, so pitch and tempo change together. The streamer applies it only in Play (`0x467630`), not in its update `0x4670a0` (I). So a tempo-only fast version must be a pre-rendered file.

**Groups (iSAL1_Group vtable `0x6a4d08`, type `[0xbdc014]`, C).** Get one with `g=0x4635e0(ECX=[0xbdbedc], hash)` (or cdecl `0x577e80`, or cached `0x45fc50`). Each group has three volume layers:
- **Mix:** `g->vt+0x18(short)` (`0x474800`, stored at +0x50).
- **Duck:** `0x475460(g, u16, u8 steps)` (+0x86/+0x88).
- **Fade:** `0x4754d0`/`0x475550` thiscall(g, ms, cb, cbArg, u16 target, u16 start or 0x2711 = keep current). Updated per frame by `0x4755f0`. Fields: +0x8c current and +0x90 target (16.16), +0x94 step (0 = idle); cb(arg) runs at the end.

**Mix presets (C).**
- `p=0x463f30(ECX=[0xbdbedc], hash)`; `p->vt+0x30()` applies every entry at once (`0x47b0d0`, the entry time is ignored).
- `e=p->vt+0x20(groupHash)` (`0x47b080`); `*(short*)(e+0x10)` is the value, and it is editable.
- Online type-1 single-viewport races apply Mix_SoloRace `0xcd49337d` at Countdown enter (`0x5b1963`) and Mix_Results `0x7d79cace` at `0x5b273b`. SG_Audience is 1050 in SoloRace and 450 in Results.

**Adding a song (I).** Sounders come from audio-scene XMLs. Egg Hangar adds channel_data + sounder entries to its own DEZ.xml; Metal Sonic replaces Music.xml. `0x460390(path, waitFlag, 0, 0)` cdecl queues an audio-scene load (C, `0x46c720` command 0xd). So a mod-owned `lapfever.xml` in Audio.xpac (SG_GameMusic, loop 1, 2D, our .str) could be loaded at race load, if ChaoGarage can add a new archive member.

**How the mod would do it.** At the local final-lap edge (section 5):
1. Check `[0xc29fd9]==0`, not paused (`[0xc2a014..16]==0`), and no All-Star music (SG_GameMusic `+0x94==0 && +0x90==0`).
2. `old=[S+0x1338]`, `new=0x45fbb0(hash)`. Call `new->vt18(10000)`, then `new->vt38()`, and write `[S+0x1338]=new`.
3. Ramp per frame over 1.5 s: `old->vt18(t*10000)` and `new->vt18((1-t)*10000)`.
4. Then `old->vt3c()` (Stop).

Edge cases:
- **An All-Star starts mid-ramp** (the SG_GameMusic target becomes 10000): stop the old song at once and set the new one to volume 0. The stock code then pauses and resumes the new one.
- **An All-Star is already running at the line:** wait until it ends.
- **Fast copy on other courses:** `new = lookup(name([old+4]) + "_FAST")`.

## 2. Crowd
**C.**
- The AUD_* hashes are built at init (`0x679b40`) into the table `0xdf0a14[8]`: START, END, AHH, OOH, BOO, YEAH, CHEER, LOOP.
- Their only users are the track's CrowdCue queue and its play function:
  - The queue is built by `0x6620d0` and lives at `[track+0x2e0]` (track = `[[0xc29fc0]+0x10e4]`); `0x4f9530` updates it.
  - `0x662180` thiscall(cc, cueHash) `ret 4` picks a take with CRT `rand()%takes` and queues it.
- **`0x662180` has no call or pointer anywhere**, so there are no in-race crowd reactions on PC. No .sbn or other XML refers to the AUD_ names.

**AUD_LOOP (C).** It lives at scene+0x1354, with SG_Audience at +0x1358.
- `0x49db20` thiscall(scene, bool fade, float s) `ret 8` fades the loop in (fade layer 4000 -> 0) and plays it. Called at CharacterIntro enter (`0x5b1515`, 1 s) and race-finished enter (`0x5b24bd`).
- `0x49c2b0`(scene, bool fade, float s) fades it to 4000 and stops it. Called at Countdown (`0x5b1922`, 7.0 s), `0x5b2558` and `0x5b2cec`.
- So the race itself has no crowd bed.

**How the mod would do it.**
- Bring the bed back at the final lap: `0x49db20([0xc29fc0], 1, 1.5f)`.
- Louder: `g=0x4635e0(SG_Audience 0xe9a23b1e)`, then `g->vt18(1050 - hype*600)`. Results uses 450; the next preset apply overrides it.
- Extra cheers, either way:
  - Direct: `0x45fbb0(AUD_CHEER 0xa66b7ae4 / AUD_YEAH 0x2cbadc33)->vt38()` (2D RANDOM group, CRT rand).
  - Queued: `0x662180(cc, hash)`, one at a time (I).
- About 3 streams can start at once (lessons-race). Take the timing from the mod's own RNG.

## 3. Ambient course life
**C.** Ambience is authored inside each track scene (Tracks.xpac FORE members). Example, Whale Lagoon `c67b2f09`: `Butterflies_Butterflies` and `Butterfly_Anim_500` (an animated mesh), `leafFallMat`, `sparkle*`, `whale*Emitter`. Emitters hang off scene objects (+0x50 array, count `[[obj+0x34]+0x38]`, +0x54 active; `0x625d30`). The only rate fields found are per car-exhaust instance (FXDATA rows EmitterRate/ParticleSpeed/..., read at `0x5834d0` and `0x57f7e0`).

**No global emission or quality multiplier was found.** What MODEL_DETAIL (settings.ini, `0x67e148`) controls was not checked. Finding a per-emitter rate field would need a pass on the emitter update (`0x625f50`/`0x6263a0`).

## 4. Confetti
**PRESS START (C).** In `fe_startscreen` (FrontEndScripts.sbn), offsets 0x0299-0x02b7:
- Native 1c11 (`0x4c6b60`) makes the animated scene object `CONFETTI` (0x16436368) from the front-end 3D package. That package is loaded by 1c00 (`0x4c6620`): package 0x33df2adc = Base.xpac member `ed1f1715`, FORE format, containing `CONFETTI`, `CONFETTIemitter1`, `CONFETTIlambert5` and `CONFETTI_BLANK.TGA`.
- 1c04 (`0x4c7d90`) plays its anim "Start" (0xb363364e); 1c17 deletes it.

So it is a **3D particle emitter in the front-end scene**, not a 2D/UI effect and not part of a video. That package is front-end only (I), so it can't be spawned in a race as it is.

**In-race sources (C).**
- **Samba_FX.Forest** (0x408ea5cb; Racers.xpac `d4941244`): `FX_Confetti` 0xef5003cb, `FX_Revenge_SambaOnlyConfetti`, `FX_Revenge_SambaExhaustConfetti`. The effects table maps Samba to this forest, so it is loaded only when Samba is in the race (I).
- **Super Monkey Ball courses** (`2a44a529`, `b829d7a2`): `S:S:CONFETTI_emitter1`.
- **Casino Park** (`00fefa29` and 5 more): rocketTrail/rocketExplosion, red/blue/green/purple burst emitters, `FIREWORKS_1-3.TGA`.
- The 13 WorldEffects kinds (fixed hash switch `0x516f90`) have no confetti.

**Spawn at a world position (C, from `0x4b1e00` and the WorldEffects functor `0x516d20`).**
1. `e=0x404460(0x70)`; `0x548ef0(e)`.
2. Build row = {type, forestHash, effectHash, locator 0x29b7cf34 "Root"}; `ok=0x548e00(e, 0, &row, 0)`. It returns false if the forest isn't loaded (lookup: `0x501d70(ECX=0xd12820, forest, 0)`).
3. For i < `[e+0x48]`: `0x4ce210(ECX=[e+4+4i], &pos4, &quat4, 1)`.
4. `0x5489b0(e)`; `0x5490f0(e,1)`; `0x548b80(e,1)`.
5. Each frame: `0x5493d0(e)` while `[e+0x50]>2`.
6. Stop: `0x5490f0(e,0)`. Free: `0x548f60(e)`, then `0x4044e0(e)`.

No shared-RNG draw was seen in this path (particle internals I). The finish-line position is up to the mod, e.g. spawn near the local car as it crosses the line.

## 5. Lap line
**C.** `0x4bc290(rec)` is called from `0x4bd3b0` <- racer slot 4. It needs `[0xce7ba8]>=7` and `[0xc2a140]==1`.
- Local racer: `0x49b9c0(ECX=[0xc29fc0])`. Lap: `[[0xcf7a68]+rec*0x90+0x74]`. Lap count: byte `[0xcf7a55]`.
- On a new lap (`!=[0xce7be0]`, which is written only at `0x4bc344`):
  - lap > laps: HUD_FINISHLINE 0xd92a3b4e.
  - lap == laps: HUD_LAST_LAP 0x28ad1d27 (SG_Hud) plus the FLAP cue 0xef87189c.
  - otherwise: HUD_NEW_LAP 0x4ab51112 plus LAPn.
- No fireworks or sting sound exists in the 205 audio XMLs. Usable extras: AUD_CHEER/AUD_YEAH (SG_Audience) and MUSIC_JINGLE_NEWRECORD (SG_Jingles).
- `0x4bc290` is a direct call (no data slot to hook).

**Hook:** poll in the mod's existing per-tick race hook on every PC: `r=0x49b9c0(...)` and read the lap as above. The edge `lap==laps` means the final lap starts, and lap/laps also gives the hype level. Don't use `[0xce7be0]` (it has the 7 s gate and only works with one viewport).

## Sync safety
- CRT `rand()` (`0x45f3e0`) is used by SounderGroup RANDOM, CrowdCue and the commentary. The shared race RNG is `0x4be040` (C, m7 passes).
- Nothing proposed here draws the shared race RNG.
- Avoid the WorldEffects manager: its `0x516c80` draws from the shared RNG (C).

## Files
- `scripts\`: run.sh, Decomp.java, l1-l34.txt, strfind.py, xmlhash.py, xpacscan.py, xpacz.py, tremit.py, opscan.py, vcall.py, strs.py, dumpmods.py.
- `out\`: d*.txt, st-fe_startscreen.txt, st-hud.txt (game-derived: don't commit `out\` or `ghidra\`).

## Smallest safe implementation (the pass's summary)
- **No new game hooks are needed.** Everything runs from the existing per-tick race hook, on the game thread: poll the local lap; at the final-lap edge, crossfade with per-sounder volume after writing `+0x1338`.
- **Crowd:** bring the crowd loop back with `0x49db20`, raise SG_Audience with the group's `vt+0x18`, and play AUD_CHEER takes directly.
- **Confetti:** spawn `FX_Confetti` through the SSREffect sequence only if `0x501d70(0xd12820, 0x408ea5cb)` finds the forest. Making it work on every course needs either a forest-load pass or a forest merge in an archive edit.
- **Live & Learn and the fast copies:** they need a mod-owned audio-scene XML. Whether ChaoGarage can add a new Audio.xpac member is still open.

## Confidence
High on the code paths, addresses and call shapes (bytes + decompile). Medium (inferred, not tested in game): when a volume change is applied, pitch being set only at Play, when Samba's forest is loaded, and how the queued crowd play behaves.
