# Fullscreen: the invisible game after a focus loss (2026-10-27)

The bug: a friend in exclusive fullscreen (settings.ini WINDOWED=0) finished an online race (b270217e, 1.1.0); on the
results screen the game became invisible and clicking it on the taskbar did not bring it back; the process kept
running (the player's SteamMultiplayer.log, last line 01:48:51 UTC 2026-10-26). All our tests were
windowed. Analysis by the reverse-engineer pass on `re\bin\SSASR-unpacked.exe` (scripts and raw output in
`scripts\`, `out\`); saved here by the main session. Nothing below was run in game.

## From the friend's log
- Steam overlay off; no "view profile" line (F1 was not the trigger).
- The `- window` launch option does nothing: the exe has no such option (CONFIRMED).
- On earlier results screens their car said a line every 15-40 s; none after 01:48:51, so the game thread probably
  stopped soon after (INFERRED).

## Device creation (CONFIRMED)
- Platform object `[0x7f9ff0]` (written at 0x40376e in its constructor 0x4036f0).
- "WINDOWED" (VA 0x67e174), only user `push` at 0x40392e; 0x403957-0x403960 stores `platform+0x60 = (WINDOWED > 0)`.
- Fullscreen = WS_POPUP window, windowed = overlapped; CreateWindowExA at 0x4027c4, hwnd at platform+0x1c.
- Present parameters at platform+0x24: SCREEN_WIDTH x SCREEN_HEIGHT, A8R8G8B8, 2 back buffers, DISCARD;
  **Windowed = byte +0x60, copied to dword platform+0x44** (0x402a2e / 0x402a38); D24S8; interval IMMEDIATE if
  VSYNC=0, else DEFAULT.
- CreateDevice (IDirect3D9 +0x40) at 0x402bdc: adapter +0x88, HAL, focus window = hwnd, flags 0x40 (hardware VP),
  device at platform+0x5c (copies at `[0xdea8e8]`, `[0xc28308]`).
- Order: settings read, RegisterClass (0x403c4a), window (0x403c59), device (0x403c60).

## Device loss (CONFIRMED)
- The only handler is 0x403680, called from render A (0x4962b1), render B (0x496dde) and the main loop (0x497487).
  TestCooperativeLevel (+0x0c):
  - DEVICELOST: SwitchToThread, Sleep(50), skip rendering.
  - DEVICENOTRESET: 0x4035a0 releases resources and sets `platform+0x82 = 1`, then Reset (+0x40) at 0x4036cd;
    failure returns 1 with +0x82 still 1; success: 0x402f30(0) recreates resources and clears +0x82.
  - any other error: exit (0x64824f(1)).
- While +0x82 = 1 the main loop 0x497470 only pumps messages and retries 0x403680 (no engine frame, no picture).
- Render and Present (platform slot +0x40 = 0x402ec0, result ignored) run every loop, even when 0x4028fd skips
  the update.
- **Nothing in this path reads `[0xc2a016]`, platform+0xa4, or depends on the skipped update.** The WndProc has no
  WM_SIZE, WM_ACTIVATEAPP or WM_PAINT handling.

## Readers of `[0xc2a016]` (27 hits, CONFIRMED): all "paused" checks, none in D3D / Present / Reset / window / input
UI/script update gate 0x495460 (used 0x496854); world/race update gate 0x495480 (0x496885); audio pause state
machine 0x495e10 (with our patch audio keeps playing); sound trigger 0x4954b0; frame counters 0x49628f, 0x496db7,
0x4a072f; time-scale 0x496813; intro camera 0x4a15c8; anim/timer steps 0x4c0103, 0x4c5fd2, 0x4d067b, 0x4d071e,
0x4e44c0; HUD timer 0x4ff8ad; ring buffers / race logic 0x5746df, 0x58ca32, 0x58ed8c, 0x59169b, 0x59172b,
0x59226c, 0x5922bc; debug pause toggle (no callers) 0x4d17fe; script native "is paused" 0x5fe1eb.
Writers: 0x403180, 0x403302, 0x403323.

## Window restore (CONFIRMED)
ShowWindow only at 0x402878 (creation) and 0x402cd6 (minimum-spec exit); SetWindowPos only at 0x402868 (windowed
centring); no ChangeDisplaySettings / SetForegroundWindow import. The game never restores its own fullscreen window:
D3D9's focus-window hook and DefWindowProc minimize and restore it (INFERRED from D3D9 behaviour).

## Likely mechanisms
- **M1 (INFERRED, medium): a non-pumping input spin.** The keyboard read (0x406075) retries `Acquire` endlessly at
  0x406090-0x4060a3 while it returns DIERR_OTHERAPPHASPRIO, with no Sleep and no message pump; the mouse has the
  same loop at 0x4065d2 (Sleep(16)). Both devices are FOREGROUND | NONEXCLUSIVE. The stock game reads input only
  after WM_ACTIVATE(active) was handled (the frame is skipped while inactive). Our wrapper reads the real device
  whenever the foreground window belongs to our process, so a taskbar click that makes the still-minimized window
  the foreground can reach that loop before the restore message is handled; if DirectInput refuses Acquire then, the
  thread spins forever: window stays minimized, clicks do nothing, process alive, log stops. UNKNOWN: whether real
  dinput8 refuses Acquire in that state.
- **M2 (INFERRED, low-medium; may also happen in the stock game): Reset never succeeds.** An unreleased DEFAULT-pool
  resource keeps +0x82 = 1 forever; the window is restored but never painted (class brush NULL: stale pixels).
  Candidate: the UI particle element (0x486920 -> 0x482100 -> 0x4812f0) makes two dynamic DEFAULT-pool vertex
  buffers per element and is not in the lost-device list `[0xdea9b0]`.
- Telling them apart: M1 = window stays minimized, one core at 100%, game thread in 0x406090 or 0x4065d2.
  M2 = restored black / stale window, `[[0x7f9ff0]+0x82]` stuck at 1, low CPU.

## Proposed fix (our code only, no new game patch sites)
1. `KbdGetState` / `MouseGetState`: if the real GetDeviceState fails, Acquire once and read again; if it still fails,
   zero the buffer and return S_OK, so the game's endless loops are never entered.
2. `GameHasFocus`: the foreground window must be the game's window (`[[0x7f9ff0]+0x1c]`) and not minimized.
3. Optional: in exclusive fullscreen (`byte [[0x7f9ff0]+0x60] == 0`) keep the stock bytes at 0x4028fd, 0x403302,
   0x403315 (background running off in fullscreen; alt-tab then pauses that player's game, as in the stock game).
4. Diagnostics: log when +0x82 stays 1 for more than 3 s, and when a wrapper's real read fails while "focused".

Test (the user): fullscreen, alt-tab on the results screen and during a race, back via the taskbar; with
`RunWhenUnfocused=0` (stock baseline: if that fails too, M2 is a stock bug) and `=1`, before and after the fix.
