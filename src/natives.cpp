// Script natives for the multiplayer menus. See natives.h and notes/NATIVE-BRIDGE.md.
//
// The script VM reaches every native through its vtable slot 3 (0x6b467c -> dispatcher 0x54bb00,
// thiscall(vm, id, descriptor), ret 8; the interpreter ignores its return value). We swap that one
// pointer: group 0x7A goes to our handler, everything else to the original dispatcher unchanged.
// Nothing fingerprints the slot; AllStarsRosterPack.dll fingerprints the dispatcher's code, which
// we leave alone.
#include "natives.h"
#include <stdint.h>
#include <stdio.h>
#include "lobby.h"
#include "log.h"
#include "modset.h"  // RefusalText: the join refusal pop-up's body
#include "race.h"
#include "race_tables.h"
#include "voice.h"  // VoiceKeyOf: the key names of config.ini

static const uint32_t kSlot = 0x6b467c;
static const uint32_t kDispatcher = 0x54bb00;
static const uint32_t kTextBufferVtable = 0x6a71f8;  // native 1610 objects

typedef void(__thiscall* DispatchFn)(void* vm, uint32_t id, void* desc);
static DispatchFn g_original = nullptr;
static bool g_installed = false;
static DWORD g_scriptThread = 0;  // first thread to call any native: the game's main thread
static bool g_wrongThreadLogged = false;
static int32_t g_localPad = 0;  // the primary pad index the lobby script last passed (native 0400)

// The lobby's race-progress bar (M7, re\m7-join-race\FINDINGS.md Q1): the console showed a player who joined while a
// race ran a track line under the bars with a head icon per racer moving toward a checkered flag. The scene
// (fe_net_lobby_posbar, root dd942497, 8 marker groups with head icons) is loaded and attached by our lobby already;
// the code that drove it is gone on PC. Ours runs in the per-frame native: while any member's "pos" says it races
// a race this game wasn't in, the bar is shown and each racing member's marker (by bar row) is posed between the
// scene's start and end keyframes at its progress, with its racer's icon (kNatMemberIcon, from the lobby script).
static const uint32_t kBarRoot = 0xdd942497, kBarShownKey = 0x02f0b24c, kMarkFrom = 0x3e3460a8, kMarkTo = 0xf5fda6b0;
static const uint32_t kBarMarkers[8] = {0x05d75801, 0xac2c9bf7, 0xcbaddd74, 0xdd4ce362, 0xbacfbf36, 0x9df3374a, 0x877b8649, 0x1b0d3544};
static const uint32_t kBarHeads[8] = {0xea88ea7d, 0xa20f540f, 0xbf672c37, 0x718f80b2, 0x7bb3b824, 0x77d6cef9, 0xa91bc8b3, 0x045680e0};
typedef void(__stdcall* ElemShowFn)(uint32_t element, uint32_t one);                                  // 0x47f1a0 show / 0x47f1d0 hide, ret 8
typedef void(__stdcall* ElemPoseFn)(uint32_t element, uint32_t keyFrom, uint32_t keyTo, float t);     // 0x47f090, ret 0x10
typedef bool(__stdcall* ElemArtFn)(uint32_t element, uint32_t art);                                   // 0x47f400, ret 8; AL 0 = no such art in its scene (unchanged)
static ElemShowFn g_elemShow = (ElemShowFn)0x47f1a0, g_elemHide = (ElemShowFn)0x47f1d0;
static ElemPoseFn g_elemPose = (ElemPoseFn)0x47f090;
static ElemArtFn g_elemArt = (ElemArtFn)0x47f400;
static uint32_t g_barIcons[kMaxMembers];   // per bar row: that member's racer icon, 0 = none yet
static bool g_barShown = false;
static uint8_t g_markerShown[8];
static uint32_t g_headArt[8];
static bool g_barLogged = false;
static DWORD g_barFrame = 0;
// The console's status in the bars while the race runs, alternating with the names every 250 lobby frames:
// "1ST (LAP 2 OF 3)" for a racer, "WAITING IN LOBBY" for the others, in the lobby's own language (its text table,
// 0x47fb50 as native 0222 reads it). Written into the bars' name buffers (the ones the script gave kNatMemberText,
// which the script's own redraw fills with the names again).
static bool WriteTextObject(uint8_t* obj, const char* text);  // below, with the text natives
static void BarName(const LobbyMember& m, int32_t cap, char* out, size_t outSize, bool yellow);  // below: the bar's name, "(muted)" kept
// M8: a muted player's bar is YELLOW (the user's design, re\m8-yellow\FINDINGS.md): the bar's blue base sprite (its red cursor
// overlay is its own sprite, so the cursor still shows red) takes the art dd4e2bb1 our lobby scenes carry (tools\lobby_bars.py).
// A lobby scene without it (another mod's version of the scene) keeps the blue bar, and the name says "(muted)" instead.
static const uint32_t kBarBases[8] = {0xa25dd079, 0x6ec737ad, 0x36cb75a9, 0xfe74649d, 0x68e2774b, 0x1583ba8a, 0x6043e324, 0x74d20750};
static const uint32_t kBarBlueArt = 0x5da8c97e, kBarMutedArt = 0xdd4e2bb1;
static uint8_t g_barYellow[kMaxMembers];    // 1 = that bar shows the yellow art (its name needs no "(muted)")
// M8 (re\m8-audit C1): while the host's Start waits for every player's game to make race connections (a Steam certificate can take
// minutes), the console's busy spinner (navigator_info's overlay, re\m8-spinners\FINDINGS.md) says CONNECTING (39E82599: in the
// data, used by no console script) at the lobby's place; it fades out when the wait ends. Script context only (the frame native).
typedef void(__stdcall* ElemTextKeyFn)(uint32_t element, uint32_t key);                                              // 0x47f3d0, ret 8
typedef void(__stdcall* ElemLoopFn)(uint32_t element, uint32_t start, uint32_t from, uint32_t to, uint32_t c, int32_t count, uint32_t recurse);  // 0x47f030 (native 020f)
typedef void(__stdcall* ElemPlayFn)(uint32_t element, uint32_t from, uint32_t to, uint32_t c, uint32_t recurse);     // 0x47f8c0 (native 020e), ret 0x14
static ElemTextKeyFn g_elemTextKey = (ElemTextKeyFn)0x47f3d0;
static ElemLoopFn g_elemLoop = (ElemLoopFn)0x47f030;
static ElemPlayFn g_elemPlay = (ElemPlayFn)0x47f8c0;
static const uint32_t kSpinPlace = 0x65c8b901, kSpinGroup = 0xfff27543, kSpinDots = 0x75d34984, kSpinTexts[3] = {0x9e70d8aa, 0xc43598db, 0x98212984};
static const uint32_t kSpinConnecting = 0x39e82599, kSpinAtLobby = 0x7f88f1b1, kSpinHidden = 0xa56426e2, kSpinShown = 0x3ecdb4b9;

static void ConnectingSpinner(bool on) {
    if (on) {
        for (uint32_t t : kSpinTexts) g_elemTextKey(t, kSpinConnecting);
        g_elemPose(kSpinPlace, kSpinAtLobby, kSpinAtLobby, 0.0f);  // (re-poses the group too: the spin and the fade follow)
        g_elemLoop(kSpinDots, 0x4240c67e, 0x4240c67e, 0x1de4ecc9, 0, -1, 1);
        g_elemPlay(kSpinGroup, kSpinHidden, kSpinShown, 0, 0);
    } else {
        g_elemPlay(kSpinGroup, kSpinShown, kSpinHidden, 0, 0);  // (only ever after an "on": a fade-out from hidden would flash it)
    }
}
// The dialogs text key NOTHING reads UNMUTE through our text tables (tools\text_tables.py): only then are a muted player's popups
// the UNMUTE ones (kFramePopupMuted); with another mod's table (or a placeholder language's) the popup says MUTE, which toggles too.
static const uint32_t kDialogsTextScene = 0x424b961f, kUnmuteKey = 0x72259212;
static const uint32_t kLobbyTextScene = 0xa363a411, kLapFormatKey = 0x1de2f2dd, kWaitingKey = 0x597ebb48, kViewingKey = 0x1c2a9041;
static const uint32_t kPosKeys[8] = {0x7cea650b, 0x05cfeb5a, 0xfe17f50c, 0x7eeda6d9, 0xf474c981, 0x3689fc84, 0x55ed1302, 0x7df05266};  // POS_1ST..POS_8TH (the 5th is the table's "POS_5ND")
static const char* const kPosEnglish[8] = {"1ST", "2ND", "3RD", "4TH", "5TH", "6TH", "7TH", "8TH"};
static const uint32_t kStatusFrames = 250;
typedef const char*(__fastcall* SceneTextFn)(uintptr_t sets, void*, uint32_t scene, uint32_t key);  // thiscall(ECX = the texture sets)
static SceneTextFn g_sceneText = (SceneTextFn)0x47fb50;
static const uintptr_t kTextureSets = 0xc282a8;
static uint8_t* g_barText[kMaxMembers];     // the bars' name buffer objects (kNatMemberText)
static uint8_t g_barStatus[kMaxMembers];    // 1 = the status is in that buffer
static uint64_t g_kickTarget = 0;           // the player the lobby popup is about (by Steam id: a bar shift can't kick another)
// The consoles' triangle / Y button (INVITE FRIENDS in the lobby, VIEW PROFILE on the game list): the key of config.ini [Menus]
// InviteKey (A), read as the voice chat reads its push-to-talk key: only while this game's window is in front (two games on one
// PC share the keyboard). The native answers the press edge.
static int g_refreshVk = -1;                // -1: the config not read yet
static bool g_refreshDown = false;
static DWORD g_refreshReadAt = 0;           // the last read: a press counts only against a reading made a moment before
static bool (*g_keyState)(int vk) = nullptr;  // tests: a fake key state

// The dialog input layer (re\m9-esc\FINDINGS.md): while a dialog is up the game reads pad layer 1, whose button bytes start at
// pad+0x102 (+0x125 confirm, +0x126 back; bit 4 = pressed this frame); the primary pad is [ctrl+0xc + 4 * [ctrl+0x960]],
// ctrl = [[0xc2a1b4] + 0x14], the pad count [ctrl+4].
static uint8_t* GamePrimaryPad() {
    uint8_t* in = *(uint8_t* volatile*)(uintptr_t)0xc2a1b4;
    uint8_t* ctrl = in ? *(uint8_t* volatile*)(in + 0x14) : nullptr;
    if (!ctrl) return nullptr;
    int32_t p = *(volatile int32_t*)(ctrl + 0x960);
    return p >= 0 && p < *(volatile int32_t*)(ctrl + 4) ? *(uint8_t* volatile*)(ctrl + 0xc + 4 * p) : nullptr;
}
static uint8_t* (*g_primaryPad)() = GamePrimaryPad;
static const uint32_t kKeyboardVtable = 0x67e444;  // the keyboard device class (re\drift-plus\FINDINGS.md); every other pad is a controller

static bool KeyDown(int vk) {
    if (g_keyState) return g_keyState(vk);
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int InviteKey() {
    if (g_refreshVk < 0) {
        wchar_t name[64] = L"";
        GetPrivateProfileStringW(L"Menus", L"InviteKey", L"A", name, 64, LobbyConfigPath());
        g_refreshVk = VoiceKeyOf(name);
        if (!g_refreshVk) g_refreshVk = 'A';
        Log(false, L"menus: the INVITE FRIENDS / VIEW PROFILE key is \"%s\" (virtual key 0x%02x)", name, g_refreshVk);
    }
    return g_refreshVk;
}
static uint32_t g_barFrames = 0;

// The popup button UNMUTE is ours: the dialogs text NOTHING as our tables write it in the 5 languages they cover.
static bool UnmuteTextShown() {
    static const char* const kOurs[] = {"UNMUTE", "ACTIVER LE SON", "TON AN", "ATTIVA AUDIO", "ACTIVAR SONIDO"};
    const char* s = g_sceneText(kTextureSets, nullptr, kDialogsTextScene, kUnmuteKey);
    if (!s) return false;
    for (const char* t : kOurs)
        if (strcmp(s, t) == 0) return true;
    return false;
}

static const char* LobbyText(uint32_t key, const char* english) {
    const char* s = g_sceneText(kTextureSets, nullptr, kLobbyTextScene, key);
    return s && *s ? s : english;
}

// "{0}", "{1}", "{2}" in the format replaced by the three texts (UTF-8 as they are), cut to fit.
static void FormatStatus(const char* fmt, const char* a0, const char* a1, const char* a2, char* out, size_t cap) {
    size_t o = 0;
    for (const char* f = fmt; *f && o + 1 < cap; ) {
        if (f[0] == '{' && f[1] >= '0' && f[1] <= '2' && f[2] == '}') {
            const char* arg = f[1] == '0' ? a0 : f[1] == '1' ? a1 : a2;
            for (const char* s = arg; *s && o + 1 < cap; ++s) out[o++] = *s;
            f += 3;
            continue;
        }
        out[o++] = *f++;
    }
    out[o] = 0;
}

static void StatusText(const LobbyMember& m, char* text, size_t cap) {
    if (m.place >= 0 && m.raceId && m.raceId != RaceLastId() && m.results) {
        FormatStatus(LobbyText(kViewingKey, "VIEWING RESULTS"), "", "", "", text, cap);  // (its race is over; it is on the results screen)
    } else if (m.place >= 0 && m.raceId && m.raceId != RaceLastId()) {
        char lap[8], laps[8];
        int32_t shown = m.lap < 1 ? 1 : m.lap > m.laps ? m.laps : m.lap;  // the record's "lap shown" runs 0.. and past the end
        sprintf_s(lap, "%d", shown);
        sprintf_s(laps, "%d", m.laps);
        int place = m.place < 8 ? m.place : 7;
        FormatStatus(LobbyText(kLapFormatKey, "{0} (LAP {1} OF {2})"), LobbyText(kPosKeys[place], kPosEnglish[place]), lap, laps, text, cap);
    } else {
        FormatStatus(LobbyText(kWaitingKey, "WAITING IN LOBBY"), "", "", "", text, cap);
    }
}

// The status half of the cycle is on (the bar is up and the frame count says so).
static bool StatusOn() { return g_barShown && (g_barFrames / kStatusFrames) % 2 == 1; }

static void BarStatus(const LobbySnapshot& s, bool show) {
    bool status = show && (g_barFrames / kStatusFrames) % 2 == 1;
    for (int k = 0; k < s.memberCount && k < kMaxMembers; ++k) {
        const LobbyMember& m = s.members[k];
        if (!g_barText[k]) continue;
        if (status && !g_barStatus[k]) {
            char text[64];
            StatusText(m, text, sizeof(text));
            if (WriteTextObject(g_barText[k], text)) g_barStatus[k] = 1;
        } else if (!status && g_barStatus[k]) {
            char name[kNameLen + 16];
            BarName(m, *(int32_t*)(g_barText[k] + 8), name, sizeof(name), g_barYellow[k] != 0);  // (the object was checked when kNatMemberText stored it)
            WriteTextObject(g_barText[k], name);
            g_barStatus[k] = 0;
        }
    }
}

// The lobby rules panel's DRIFT+ row (an addition, the user's request 2026-10-27; tools\lobby_drift_row.py): darkened, never
// selectable, under CATCH-UP for everyone in the lobby; ON while every player drives with a controller, OFF while a keyboard player
// is in (RaceDriftPlusOff: the race's own rule). The scene's own value says ON; this writes the value every few frames (each lobby
// Show builds fresh elements), only with our lobby scene (the row's label text found), so another mod's scene is left alone.
static const uint32_t kLobbyPanelScene = 0x8c2f0f09, kDriftKey = 0x852717f3, kDriftValue = 0xc96a369e;  // STEAMMP_LOBBY_DRIFT_ROW_VALUE
static const uint32_t kTextOn = 0xffc1df26, kTextOff = 0x724c3b00;
static uint32_t g_driftFrames = 0;
static int32_t g_driftShown = -1;  // the value last written: 1 ON, 0 OFF, -1 none yet; -2 not our scene (checked again later)

static void DrawDriftRow(const LobbySnapshot& s) {
    int32_t on = RaceDriftPlusOff(s) ? 0 : 1;
    if (on == g_driftShown ? ++g_driftFrames % 10 != 0 : g_driftShown == -2 && ++g_driftFrames % 250 != 0) return;
    const char* label = g_sceneText(kTextureSets, nullptr, kLobbyPanelScene, kDriftKey);
    if (!label || strcmp(label, "DRIFT+") != 0) {
        if (g_driftShown != -2) Log(false, L"lobby: no DRIFT+ row (the lobby scene isn't ours: another mod's?)");
        g_driftShown = -2;
        return;
    }
    g_elemTextKey(kDriftValue, on ? kTextOn : kTextOff);
    if (on != g_driftShown) Log(false, L"lobby: the DRIFT+ row says %s", on ? L"ON" : L"OFF (a keyboard player is in)");
    g_driftShown = on;
}

static void DrawRaceBar(const LobbySnapshot& s) {
    // The lobby screen was left and entered again (its scene comes back fresh, the bar invisible; the bars' text
    // buffers may be new objects): start over, and forget the buffers until the redraw gives them again.
    if (GetTickCount() - g_barFrame > 1000) {
        g_barShown = false;
        memset(g_barText, 0, sizeof(g_barText));
        memset(g_barStatus, 0, sizeof(g_barStatus));
    }
    g_barFrame = GetTickCount();
    bool racing = false;
    for (int k = 0; s.state == kLobbyInLobby && k < s.memberCount && k < kMaxMembers; ++k)
        racing = racing || (s.members[k].place >= 0 && s.members[k].raceId && s.members[k].raceId != RaceLastId());
    if (!racing) {
        BarStatus(s, false);  // the names back, if the status was up
        g_barFrames = 0;
        if (!g_barShown) return;
        g_elemHide(kBarRoot, 1);
        for (int k = 0; k < 8; ++k) {
            g_elemHide(kBarMarkers[k], 1);
            g_elemPose(kBarMarkers[k], kMarkFrom, kMarkTo, 0.0f);
        }
        g_barShown = false;
        return;
    }
    BarStatus(s, true);  // (frames 0..249 the names, 250..499 the status, and so on)
    ++g_barFrames;
    if (!g_barShown) {
        g_elemShow(kBarRoot, 1);
        g_elemPose(kBarRoot, kBarShownKey, kBarShownKey, 0.0f);
        for (int k = 0; k < 8; ++k) g_elemHide(kBarMarkers[k], 1);
        memset(g_markerShown, 0, sizeof(g_markerShown));
        memset(g_headArt, 0, sizeof(g_headArt));
        g_barShown = true;
        if (!g_barLogged) Log(false, L"lobby: a race is running here; the race bar shows it");
        g_barLogged = true;
    }
    for (int k = 0; k < 8; ++k) {
        const LobbyMember* m = k < s.memberCount && k < kMaxMembers ? &s.members[k] : nullptr;
        bool on = m && m->place >= 0 && m->raceId && m->raceId != RaceLastId();
        if (!on) {
            if (g_markerShown[k]) g_elemHide(kBarMarkers[k], 1);
            g_markerShown[k] = 0;
            continue;
        }
        if (g_barIcons[k] && g_headArt[k] != g_barIcons[k]) {
            g_elemArt(kBarHeads[k], g_barIcons[k]);
            g_headArt[k] = g_barIcons[k];
        }
        if (!g_markerShown[k]) g_elemShow(kBarMarkers[k], 1);
        g_markerShown[k] = 1;
        float t = m->progress < 0 ? 0.0f : m->progress > 1000 ? 1.0f : (float)m->progress / 1000.0f;
        g_elemPose(kBarMarkers[k], kMarkFrom, kMarkTo, t);
    }
}

// Native call descriptor (C, notes/NATIVE-BRIDGE.md §2). The argument arrays live on the VM stacks.
struct Descriptor {
    uint32_t id;
    uint8_t normalCall;
    uint8_t pad[3];
    const uint8_t* types;
    const uint32_t* values;
    uint32_t argc;
    uint32_t retValue;
    uint32_t retType;
};

static void ReturnInt(Descriptor* d, int32_t v) {
    // Equivalent to the game's setter 0x48c520 for a freshly zeroed return slot.
    d->retValue = (uint32_t)v;
    d->retType = kTypeInt;
}

static void ReturnTyped(Descriptor* d, uint32_t v, uint32_t type) {
    d->retValue = v;
    d->retType = type;
}

static bool IntArg(const Descriptor* d, uint32_t i, int32_t& out) {
    if (i >= d->argc || d->types[i] != kTypeInt) return false;
    out = (int32_t)d->values[i];
    return true;
}

// Copies UTF-8 text into a native-1610 text buffer: whole code points only (U+0000..U+FFFF, which
// is all the game's converter 0x482c90 handles), invalid bytes become '?', always NUL-terminated.
static bool RefuseText(const Descriptor* d, const wchar_t* why) {
    static bool logged = false;  // once: a script bug would repeat it every frame
    if (!logged) Log(false, L"menu native %04x: text buffer refused (%s)", d->id, why);
    logged = true;
    return false;
}

static bool WriteTextObject(uint8_t* obj, const char* text);

// The capacity of the native-1610 text buffer in argument i (bytes with the NUL), or 0 if it isn't one.
static int32_t TextCapacity(const Descriptor* d, uint32_t i) {
    if (i >= d->argc || d->types[i] != kTypeObject) return 0;
    const uint8_t* obj = (const uint8_t*)(uintptr_t)d->values[i];
    if (!obj || *(const uint32_t*)obj != kTextBufferVtable) return 0;
    int32_t cap = *(const int32_t*)(obj + 8);
    return cap >= 1 && cap <= 4096 ? cap : 0;
}

// The first `maxBytes` of a UTF-8 string, whole characters only (1-3 bytes; an invalid lead byte counts as one), into `out`.
static void CutUtf8(const char* s, size_t maxBytes, char* out, size_t outSize) {
    size_t n = 0;
    while (*s && n + 1 < outSize) {
        uint8_t c = (uint8_t)*s;
        size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 1;
        if (n + len > maxBytes) break;
        for (size_t k = 0; k < len && s[k]; ++k) out[n++] = s[k];
        s += len;
    }
    out[n] = 0;
}

// The bar's name; a muted member's (the popup's MUTE) on a bar that couldn't turn yellow ends in " (muted)", the name cut (whole
// characters) so that the suffix fits the bar's text buffer of `cap` bytes with its NUL (the lobby's are 32: a long Steam name
// would otherwise lose the suffix).
static void BarName(const LobbyMember& m, int32_t cap, char* out, size_t outSize, bool yellow) {
    static const char kMuted[] = " (muted)";
    const char* name = m.name[0] ? m.name : "?";
    if (!m.muted || yellow) { CutUtf8(name, outSize - 1, out, outSize); return; }
    int32_t room = cap - 1 - (int32_t)(sizeof(kMuted) - 1);
    CutUtf8(name, room > 0 ? (size_t)room : 0, out, outSize - (sizeof(kMuted) - 1));
    strcat_s(out, outSize, kMuted);
}

static bool WriteText(const Descriptor* d, uint32_t i, const char* text) {
    if (i >= d->argc || d->types[i] != kTypeObject) return RefuseText(d, L"argument is not an object");
    uint8_t* obj = (uint8_t*)(uintptr_t)d->values[i];
    if (!obj || *(uint32_t*)obj != kTextBufferVtable) return RefuseText(d, L"not a text buffer");
    if (*(int32_t*)(obj + 8) < 1 || *(int32_t*)(obj + 8) > 4096 || !*(char**)(obj + 0xc)) return RefuseText(d, L"bad capacity");
    return WriteTextObject(obj, text);
}

// The same copy into a native-1610 text buffer object the script gave us earlier (checked again: vtable, capacity).
static bool WriteTextObject(uint8_t* obj, const char* text) {
    if (!obj || *(uint32_t*)obj != kTextBufferVtable) return false;
    int32_t cap = *(int32_t*)(obj + 8);
    char* buf = *(char**)(obj + 0xc);
    if (cap < 1 || cap > 4096 || !buf) return false;
    int n = 0;
    const uint8_t* s = (const uint8_t*)text;
    while (*s) {
        uint8_t c = *s;
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 0;
        bool ok = len > 0;
        for (int k = 1; ok && k < len; ++k) ok = (s[k] & 0xc0) == 0x80;
        if (ok && len == 2 && c < 0xc2) ok = false;                        // overlong
        if (ok && len == 3 && c == 0xe0 && s[1] < 0xa0) ok = false;        // overlong
        if (ok && len == 3 && c == 0xed && s[1] >= 0xa0) ok = false;       // surrogate
        if (ok && len == 1 && c < 0x20) ok = false;                        // control
        const char* piece = ok ? (const char*)s : "?";
        int pieceLen = ok ? len : 1;
        if (n + pieceLen > cap - 1) break;
        memcpy(buf + n, piece, pieceLen);
        n += pieceLen;
        if (ok) s += len;
        else {  // skip the bad lead byte and any continuation bytes after it
            ++s;
            while ((*s & 0xc0) == 0x80) ++s;
        }
    }
    buf[n] = 0;
    return true;
}

// The console's game-list flag (re\m8-flag\FINDINGS.md): the host's license nationality (profile +0x18 of the active license),
// published as lobby data "flag"; each row's flag cell (node 1) shows that code's sprite from the search scene (the same 30 pairs
// as the license card's table, fe_mainmenu 0x07b6; 0 = the SEGA logo).
static int32_t ReadLicenseFlag() {
    uint32_t i = *(volatile uint32_t*)(uintptr_t)0xcdb530;
    return i < 4 ? *(volatile int32_t*)(uintptr_t)(0xc2a520 + i * 0x3af0 + 0x18) : -1;
}
static int32_t (*g_licenseFlag)() = ReadLicenseFlag;

// The row's mod-icons sprite (tools\mod_icons.py art_name: "STEAMMP_MODS_" + Egg's state + Metal's, N K D X, its path hash), 0 = none.
static uint32_t ModsArt(int32_t mods) {
    const char letters[] = "NKDX";
    char name[] = "STEAMMP_MODS_??";
    name[13] = letters[mods & 3];
    name[14] = letters[(mods >> 2) & 3];
    if (!(mods & 15)) return 0;
    uint32_t v = 0;  // (build_ui.path_hash: from the last character to the first, v = c + 131 * v)
    for (int i = (int)sizeof(name) - 2; i >= 0; --i) v = (uint8_t)name[i] + 131 * v;
    return v;
}

static uint32_t FlagArt(int32_t code) {
    static const struct { int32_t code; uint32_t art; } kFlags[] = {
        {103, 0x7103b4d4}, {6, 0x03d7659e}, {5, 0x9edb3b89}, {8, 0xc8f9043e}, {13, 0xa4b0616c}, {35, 0xd9d21c5e}, {16, 0x46eeafc8},
        {20, 0x39ecf13c}, {25, 0xeb609853}, {32, 0xb97d42e9}, {34, 0x239f1c4a}, {24, 0x24398b27}, {39, 0x7a1b0b7d}, {46, 0x9e15fc1b},
        {99, 0x0b95be26}, {50, 0xbc74bbe7}, {53, 0x7b85947a}, {56, 0x9764b9e4}, {71, 0x3537e3b3}, {74, 0xb4698c95}, {76, 0x04f2a447},
        {75, 0xb8c40e16}, {84, 0x006c260c}, {88, 0x8081c0e3}, {91, 0x57d73ff3}, {31, 0x2669a16a}, {90, 0x23015fd9}, {18, 0x4cb915a0},
        {97, 0x4adeb129}, {0, 0x47ace633}};
    for (const auto& f : kFlags)
        if (f.code == code) return f.art;
    return 0;
}

static void Handle(Descriptor* d) {
    const uint32_t op = d->id & 0xff;
    // (read where this game hosts or joins: the host publishes it, also after a handover)
    if (op == kNatHost || op == kNatJoinRow || op == kNatJoinTake) LobbyNoteFlag(g_licenseFlag());
    LobbySnapshot s;
    LobbyRead(s);
    int32_t a = 0, b = 0;
    switch (op) {
    case kNatVersion: ReturnInt(d, kBridgeVersion); return;
    case kNatState: ReturnInt(d, s.state); return;
    case kNatError: ReturnInt(d, s.error); return;
    case kNatRevision: ReturnInt(d, (int32_t)s.revision); return;
    case kNatIsHost: ReturnInt(d, s.state == kLobbyInLobby && s.isHost); return;
    case kNatHostChanges: ReturnInt(d, s.state == kLobbyInLobby ? s.hostChanges : 0); return;
    case kNatMemberIcon:
        if (IntArg(d, 0, a) && a >= 0 && a < kMaxMembers && d->argc >= 2 && (d->types[1] == kTypeHash || d->types[1] == kTypeInt))
            g_barIcons[a] = d->values[1];
        ReturnInt(d, 1);
        return;
    case kNatRowRacing:  // the search rows' STATUS column: GAME while that lobby's race runs, else LOBBY
        if (IntArg(d, 0, a)) ReturnInt(d, s.state == kLobbyResults && a >= 0 && a < s.rowCount && s.rows[a].racing ? 1 : 0);
        return;
    case kNatRacerOn:  // the racer picker's row: 0 when its mod is off in this lobby (ENABLE MODIFICATIONS; the console skipped DLC racers)
        if (IntArg(d, 0, a)) ReturnInt(d, RaceRacerOn(a) ? 1 : 0);
        return;
    case kNatMods: {  // ENABLE MODIFICATIONS: what the CREATE question offers, and its answers
        int32_t value;
        if (!IntArg(d, 0, a) || !IntArg(d, 1, value)) return;
        if (a == 0) ReturnInt(d, LobbyModInstalled("metal-sonic") ? 1 : 0);
        else if (a == 1) ReturnInt(d, LobbyModInstalled("egg-hangar") ? 1 : 0);
        else if (a == 2) { LobbySetHostMods((value & 1) != 0, (value & 2) != 0); ReturnInt(d, 1); }
        else if (a == 3) ReturnInt(d, RefusalText(s.refusedMods));  // the join refusal pop-up's body (steam.py MODS_REFUSALS)
        else ReturnInt(d, -1);
        return;
    }
    case kNatDialogBack: {  // the modifications question's Esc (the dialog layer's back, not with its confirm; never over the overlay)
        uint8_t* pad = g_primaryPad();
        ReturnInt(d, !LobbyOverlayOpen() && pad && (pad[0x126] & 0x10) && !(pad[0x125] & 0x10) ? 1 : 0);
        return;
    }
    case kNatRowMods: {  // the search row's SIGNAL cell: the lobby's mod icons (tools\mod_icons.py), 0 = none (the stock signal bars)
        if (!IntArg(d, 0, a)) return;
        uint32_t art = s.state == kLobbyResults && a >= 0 && a < s.rowCount && a < kMaxRows ? ModsArt(s.rows[a].mods) : 0;
        if (art) ReturnTyped(d, art, kTypeHash);
        else ReturnInt(d, 0);
        return;
    }
    case kNatRowFlag:  // the search row's flag cell: its host's license flag (art hash), 0 = none known (the cell stays hidden)
        if (IntArg(d, 0, a)) {
            uint32_t art = s.state == kLobbyResults && a >= 0 && a < s.rowCount && a < kMaxRows ? FlagArt(s.rows[a].flag) : 0;
            if (art) ReturnTyped(d, art, kTypeHash);
            else ReturnInt(d, 0);
        }
        return;
    case kNatRowProfile: {  // the search rows' VIEW PROFILE (F2): the selected row's host in the Steam overlay
        if (!IntArg(d, 0, a)) return;
        bool ok = s.state == kLobbyResults && a >= 0 && a < s.rowCount && a < kMaxRows && s.rows[a].hostId && !IsTestPlayer(s.rows[a].hostId);
        if (ok) {
            int32_t id[2] = {(int32_t)(uint32_t)s.rows[a].hostId, (int32_t)(uint32_t)(s.rows[a].hostId >> 32)};
            ok = LobbyPostArgs(kCmdProfile, id, 2);
        }
        if (ok) Log(false, L"view profile: the Steam profile of the host in row %d asked for (F2)", a + 1);
        ReturnInt(d, ok ? 1 : 0);
        return;
    }
    case kNatKick: {  // KICK (M7, host): the player the popup was about (kFramePopup, by Steam id: the bars may have shifted while the
                      // question was up) leaves the lobby and can't rejoin it (steam_mp.cpp Kick). (bar) is the script's cursor, for the log.
        if (!IntArg(d, 0, a)) return;
        int n = s.state == kLobbyInLobby ? (s.memberCount < kMaxMembers ? s.memberCount : kMaxMembers) : 0, who = -1;
        for (int i = 0; i < n; ++i)
            if (g_kickTarget && s.members[i].id == g_kickTarget) who = i;
        bool ok = s.isHost && who >= 0 && !s.members[who].isMe && !IsTestPlayer(g_kickTarget);
        if (ok) {
            int32_t id[2] = {(int32_t)(uint32_t)g_kickTarget, (int32_t)(uint32_t)(g_kickTarget >> 32)};
            ok = LobbyPostArgs(kCmdKick, id, 2);
        }
        if (ok) Log(true, L"kick: the player in bar %d (Steam id %llu) is kicked out of the lobby%s", who + 1, g_kickTarget, who != a ? L" (the bars moved while the question was up)" : L"");
        else Log(false, L"kick: nobody kicked (not the host, no question asked, or the player asked about has left; bar %d)", a + 1);
        g_kickTarget = 0;
        ReturnInt(d, ok ? 1 : 0);
        return;
    }
    case kNatMemberProfile: {  // the lobby popup's VIEW STEAM PROFILE: that member's Steam profile in the overlay
        if (!IntArg(d, 0, a)) return;
        int n = s.state == kLobbyInLobby ? (s.memberCount < kMaxMembers ? s.memberCount : kMaxMembers) : 0;
        // The player the popup was about (by Steam id, as the kick: the bars may have shifted while it was up); gone: nobody (never
        // whoever holds that bar now). Without a popup (no target remembered): the bar given.
        if (g_kickTarget) {
            int who = -1;
            for (int i = 0; i < n; ++i)
                if (s.members[i].id == g_kickTarget) who = i;
            a = who;
        }
        bool ok = a >= 0 && a < n && !s.members[a].isMe && !IsTestPlayer(s.members[a].id);
        if (ok) {
            int32_t id[2] = {(int32_t)(uint32_t)s.members[a].id, (int32_t)(uint32_t)(s.members[a].id >> 32)};
            ok = LobbyPostArgs(kCmdProfile, id, 2);
        }
        if (ok) Log(false, L"view profile: the Steam profile of the player in bar %d asked for (the lobby popup)", a + 1);
        ReturnInt(d, ok ? 1 : 0);
        return;
    }
    case kNatMemberMute: {  // the lobby popup's MUTE: toggle hearing that member's voice (this game only)
        if (!IntArg(d, 0, a)) return;
        int n = s.state == kLobbyInLobby ? (s.memberCount < kMaxMembers ? s.memberCount : kMaxMembers) : 0;
        if (g_kickTarget) {  // the player the popup was about, as VIEW STEAM PROFILE and KICK (the bars may have shifted)
            int who = -1;
            for (int i = 0; i < n; ++i)
                if (s.members[i].id == g_kickTarget) who = i;
            a = who;
        }
        bool ok = a >= 0 && a < n && !s.members[a].isMe && !IsTestPlayer(s.members[a].id);
        if (ok) {
            int32_t id[2] = {(int32_t)(uint32_t)s.members[a].id, (int32_t)(uint32_t)(s.members[a].id >> 32)};
            ok = LobbyPostArgs(kCmdMute, id, 2);
        }
        if (ok) Log(false, L"mute: the player in bar %d %s (the lobby popup)", a + 1, s.members[a].muted ? L"is heard again" : L"is muted");
        else Log(false, L"mute: nobody (the popup's player left, this player or a test player; bar %d)", a + 1);
        ReturnInt(d, !ok ? -1 : s.members[a].muted ? 0 : 1);  // (the snapshot shows the toggle from the worker's next loop)
        return;
    }
    case kNatInviteKey: {  // the INVITE FRIENDS / VIEW PROFILE key: pressed since the last call?
        // Not while the Steam overlay is up (it draws inside the game's window, so the foreground check passes, and a typed "a"
        // in its friend search would ask for the invite dialog again); and a key held since before the last reading a while ago
        // (a popup, a dialog, a race in between) is a baseline, not a press.
        DWORD now = GetTickCount();
        bool down = !LobbyOverlayOpen() && KeyDown(InviteKey());
        bool pressed = down && !g_refreshDown && now - g_refreshReadAt < 250;
        g_refreshDown = down;
        g_refreshReadAt = now;
        ReturnInt(d, pressed ? 1 : 0);
        return;
    }
    case kNatProfile: {  // the results' profile cursor (race.cpp): (field, up, down, F1)
        int32_t field = 0, up = 0, down = 0, f1 = 0;
        if (IntArg(d, 0, field) && IntArg(d, 1, up) && IntArg(d, 2, down) && IntArg(d, 3, f1)) ReturnInt(d, RaceProfileNative(field, up, down, f1));
        return;
    }
    case kNatHost:
        if (IntArg(d, 0, a)) ReturnInt(d, LobbyPost(kCmdHost, a) ? 1 : 0);
        return;
    case kNatSearch:
        if (IntArg(d, 0, a)) ReturnInt(d, LobbyPost(kCmdSearch, a) ? 1 : 0);
        return;
    case kNatJoinRow:
        if (IntArg(d, 0, a)) ReturnInt(d, LobbyPost(kCmdJoinRow, a) ? 1 : 0);
        return;
    case kNatLeave:
        RaceCancel(kAbortQuit);  // a race arranged but not started yet goes with the lobby
        ReturnInt(d, LobbyPost(kCmdLeave, 0) ? 1 : 0);
        return;
    case kNatRowCount: ReturnInt(d, s.state == kLobbyResults ? s.rowCount : 0); return;
    case kNatRowText:
        if (IntArg(d, 0, a) && IntArg(d, 1, b) && s.state == kLobbyResults && a >= 0 && a < s.rowCount) {
            char text[kNameLen + 24];
            const LobbyRow& r = s.rows[a];
            if (b == 0) {
                // A host on another Steam Multiplayer build, or with other mods, is listed and marked (the console's list had
                // a compatibility column; the join is refused with the version / mods notice). The row's text buffer is small
                // (32 bytes): the name is cut at a character boundary so the mark always fits.
                // 1.1: a difference in Metal Sonic / Egg Hangar shows on the row's mod icons (the user's design), not here.
                const char* mark = r.otherVersion ? " (other version)" : r.otherModsRest ? " (other mods)" : "";
                int32_t cap = TextCapacity(d, 2);
                size_t room = cap > (int32_t)strlen(mark) + 1 ? (size_t)cap - strlen(mark) - 1 : 0;
                char name[kNameLen];
                CutUtf8(r.host[0] ? r.host : "?", room, name, sizeof(name));
                sprintf_s(text, "%s%s", name, mark);
            } else {
                sprintf_s(text, "%d/%d", r.members, r.maxMembers);
            }
            ReturnInt(d, WriteText(d, 2, text) ? 1 : 0);
        }
        return;
    case kNatSetLocal: {
        static int32_t last[2] = {-2, -2};  // what Steam was last given
        int32_t v[2];
        if (!IntArg(d, 0, v[0]) || !IntArg(d, 1, v[1])) return;
        v[1] = v[1] ? 1 : 0;
        if ((v[0] != last[0] || v[1] != last[1]) && LobbyPostArgs(kCmdSetLocal, v, 2)) { last[0] = v[0]; last[1] = v[1]; }
        ReturnInt(d, 1);
        return;
    }
    case kNatSetRules: {
        static int32_t last[kRuleCount] = {-2, -2, -2, -2, -2, -2};
        int32_t v[kRuleCount];
        for (int i = 0; i < kRuleCount; ++i)
            if (!IntArg(d, i, v[i])) return;
        if (s.state == kLobbyInLobby && s.isHost && memcmp(v, last, sizeof(v)) != 0 && LobbyPostArgs(kCmdSetRules, v, kRuleCount))
            memcpy(last, v, sizeof(v));
        ReturnInt(d, 1);
        return;
    }
    case kNatCanStart: {
        if (s.state != kLobbyInLobby || !s.isHost) { ReturnInt(d, kStartNotHost); return; }
        if (s.memberCount < 2) { ReturnInt(d, kStartAlone); return; }
        for (int i = 0; i < s.memberCount; ++i)
            if (s.members[i].racer < 0 || !s.members[i].ready) { ReturnInt(d, kStartNotReady); return; }
        ReturnInt(d, kStartOk);
        return;
    }
    case kNatStart: {
        int32_t r = RaceHostStart();
        Log(false, L"host Start: %d", r);
        ReturnInt(d, r);
        return;
    }
    case kNatRacePoll: ReturnInt(d, RaceMenuPoll()); return;
    case kNatRaceSlot: {
        const RaceContract* c = RaceContractArmed();
        if (!c || !IntArg(d, 0, a) || !IntArg(d, 1, b) || a < 0 || a >= kGridSlots) return;
        if (b == 0) ReturnTyped(d, c->roster[a], kTypeHash);
        else ReturnInt(d, a == RaceLocalIndex() ? g_localPad : -1);
        return;
    }
    case kNatFrame: {
        int32_t v[12];
        for (int i = 0; i < 10; ++i)
            if (!IntArg(d, i, v[i])) return;
        // Native 0400 (primary pad) returns hash 0 when no pad is assigned yet.
        if (d->argc < 12 || (d->types[10] != kTypeInt && d->types[10] != kTypeHash) || !IntArg(d, 11, v[11])) return;
        v[10] = (int32_t)d->values[10];
        g_localPad = v[10];
        {  // the primary pad's class: the keyboard's vtable, or a controller (XInput 0x67e3f8, DirectInput 0x67e39c)
            uint8_t* pad = d->types[10] == kTypeInt ? g_primaryPad() : nullptr;
            LobbyNoteInput(!pad ? kInputUnknown : *(uint32_t*)pad == kKeyboardVtable ? kInputKeyboard : kInputController);
        }
        DrawRaceBar(s);  // the race running here, for a player who joined meanwhile
        DrawDriftRow(s);
        // Until this player has picked a racer here, none (the others show "?"); ready (the others' check) = a racer and
        // this player is at the lobby itself: no racer popup, no Settings panel, no Steam overlay (INVITE FRIENDS) open,
        // as the console's check (user, 2026-10-22, from console footage).
        bool atLobby = v[1] == 0 && v[8] == 0 && !LobbyOverlayOpen();
        int32_t local[2] = {v[11] ? -1 : v[0], !v[11] && atLobby ? 1 : 0};
        static int32_t lastLocal[2] = {-2, -2};
        // The heartbeat: away from the lobby screen for a moment (the race and its results, a loading screen), the worker
        // published "not ready"; back here, post again. (This runs every lobby frame: 500 per 10 s, gaps up to 32 ms.)
        static DWORD s_frameSeen = 0;
        DWORD frameNow = GetTickCount();
        if (s_frameSeen && frameNow - s_frameSeen > kLobbyAwayMs) {
            lastLocal[0] = lastLocal[1] = -2;
            Log(false, L"lobby: back at the lobby screen after %u ms away", frameNow - s_frameSeen);
        }
        s_frameSeen = frameNow;
        LobbyNoteLobbyFrame();
        if (memcmp(local, lastLocal, sizeof(local)) != 0 && LobbyPostArgs(kCmdSetLocal, local, 2)) {
            memcpy(lastLocal, local, sizeof(local));
            Log(false, L"lobby: this player is %s (racer %d)", local[1] ? L"ready" : L"not ready", local[0]);
        }
        static int32_t lastRules[kRuleCount] = {-2, -2, -2, -2, -2, -2};
        if (s.state == kLobbyInLobby && s.isHost && memcmp(v + 2, lastRules, sizeof(lastRules)) != 0 &&
            LobbyPostArgs(kCmdSetRules, v + 2, kRuleCount))
            memcpy(lastRules, v + 2, sizeof(lastRules));
        if (RaceMenuPoll() == kRaceStart) {
            // Not ready while racing: the host can't start the next race until everyone is back in
            // the lobby (its first frame there publishes ready again).
            int32_t racing[2] = {v[0], 0};
            if (memcmp(racing, lastLocal, sizeof(racing)) != 0 && LobbyPostArgs(kCmdSetLocal, racing, 2)) memcpy(lastLocal, racing, sizeof(racing));
            ReturnInt(d, kFrameStartRace);
            return;
        }
        // A Steam Join Game / invite: the script leaves this lobby and takes it (not while a popup is open).
        if (s.pendingJoin && v[1] == 0 && v[8] == 0) {
            ReturnInt(d, kFrameJoin);
            return;
        }
        // The select key (arg 12: F2, the Mac's triangle): with the lobby cursor (arg 13) on ANOTHER player's bar it opens the
        // console's player popup (VIEW STEAM PROFILE / KICK / RETURN TO LOBBY for the host, VIEW / RETURN for a guest); on your own
        // bar the script opens the racer picker (CHANGE RACER). INVITE FRIENDS is the script's (kNatInviteKey + kNatInvite).
        int32_t selectKey = 0;
        bool select = d->argc >= 13 && IntArg(d, 12, selectKey) && selectKey;
        // Start while a player's game can't make race connections yet (no Steam certificate, which can
        // take minutes after the game starts) waits, and the race starts by itself once all can. A player
        // not ready (still on the last race's results, or away from the lobby) is a plain refusal: the host
        // presses Start again (user, 2026-10-22: no start by itself once that player is back).
        static bool s_waiting = false;
        static DWORD s_waitingSince = 0, s_lastFrame = 0;
        if (s_waiting && GetTickCount() - s_lastFrame > 1000) {  // the lobby screen was left meanwhile, or a dialog was up a while
            s_waiting = false;
            g_elemPose(kSpinGroup, kSpinHidden, kSpinHidden, 0.0f);  // CONNECTING off at once (a fade could flash; safe to repeat)
        }
        s_lastFrame = GetTickCount();
        bool canStart = s.state == kLobbyInLobby && s.isHost && v[1] == 0 && v[8] == 0;
        // The lobby cursor (arg 13; the console's: Up/Down over every bar, for everyone); the popup's KICK asks first, then kNatKick
        // kicks the player the question was about. The host's Start key (arg 9: Enter, the Mac's START) starts whatever the cursor.
        int32_t cursor = -1;
        bool cursorOnOther = d->argc >= 14 && IntArg(d, 13, cursor) && cursor >= 0 && cursor < s.memberCount && cursor < kMaxMembers && !s.members[cursor].isMe;
        if (!cursorOnOther) g_kickTarget = 0;  // (the cursor left the player: no question stands)
        if (cursorOnOther && select && s.state == kLobbyInLobby && v[1] == 0 && v[8] == 0) {
            if (IsTestPlayer(s.members[cursor].id)) {
                Log(false, L"lobby: bar %d is a test player; no popup", cursor + 1);
                ReturnInt(d, kFrameStartRefused);
                return;
            }
            g_kickTarget = s.members[cursor].id;  // a kick from the popup is about this player, whatever the bars do meanwhile
            ReturnInt(d, s.members[cursor].muted && UnmuteTextShown() ? kFramePopupMuted : kFramePopup);
            return;
        }
        if (!canStart && s_waiting) {
            s_waiting = false;
            ConnectingSpinner(false);
            Log(false, L"host Start: no longer waiting (the lobby changed)");
        }
        if (canStart && (v[9] || s_waiting)) {
            int32_t r = RaceHostStart();
            if (r == kStartNetNotReady) {
                if (!s_waiting) {
                    s_waiting = true;
                    s_waitingSince = GetTickCount();
                    ConnectingSpinner(true);
                    Log(true, L"host Start: waiting until every player's game can make race connections (CONNECTING); the race then starts by itself");
                    ReturnInt(d, kFrameStartSent);  // the Start sound: heard
                    return;
                }
                if (GetTickCount() - s_waitingSince > 180000) {
                    s_waiting = false;
                    ConnectingSpinner(false);
                    Log(true, L"host Start: a player's game still can't make race connections after 3 minutes; press Start again later");
                    ReturnInt(d, kFrameStartRefused);
                    return;
                }
                ReturnInt(d, kFrameNothing);
                return;
            }
            bool pressed = v[9] != 0, waited = s_waiting;
            s_waiting = false;
            if (waited) ConnectingSpinner(false);
            Log(r ? true : false, L"host Start: %s (code %d)%s", r ? L"refused" : L"race sent to everyone", r, waited ? L" after waiting" : L"");
            if (r == kStartAlone) { ReturnInt(d, kFrameStartAlone); return; }  // the console's "at least two players" dialog
            if (r == kStartNotReady || r == kStartPlayerInRace || r == kStartRacerOff) { ReturnInt(d, kFrameStartNotReady); return; }  // "Not all players are ready."
            ReturnInt(d, r ? kFrameStartRefused : (pressed ? kFrameStartSent : kFrameNothing));
            return;
        }
        ReturnInt(d, kFrameNothing);
        return;
    }
    case kNatRaceCourse: {
        const RaceContract* c = RaceContractArmed();
        if (c && c->course < sizeof(kCourses) / sizeof(kCourses[0]))  // a literal string: the VM never frees it
            ReturnTyped(d, (uint32_t)(uintptr_t)kCourses[c->course].name, kTypeLiteralString);
        return;
    }
    case kNatRaceLaunched:
        if (IntArg(d, 0, a)) RaceLaunched(a != 0);
        ReturnInt(d, 1);
        return;
    case kNatMemberRacer:
    case kNatMemberField:
    case kNatMemberText: {
        // Every member in lobby order (host first, steam_mp.cpp), this player included.
        int n = s.state == kLobbyInLobby ? (s.memberCount < kMaxMembers ? s.memberCount : kMaxMembers) : 0;
        if (op == kNatMemberField && IntArg(d, 1, b) && b == kMemberCount) { ReturnInt(d, n); return; }
        if (!IntArg(d, 0, a) || a < 0 || a >= n) return;
        const LobbyMember& m = s.members[a];
        if (op == kNatMemberRacer) {
            // The portrait follows "has a racer" (the check shows ready), as on the console. A non-hash
            // key would match some other racer in the script's portrait table: none is an int 0.
            uint32_t hash = m.racer >= 0 ? RaceRacerHash(m.racer) : 0;
            if (hash) ReturnTyped(d, hash, kTypeHash);
            else ReturnInt(d, 0);
        } else if (op == kNatMemberField) {
            // Ready: a racer, the popup closed, and back from the last race (its results: "pos" still set), so a player
            // still on the results screen shows no check (user, 2026-10-22).
            if (b == kMemberReady) ReturnInt(d, m.racer >= 0 && m.ready && m.place < 0 ? 1 : 0);
            else if (b == kMemberIsMe) ReturnInt(d, m.isMe ? 1 : 0);
            else if (b == kMemberTalking) ReturnInt(d, m.talking ? 1 : 0);  // (voice chat: the bar's voice icon, as the console's)
            else if (b == kMemberRacingHere) {  // a race runs here (the console's guest footer drops CHANGE RACER then)
                int racing = 0;
                for (int i = 0; i < n; ++i) racing |= s.members[i].place >= 0 && s.members[i].raceId && s.members[i].raceId != RaceLastId() ? 1 : 0;
                ReturnInt(d, racing);
            }
        } else {
            // The bar's name; during the race bar's status half (a redraw comes with every racer's progress update)
            // the status instead, so it doesn't blink back to the name for a frame.
            bool status = a < kMaxMembers && StatusOn();
            // The bar's colour first (every redraw, so a bar a player moved into is right too): yellow when muted, else blue.
            bool yellow = false;
            if (a < 8) {
                yellow = m.muted && g_elemArt(kBarBases[a], kBarMutedArt);
                if (!yellow) g_elemArt(kBarBases[a], kBarBlueArt);
            }
            char text[kNameLen + 16];
            if (status) StatusText(m, text, sizeof(text));
            else BarName(m, TextCapacity(d, 1), text, sizeof(text), yellow);  // muted on a blue bar: "(muted)"
            bool ok = WriteText(d, 1, text);
            if (a < kMaxMembers) {  // the bar's buffer, for the race bar's status text
                g_barText[a] = ok ? (uint8_t*)(uintptr_t)d->values[1] : nullptr;
                g_barStatus[a] = ok && status ? 1 : 0;
                g_barYellow[a] = yellow ? 1 : 0;
            }
            ReturnInt(d, ok ? 1 : 0);
        }
        return;
    }
    case kNatHostRule: {
        // The menu's bounds per field (tools/netmenu/settings.py BOUNDS, laps 1-9; courses 0-24, Egg Hangar's the 25th): a
        // value outside them would index the panel's label tables past their end.
        static const int32_t kLow[kRuleCount] = {0, 1, 0, 0, 0, 0},
                             kHigh[kRuleCount] = {(int32_t)(sizeof(kCourses) / sizeof(kCourses[0])) - 1, 9, 7, 3, 1, 1};
        const int32_t* r = &s.rules.course;
        if (!IntArg(d, 0, a)) return;
        bool ok = s.state == kLobbyInLobby && !s.isHost && a >= 0 && a < kRuleCount && r[a] >= kLow[a] && r[a] <= kHigh[a];
        ReturnInt(d, ok ? r[a] : -1);
        return;
    }
    case kNatInvite: ReturnInt(d, s.state == kLobbyInLobby && LobbyPost(kCmdInvite, 0) ? 1 : 0); return;
    case kNatJoinPoll: ReturnInt(d, s.state != kLobbyOff && s.pendingJoin ? 1 : 0); return;
    case kNatJoinTake: {
        int32_t id[2] = {(int32_t)(uint32_t)s.pendingJoin, (int32_t)(uint32_t)(s.pendingJoin >> 32)};
        bool ok = s.state != kLobbyOff && s.pendingJoin && LobbyPostArgs(kCmdJoinLobby, id, 2);
        if (ok) Log(false, L"menus: taking the Steam join of lobby %llu", s.pendingJoin);
        ReturnInt(d, ok ? 1 : 0);
        return;
    }
    case kNatOtherReady:
    case kNatOtherCount:
    case kNatOtherText: {
        // The other players in the lobby (host first); this player's own row is drawn by the script.
        int others[kMaxMembers], n = 0;
        if (s.state == kLobbyInLobby)
            for (int i = 0; i < s.memberCount && i < kMaxMembers; ++i)
                if (!s.members[i].isMe) others[n++] = i;
        if (op == kNatOtherCount) ReturnInt(d, n);
        else if (op == kNatOtherReady) {
            if (IntArg(d, 0, a) && a >= 0 && a < n) {
                const LobbyMember& o = s.members[others[a]];
                ReturnInt(d, o.racer >= 0 && o.ready && o.place < 0);  // (as kMemberReady)
            }
        } else if (IntArg(d, 0, a) && a >= 0 && a < n)
            ReturnInt(d, WriteText(d, 1, s.members[others[a]].name[0] ? s.members[others[a]].name : "?") ? 1 : 0);
        return;
    }
    }
    // Unknown op: no return value set, so the script reads 0.
}

#ifdef NATIVES_TEST
// tests/test_natives.cpp: run the real handler on a fake descriptor.
void NativesTestHandle(void* desc) { Handle((Descriptor*)desc); }
void NativesTestSetBar(void* show, void* hide, void* pose, void* art) {
    g_elemShow = (ElemShowFn)show; g_elemHide = (ElemShowFn)hide; g_elemPose = (ElemPoseFn)pose; g_elemArt = (ElemArtFn)art;
}
void NativesTestSetSceneText(void* fn) { g_sceneText = (SceneTextFn)fn; }
void NativesTestSetLicenseFlag(int32_t (*fn)()) { g_licenseFlag = fn; }
void NativesTestSetSpinner(void* text, void* loop, void* play) { g_elemTextKey = (ElemTextKeyFn)text; g_elemLoop = (ElemLoopFn)loop; g_elemPlay = (ElemPlayFn)play; }
void NativesTestSetPrimaryPad(uint8_t* (*fn)()) { g_primaryPad = fn; }
void NativesTestSetKeyState(bool (*fn)(int)) { g_keyState = fn; g_refreshDown = false; g_refreshReadAt = GetTickCount(); }
#endif

// Same frame as the original thiscall (ECX = vm, two stack args, callee pops 8).
static void __fastcall Dispatch(void* vm, void* /*edx*/, uint32_t id, void* desc) {
    if (!g_scriptThread) g_scriptThread = GetCurrentThreadId();  // the game's first native call
    if (((id >> 8) & 0xff) != kNatGroup) {
        NativeCall* d = (NativeCall*)desc;
        if (d && d->id == id && RaceNativeBefore(vm, d)) return;  // (the descriptor carries the same id)
        g_original(vm, id, desc);
        if (d && d->id == id) RaceNativeAfter(vm, d);
        return;
    }
    if (GetCurrentThreadId() != g_scriptThread) {
        if (!g_wrongThreadLogged) Log(true, L"menu native %04x called off the game thread; ignored", id);
        g_wrongThreadLogged = true;
        return;
    }
    Handle((Descriptor*)desc);
}

static bool WriteSlot(uint32_t expected, uint32_t value) {
    DWORD old, ignored;
    if (!VirtualProtect((void*)(uintptr_t)kSlot, 4, PAGE_READWRITE, &old)) return false;
    LONG prev = InterlockedCompareExchange((volatile LONG*)(uintptr_t)kSlot, (LONG)value, (LONG)expected);
    VirtualProtect((void*)(uintptr_t)kSlot, 4, old, &ignored);
    return (uint32_t)prev == expected;
}

bool NativesInstall(HMODULE gameImage) {
    if ((uintptr_t)gameImage != 0x400000) return false;
    if (*(volatile uint32_t*)(uintptr_t)kSlot != kDispatcher) {
        Log(true, L"menu natives: off (script dispatch slot 0x%06x holds 0x%08x, expected 0x%06x; another mod may hook it)", kSlot,
            *(volatile uint32_t*)(uintptr_t)kSlot, kDispatcher);
        return false;
    }
    g_original = (DispatchFn)(uintptr_t)kDispatcher;
    if (!WriteSlot(kDispatcher, (uint32_t)(uintptr_t)&Dispatch)) {
        Log(true, L"menu natives: off (could not patch 0x%06x, error %lu)", kSlot, GetLastError());
        return false;
    }
    g_installed = true;
    Log(false, L"menu natives: installed (group %02x)", kNatGroup);
    return true;
}

void NativesRollback() {
    if (!g_installed) return;
    if (!WriteSlot((uint32_t)(uintptr_t)&Dispatch, kDispatcher)) Log(false, L"menu natives: slot changed by someone else; left as is");
    g_installed = false;
}
