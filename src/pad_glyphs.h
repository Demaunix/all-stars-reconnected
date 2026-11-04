#pragma once
// Controller button pictures in the online menus (1.2; the user's request 2026-10-27, choices 2026-10-29; re\pad-glyphs\FINDINGS.md,
// FINDINGS2.md). The footers draw button pictures as private-use characters F000-F007 of a font; tools\pad_glyphs.py adds the
// consoles' own pictures as F010-F017 (Xbox) and F018-F01F (PlayStation), and a copy of the PC's keys as F020-F027, to every scene
// that draws them. While this player is in the online menus or an online race and plays with a controller, the wanted set's
// records are copied over F000-F007 (the keys again otherwise), in the lobby / game list footer and the online results / pause
// button bars. config.ini [Menus] PadPictures = auto | xbox | playstation | keys.
#include <stdint.h>

// Game thread, from the native dispatcher (natives.cpp): every few ms, the wanted set into the footer and button bar fonts.
void PadGlyphsFrame();
// INVITE FRIENDS / VIEW PROFILE on a controller (the user's choice: Back / View; Share / Create on a PlayStation pad): held now on
// the primary pad. False when the primary device is the keyboard (whose action 34 is Esc).
bool PadGlyphsInviteHeld();
// Steam worker: which pad Steam Input presents behind each XInput slot (a PlayStation pad over Steam's Xbox emulation), every
// second; Stop before SteamAPI_Shutdown.
void PadGlyphsSteamPoll();
void PadGlyphsSteamStop();

#ifdef PADGLYPHS_TEST
// tests/test_pad_glyphs.cpp: the game's lookups replaced by fakes; the throttle off.
typedef uint8_t* (*PadGlyphsFindFn)(uint32_t element);
typedef bool (*PadGlyphsProbeFn)(uint8_t* scene, uint32_t sprite);
void PadGlyphsTestSetup(PadGlyphsFindFn find, PadGlyphsProbeFn probe, uint8_t* (*primary)(), int (*layer)(), bool (*online)());
void PadGlyphsTestSetMode(int mode);            // 0 auto, 1 xbox, 2 playstation, 3 keys
void PadGlyphsTestSetFocused(bool focused);     // this game's window in front (true by default)
void PadGlyphsTestSetSlot(int slot, int kind);  // Steam Input's view of an XInput slot: 0 none, 1 another pad, 2 PlayStation
int PadGlyphsTestShown();                       // the set last chosen: 0 keys, 1 Xbox, 2 PlayStation
#endif
