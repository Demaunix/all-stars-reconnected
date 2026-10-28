# Voice chat on Steam: design facts (2026-10-23, researcher pass; saved by the main session)

Tags: C = seen in a file/header; R = recalled from Valve's docs / general knowledge, not verified here. Goal: the console's
online voice chat (the lobby bars' voice icon lit while a player talks; in-race voice) built with Steam voice inside the mod.

## Conclusion
Feasible in-process: record and encode with ISteamUser on our Steam worker thread, send the compressed packets unreliably on
a NEW networking channel, decode on the receiver's worker, play through our own XAudio2 engine (the game has its own XAudio2
streamer; a second engine is independent). The bar's voice node exists in the PC data and is only blanked today. In-game ears
are needed for latency, volume, echo and the visible icon art.

## 1. The Steam voice API (SDK 1.65: isteamuser.h 71-123, steamclientpublic.h 153-162) C
- `StartVoiceRecording()` / `StopVoiceRecording()`. Stop keeps recording briefly (players release push-to-talk early): keep
  calling `GetVoice` until it returns `k_EVoiceResultNotRecording` (2).
- `EVoiceResult`: OK 0, NotInitialized 1, NotRecording 2, NoData 3, BufferTooSmall 4, DataCorrupted 5, Restricted 6,
  UnsupportedCodec 7, ReceiverOutOfDate 8, ReceiverDidNotAnswer 9.
- `GetAvailableVoice(&cbCompressed)`: bytes ready. The deprecated "uncompressed" parameters stay null/0.
- `GetVoice(true, buf, cb, &written)`: compressed data; the header says call at least once per frame, better every few ms, to
  keep the mic delay low. `NoData` is normal (no packet).
- `DecompressVoice(pCompressed, cb, dest, cbDest, &written, desiredRate)`: raw mono 16-bit PCM at any rate 11025-48000; the
  header suggests a 20 KB start buffer; `BufferTooSmall` sets `*written` to the size needed (retry).
- `GetVoiceOptimalSampleRate()`: the decoder's native rate (least CPU); the header says output quality may be better at the
  device rate (44100/48000). Plan: decode at the optimal rate and make the XAudio2 source voice match (XAudio2 resamples).
- The codec is Opus; each `GetVoice` chunk decodes on its own (R). Typical chunk: tens to ~150 bytes per 20-50 ms (R: measure).
- Steam's own Voice settings (mic, push-to-talk) are NOT applied by the API as far as recalled: the game owns push-to-talk (R).
  `SteamFriends()->SetInGameVoiceSpeaking(id, bool)` only tells the Steam friends UI that the user is talking in-game and
  suppresses the Steam-UI mic (isteamfriends.h 266-267, C): call it with our own id on the transmit edges; optional.
- Transport: ISteamNetworkingMessages as the race uses (`steam_mp.cpp` ~475: `SendMessageToUser(to, data, size, flags,
  kRaceChannel)` with UnreliableNoNagle | AutoRestartBrokenSession). For voice prefer `k_nSteamNetworkingSend_UnreliableNoDelay`
  (= Unreliable|NoDelay|NoNagle, steamnetworkingtypes.h 1005, C): a packet that can't go out now is dropped, which suits voice.
  Max message 512 KB (steamnetworkingtypes.h 841); unreliable messages may be dropped or reordered (isteamnetworkingmessages.h
  62): a sequence number per packet. The race wire's `kMsgVoice = 13` is the DRIVER LINES mirror, not chat: voice chat takes a
  NEW channel (2) with its own tiny header (magic "ASV1", version, sender slot, u16 seq, u16 len) because race messages need a
  race id and are taken only in race ticks, while voice must work in the lobby too. Receive like PumpRace: `IsMember(from)`.

## 2. Playback C unless noted
- `xaudio2.h` is in `C:\Program Files (x86)\Windows Kits\10\Include\{10.0.19041.0, 10.0.22621.0, 10.0.28000.0}\um\`; libs
  `...\Lib\*\um\x86\xaudio2.lib`, `xaudio2_8.lib`, `WinMM.Lib`. SysWOW64 has XAudio2_0..9.dll (XAudio2_9 exists, 32-bit).
- The Win10 SDK header is XAudio2.9: `XAudio2Create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)`; linking xaudio2.lib needs
  XAudio2_9 at runtime (Win10+; fine on the user's Win11). For friends on Win7/8: `LoadLibrary("xaudio2_9.dll")`, else waveOut
  or voice off (R).
- Minimal setup: `CoInitializeEx(MULTITHREADED)` on our thread, `XAudio2Create`, `CreateMasteringVoice`, then per peer a
  `WAVEFORMATEX {WAVE_FORMAT_PCM, 1 ch, rate, 16 bits, blockAlign 2, avgBytes rate*2}` -> `CreateSourceVoice(&v, &fmt, 0, 1.0f,
  &callback)`, `v->Start()`; `SubmitSourceBuffer` per decoded chunk from a ring of heap buffers; jitter target 2-3 chunks
  (~60-100 ms): when the queue empties wait for 2 chunks; past ~6 drop the oldest (Flush) to cut latency. Use
  `IXAudio2VoiceCallback::OnBufferEnd` only to free buffers (no Steam or XAudio2 calls inside callbacks: their thread).
- The game's own audio: its own XAudio2 streamer (`iSAL1_Streamer_XAudio2`, vtable 0x6a2300; `re\m7-announcer2\FINDINGS.md`
  93). Its ~3-streams limit is its own sound system's, not XAudio2's. Which XAudio2 DLL the game loads: not determined; our
  own engine is independent. The game's volume settings: not located in the notes; give voice its own `[Voice] Volume=` (0-100).
- waveOut (winmm) works too (higher latency, fixed buffers): a fallback only.

## 3. The console's voice UI in the PC data
- Lobby bar tuple column n4 = voice (`re\m6-lobby\FINDINGS.md` 15-27, C): row n4 node hashes 4e7d2ee9, e9cc98e1, 6c2a3eb0,
  e9cb15e3, 9f1504f9, 00154c74, b9fa1d90, fcc01dd8 (rows 0-7); the PC scene's default art 24064704 is **a white speech
  bubble** (54x46, the lobby atlas's page 0, x948 y420) — CORRECTED 2026-10-23 by a second pass: the m6 note "fully transparent"
  came from a page-index bug in `re\m6-lobby\scripts\atlas.py` (it read the page from before the hash = the previous record's);
  `out\list2.py` parses the 110 records right (`[u32 hash][8 f32 UV][u32 page]`, 28-byte header, DDS pages at 4460) and wrote
  `out\s_24064704.png` (the bubble) and `out\page0..3.png`. So showing the node (0217) shows the icon; no 0228 swap needed.
  Other small cells: the green check 9309f511 (50x50), the 1P badge 106e548c, green signal bars 7bf528a4 (page 1).
- The Mac/console draw rule (FINDINGS.md 35, C): per bar `voice r[8] ? 0217(show) : 0218(hide)` on n4, r = member info from
  native 221c(i), r[8] = the voice flag: the icon lit while that member talked.
- Today: `tools\netmenu\steam.py` BAR_VOICE hidden with 0218 ("no voice chat (blank art on PC)"): replace with the flag from
  our member native once voice exists.
- Race HUD speaker icon: none found. `re\m7-inventory\FINDINGS.md` 81/156/169: the console used the platform's party voice
  (PSN / Xbox party chat); strings MSG_BOX_CHAT_RESTRICT and TALKING exist. The PS3 notes have no voice text (C).

## 4. Mute / privacy
- No SDK call for per-user mute of in-game Steam voice found; Steam's overlay mute applies to Steam's own chat (R). So mute is
  ours: `[Voice] Muted=<steamid64,...>` in config.ini (a key toggle later; every lobby key is taken), and ignore voice from
  anyone ISteamFriends reports blocked (R, unverified).
- Push-to-talk: `[Voice] Mode=off|ptt|open`, `Key=` (virtual key name) polled with `GetAsyncKeyState` on the worker, only while
  our window is in the foreground (two games on one PC share the keyboard). Recommended default `Mode=off` (privacy) until the
  user opts in.
- Scope: lobby and race (the console's party chat ran through both, R). Only current lobby members are heard; never forward;
  cap 8 senders and ~60 packets/s per sender.

## 5. Threading
- Steam calls on our worker only (`GetVoice` / `DecompressVoice` in the worker loop, ~every 20 ms: the loop runs every 30 ms in
  the menus and 2 ms in a race today, `steam_mp.cpp` ~1380).
- PCM to XAudio2 through per-peer buffers; `SubmitSourceBuffer` is thread-safe from the worker; no XAudio2 calls inside its
  callbacks. Set up XAudio2 on the worker; shut it down in Stop().
- Decode per sender in sequence order; drop older out-of-order packets; on a gap just continue (Opus hides loss, R).
- The bar icon follows the snapshot: a member's `voice` flag = (now - lastVoiceTick) < 300 ms, compared signed (the M7 lesson);
  our own flag while transmitting; read on the game thread in the lobby frame native only.
- No new DLL to ship: XAudio2_9 comes with Windows; steam_api.dll is ours already.

## Recommended data flow
1. Worker, every ~20 ms: Mode open, or the PTT key down -> `StartVoiceRecording` once; `GetAvailableVoice` -> `GetVoice` into a
   1.5 KB buffer -> one packet. Key up / Mode off -> `StopVoiceRecording`, drain until NotRecording.
2. Send: header + data, UnreliableNoDelay (+ AutoRestartBrokenSession) to every other member on channel 2. Budget: ~40-120 bytes
   a chunk (R: measure), 40-60 packets/s = 5-10 KB/s per listener; 7 listeners < 70 KB/s up at worst. Cap the payload at 1200 B.
3. Receive (worker): `ReceiveMessagesOnChannel(2)` -> member check -> `DecompressVoice` -> that peer's source voice (created on
   its first packet); `peer.lastVoiceTick = GetTickCount()`.
4. Playout: one mastering voice, one mono 16-bit source voice per peer, 2-3 chunks of jitter buffer.
5. UI: the member snapshot's voice flag -> the lobby bar's n4 node shown/hidden per bar (and visible art for it);
   `SetInGameVoiceSpeaking(me, transmitting)` on the edges.
6. Config: a new `[Voice]` section (the shipped `package\config.ini` has Background/Menus/Race today): Mode, Key, Volume, Muted.

## Open questions needing in-game ears or more checking
- The real chunk size and cadence of `GetVoice`; the best sample rate; the latency with 2-3 chunks; two games on one PC share
  the mic and the speakers (test with headphones; feedback).
- Whether Steam's own Voice settings (PTT / open mic / mic choice) are honoured by `GetVoice` (R says the game decides).
- XAudio2 next to the game's own streamer: glitches, volume balance; scaling voice by the game's volume setting (not researched).
- ~~Visible speaker art for the n4 node~~: solved, the node's own art is a speech bubble (§3); confirm in game that 0217 shows it.
- A race HUD icon: no console precedent found; skip.
- The unreliable path through Steam's relay with NoDelay during the pause/loading screens.
