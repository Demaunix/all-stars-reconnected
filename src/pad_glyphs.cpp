// Controller button pictures in the online menus: see pad_glyphs.h.
//
// A footer TEXT element draws a private-use character as the glyph record of its font: {+0 sprite hash, +4 char, +6 u, +8 v,
// +0xa w, +0xc h, +0xe xoff, +0x10 advance, +0x12 yoff, +0x14 draw flag, +0x16 unused}, read on every draw (no cache), the sprite
// looked up in the element's own scene only, and a sprite that isn't there ends the string (re\pad-glyphs\FINDINGS2.md Q3, Q4).
// The element: [elem+0x104] its text object, whose +0x14 is the font row it was built with ({+0 hash, +4 u16 glyph count, +8 the
// glyph array, sorted by char}); [elem+0xe8] its scene, whose sprites 0x47b630 probes. So each target element's own font gets the
// wanted set copied into F000.. (every byte but the char), and only after its scene answers for that set's sprite.
#include "pad_glyphs.h"
#include <windows.h>
#include <string.h>
#include "lobby.h"
#include "log.h"
#ifndef PADGLYPHS_TEST
#include "steam/steam_api.h"
#endif

enum { kKeys = 0, kXbox = 1, kPlayStation = 2 };
static const uint16_t kSetChar[3] = {0xF020, 0xF010, 0xF018};  // the keys' copy, Xbox, PlayStation (tools\pad_glyphs.py)
static const wchar_t* const kSetName[3] = {L"the keyboard's keys", L"Xbox buttons", L"PlayStation buttons"};
static const int kRecord = 0x18;

// The elements that draw button pictures and the fonts their scenes give them (re\pad-glyphs\FINDINGS.md, FINDINGS2.md Q4).
struct Target {
    uint32_t element;
    uint32_t fonts[2];
    int count;  // F000.. it uses
    const wchar_t* name;
};
static const Target kTargets[] = {
    {0xe0c0001b, {0xff7f8ec5, 0}, 8, L"the lobby / game list footer"},                 // navigator_info, 7 languages
    {0x56ff27d8, {0x7ea1f827, 0x5e0a0003}, 7, L"the results / pause button bar"},       // the race results scenes, 16:9 / 4:3
    {0xa8541e22, {0x3c005ba2, 0x30f8365e}, 7, L"the second results / pause button text"},
};
static const int kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

// The primary input device's class (its vtable; re\drift-plus\FINDINGS.md) and the fields read from it (FINDINGS2.md Q1, Q2).
static const uint32_t kKeyboardVtable = 0x67e444, kXInputVtable = 0x67e3f8, kDirectInputVtable = 0x67e39c;
static const int kXInputSlot = 0x1d8;           // the XInput pad's user index
static const int kFlags = 0xd4, kLayerSize = 0x2e, kActionBack = 34;  // pad + 0xd4 + 0x2e * layer + action: bit 0 held

#ifndef PADGLYPHS_TEST
typedef uint8_t*(__thiscall* FindTextFn)(uintptr_t table, uint32_t hash);  // 0x483860 (ECX = the element table 0xc28340): a TEXT element or 0
typedef bool(__cdecl* ProbeFn)(uint8_t* scene, uint32_t sprite);           // 0x47b630: the sprite is in that scene's table (AL)
static uint8_t* GameFind(uint32_t element) { return ((FindTextFn)(uintptr_t)0x483860)(0xc28340, element); }
static bool GameProbe(uint8_t* scene, uint32_t sprite) { return ((ProbeFn)(uintptr_t)0x47b630)(scene, sprite); }
// The primary device: [mgr+0xc + 4*[mgr+0x960]], mgr = [[0xc2a1b4]+0x14], trusted only once the title screen chose it (byte
// [mgr+0x95c], native 0400's own check).
static uint8_t* GamePrimary() {
    uint8_t* in = *(uint8_t* volatile*)(uintptr_t)0xc2a1b4;
    uint8_t* mgr = in ? *(uint8_t* volatile*)(in + 0x14) : nullptr;
    if (!mgr || !*(volatile uint8_t*)(mgr + 0x95c)) return nullptr;
    int32_t p = *(volatile int32_t*)(mgr + 0x960);
    return p >= 0 && p < *(volatile int32_t*)(mgr + 4) ? *(uint8_t* volatile*)(mgr + 0xc + 4 * p) : nullptr;
}
static int GameLayer() { return *(volatile int32_t*)(uintptr_t)0x6eca88; }  // the input layer: 0 menus, 1 a dialog, 3 none
// Online: the game list, a lobby (its races, results and pause included) or a create / join on the way.
static bool GameOnline() {
    static LobbySnapshot s;  // (game thread only; too big for every call's stack)
    LobbyRead(s);
    return s.state == kLobbyCreating || s.state == kLobbySearching || s.state == kLobbyResults || s.state == kLobbyJoining ||
           s.state == kLobbyInLobby;
}
// This game's window is in front (two games on one PC share the pads, as natives.cpp KeyDown's keyboard).
static bool GameFocused() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static uint8_t* (*g_find)(uint32_t) = GameFind;
static bool (*g_probe)(uint8_t*, uint32_t) = GameProbe;
static uint8_t* (*g_primary)() = GamePrimary;
static int (*g_layer)() = GameLayer;
static bool (*g_online)() = GameOnline;
static bool (*g_focused)() = GameFocused;
#else
static uint8_t* (*g_find)(uint32_t) = nullptr;
static bool (*g_probe)(uint8_t*, uint32_t) = nullptr;
static uint8_t* (*g_primary)() = nullptr;
static int (*g_layer)() = nullptr;
static bool (*g_online)() = nullptr;
static bool g_testFocused = true;
static bool TestFocused() { return g_testFocused; }
static bool (*g_focused)() = TestFocused;
#endif

static volatile LONG g_slotKind[4];  // Steam Input's view of each XInput slot (the worker): 0 none, 1 another pad, 2 PlayStation
static int g_mode = -1;              // config.ini [Menus] PadPictures: 0 auto, 1 xbox, 2 playstation, 3 keys (-1 not read yet)
static int g_shown = -1;             // the set last chosen (logged on change)
static LARGE_INTEGER g_lastRun, g_freq;
static uint8_t* g_noSets[kTargetCount];  // the font row last logged as lacking the sets (once per row)

static int Mode() {
    if (g_mode < 0) {
        const wchar_t* config = LobbyConfigPath();
        if (!config || !*config) return 0;  // (not known yet: auto, read again next time)
        wchar_t v[32] = L"";
        GetPrivateProfileStringW(L"Menus", L"PadPictures", L"auto", v, 32, config);
        g_mode = _wcsicmp(v, L"xbox") == 0 ? 1 : _wcsicmp(v, L"playstation") == 0 ? 2 : _wcsicmp(v, L"keys") == 0 ? 3 : 0;
        if (g_mode == 0 && _wcsicmp(v, L"auto") != 0)
            Log(false, L"pad pictures: config.ini [Menus] PadPictures=\"%s\" is not auto, xbox, playstation or keys; auto", v);
        Log(false, L"pad pictures: %s (config.ini [Menus] PadPictures)", g_mode == 1 ? L"always Xbox" : g_mode == 2 ? L"always PlayStation"
                                                                      : g_mode == 3 ? L"always the keys" : L"auto");
    }
    return g_mode;
}

// The set this frame wants, and why (for the log).
static int Wanted(const wchar_t** why) {
    int mode = Mode();
    if (mode == 3) return *why = L"PadPictures=keys", kKeys;
    if (!g_online()) return *why = L"not online", kKeys;
    uint8_t* pad = g_primary();
    uint32_t vt = pad ? *(uint32_t*)pad : 0;
    if (!pad || vt == kKeyboardVtable) return *why = L"the primary device is the keyboard", kKeys;
    if (vt != kXInputVtable && vt != kDirectInputVtable) return *why = L"the primary device is unknown", kKeys;
    if (mode == 1) return *why = L"a controller; PadPictures=xbox", kXbox;
    if (mode == 2) return *why = L"a controller; PadPictures=playstation", kPlayStation;
    // The game binds a DirectInput pad only when it is the PS2-to-USB adapter 0B43:0003 (FINDINGS2.md Q2), whose buttons 2 / 1 / 3 / 0
    // / 8 it reads as confirm / back / F1 / F2 / Back: Cross, Circle, Square, Triangle, Select on that adapter (INFERRED).
    if (vt == kDirectInputVtable) return *why = L"a DirectInput controller (the game binds only the PS2 adapter)", kPlayStation;
    int slot = *(int32_t*)(pad + kXInputSlot);
    if (slot >= 0 && slot < 4 && g_slotKind[slot] == 2) return *why = L"Steam Input: a PlayStation controller", kPlayStation;
    return *why = L"an XInput controller", kXbox;
}

// The record of character `ch` in a font's sorted glyph array, or null.
static uint8_t* Glyph(uint8_t* glyphs, int count, uint16_t ch) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint16_t c = *(uint16_t*)(glyphs + kRecord * mid + 4);
        if (c == ch) return glyphs + kRecord * mid;
        if (c < ch) lo = mid + 1;
        else hi = mid - 1;
    }
    return nullptr;
}

// One element: its font's F000.. become the wanted set's records (the keys when the set's sprite isn't in its scene).
static void Apply(int t, int want) {
    const Target& tg = kTargets[t];
    uint8_t* elem = g_find(tg.element);
    if (!elem) return;
    uint8_t* text = *(uint8_t**)(elem + 0x104);
    uint8_t* row = text ? *(uint8_t**)(text + 0x14) : nullptr;
    uint8_t* scene = *(uint8_t**)(elem + 0xe8);
    if (!row || !scene) return;
    uint32_t font = *(uint32_t*)row;
    if (font != tg.fonts[0] && font != tg.fonts[1]) return;  // (another font: not ours to touch)
    int count = *(uint16_t*)(row + 4);
    uint8_t* glyphs = *(uint8_t**)(row + 8);
    if (!glyphs || count <= 0 || count > 4096) return;
    uint8_t* dst[8];
    uint8_t* src[3][8];
    for (int i = 0; i < tg.count; ++i) {
        dst[i] = Glyph(glyphs, count, (uint16_t)(0xF000 + i));
        for (int s = 0; s < 3; ++s) src[s][i] = Glyph(glyphs, count, (uint16_t)(kSetChar[s] + i));
        if (!dst[i] || !src[0][i] || !src[1][i] || !src[2][i]) {
            if (g_noSets[t] != row) Log(false, L"pad pictures: %s's font %08x has no controller pictures (another mod's version of its scene?): its keys stay", tg.name, font);
            g_noSets[t] = row;
            return;
        }
    }
    if (want != kKeys && !g_probe(scene, *(uint32_t*)src[want][0])) {
        if (g_noSets[t] != row) Log(false, L"pad pictures: %s's scene lacks the %s' sprite %08x: its keys stay", tg.name, kSetName[want], *(uint32_t*)src[want][0]);
        g_noSets[t] = row;
        want = kKeys;
    }
    for (int i = 0; i < tg.count; ++i) {
        uint8_t* d = dst[i];
        const uint8_t* s = src[want][i];
        if (memcmp(d, s, 4) != 0) memcpy(d, s, 4);                   // the sprite
        if (memcmp(d + 6, s + 6, kRecord - 6) != 0) memcpy(d + 6, s + 6, kRecord - 6);  // u v w h xoff advance yoff, the flag words
    }
}

void PadGlyphsFrame() {
#ifndef PADGLYPHS_TEST
    // At most every 5 ms: the dispatcher sees hundreds of native calls a frame; once a frame is enough.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    if (g_lastRun.QuadPart && now.QuadPart - g_lastRun.QuadPart < g_freq.QuadPart / 200) return;
    g_lastRun = now;
#endif
    const wchar_t* why = L"";
    int want = Wanted(&why);
    if (want != g_shown) {
        Log(false, L"pad pictures: %s (%s)", kSetName[want], why);
        g_shown = want;
    }
    for (int t = 0; t < kTargetCount; ++t) Apply(t, want);
}

bool PadGlyphsInviteHeld() {
    if (!g_focused()) return false;  // (as the keyboard's key: only while this game's window is in front)
    uint8_t* pad = g_primary();
    if (!pad) return false;
    uint32_t vt = *(uint32_t*)pad;
    if (vt != kXInputVtable && vt != kDirectInputVtable) return false;  // (the keyboard's action 34 is Esc)
    int layer = g_layer();
    if (layer != 0 && layer != 1) return false;
    return (pad[kFlags + kLayerSize * layer + kActionBack] & 1) != 0;
}

#ifndef PADGLYPHS_TEST
// ---------------------------------------------------------------- Steam Input (the worker thread)
static bool g_steamInputTried = false, g_steamInputOn = false, g_steamPolled = false;
static DWORD g_steamPollAt = 0;
static int g_loggedType[4] = {-1, -1, -1, -1};

void PadGlyphsSteamPoll() {
    DWORD now = GetTickCount();
    if (g_steamPolled && (int32_t)(now - g_steamPollAt) < 1000) return;
    g_steamPolled = true;
    g_steamPollAt = now;
    ISteamInput* si = SteamInput();
    if (!g_steamInputTried) {
        g_steamInputTried = true;
        // Init(false): Steam runs its frame from SteamAPI_RunCallbacks. The game keeps reading the pad through Steam's XInput
        // emulation; GetControllerForGamepadIndex names the pad behind an emulated slot (isteaminput.h).
        g_steamInputOn = si && si->Init(false);
        Log(false, L"pad pictures: Steam Input %s", g_steamInputOn ? L"answers (PlayStation pads behind Steam's Xbox emulation can be told apart)"
                                                                   : L"is not available: controllers show Xbox buttons unless config.ini says otherwise");
    }
    if (!g_steamInputOn) return;
    for (int i = 0; i < 4; ++i) {
        InputHandle_t h = si->GetControllerForGamepadIndex(i);
        ESteamInputType type = h ? si->GetInputTypeForHandle(h) : k_ESteamInputType_Unknown;
        bool ps = type == k_ESteamInputType_PS3Controller || type == k_ESteamInputType_PS4Controller || type == k_ESteamInputType_PS5Controller;
        InterlockedExchange(&g_slotKind[i], ps ? 2 : h ? 1 : 0);
        int t = h ? (int)type : -2;
        if (t != g_loggedType[i]) {
            if (h) Log(false, L"pad pictures: Steam Input: XInput slot %d is a %s (type %d)", i, ps ? L"PlayStation controller" : L"non-PlayStation controller", (int)type);
            else if (g_loggedType[i] != -1) Log(false, L"pad pictures: Steam Input: XInput slot %d has no Steam Input controller now", i);
            g_loggedType[i] = t;
        }
    }
}

void PadGlyphsSteamStop() {
    if (g_steamInputOn) SteamInput()->Shutdown();
    g_steamInputOn = false;
    g_steamInputTried = g_steamPolled = false;
    for (int i = 0; i < 4; ++i) {
        InterlockedExchange(&g_slotKind[i], 0);
        g_loggedType[i] = -1;
    }
}
#else
void PadGlyphsSteamPoll() {}
void PadGlyphsSteamStop() {}
void PadGlyphsTestSetup(PadGlyphsFindFn find, PadGlyphsProbeFn probe, uint8_t* (*primary)(), int (*layer)(), bool (*online)()) {
    g_find = find;
    g_probe = probe;
    g_primary = primary;
    g_layer = layer;
    g_online = online;
    g_shown = -1;
    for (int t = 0; t < kTargetCount; ++t) g_noSets[t] = nullptr;
}
void PadGlyphsTestSetMode(int mode) { g_mode = mode; }
void PadGlyphsTestSetFocused(bool focused) { g_testFocused = focused; }
void PadGlyphsTestSetSlot(int slot, int kind) { g_slotKind[slot] = kind; }
int PadGlyphsTestShown() { return g_shown; }
#endif
