# M6 intro vote-to-skip: what the bytes say (reverse-engineer report, 2026-10-21)

Saved by the main session. Read-only, static pass; nothing run in game. **C** = confirmed in the bytes or a decompile;
**I** = inferred. Script offsets are `hud` module offsets (the PC after an 8-byte native call is the offset + 8). `hud` is
byte-identical in the stock, Roster Pack and m6-race-hud FrontEndScripts copies (sha256 prefix 397b8e2cd2137876, size
41962) (C). The decrypted exe is `re\bin\SSASR-unpacked.exe`. Helper scripts and raw output: `re\m6-intro\scripts\`,
`re\m6-intro\out\` (game data, not committed), `re\m6-intro\ghidra\` (a copy of the loader project, not committed).

## 0. Short answer
- **There is one CONTINUE prompt, and only the course intro has it.** It sits in the per-course `TRACK_INTRO` HUD scene
  and has two parts: the text element `d80223a4` (the scene's own "CONTINUE" string) and the key picture `faf4c381`. The
  racer intro has **no** prompt on PC. When the racer intro starts, `hud` fades out the whole TRACK_INTRO group, then
  hides the scene. The press still works silently from 1.0 s on (C).
- **The course intro never ends by itself on PC.** The update `0x5b1120` ignores its 5 s timer, and the fly-by camera loops
  its shots forever. It ends only on a press, a missing course graph, or the autotest flags (C). So "no full vote -> the
  intros play out by themselves" needs our own course-intro timeout. The racer intro does end by itself, after 5 s (250
  ticks at 50 Hz) (C).
- **Setting the text:** `0x47f3a0(0xd80223a4, textObj)`, stdcall. Read the localized base string from the element itself,
  at `[[elem+0x104]+0x18]`. The text is left-aligned, and "CONTINUE (8/8)" fits in every language scene (widest right
  edge 0.970 of the layout width). No script and no exe code ever rewrites this element's text (C).
- **Skipping:** call `0x4cce60()` once in the course-intro hook, then once on the racer intro's first tick. Never jump
  straight to `0x4ccd10(0x3b)`: that skips the racer intro's enter, which stops the fly-by camera, sends
  HUD_INITMISSIONHUD (its only sender) and draws once from the shared random-number generator (C).
- **Pressing without skipping:** both press tests are edge-triggered ("pressed this poll") (C). The cleanest block is the
  state's own gate byte `self+0x10`. Raise it to 2 around the course-intro update and lower it to 0 around the racer-intro
  update. The stock update then runs every tick but never skips locally. No input bytes are touched (C).

## 1. The CONTINUE prompt
**Scene and elements (C, from the scene files)**
- The scene is `TRACK_INTRO`, scene hash `e2be26e2` (= name_hash "TRACK_INTRO"). Files
  `Resource\SumoToolResources\TrackIntro\track_intro_<course>` (plus `_4_3`), listed by `loading` at module offsets
  0x0595 and 0x0745; read from `SumoToolResources.xpac` (nested zlib at +0x48, PTEX container), one scene per course and
  language.
- Root group `f87e27a8` = "TRACK_INTRO\TRACK_INTRO", 9 children:
  - `faf4c381` TXTR: the key picture. Sprite `5696d0f2`, atlas `10601bec`, size 0.050x0.089 at scale 0.797, keyframe
    position x 0.771, y 0.869.
  - `d80223a4` TEXT: the word. Font per language; English `33486712` or `81440e86` (16-px line height). Text key
    `0b965bf6` in the scene's own TEXT table (some variants `c64838b0`). Left-aligned (align short desc+0x28 = 0), no
    auto-fit (byte +0x2b = 0). Keyframe position x 0.814 (0.719 / 0.816 in some FR/ES variants), y 0.921 (0.925 in the
    c64838b0 variants).
  - `2f676e7a` TRACK_STRING, `f127538e` TRACK_STRING_0, `7bcae792` TRACK_STRING_1 (course and zone names, right-aligned),
    `49099dbc` LOGO, `b7ed3659`, `b8993c14` (pictures), `611ad389` (GORD).
- The prompt pair's element names could not be recovered (hashes only). Text per language (C): EN "CONTINUE", FR
  "CONTINUER", IT "CONTINUA", ES "CONTINUAR", DE "WEITER". The US scene set holds "Empty" placeholders (I: the game uses EN).
- Group keyframes: slide in `87ea5032` t0 y 0.456 -> `953877d9` t10 y -0.017; fade out `cf96ea4e` t63 -> `096355fa` t83,
  alpha ff -> 00.

**Who shows and hides it (C)**
- **Course intro.** The update `0x5b1120` posts `HUD_TRACKINTRO` on its first tick where `0x4c0b00()` (byte `[0xce8625]`,
  normally 0) is false; the same tick sets `self+0x10 = 1` (0x5b114f). `hud` handles it at 0x4e04 (dispatch at hud
  0x2e6f): shows the scene 0205(e2be26e2) at 0x50b5; sets the three name texts with 021f at 0x50f0, 0x5101, 0x5112;
  **hides the prompt only for game type 5** (bytes `32 00 16 09 00 00 d1 07 51 05 4a 14 1c 00 02 81 c3 f4 fa 32 01 18 02
  00 00 d3 07 18 02 a4 23 02 d8 32 01 18 02 00 00 d4 07 18` at hud 0x511b); plays the slide-in 020e(node, 87ea5032,
  953877d9, 0) at 0x5158. So the prompt appears on the first course-intro update and slides in over 10 animation frames
  (frame timing: I). The press is accepted from the same tick (the skip also needs `+0x10 == 1`).
- **Racer intro.** Entering 0x3a posts `HUD_CHARACTERINTRO` (flow record 0x6f5304, field +0x10 = b00bd395, posted by
  `0x4ccd10`). `hud` handles it at 0x450a: 0212(node, cf96ea4e, 096355fa, 0) at 0x4528 (the fade-out, waited for); then
  0206 hides scenes e2be26e2 (0x4536), 14527a4f TRACK_INTRO_GP (0x4544), ab2ae52f (the music-title banner, 0x4552) and
  7cecc470 BUTTONS (0x4560). **No prompt during the racer intro**; the press is accepted from 1.0 s (C).
- d80223a4 and faf4c381 occur exactly once each in FrontEndScripts: hud 0x5138 and 0x512a (package offsets 0x2dcf8 /
  0x2dcea). The exe has no reference to either hash, nor to 0b965bf6 (C). `INTRO_CAMERA_SWITCHED` (posted by the fly-by at
  0x53ea85; handler hud 0x5168) matters only in Grand Prix (native 2112 = `0x4d4260`) (C).

## 2. Writing "CONTINUE (1/2)"
**Find the element.** `0x483860` is thiscall with ECX = `0xc28340` and one stack arg (the hash), `ret 4`. Returns the
element, or 0 if missing or not a TEXT element (checks `[[e+0xdc]+4] == 'TEXT'`). Bytes `8b 44 24 04 50 e8 b6 ff ff ff 85
c0` (C).

**Localized base string.** `desc = [elem+0x104]`, `str = [desc+0x18]` (UTF-8, into the loaded scene's TEXT table) (C): the
element constructor `0x485260` fills desc+0x18 = `0x486770(desc+4 key)`; the draw `0x47ece0` uses that pointer when there
is no free text. Prefer this over a fixed key (0b965bf6 or c64838b0 per variant). (The 021f path: `0x4a6f80` -> `0x47f3d0`
-> `0x4855d0` looks the key up in `[elem+0xe8]+0x2c8`, writes desc+0x18 and clears the free text (C).)

**Set the text.** `0x47f3a0` is effectively **stdcall** with 2 args, `ret 8`; it loads ECX = 0xc28340 itself. Bytes `8b 44
24 04 50 b9 40 83 c2 00 e8 b1` (C). `0x485410` renders `obj->vtbl[0]` into a 4 KB stack buffer (slot 0 of vtable
0x6a71f8 = `0x485200` = `sprintf(dst, "%s", [obj+0xc])`), makes its own heap copy at elem+0x108 (owned flag +0x10c = 1)
and frees the previous owned copy through `0x4853c0` (C). A static `{0x6a71f8, 0, cap, char*}` object can be reused; keep
the string well under 4 KB.

**Draw (C).** Element vtable 0x6a7200 slot +0x28 = `0x485520` -> `0x47ece0`: free text +0x108 wins over the key string;
UTF-8 -> UTF-16 in `0x482c90` (1- to 3-byte sequences); layout `0x47da60` (alignment short desc+0x28: 1 centre, 2 right,
else left; auto-fit byte desc+0x2b with width desc+0x2c). Each font has 192 glyphs: ASCII 0x20-0x7e, Latin-1 0xa0-0xff and
a 0xffff fallback; "(", ")", "/" and digits exist in every prompt font (C).

**Room (C, advance sums, kerning ignored).** EN "CONTINUE" 121 px -> "CONTINUE (8/8)" 178 px at 1280 layout units. Over all
112 scenes the widest right edge is 0.970 of the screen width (ES "CONTINUAR (8/8)" at x 0.816). It fits.

**Lifetime (C).** Nothing rewrites the text (no 021f/021e/0248 on d80223a4, no exe reference). The element is destroyed when
Countdown's enter (`0x5b17a0`, 0x5b183f) posts UNLOAD_TRACK_INTRO_RESOURCES (`loading` 0x0d34). Apply on the first
course-intro tick where `0x483860` returns non-zero, then again whenever X or Y changes; game thread only (it frees the
previous copy). Do **not** return 5 from 0916 at hud PC 0x5123 (that hides the prompt; m6-finish (d)1 conflicts with this).

## 3. Skipping from code
**`0x4cce60`** is cdecl, no args: `a1 70 7a cf 00 8b 48 08 51 e8 a2 fe ff ff 59 c3`. It calls `0x4ccd10([[0xcf7a70]+8])`
(record: +0 name, +4 state, +8 next, +0xc seconds, +0x10 HUD message, +0x14 racer mode). `0x4ccd10` (C): 1. sets
`[0xcf7a70]` to the new record (`0x5744e0`, ECX = `[0xcf7a78]`; the PC table 0x6f5238 has 0x39, 0x3a, 0x3b); 2. posts the
record's HUD message to `hud` (from "RaceManager", through `0x4bb220`); 3. resets the phase timer object 0xcf7a88: counter
`[0xcf7a8c]` = 0, limit `[0xcf7a90]` = seconds x `[0xc2a020]`; 4. `0x49b660(record+0x14, -1)` for all racers; 5.
`0x50e140(state)` with ECX = 0xcf7a98: flushes any pending state operation (`0x50d210`), then only **queues** the replace
(flag 4, +0x38 = new state); the old state's exit (+0x18) and the new state's enter (+8) run at the next flush.
`[0xc2a020]` = 50 ticks per second (C).

**From TrackIntro (C):** goes to 0x3a (record 0x6f52d4, next 0x3a, 5 s, no HUD message); posts HUD_CHARACTERINTRO; the
queued CharacterIntro enter `0x5b1390` runs and its first update is the next HookCharacterIntro call; a second `0x4cce60`
there (record 0x6f5304 -> 0x3b, 3 s, HUD_GETREADY) reaches the Countdown. **Call it at most once per tick, and only after
the original update** (a second call in the same tick would run `0x50d210`, which exits and deletes `self` while inside
its update (I)). Guard with `[[0xcf7a70]+4] == 0x39` (or 0x3a); 0x4ccd10 updates that record at once (`0x4ccd26`).

**What `0x4ccd10(0x3b)` straight from TrackIntro would skip (C, CharacterIntro enter `0x5b1390`):** `0x4a0de0` ->
`0x53e3e0` (**stops the fly-by camera**); one shared-RNG draw at 0x5b13d4 (camera pick % 3); the per-viewport intro camera
(`0x4deab0`) and the 5 s timer (`0x4ccf40(5)`; 6 s if `[0xd11b18+0x8ac]`); the only `HUD_INITMISSIONHUD` post (hud 0x44ac
runs 0x3a88, 0x5508, 0x4038 and native 1f05); `0x49db20(1, 1.0)`, `0x4f97a0(1)`, `0x4d2bf0(0x34...)`, `0x4b5f00` /
`0x4b5f40(1)`, `0x5178a0(1,0)`, racer event `0x540500(0xb)` for every racer, announcer `0x4bbda0(0)`; the HUD_CHARACTERINTRO
post; CharacterIntro exit `0x5b1600` (`0x4dea90` per viewport, `0x4b6010(1)`). **Not recommended.**

**Natural endings (C):** course intro: none (`0x5b1127 call 0x4cbbb0` increments the counter, result ignored; the fly-by
`[0xc2a0fc]`, updated by `0x4a15a0` -> `0x53e930`, cycles its shots and wraps (`0x53e310`), never calls 0x4cce60). Racer
intro: `0x5b16d3` `0x4cbbb0` true at counter >= limit (250 ticks = 5.0 s) -> `0x4cce60` at 0x5b1745; elapsed `0x4cc450()`
= counter / 50 s. Shared-RNG use during the intros is once per race and independent of their length (C): the fly-by shot
shuffle at 0x53e2c0 (inside TrackIntro enter's `0x4a0da0`), the CharacterIntro enter draw, possibly hud native 1d01 in the
HUD_TRACKINTRO handler. TrackIntro -> CharacterIntro -> Countdown on every PC keeps every PC's draw count the same.

## 4. Pressing to vote without skipping locally
**The inputs (C).** P = `[[0xc2a1b4]+0x14]` (`0x4a23a0` returns `[0xc2a1b4]`; no null check in stock). Device =
`[P+0xc+[P+0x960]*4]`; the keyboard device (vtable 0x67e444) is at P+0x4c and in that device list (0x406f99 / 0x406f9c).
Course intro: device byte +0xf7 = +0xd4 + id 0x23 (0x5b1234), or `kb->vtbl+0x28(0x1c)` (0x5b124f). Racer intro: device byte
+0xf3 = id 0x1f (0x5b1706), or the same raw Enter (0x5b1721). Keyboard defaults (`0x406f24..0x406f8c`, scancode at
dev+0x18+4*id): id 0x23 = Enter (0x1c), **id 0x1f = Space (0x39)**, id 0x1c = Enter (START). Pad: ids 0x23 and 0x1f are
both button 0x16 (I: A). Keyboard slot +0x28 = `0x406360`: bit 4 of `[kb+0x4f8+key]` (`8b 44 24 04 0f b6 84 08 f8 04 00
00 c1 e8 04 83`).

**Edge, not level (C)** (`0x4ae190` pad/id flags, `0x406230` raw keys). Bits per poll: 0x01 held; 0x10 pressed this poll
(0x3d on the up->down edge, or a 0x15 repeat pulse only for ids with bit 0x02 set: ids 0xd-0x10 and 0x17-0x1a, not 0x1f,
0x23 or raw Enter); 0x40 released this poll; 0x80 blocked until released (`0x4ae2e0`). The next poll depends only on bits
0, 1 and 7: clearing bit 4 is a safe "consume" but not needed. Stock read-only helper `0x4ac9a0` (thiscall ECX = P; args
id, buf) returns bit 4 of `[dev+buf*0x2e+0xd4+id]` (the Podium uses (0x1f, 0)).

**Per tick besides the skip test (C):** TrackIntro: the counter tick, plus the one-time HUD_TRACKINTRO post with `+0x10 =
1`; the fly-by runs elsewhere. CharacterIntro: `0x4cc000(0)`; per racer `0x4e82d0` and `0x587220` (or `0x4cbbd0`);
`0x4dea40(t)` per viewport; the timer; the skip; else `0x4cd090` (position sort and events). The polls are per frame and
the state update is a fixed 50 Hz tick, so one press can stay set across two ticks (I).

**The gate byte (C).** Both skips require `byte [self+0x10] == 1` (course intro `80 7d 10 01` at 0x5b121b; racer intro `80
7f 10 01` at 0x5b1729). The course intro's HUD post fires only when the byte is 0 (`38 45 10` at 0x5b113d). So: course
intro: if +0x10 == 1, set it to 2 -> original -> back to 1 (no HUD resend, no skip; the press sound and the
no-course-graph/autotest auto-skips are suppressed too). Racer intro: save, set 0 -> original -> restore; the timer still
ends it at 5 s. The state object stays alive until the next flush, so restoring after the original is safe even when its
timer has just queued the Countdown (C). For the vote, read the stock inputs read-only before the original; count a press
only when the stock code would accept it: course intro with +0x10 == 1; racer intro with +0x10 == 1 and t >= 1.0 (optional).

## 5. Game type 5 and the console's online code
PC (C): hud 0x511b hides the prompt for type 5; TrackIntro enter skips the course voice line for type 5 (`0x5b0fa2`);
Countdown exit `0x5b1910` plays the same music cue for types 1, 2, 5; hud per frame `baa59db4` (0x3638): type 5 calls the
empty 0x5b02 and 0x622c; the intro updates and CharacterIntro enter make no type test; no `NetRace` state names in the PC exe.
Mac build (read-only reference): `State_NetRace_TrackIntro` (typeinfo 0x67c820) and `State_NetRace_CharacterIntro`
(0x67c860) exist (C). The online course intro (update 0x163fb0) has **no press skip**: a ready barrier (marks the local
player ready via `0x1999b0` once an averaged value is <= 9; advances through `0x15d610` when the network reports everyone
ready; never posts HUD_TRACKINTRO) (C; the averaged value's meaning: I). The online racer intro (enter 0x164080) sets
`+0x10 = 0`: **not skippable**, full 5 s (C). So the console never skipped online intros; our vote is a new feature.

## 6. Smallest safe implementation (as built in src\race.cpp IntroUpdate)
```
HookTrackIntro(self):                       // slot 0x6b2100 -> 0x5b1120
  Tick(); shown = s[0x10]==1;
  if (shown && PressedTrack()) CastVote();  // dev[+0xf7]&0x10 || kb->vt[+0x28](0x1c)
  if (shown) s[0x10]=2;  Original(self);  if (shown) s[0x10]=1;
  if (shown) ApplyPrompt();                 // 0x483860(ECX=0xc28340,0xd80223a4); "%s (%u/%u)" from [[e+0x104]+0x18]; 0x47f3a0 on change
  if (State()==0x39 && (AllVoted() || [0xcf7a8c] >= limit)) Advance();  // 0x4cce60 once; limit = [0xcf7a90] (5 s)
HookCharacterIntro(self):                   // slot 0x6b2124 -> 0x5b1640
  Tick(); b=s[0x10]; if (b==1 && PressedChar()) CastVote();   // dev[+0xf3]&0x10 || raw Enter
  s[0x10]=0;  Original(self);  s[0x10]=b;
  if (State()==0x3a && AllVoted()) Advance();
State() = *(int*)(*(u32*)0xcf7a70 + 4).
```
Optional skip sound: `0x577ea0(0x91beb24c, 0, 1)`, cdecl (`6a 01 6a 00 68 4c b2 be 91 e8 ... 83 c4 0c`). Hold B stays the
final sync. The course intro needs the timeout, or a race with no full vote waits forever. Keep the race-wide vote state
(both intros); recompute Y when a player leaves.
Optional (untested, every point I): keep a counter on screen during the racer intro by ScriptSite-skipping 0212 at hud
0x4528 (PC 0x4530; `32 04 12 02 00 00 1c 06`) and 0206 at hud 0x4536 (PC 0x453e; `02 e2 26 be e2 | 32 01 06 02 00 00 1d
06`) and hiding the banner children with `0x47f1d0(hash, 1)` (stdcall, `ret 8`): 2f676e7a, f127538e, 7bcae792, 49099dbc,
b7ed3659, b8993c14. Simpler, recommended: the counter only on the course intro; racer-intro presses vote silently.

## 7. Corrections to earlier notes
- **m6-finish §6.** "TrackIntro ... or the fly-by's end (shot sequencer 0x534730 -> 0x4cce60)" is wrong: `0x534730` is
  called only from `0x5347a0`, which has no call and no dword reference (likely dead code) (C); the course-intro fly-by is
  `0x53e930` and loops forever; the "5 s" for TrackIntro in the flow table is never used on PC.
- **m6-race-hud.** `0x47f3a0` is stdcall (`ret 8`) and sets ECX itself; calling it as thiscall with ECX = 0xc282a8 is harmless.

## 8. Confidence
High (C): the element identities, positions, text, alignment and fit; the text API and its lifetime; the gate bytes and the
edge bits; `0x4cce60`'s behaviour and its once-per-tick rule; the course intro having no natural end; the racer intro's 5 s
timer and 1 s skip threshold; what CharacterIntro's enter does. Medium (I): the animation frame timing; the use-after-free
if 0x4cce60 runs twice in one tick; the UX of the optional racer-intro prompt; the Mac barrier values. Not tested in game.
