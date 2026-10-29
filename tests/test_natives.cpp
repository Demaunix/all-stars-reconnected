// Offline test of the menu natives' handler (src/natives.cpp, NATIVES_TEST) with a fake snapshot,
// fake descriptors and fake native-1610 text buffers. The game's buffer vtable address 0x6a71f8 is
// only compared, never dereferenced.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lobby.h"
#include "natives.h"

void NativesTestHandle(void* desc);
void Log(bool, const wchar_t*, ...) {}

static LobbySnapshot g_fake = {};
static int g_posts = 0, g_lastKind = -1, g_lastArg = -1;
bool LobbyPost(LobbyCommandKind kind, int32_t arg) { ++g_posts; g_lastKind = kind; g_lastArg = arg; return true; }
static bool g_overlay = false;
bool LobbyOverlayOpen() { return g_overlay; }
const wchar_t* LobbyConfigPath() { return L""; }  // no config.ini: the REFRESH key native falls back to F3
static int g_lobbyFrames = 0;
void LobbyNoteLobbyFrame() { ++g_lobbyFrames; }
static int32_t g_noted = -1;
void LobbyNoteFlag(int32_t code) { g_noted = code; }
static int32_t g_input = -2;
void LobbyNoteInput(int32_t kind) { g_input = kind; }
static int g_hostMods = -1;
bool LobbyModInstalled(const char* id) { return !strcmp(id, "egg-hangar"); }  // (fake: Egg Hangar only)
void LobbySetHostMods(bool metalOn, bool eggOn) { g_hostMods = (metalOn ? 1 : 0) | (eggOn ? 2 : 0); }
static int32_t FakeLicenseFlag() { return 34; }  // France
void NativesTestSetLicenseFlag(int32_t (*fn)());
static int32_t g_lastArgs[8];
bool LobbyPostArgs(LobbyCommandKind kind, const int32_t* args, int count) {
    ++g_posts;
    g_lastKind = kind;
    memcpy(g_lastArgs, args, count * sizeof(int32_t));
    return true;
}
void LobbyRead(LobbySnapshot& out) { out = g_fake; }

// The race module, faked: natives only route to it.
#include "race.h"
static RaceStatus g_poll = kRaceNone;
static int g_starts = 0, g_launched = -1;
static RaceContract g_contract = {};
RaceStatus RaceMenuPoll() { return g_poll; }
void RaceLaunched(bool ok) { g_launched = ok; }
static int32_t g_startResult = kStartOk;
int32_t RaceHostStart() { ++g_starts; return g_startResult; }
static bool g_driftOff = false;
bool RaceDriftPlusOff(const LobbySnapshot&) { return g_driftOff; }
const RaceContract* RaceContractArmed() { return g_poll == kRaceStart ? &g_contract : nullptr; }
int RaceLocalIndex() { return 1; }
uint32_t RaceRacerHash(int index) { return index >= 0 && index < 25 ? 0x9a530000u + (uint32_t)index : 0; }
bool RaceRacerOn(int index) { return index != 24; }  // (fake: the last racer's mod is off)
static uint32_t g_lastRace = 0;
uint32_t RaceLastId() { return g_lastRace; }
// The lobby's race bar: the element helpers, faked.
void NativesTestSetBar(void* show, void* hide, void* pose, void* art);
struct ElemCall { int op; uint32_t e, a, b; float t; };
static ElemCall g_elem[256];
static int g_elemCount = 0;
static void Elem(int op, uint32_t e, uint32_t a, uint32_t b, float t) { if (g_elemCount < 256) g_elem[g_elemCount++] = {op, e, a, b, t}; }
static void __stdcall FakeShow(uint32_t e, uint32_t one) { Elem(1, e, one, 0, 0); }
static void __stdcall FakeHide(uint32_t e, uint32_t one) { Elem(2, e, one, 0, 0); }
static void __stdcall FakePose(uint32_t e, uint32_t a, uint32_t b, float t) { Elem(3, e, a, b, t); }
static bool g_artMissing = false;  // the yellow bar's art not in the scene (another mod's lobby scene): 0x47f400 answers 0
static bool __stdcall FakeArt(uint32_t e, uint32_t art) { Elem(4, e, art, 0, 0); return !(g_artMissing && art == 0xdd4e2bb1); }
static void __stdcall FakeTextKey(uint32_t e, uint32_t key) { Elem(5, e, key, 0, 0); }
static void __stdcall FakeLoop(uint32_t e, uint32_t s, uint32_t f, uint32_t to, uint32_t, int32_t, uint32_t) { Elem(6, e, s, f, 0); (void)to; }
static void __stdcall FakePlay(uint32_t e, uint32_t from, uint32_t to, uint32_t, uint32_t) { Elem(7, e, from, to, 0); }
void NativesTestSetSpinner(void* text, void* loop, void* play);
static int ElemCalls(int op, uint32_t e) { int n = 0; for (int i = 0; i < g_elemCount; ++i) if (g_elem[i].op == op && g_elem[i].e == e) ++n; return n; }
void NativesTestSetSceneText(void* fn);
void NativesTestSetKeyState(bool (*fn)(int));
void NativesTestSetPrimaryPad(uint8_t* (*fn)());
static int g_fakeKeyDown = 0;  // the virtual key held down, for the REFRESH key native
static bool FakeKeyState(int vk) { return vk == g_fakeKeyDown; }
static const char* g_lapFormat = nullptr;  // the lobby's own "{0} (LAP {1} OF {2})" text, when there is one
static const char* g_unmuteText = "UNMUTE";  // the dialogs text NOTHING (72259212) as our text tables write it
static const char* __fastcall FakeSceneText(uintptr_t sets, void*, uint32_t scene, uint32_t key) {
    if (sets == 0xc282a8 && scene == 0x424b961f && key == 0x72259212) return g_unmuteText;
    if (sets != 0xc282a8 || scene != 0xa363a411) return nullptr;
    return key == 0x1de2f2dd ? g_lapFormat : nullptr;
}
static const ElemCall* LastElem(int op, uint32_t e) { for (int i = g_elemCount - 1; i >= 0; --i) if (g_elem[i].op == op && g_elem[i].e == e) return &g_elem[i]; return nullptr; }
static int g_cancels = 0;
void RaceCancel(AbortReason) { ++g_cancels; }
static int32_t g_profileArgs[4] = {-9, -9, -9, -9};
int32_t RaceProfileNative(int32_t field, int32_t up, int32_t down, int32_t f1) {
    g_profileArgs[0] = field; g_profileArgs[1] = up; g_profileArgs[2] = down; g_profileArgs[3] = f1;
    return 7;
}
static int g_afterCalls = 0;
void RaceNativeAfter(void*, NativeCall*) { ++g_afterCalls; }
bool RaceNativeBefore(void*, NativeCall*) { return false; }
void RaceMainMenuJoinCheck(void*) {}

struct Desc {
    uint32_t id; uint8_t normal, pad[3]; const uint8_t* types; const uint32_t* values; uint32_t argc, ret, retType;
};
struct Buffer { uint32_t vtable, zero; int32_t cap; char* buf; };

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("  %s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static Desc Call(uint32_t op, const uint8_t* types, const uint32_t* values, uint32_t argc) {
    Desc d = {0x7A00u | op, 1, {}, types, values, argc, 0, 0};
    NativesTestHandle(&d);
    return d;
}

static Desc RowText(int row, int field, Buffer* b) {
    static uint8_t t[3] = {2, 2, 7};
    uint32_t v[3] = {(uint32_t)row, (uint32_t)field, (uint32_t)(uintptr_t)b};
    return Call(kNatRowText, t, v, 3);
}

int main() {
    char storage[64];
    Buffer b = {0x6a71f8, 0, 32, storage};
    // The scene helpers faked from the start: the bar names (kNatMemberText) set the bars' art, the popup reads a dialogs text.
    NativesTestSetBar((void*)&FakeShow, (void*)&FakeHide, (void*)&FakePose, (void*)&FakeArt);
    NativesTestSetSceneText((void*)&FakeSceneText);
    NativesTestSetSpinner((void*)&FakeTextKey, (void*)&FakeLoop, (void*)&FakePlay);
    NativesTestSetLicenseFlag(&FakeLicenseFlag);
    g_fake.state = kLobbyResults;
    g_fake.rowCount = 2;
    strcpy_s(g_fake.rows[0].host, "Alfa");
    g_fake.rows[0].members = 1;
    g_fake.rows[0].maxMembers = 4;
    // 30 ASCII bytes then a 2-byte character: only 31 bytes fit, so the character is dropped whole.
    strcpy_s(g_fake.rows[1].host, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123\xc3\xa9");

    Desc d = Call(kNatVersion, nullptr, nullptr, 0);
    Check(d.retType == 2 && d.ret == (uint32_t)kBridgeVersion, "version");
    d = Call(kNatRowCount, nullptr, nullptr, 0);
    Check(d.ret == 2, "row count in Results");
    d = RowText(0, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "Alfa") == 0, "row 0 host name");
    d = RowText(0, 1, &b);
    Check(d.ret == 1 && strcmp(storage, "1/4") == 0, "row 0 players");
    d = RowText(1, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123") == 0, "truncated at a character boundary");
    strcpy_s(g_fake.rows[1].host, "a\xff" "b\xe2\x82" "c\xf0\x9f\x98\x80" "d\xc3\xa9");
    d = RowText(1, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "a?b?c?d\xc3\xa9") == 0, "invalid / 4-byte UTF-8 replaced");
    g_fake.rows[0].otherVersion = 1;  // a host on another mod build: listed, marked (its join is refused)
    d = RowText(0, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "Alfa (other version)") == 0, "row of another mod version marked");
    g_fake.rows[1].otherVersion = 1;  // a long name: cut at a character boundary so the mark fits the 32-byte buffer
    strcpy_s(g_fake.rows[1].host, "ABCDEFGHIJKLM\xc3\xa9OPQRSTUVWXYZ0123456");
    d = RowText(1, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "ABCDEFGHIJKLM\xc3\xa9 (other version)") == 0, "long name cut before the mark (whole characters)");
    strcpy_s(g_fake.rows[1].host, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123\xc3\xa9");
    g_fake.rows[1].otherVersion = 0;
    g_fake.rows[0].otherVersion = 0;
    d = RowText(0, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "Alfa") == 0, "row of this version unmarked");
    g_fake.rows[0].otherMods = g_fake.rows[0].otherModsRest = 1;  // the host's mods differ: marked (its join is refused)
    d = RowText(0, 0, &b);
    Check(d.ret == 1 && strcmp(storage, "Alfa (other mods)") == 0, "row with other mods marked");
    g_fake.rows[0].otherMods = g_fake.rows[0].otherModsRest = 0;
    {  // 1.1: the row's mod icons (the SIGNAL cell): none -> int 0 (the stock bars), else the sprite of tools\mod_icons.py
        uint8_t t[1] = {2};
        uint32_t v[1] = {0};
        // (tools\mod_icons.py's art names, Egg's state then Metal's: N unused, K same, D other version, X missing)
        const struct { int mods; uint32_t art; } cases[] = {{0, 0}, {1, 0x8ecec74d /* KN */}, {1 << 2, 0xd3189cdb /* NK */},
            {1 | 1 << 2, 0x969345e2 /* KK */}, {3 | 2 << 2, 0x599f90ca /* XD */}, {2 | 3 << 2, 0x3d08abc2 /* DX */}, {3 | 3 << 2, 0xd0819a3e /* XX */}};
        for (const auto& k : cases) {
            g_fake.rows[0].mods = k.mods;
            d = Call(kNatRowMods, t, v, 1);
            Check(d.ret == k.art && d.retType == (k.art ? 3u : 2u), "row mod icons");
        }
        g_fake.rows[0].mods = 3 | 3 << 2;
        g_fake.rows[0].otherMods = 1;  // refused because of the icons' mods only: the name stays plain
        d = RowText(0, 0, &b);
        Check(d.ret == 1 && strcmp(storage, "Alfa") == 0, "a row refused for its icons' mods keeps its plain name");
        g_fake.rows[0].otherModsRest = 1;
        d = RowText(0, 0, &b);
        Check(d.ret == 1 && strcmp(storage, "Alfa (other mods)") == 0, "a row refused for another mod says (other mods)");
        g_fake.rows[0].otherMods = g_fake.rows[0].otherModsRest = 0;
        g_fake.rows[0].mods = 0;
        v[0] = 7;
        d = Call(kNatRowMods, t, v, 1);
        Check(d.ret == 0, "row mod icons out of range");
    }
    {
        uint8_t t[1] = {2};  // the STATUS column: GAME while that lobby's race runs
        uint32_t v[1] = {0};
        g_fake.rows[0].racing = 1;
        d = Call(kNatRowRacing, t, v, 1);
        Check(d.ret == 1, "row racing");
        g_fake.rows[0].racing = 0;
        d = Call(kNatRowRacing, t, v, 1);
        Check(d.ret == 0, "row in its lobby");
        v[0] = 7;
        d = Call(kNatRowRacing, t, v, 1);
        Check(d.ret == 0, "row racing out of range");
        // M8: the row's flag cell: the host's license nationality as the search scene's flag sprite; unknown or an older build: 0.
        v[0] = 0;
        g_fake.rows[0].flag = 103;
        d = Call(kNatRowFlag, t, v, 1);
        Check(d.ret == 0x7103b4d4 && d.retType == 3, "row flag: a known code's sprite (a hash)");
        g_fake.rows[0].flag = 0;
        d = Call(kNatRowFlag, t, v, 1);
        Check(d.ret == 0x47ace633, "row flag: code 0, the SEGA logo");
        g_fake.rows[0].flag = -1;
        d = Call(kNatRowFlag, t, v, 1);
        Check(d.ret == 0 && d.retType == 2, "row flag: none known: int 0");
        g_fake.rows[0].flag = 42;
        d = Call(kNatRowFlag, t, v, 1);
        Check(d.ret == 0, "row flag: a code without a sprite: 0");
        v[0] = 7;
        d = Call(kNatRowFlag, t, v, 1);
        Check(d.ret == 0, "row flag out of range");
        // 1.1 ENABLE MODIFICATIONS: the racer picker's per-row question (RaceRacerOn).
        v[0] = 3;
        d = Call(kNatRacerOn, t, v, 1);
        Check(d.ret == 1, "racer on: a racer whose mod is on");
        v[0] = 24;
        d = Call(kNatRacerOn, t, v, 1);
        Check(d.ret == 0, "racer on: a racer whose mod is off");
        // The CREATE question: what is installed, then the answers for the next lobby.
        uint8_t t2[2] = {2, 2};
        uint32_t m[2] = {0, 0};
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 0, "mods: Metal Sonic not installed");
        m[0] = 1;
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 1, "mods: Egg Hangar installed");
        m[0] = 2;
        m[1] = 2;
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 1 && g_hostMods == 2, "mods: the answers reach the lobby (Egg on, Metal off)");
        m[0] = 7;
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 0xffffffffu && g_hostMods == 2, "mods: another field changes nothing");
        m[0] = 3;
        g_fake.refusedMods = 3;  // (Egg Hangar not installed here: modset.h kModMissing)
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 11 && g_hostMods == 2, "mods: the refusal pop-up's text for Egg Hangar not installed");
        g_fake.refusedMods = 0;
        d = Call(kNatMods, t2, m, 2);
        Check(d.ret == 0, "mods: no named mod, the plain refusal text");
        // The modifications question's Esc: the dialog layer's back byte (+0x126), pressed bit 4; not with its confirm (+0x125).
        static uint8_t pad[0x200];
        NativesTestSetPrimaryPad([]() -> uint8_t* { return pad; });
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 0, "dialog back: nothing pressed");
        pad[0xf8] = 0x11;  // (the menu layer's back: not the dialog's)
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 0, "dialog back: the menu layer's back doesn't count");
        pad[0x126] = 0x11;
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 1, "dialog back: Esc in the dialog layer");
        pad[0x126] = 0x01;
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 0, "dialog back: held, not pressed this frame");
        pad[0x126] = pad[0x125] = 0x11;
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 0, "dialog back: not with Enter");
        NativesTestSetPrimaryPad([]() -> uint8_t* { return nullptr; });
        d = Call(kNatDialogBack, nullptr, nullptr, 0);
        Check(d.ret == 0, "dialog back: no pad");
        // The license's code is noted where this game hosts or joins (the host publishes it).
        g_noted = -9;
        v[0] = 1;
        Call(kNatHost, t, v, 1);
        Check(g_noted == 34, "host: the license flag noted");
    }
    d = RowText(2, 0, &b);
    Check(d.retType == 0, "row out of range: no return");
    Buffer wrong = {0x12345678, 0, 32, storage};
    d = RowText(0, 0, &wrong);
    Check(d.ret == 0 && d.retType == 2, "wrong buffer vtable refused");
    Buffer huge = {0x6a71f8, 0, 5000, storage};
    d = RowText(0, 0, &huge);
    Check(d.ret == 0, "capacity over 4096 refused");
    Buffer tiny = {0x6a71f8, 0, 1, storage};
    d = RowText(0, 0, &tiny);
    Check(d.ret == 1 && storage[0] == 0, "capacity 1 gives an empty string");
    {
        uint8_t t[3] = {2, 2, 2};  // buffer argument is an int, not an object
        uint32_t v[3] = {0, 0, (uint32_t)(uintptr_t)&b};
        d = Call(kNatRowText, t, v, 3);
        Check(d.ret == 0, "non-object buffer argument refused");
    }
    {
        uint8_t t[1] = {3};  // hash instead of int
        uint32_t v[1] = {1};
        g_posts = 0;
        d = Call(kNatSearch, t, v, 1);
        Check(g_posts == 0 && d.retType == 0, "search with a non-int mode is ignored");
        t[0] = 2;
        d = Call(kNatSearch, t, v, 1);
        Check(g_posts == 1 && g_lastKind == kCmdSearch && g_lastArg == 1 && d.ret == 1, "search(friends) posted");
    }
    {
        uint8_t t[1] = {2};  // the search rows' VIEW PROFILE: the row's host id to the Steam worker, in two dwords
        uint32_t v[1] = {0};
        g_fake.rows[0].hostId = 0x0110000112345678ull;
        g_posts = 0;
        d = Call(kNatRowProfile, t, v, 1);
        Check(d.ret == 1 && g_posts == 1 && g_lastKind == kCmdProfile && (uint32_t)g_lastArgs[0] == 0x12345678u && (uint32_t)g_lastArgs[1] == 0x01100001u,
              "row profile asked for");
        v[0] = 1;  // a row from an older build: no host id
        d = Call(kNatRowProfile, t, v, 1);
        Check(d.ret == 0 && g_posts == 1, "row profile without a host id: nothing");
        v[0] = 5;  // no such row
        d = Call(kNatRowProfile, t, v, 1);
        Check(d.ret == 0 && g_posts == 1, "row profile out of range: nothing");
        g_fake.rows[0].hostId = kTestPlayerBase | 1;  // a test player's id: nothing
        v[0] = 0;
        d = Call(kNatRowProfile, t, v, 1);
        Check(d.ret == 0 && g_posts == 1, "row profile of a test player: nothing");
        g_fake.rows[0].hostId = 0;
    }
    {
        uint8_t t[4] = {2, 2, 2, 2};  // the results' profile cursor: (field, up, down, F1) routed to the race module
        uint32_t v[4] = {3, 1, 0, 1};
        d = Call(kNatProfile, t, v, 4);
        Check(d.ret == 7 && d.retType == 2 && g_profileArgs[0] == 3 && g_profileArgs[1] == 1 && g_profileArgs[2] == 0 && g_profileArgs[3] == 1, "profile cursor routed");
        t[3] = 3;  // a hash where an int is due: ignored
        g_profileArgs[0] = -9;
        d = Call(kNatProfile, t, v, 4);
        Check(d.retType == 0 && g_profileArgs[0] == -9, "profile cursor with a non-int key is ignored");
    }
    g_fake.state = kLobbySearching;
    d = Call(kNatRowCount, nullptr, nullptr, 0);
    Check(d.ret == 0, "no rows while searching");
    g_fake.state = kLobbyInLobby;
    g_fake.isHost = 1;
    g_fake.memberCount = 3;
    strcpy_s(g_fake.members[0].name, "Host");
    g_fake.members[0].isHost = 1;
    strcpy_s(g_fake.members[1].name, "Me");
    g_fake.members[1].isMe = 1;
    strcpy_s(g_fake.members[2].name, "Bravo 2");
    d = Call(kNatOtherCount, nullptr, nullptr, 0);
    Check(d.ret == 2, "two other players");
    {
        uint8_t t[2] = {2, 7};
        uint32_t v[2] = {1, (uint32_t)(uintptr_t)&b};
        d = Call(kNatOtherText, t, v, 2);
        Check(d.ret == 1 && strcmp(storage, "Bravo 2") == 0, "second other player skips me");
        v[0] = 0;
        d = Call(kNatOtherText, t, v, 2);
        Check(d.ret == 1 && strcmp(storage, "Host") == 0, "first other player is the host");
        v[0] = 2;
        d = Call(kNatOtherText, t, v, 2);
        Check(d.retType == 0, "no third other player");
    }
    {
        // Lobby bars: every member in lobby order (host first), this player included.
        uint8_t t[2] = {2, 2};
        uint32_t v[2] = {0, kMemberCount};
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 3 && d.retType == 2, "members: 3");
        v[0] = 1;
        v[1] = kMemberIsMe;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 1, "member 1 is this player");
        v[0] = 0;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0 && d.retType == 2, "member 0 is not");
        // Voice chat: the bar's voice icon follows the snapshot's talking flag.
        v[1] = kMemberTalking;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0 && d.retType == 2, "member 0 isn't talking");
        g_fake.members[0].talking = 1;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 1, "member 0 talks: the icon");
        g_fake.members[0].talking = 0;
        v[1] = kMemberRacingHere;  // the console's guest footer drops CHANGE RACER while a race runs here
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0, "racing here: nobody racing");
        g_fake.members[2].place = 2; g_fake.members[2].raceId = 0x77;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 1, "racing here: a member races another race");
        g_fake.members[2].place = -1; g_fake.members[2].raceId = 0;
        {
            uint8_t pt[1] = {2};
            uint32_t pv[1] = {2};
            g_fake.members[2].id = 0x0110000112345678ull;
            g_posts = 0;
            d = Call(kNatMemberProfile, pt, pv, 1);
            Check(d.ret == 1 && g_posts == 1 && g_lastKind == kCmdProfile && (uint32_t)g_lastArgs[0] == 0x12345678u, "member profile: posted");
            pv[0] = 1;
            d = Call(kNatMemberProfile, pt, pv, 1);
            Check(d.ret == 0 && g_posts == 1, "member profile: not this player");
            // MUTE (the popup's toggle): posted for another player, answers the state it leads to; never this player.
            pv[0] = 2;
            d = Call(kNatMemberMute, pt, pv, 1);
            Check(d.ret == 1 && g_posts == 2 && g_lastKind == kCmdMute && (uint32_t)g_lastArgs[0] == 0x12345678u && (uint32_t)g_lastArgs[1] == 0x01100001u,
                  "mute: posted, muted now");
            g_fake.members[2].muted = 1;
            d = Call(kNatMemberMute, pt, pv, 1);
            Check(d.ret == 0 && g_posts == 3 && g_lastKind == kCmdMute, "mute: a muted player is heard again");
            g_fake.members[2].muted = 0;
            pv[0] = 1;
            d = Call(kNatMemberMute, pt, pv, 1);
            Check(d.ret == -1 && g_posts == 3, "mute: not this player");
            pv[0] = 9;
            d = Call(kNatMemberMute, pt, pv, 1);
            Check(d.ret == -1 && g_posts == 3, "mute: no such bar");
            g_fake.members[2].id = 0;
        }
        // Portrait: the racer's character hash (hash type) once it has one, else int 0 ("?"); check: ready.
        g_fake.members[2].racer = 24;
        g_fake.members[2].ready = 0;
        v[0] = 2;
        d = Call(kNatMemberRacer, t, v, 1);
        Check(d.ret == 0x9a530018u && d.retType == 3, "member's racer as a hash");
        v[1] = kMemberReady;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0 && d.retType == 2, "picked, not ready: no check");
        g_fake.members[2].ready = 1;
        g_fake.members[2].place = 3;  // still on the last race's results ("pos" set): no check yet
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0, "ready, still in the last race: no check");
        g_fake.members[2].place = -1;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 1, "ready: check");
        g_fake.members[2].racer = -1;
        d = Call(kNatMemberRacer, t, v, 1);
        Check(d.ret == 0 && d.retType == 2, "no racer yet: int 0");
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0, "no racer: no check");
        uint8_t tt[2] = {2, 7};
        uint32_t vt[2] = {1, (uint32_t)(uintptr_t)&b};
        d = Call(kNatMemberText, tt, vt, 2);
        Check(d.ret == 1 && strcmp(storage, "Me") == 0, "member 1's name");
        {   // MUTE (M8): the bar turns yellow (its base sprite a25dd079.. per bar takes the art dd4e2bb1), the name as it is; back
            // to blue (5da8c97e) when heard again; every redraw sets the colour, so a bar another player moved into is right too.
            char saved[kNameLen];
            memcpy(saved, g_fake.members[2].name, sizeof(saved));
            b.cap = 32;
            vt[0] = 2;
            g_fake.members[2].muted = 1;
            strcpy_s(g_fake.members[2].name, "Short");
            g_elemCount = 0;
            d = Call(kNatMemberText, tt, vt, 2);
            const ElemCall* art = LastElem(4, 0x36cb75a9);
            Check(d.ret == 1 && strcmp(storage, "Short") == 0 && art && art->a == 0xdd4e2bb1 && ElemCalls(4, 0x36cb75a9) == 1,
                  "muted: bar 3 yellow, the name without (muted)");
            g_fake.members[2].muted = 0;
            d = Call(kNatMemberText, tt, vt, 2);
            art = LastElem(4, 0x36cb75a9);
            Check(d.ret == 1 && strcmp(storage, "Short") == 0 && art && art->a == 0x5da8c97e, "heard again: bar 3 blue");
            vt[0] = 0;
            d = Call(kNatMemberText, tt, vt, 2);
            art = LastElem(4, 0xa25dd079);
            Check(art && art->a == 0x5da8c97e && ElemCalls(4, 0x36cb75a9) == 2, "bar 1: its own sprite, blue");
            vt[0] = 2;
            g_fake.members[2].muted = 1;
            // A lobby scene without the yellow art (another mod's): the bar stays blue, the name says "(muted)", cut so it fits.
            g_artMissing = true;
            d = Call(kNatMemberText, tt, vt, 2);
            art = LastElem(4, 0x36cb75a9);
            Check(d.ret == 1 && strcmp(storage, "Short (muted)") == 0 && art && art->a == 0x5da8c97e, "no yellow art: blue and (muted)");
            d = Call(kNatMemberText, tt, vt, 2);
            Check(d.ret == 1 && strcmp(storage, "Short (muted)") == 0, "muted: the suffix");
            strcpy_s(g_fake.members[2].name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcde");  // 31 bytes, a full-length Steam name
            d = Call(kNatMemberText, tt, vt, 2);
            Check(d.ret == 1 && strcmp(storage, "ABCDEFGHIJKLMNOPQRSTUVW (muted)") == 0, "muted, a long name: cut so the suffix fits");
            strcpy_s(g_fake.members[2].name, "\xd0\x90\xd0\x91\xd0\x92\xd0\x93\xd0\x94\xd0\x95\xd0\x96\xd0\x97\xd0\x98\xd0\x99\xd0\x9a\xd0\x9b");  // 12 Cyrillic letters, 24 bytes
            d = Call(kNatMemberText, tt, vt, 2);
            Check(d.ret == 1 && strlen(storage) == 22 + 8 && strcmp(storage + 22, " (muted)") == 0, "muted, a UTF-8 name: whole characters, the suffix kept");
            g_fake.members[2].muted = 0;
            strcpy_s(g_fake.members[2].name, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcde");
            d = Call(kNatMemberText, tt, vt, 2);
            Check(d.ret == 1 && strcmp(storage, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcde") == 0, "not muted: the whole name");
            g_artMissing = false;
            g_fake.members[2].muted = 1;
            d = Call(kNatMemberText, tt, vt, 2);
            Check(d.ret == 1 && strcmp(storage, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcde") == 0, "muted and yellow, a long name: the whole name");
            g_fake.members[2].muted = 0;
            memcpy(g_fake.members[2].name, saved, sizeof(saved));
            vt[0] = 1;
        }
        v[0] = 3;
        d = Call(kNatMemberRacer, t, v, 1);
        Check(d.retType == 0, "no fourth member");
        // A full console-size lobby: 8 members; a count beyond the snapshot's array is clipped.
        for (int i = 3; i < kMaxMembers; ++i) sprintf_s(g_fake.members[i].name, "P%d", i + 1);
        g_fake.memberCount = 8;
        v[0] = 0;
        v[1] = kMemberCount;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 8, "members: 8");
        vt[0] = 7;
        d = Call(kNatMemberText, tt, vt, 2);
        Check(d.ret == 1 && strcmp(storage, "P8") == 0, "member 8's name");
        g_fake.memberCount = 9;
        v[0] = 0;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 8, "a ninth member is clipped");
        v[0] = 8;
        d = Call(kNatMemberRacer, t, v, 1);
        Check(d.retType == 0, "no ninth member");
        g_fake.memberCount = 3;
        g_fake.state = kLobbySearching;
        v[0] = 0;
        v[1] = kMemberCount;
        d = Call(kNatMemberField, t, v, 2);
        Check(d.ret == 0, "no members outside a lobby");
        g_fake.state = kLobbyInLobby;
    }
    {
        // A guest's lobby panel: the host's rules, each within the menu's bounds; the host keeps its own.
        uint8_t t[1] = {2};
        uint32_t v[1] = {1};
        g_fake.rules = LobbyRules{12, 5, 0, 2, 0, 1};
        d = Call(kNatHostRule, t, v, 1);
        Check(d.ret == (uint32_t)-1 && d.retType == 2, "host rule: the host keeps its own");
        g_fake.isHost = 0;
        const int32_t want[kRuleCount] = {12, 5, 0, 2, 0, 1};
        for (int i = 0; i < kRuleCount; ++i) {
            v[0] = (uint32_t)i;
            d = Call(kNatHostRule, t, v, 1);
            Check(d.ret == (uint32_t)want[i] && d.retType == 2, "host rule: a guest gets the host's value");
        }
        v[0] = 6;
        Check(Call(kNatHostRule, t, v, 1).ret == (uint32_t)-1, "host rule: no such field");
        g_fake.rules = LobbyRules{25, 0, 8, 4, 2, 2};  // each just past its bound (course 24 is Egg Hangar's)
        for (int i = 0; i < kRuleCount; ++i) {
            v[0] = (uint32_t)i;
            Check(Call(kNatHostRule, t, v, 1).ret == (uint32_t)-1, "host rule: out of range");
        }
        g_fake.rules = LobbyRules{-1, -1, -1, -1, -1, -1};  // not published yet
        v[0] = 1;
        Check(Call(kNatHostRule, t, v, 1).ret == (uint32_t)-1, "host rule: none yet");
        g_fake.rules = LobbyRules{0, 1, 7, 3, 1, 0};
        Check(Call(kNatHostRule, t, v, 1).ret == 1, "host rule: 1 lap");
        g_posts = 0;
        d = Call(kNatInvite, nullptr, nullptr, 0);
        Check(d.ret == 1 && g_posts == 1 && g_lastKind == kCmdInvite, "invite: asked in a lobby");
        // A Steam Join Game waiting: polled, then taken (posts the join with the lobby id's two halves).
        // Host migration: the handover count, 0 outside a lobby.
        g_fake.state = kLobbyInLobby;
        g_fake.hostChanges = 2;
        Check(Call(kNatHostChanges, nullptr, nullptr, 0).ret == 2, "host handovers in the lobby");
        g_fake.state = kLobbySearching;
        Check(Call(kNatHostChanges, nullptr, nullptr, 0).ret == 0, "host handovers: 0 outside a lobby");
        g_fake.state = kLobbyInLobby;
        g_fake.hostChanges = 0;
        Check(Call(kNatJoinPoll, nullptr, nullptr, 0).ret == 0, "join poll: none waiting");
        g_fake.pendingJoin = 0x0186000012345678ull;
        Check(Call(kNatJoinPoll, nullptr, nullptr, 0).ret == 1, "join poll: one waiting");
        g_posts = 0;
        d = Call(kNatJoinTake, nullptr, nullptr, 0);
        Check(d.ret == 1 && g_posts == 1 && g_lastKind == kCmdJoinLobby && (uint32_t)g_lastArgs[0] == 0x12345678u &&
              (uint32_t)g_lastArgs[1] == 0x01860000u, "join take: the lobby's id posted");
        g_fake.state = kLobbyOff;
        Check(Call(kNatJoinPoll, nullptr, nullptr, 0).ret == 0 && Call(kNatJoinTake, nullptr, nullptr, 0).ret == 0, "join: not without Steam");
        g_fake.state = kLobbyInLobby;
        g_fake.pendingJoin = 0;
        Check(Call(kNatJoinTake, nullptr, nullptr, 0).ret == 0, "join take: nothing waiting");
        Check(IsSessionCommand(kCmdJoinLobby) && FirstState(kCmdJoinLobby) == kLobbyJoining && !IsSessionCommand(kCmdInvite) &&
              FirstState(kCmdLeave) == kLobbyIdle && FirstState(kCmdHost) == kLobbyCreating, "join: a session command, shown as joining");
        g_fake.state = kLobbySearching;
        d = Call(kNatInvite, nullptr, nullptr, 0);
        Check(d.ret == 0 && g_posts == 1, "invite: nothing outside a lobby");
        Check(Call(kNatHostRule, t, v, 1).ret == (uint32_t)-1, "host rule: not in a lobby");
        g_fake.state = kLobbyInLobby;
        g_fake.isHost = 1;
    }
    d = Call(kNatIsHost, nullptr, nullptr, 0);
    Check(d.ret == 1, "is host");
    {
        // The per-frame native: racer/ready and rules are posted only when they change; F2 starts (host).
        uint8_t t[12] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
        uint32_t v[12] = {5, 0, 3, 3, 0, 1, 1, 1, 0, 0, 2, 0};  // racer 5, popup closed, course 3, laps 3, ..., picked
        g_posts = 0;
        g_lobbyFrames = 0;
        d = Call(kNatFrame, t, v, 12);
        Check(d.ret == kFrameNothing && g_posts == 2, "frame: racer and rules posted");
        Check(g_lobbyFrames == 1, "frame: the lobby's heartbeat");
        Sleep(kLobbyAwayMs + 100);  // away from the lobby screen (a loading screen): the worker published "not ready"
        d = Call(kNatFrame, t, v, 12);
        Check(g_posts == 3 && g_lastKind == kCmdSetLocal && g_lastArgs[1] == 1, "frame: back at the lobby: ready posted again");
        g_posts = 2;
        // The worker's staleness test (m7o compared unsigned: a frame in the same clock step read as 49 days away).
        Check(!LobbyFrameStale(1000u | 1, 1000u), "stale: a frame in this clock step (seen = now + 1) is fresh");
        Check(!LobbyFrameStale(0, 5000u), "stale: no lobby frame yet");
        Check(!LobbyFrameStale(1001u, 1001u + kLobbyAwayMs), "stale: exactly the limit is fresh");
        Check(LobbyFrameStale(1001u, 1002u + kLobbyAwayMs), "stale: past the limit");
        Check(LobbyFrameStale(0xffffff01u, 0xffffff01u + kLobbyAwayMs + 50), "stale: across the clock's wrap");
        d = Call(kNatFrame, t, v, 12);
        Check(d.ret == kFrameNothing && g_posts == 2, "frame: nothing re-posted when unchanged");
        // The device that drives this game's car (member data "inp"): the primary pad's class, by its vtable.
        static uint8_t devKbd[0x200], devPad[0x200];
        *(uint32_t*)devKbd = 0x67e444;
        *(uint32_t*)devPad = 0x67e3f8;
        NativesTestSetPrimaryPad([]() -> uint8_t* { return devKbd; });
        Call(kNatFrame, t, v, 12);
        Check(g_input == kInputKeyboard, "frame: a keyboard primary pad drives with the keyboard");
        NativesTestSetPrimaryPad([]() -> uint8_t* { return devPad; });
        Call(kNatFrame, t, v, 12);
        Check(g_input == kInputController, "frame: an XInput primary pad is a controller");
        t[10] = 3;  // (native 0400's hash 0: no pad assigned yet)
        Call(kNatFrame, t, v, 12);
        Check(g_input == kInputUnknown, "frame: no primary pad yet: unknown");
        t[10] = 2;
        NativesTestSetPrimaryPad([]() -> uint8_t* { return nullptr; });
        Call(kNatFrame, t, v, 12);
        Check(g_input == kInputUnknown, "frame: no device: unknown");
        g_posts = 2;
        v[1] = 1;  // racer popup open: not ready
        d = Call(kNatFrame, t, v, 12);
        Check(g_posts == 3 && g_lastKind == kCmdSetLocal && g_lastArgs[0] == 5 && g_lastArgs[1] == 0, "frame: popup open = not ready");
        // At the lobby itself, as the console's check: the Settings panel or Steam's overlay open = not ready either.
        v[1] = 0;
        v[8] = 1;
        d = Call(kNatFrame, t, v, 12);
        Check(g_lastKind == kCmdSetLocal && g_lastArgs[1] == 0, "frame: Settings open = not ready");
        v[8] = 0;
        g_overlay = true;
        d = Call(kNatFrame, t, v, 12);
        Check(g_lastKind == kCmdSetLocal && g_lastArgs[1] == 0, "frame: Steam overlay open = not ready");
        g_overlay = false;
        d = Call(kNatFrame, t, v, 12);
        Check(g_lastKind == kCmdSetLocal && g_lastArgs[1] == 1, "frame: back at the lobby = ready");
        int postsBefore = g_posts;
        v[1] = 1;
        d = Call(kNatFrame, t, v, 12);
        Check(g_posts == postsBefore + 1 && g_lastArgs[1] == 0, "frame: popup open again = not ready");
        v[9] = 1;  // F2 while the popup is open: no start
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 0 && d.ret == kFrameNothing, "frame: no start with the racer popup open");
        v[1] = 0;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 1 && d.ret == kFrameStartSent, "frame: host's Start key starts the race");
        // F2 while a player's game can't make race connections yet: heard, then retried every frame
        // without the key, and the race goes out by itself once it can.
        g_startResult = kStartNetNotReady;
        g_elemCount = 0;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 2 && d.ret == kFrameStartSent, "frame: Start while a connection isn't ready: waiting (Start sound)");
        // M8: the console's busy spinner says CONNECTING while it waits (3 text layers, the lobby's place, the dots, the fade-in).
        Check(ElemCalls(5, 0x9e70d8aa) == 1 && LastElem(5, 0x98212984) && LastElem(5, 0x98212984)->a == 0x39e82599 &&
              LastElem(3, 0x65c8b901) && LastElem(3, 0x65c8b901)->a == 0x7f88f1b1 && ElemCalls(6, 0x75d34984) == 1 &&
              LastElem(7, 0xfff27543) && LastElem(7, 0xfff27543)->a == 0xa56426e2 && LastElem(7, 0xfff27543)->b == 0x3ecdb4b9,
              "frame: the CONNECTING spinner while waiting");
        v[9] = 0;
        d = Call(kNatFrame, t, v, 12);
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 4 && d.ret == kFrameNothing && ElemCalls(7, 0xfff27543) == 1, "frame: still waiting, retried each frame without the key");
        g_startResult = kStartOk;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 5 && d.ret == kFrameNothing, "frame: ready: the race goes out by itself");
        Check(ElemCalls(7, 0xfff27543) == 2 && LastElem(7, 0xfff27543)->a == 0x3ecdb4b9 && LastElem(7, 0xfff27543)->b == 0xa56426e2,
              "frame: the spinner fades out when the wait ends");
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 5, "frame: and the wait is over");
        // Start while a player is still on the last race's results: refused, no start by itself later (user, 2026-10-22).
        g_startResult = kStartPlayerInRace;
        v[9] = 1;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 6 && d.ret == kFrameStartNotReady, "frame: Start with a player still in the last race: the \"not all players are ready\" dialog");
        v[9] = 0;
        g_startResult = kStartOk;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 6 && d.ret == kFrameNothing, "frame: and no start by itself when they're back");
        g_starts = 5;
        g_startResult = kStartNetNotReady;  // waiting, then the racer popup opens: the wait ends
        v[9] = 1;
        g_elemCount = 0;
        d = Call(kNatFrame, t, v, 12);
        v[9] = 0;
        v[1] = 1;
        d = Call(kNatFrame, t, v, 12);
        v[1] = 0;
        d = Call(kNatFrame, t, v, 12);
        Check(g_starts == 6 && d.ret == kFrameNothing, "frame: a lobby change ends the wait");
        Check(ElemCalls(7, 0xfff27543) == 2 && LastElem(7, 0xfff27543)->b == 0xa56426e2, "frame: and fades the spinner out");
        g_startResult = kStartOk;
        g_poll = kRaceStart;
        g_contract.roster[1] = 0xabcdef01;
        int posts = g_posts;
        d = Call(kNatFrame, t, v, 12);
        Check(d.ret == kFrameStartRace, "frame: an armed race is started by the script");
        Check(g_posts == posts + 1 && g_lastKind == kCmdSetLocal && g_lastArgs[0] == 5 && g_lastArgs[1] == 0,
              "frame: not ready while racing (the next race can't start before everyone is back)");
        uint8_t t2[2] = {2, 2};
        uint32_t v2[2] = {1, 0};
        d = Call(kNatRaceSlot, t2, v2, 2);
        Check(d.ret == 0xabcdef01 && d.retType == 3, "race slot: character as a hash");
        v2[1] = 1;
        d = Call(kNatRaceSlot, t2, v2, 2);
        Check((int32_t)d.ret == 2 && d.retType == 2, "race slot: our pad at our index");
        v2[0] = 0;
        d = Call(kNatRaceSlot, t2, v2, 2);
        Check((int32_t)d.ret == -1, "race slot: -1 (AI) elsewhere");
        d = Call(kNatRaceCourse, nullptr, nullptr, 0);
        Check(d.retType == 6 && strcmp((const char*)(uintptr_t)d.ret, "seasidehill_easy") == 0, "race course: name string");
        uint8_t t1[1] = {2};
        uint32_t v1[1] = {1};
        Call(kNatRaceLaunched, t1, v1, 1);
        Check(g_launched == 1, "race launched reported");
        g_poll = kRaceNone;
        t[10] = 3;  // native 0400 gives hash 0 before a pad is assigned
        v[10] = 0;
        posts = g_posts;
        d = Call(kNatFrame, t, v, 12);
        Check(d.retType == 2 && d.ret == kFrameNothing, "frame: a hash pad number is accepted");
        Check(g_posts == posts + 1 && g_lastKind == kCmdSetLocal && g_lastArgs[0] == 5 && g_lastArgs[1] == 1,
              "frame: back in the lobby after the race: ready again");
        v[11] = 1;  // a new lobby: no racer picked here yet
        d = Call(kNatFrame, t, v, 12);
        Check(g_lastKind == kCmdSetLocal && (int32_t)g_lastArgs[0] == -1 && g_lastArgs[1] == 0, "frame: choosing = no racer, not ready");
        v[11] = 0;
        d = Call(kNatFrame, t, v, 12);
        Check(g_lastKind == kCmdSetLocal && g_lastArgs[0] == 5 && g_lastArgs[1] == 1, "frame: picked: the racer, ready");
        // The select key (F2, arg 12) without a cursor on another player's bar does nothing here (the script's CHANGE RACER);
        // INVITE FRIENDS is the script's own (kNatInvite). A guest's Enter does nothing.
        uint8_t t13[13];
        uint32_t v13[13];
        memcpy(t13, t, sizeof(t));
        memcpy(v13, v, sizeof(v));
        t13[12] = 2;
        v13[12] = 1;
        int starts = g_starts;
        g_lastKind = -1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameNothing && g_lastKind != kCmdInvite && g_starts == starts, "frame: the host's F2 alone: nothing (CHANGE RACER is the script's)");
        v13[12] = 0;
        v13[9] = 1;
        g_lastKind = -1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(g_starts == starts + 1 && g_lastKind != kCmdInvite, "frame: host's Enter starts");
        v13[9] = 0;
        v13[12] = 1;
        v13[8] = 1;  // Settings open
        g_lastKind = -1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameNothing && g_lastKind != kCmdInvite, "frame: F2 with Settings open: nothing");
        v13[8] = 0;
        g_fake.isHost = 0;
        g_lastKind = -1;
        v13[12] = 0;
        v13[9] = 1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameNothing && g_lastKind != kCmdInvite && g_starts == starts + 1, "frame: a guest's Enter does nothing");
        v13[9] = 0;
        v13[12] = 1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameNothing && g_lastKind != kCmdInvite && g_starts == starts + 1, "frame: a guest's F2 alone: nothing");
        v13[1] = 1;  // racer popup open
        g_lastKind = -1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameNothing && g_lastKind != kCmdInvite, "frame: nothing with the racer popup open");
        // A Steam join waiting: the lobby frame hands it to the script (not while a popup is open).
        v13[1] = 0;
        g_fake.pendingJoin = 0x0186000012345678ull;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret == kFrameJoin, "frame: a Steam join waits: leave this lobby for it");
        v13[8] = 1;
        d = Call(kNatFrame, t13, v13, 13);
        Check(d.ret != kFrameJoin, "frame: not with Settings open");
        v13[8] = 0;
        g_fake.pendingJoin = 0;
        g_fake.isHost = 1;
        {
            // The lobby cursor (arg 13; the Mac's keys): F2 (arg 12) with the cursor on another player's bar opens the popup (host and
            // guest); on this player's bar, an empty one or none it does nothing here; a test player's bar refuses; the host's Enter
            // starts whatever the cursor.
            uint8_t t14[14];
            uint32_t v14[14];
            memcpy(t14, t13, sizeof(t13));
            memcpy(v14, v13, sizeof(v13));
            t14[13] = 2;
            v14[12] = 1;  // F2
            v14[9] = 0;
            v14[13] = 2;  // bar 3: another player
            g_fake.members[2].id = 0x0110000112345678ull;
            int before = g_starts;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopup && g_starts == before, "cursor: F2 on another player's bar opens the popup, no start");
            // A muted player's popup says UNMUTE (M8), only when the dialogs text reads it (our text tables); with another mod's
            // table or a placeholder language's the MUTE popup (which toggles back too).
            g_fake.members[2].muted = 1;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopupMuted && g_starts == before, "muted: the UNMUTE popup");
            g_unmuteText = "NOTHING";
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopup, "muted, the stock dialogs text: the MUTE popup");
            g_unmuteText = nullptr;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopup, "muted, no dialogs text: the MUTE popup");
            g_unmuteText = "TON AN";
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopupMuted, "muted, German: the UNMUTE popup");
            g_unmuteText = "UNMUTE";
            g_fake.members[2].muted = 0;
            v14[13] = 1;  // this player's own bar: nothing here (the script's CHANGE RACER)
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFrameNothing && g_starts == before, "cursor: F2 on your own bar: nothing here");
            v14[13] = 5;  // no such bar: as no cursor
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFrameNothing && g_starts == before, "cursor: an empty bar is no cursor: F2 nothing");
            g_fake.members[2].id = kTestPlayerBase | 1;
            v14[13] = 2;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFrameStartRefused && g_starts == before, "cursor: a test player's bar: no popup");
            g_fake.members[2].id = 0x0110000112345678ull;
            v14[12] = 0;
            v14[9] = 1;   // Enter: START, with the cursor on another player's bar too (the Mac's START key)
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret != kFramePopup && g_starts == before + 1, "cursor: Enter with the cursor on another player's bar starts");
            v14[13] = (uint32_t)-1;
            d = Call(kNatFrame, t14, v14, 14);
            Check(g_starts == before + 2, "cursor: none: Enter starts");
            before = g_starts;
            v14[9] = 0;
            v14[12] = 1;
            g_fake.isHost = 0;
            v14[13] = 2;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopup && g_starts == before, "cursor: a guest's F2 on another player's bar opens the popup too");
            g_fake.isHost = 1;
            // The kick itself goes by the player the question was about (its Steam id), not by the bar: a bar that shifted while
            // the question was up (a player left) can't kick another player; without a question nothing is kicked; one question,
            // one kick; a guest never kicks.
            uint8_t kt[1] = {2};
            uint32_t kv[1] = {2};
            g_posts = 0;
            v14[13] = (uint32_t)-1;  // the cursor left the player: the popup's question is void
            v14[9] = 0;
            Call(kNatFrame, t14, v14, 14);
            v14[9] = 1;
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 0, "kick: nothing without a question");
            v14[13] = 2;
            d = Call(kNatFrame, t14, v14, 14);
            Check(d.ret == kFramePopup, "kick: the popup about bar 3's player");
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 1 && g_posts == 1 && g_lastKind == kCmdKick && (uint32_t)g_lastArgs[0] == 0x12345678u &&
                  (uint32_t)g_lastArgs[1] == 0x01100001u, "kick: the player's Steam id posted");
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "kick: one question, one kick");
            Call(kNatFrame, t14, v14, 14);
            g_fake.members[2].id = 0x0110000112345679ull;  // that player left; another has bar 3 now
            d = Call(kNatMemberProfile, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "view profile: the popup's player left, another has the bar: nothing");
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "kick: the player asked about left: nobody kicked");
            g_fake.members[2].id = 0x0110000112345678ull;
            Call(kNatFrame, t14, v14, 14);
            g_fake.memberCount = 2;  // the player left and nobody took the bar
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "kick: the player asked about left (fewer bars): nobody kicked");
            g_fake.memberCount = 3;
            Call(kNatFrame, t14, v14, 14);
            {   // VIEW STEAM PROFILE goes to the player the popup was about, even when the bars shifted meanwhile (bar 3 -> bar 1).
                uint64_t host = g_fake.members[0].id;
                g_fake.members[0].id = 0x0110000112345678ull;
                g_fake.members[2].id = 0x0110000112345679ull;
                g_fake.memberCount = 4;
                g_fake.members[3].id = host;
                d = Call(kNatMemberProfile, kt, kv, 1);
                Check(d.ret == 1 && g_posts == 2 && g_lastKind == kCmdProfile && (uint32_t)g_lastArgs[0] == 0x12345678u, "view profile: the popup's player, not the bar");
                g_fake.members[0].id = host;
                g_fake.members[2].id = 0x0110000112345678ull;
                g_fake.members[3].id = 0;
                g_fake.memberCount = 3;
                g_posts = 1;  // (the kick checks below count their own posts from here)
            }
            v14[13] = (uint32_t)-1;
            v14[9] = 0;
            Call(kNatFrame, t14, v14, 14);  // the selection ended before any answer: the question is void
            v14[9] = 1;
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "kick: the selection ended: no kick");
            v14[13] = 2;
            Call(kNatFrame, t14, v14, 14);
            g_fake.isHost = 0;
            d = Call(kNatKick, kt, kv, 1);
            Check(d.ret == 0 && g_posts == 1, "kick: not by a guest");
            g_fake.isHost = 1;
            Check(!IsSessionCommand(kCmdKick), "kick: not a session command");
            // The bars' names carry no mark (the cursor is the red bar, drawn by the script).
            uint8_t mt[2] = {2, 7};
            uint32_t mv[2] = {2, (uint32_t)(uintptr_t)&b};
            d = Call(kNatMemberText, mt, mv, 2);
            Check(d.ret == 1 && strcmp(storage, "Bravo 2") == 0, "cursor: the bar's plain name");
        }
        Call(kNatLeave, nullptr, nullptr, 0);
        Check(g_cancels == 1, "leave cancels an arranged race");
    }
    {   // The invite key (INVITE FRIENDS / VIEW PROFILE): the configured key's press edge (A by default; the key state is faked).
        NativesTestSetKeyState(&FakeKeyState);
        g_fakeKeyDown = 0;
        Desc d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 0 && d.retType == 2, "invite key: not pressed");
        g_fakeKeyDown = 'A';
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 1, "invite key: A pressed");
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 0, "invite key: still held: no second press");
        g_fakeKeyDown = 0;
        Call(kNatInviteKey, nullptr, nullptr, 0);
        g_fakeKeyDown = 'A';
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 1, "invite key: pressed again after a release");
        g_fakeKeyDown = VK_F2;
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 0, "invite key: another key is not it");
        g_fakeKeyDown = 0;
        Call(kNatInviteKey, nullptr, nullptr, 0);
        g_overlay = 1;  // the Steam overlay up: a typed "a" there is not the invite key
        g_fakeKeyDown = 'A';
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 0, "invite key: not while the Steam overlay is open");
        g_overlay = 0;
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 1, "invite key: pressed once the overlay closed (it went down meanwhile)");
        g_fakeKeyDown = 0;
        Call(kNatInviteKey, nullptr, nullptr, 0);
        Sleep(300);  // not read for a while (a dialog, a race): a key found down at the next reading is a baseline, not a press
        g_fakeKeyDown = 'A';
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 0, "invite key: held since before a long gap: no press");
        g_fakeKeyDown = 0;
        Call(kNatInviteKey, nullptr, nullptr, 0);
        g_fakeKeyDown = 'A';
        d = Call(kNatInviteKey, nullptr, nullptr, 0);
        Check(d.ret == 1, "invite key: a fresh press after the gap counts");
        g_fakeKeyDown = 0;
    }
    // PRIVATE SLOTS: the public seats a public search allows (8 seats, 0-7 private).
    Check(PublicSeats("0,3,0,1,1,1") == 8, "public seats: no private slots");
    Check(PublicSeats("0,3,7,1,1,1") == 1, "public seats: 7 private = the host's seat only");
    Check(PublicSeats("0,3,6,1,1,1") == 2, "public seats: 6 private");
    Check(PublicSeats("") == 8 && PublicSeats(nullptr) == 8 && PublicSeats("0,3,9,1,1,1") == 8, "public seats: no or bad rules: no limit");
    d = Call(0x7f, nullptr, nullptr, 0);
    Check(d.retType == 0, "unknown op: no return");
    // The lobby's race bar (M7): a race running here that this game isn't in shows the bar with a marker per
    // racing member (by bar row) at its progress, with its racer's icon; it goes when the race ends or if it was ours.
    NativesTestSetBar((void*)&FakeShow, (void*)&FakeHide, (void*)&FakePose, (void*)&FakeArt);
    {
        uint8_t ft[12] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
        uint32_t fv[12] = {5, 0, 3, 3, 0, 1, 1, 1, 0, 0, 2, 0};
        g_fake.state = kLobbyInLobby;
        g_fake.memberCount = 3;
        g_fake.pendingJoin = 0;
        for (int i = 0; i < 3; ++i) { g_fake.members[i].place = -1; g_fake.members[i].raceId = 0; }
        g_elemCount = 0;
        Call(kNatFrame, ft, fv, 12);
        Check(g_elemCount == 0, "race bar: nothing while nobody races");
        g_fake.members[0].place = 0; g_fake.members[0].lap = 2; g_fake.members[0].progress = 500; g_fake.members[0].laps = 3; g_fake.members[0].raceId = 0x1234;
        g_fake.members[2].place = 1; g_fake.members[2].progress = 250; g_fake.members[2].laps = 3; g_fake.members[2].raceId = 0x1234;
        uint8_t it[2] = {2, 3};
        uint32_t iv[2] = {0, 0xabc001};
        Check(Call(kNatMemberIcon, it, iv, 2).ret == 1, "member icon: taken");
        iv[0] = 2; iv[1] = 0;  // the third member: no racer picked (no icon)
        Call(kNatMemberIcon, it, iv, 2);
        Call(kNatFrame, ft, fv, 12);
        const ElemCall* p0 = LastElem(3, 0x05d75801);
        const ElemCall* p2 = LastElem(3, 0xcbaddd74);
        Check(ElemCalls(1, 0xdd942497) == 1 && LastElem(3, 0xdd942497) && LastElem(3, 0xdd942497)->a == 0x02f0b24c, "race bar: shown and posed");
        Check(ElemCalls(2, 0xac2c9bf7) == 1 && ElemCalls(1, 0xac2c9bf7) == 0, "race bar: this player's marker hidden (not racing)");
        Check(ElemCalls(1, 0x05d75801) == 1 && p0 && p0->a == 0x3e3460a8 && p0->b == 0xf5fda6b0 && p0->t == 0.5f, "race bar: the host's marker at half way");
        Check(ElemCalls(4, 0xea88ea7d) == 1 && LastElem(4, 0xea88ea7d)->a == 0xabc001, "race bar: the host's head icon");
        Check(ElemCalls(1, 0xcbaddd74) == 1 && p2 && p2->t == 0.25f && ElemCalls(4, 0xbf672c37) == 0, "race bar: the third member's marker, no icon");
        int n = g_elemCount;
        g_fake.members[0].progress = 600;
        Call(kNatFrame, ft, fv, 12);
        Check(g_elemCount == n + 2 && LastElem(3, 0x05d75801)->t == 0.6f && ElemCalls(1, 0x05d75801) == 1 && ElemCalls(4, 0xea88ea7d) == 1, "race bar: only the poses move on");
        g_fake.members[2].place = -1;  // the third member finished / left the race
        Call(kNatFrame, ft, fv, 12);
        Check(ElemCalls(2, 0xcbaddd74) == 2, "race bar: a marker goes when its racer stops");
        g_lastRace = 0x1234;  // we were in that race (back from it): no bar
        Call(kNatFrame, ft, fv, 12);
        Check(ElemCalls(2, 0xdd942497) == 1 && ElemCalls(2, 0x05d75801) >= 2 && LastElem(3, 0x05d75801)->t == 0.0f, "race bar: hidden and reset once the race is ours or over");
        g_lastRace = 0;
        g_fake.members[0].place = -1;
        g_fake.members[0].raceId = 0;
        n = g_elemCount;
        Call(kNatFrame, ft, fv, 12);
        Check(g_elemCount == n, "race bar: stays hidden");
        // The status text in the bars: after 250 lobby frames the names give way to "1ST (LAP 2 OF 3)" / "WAITING IN
        // LOBBY" (the lobby's own text when it has one), after 250 more the names are back; the script's redraw (the
        // name native) always puts the name first.
        NativesTestSetSceneText((void*)&FakeSceneText);
        static char names[3][32];
        static Buffer bufs[3];
        for (int k = 0; k < 3; ++k) {
            bufs[k] = {0x6a71f8, 0, 32, names[k]};
            uint8_t tt[2] = {2, 7};
            uint32_t tv[2] = {(uint32_t)k, (uint32_t)(uintptr_t)&bufs[k]};
            Call(kNatMemberText, tt, tv, 2);
        }
        Check(strcmp(names[0], "Host") == 0 && strcmp(names[1], "Me") == 0, "status: the names first");
        g_fake.members[0].place = 0; g_fake.members[0].lap = 2; g_fake.members[0].laps = 3; g_fake.members[0].raceId = 0x5678;
        g_fake.members[2].place = 1; g_fake.members[2].lap = 0; g_fake.members[2].laps = 3; g_fake.members[2].raceId = 0x5678;  // (lap 0: before the line)
        for (int i = 0; i < 250; ++i) Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[0], "Host") == 0, "status: the names for the first 250 frames");
        Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[0], "1ST (LAP 2 OF 3)") == 0 && strcmp(names[1], "WAITING IN LOBBY") == 0 && strcmp(names[2], "2ND (LAP 1 OF 3)") == 0,
              "status: then the places and laps, WAITING IN LOBBY for this player");
        g_fake.members[0].lap = 3;
        Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[0], "1ST (LAP 2 OF 3)") == 0, "status: not rewritten each frame");
        for (int i = 0; i < 250; ++i) Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[0], "Host") == 0 && strcmp(names[1], "Me") == 0 && strcmp(names[2], "Bravo 2") == 0, "status: the names back after 250 more");
        g_lapFormat = "{0} (TOUR {1} SUR {2})";
        for (int i = 0; i < 250; ++i) Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[0], "1ST (TOUR 3 SUR 3)") == 0, "status: the lobby's own format");
        g_lapFormat = nullptr;
        {
            uint8_t tt[2] = {2, 7};
            uint32_t tv[2] = {0, (uint32_t)(uintptr_t)&bufs[0]};
            Call(kNatMemberText, tt, tv, 2);  // the script's redraw during the status half (a racer's progress update): the status stays
            Check(strcmp(names[0], "1ST (LAP 3 OF 3)") == 0, "status: a redraw during the status half writes the status");
            for (int i = 0; i < 250; ++i) Call(kNatFrame, ft, fv, 12);
            Check(strcmp(names[0], "Host") == 0, "status: the names again after it");
            for (int i = 0; i < 250; ++i) Call(kNatFrame, ft, fv, 12);
            Check(strcmp(names[0], "1ST (LAP 3 OF 3)") == 0, "status: and the status again");
            // A player whose race is over and who is on the results screen ("pos" flag): VIEWING RESULTS instead of its place.
            g_fake.members[2].results = 1;
            for (int i = 0; i < 500; ++i) Call(kNatFrame, ft, fv, 12);  // the names half, then the status half again
            Check(strcmp(names[2], "VIEWING RESULTS") == 0 && strcmp(names[0], "1ST (LAP 3 OF 3)") == 0, "status: VIEWING RESULTS for a player on the results");
            g_fake.members[2].results = 0;
            g_fake.members[0].place = -1; g_fake.members[0].raceId = 0;
            g_fake.members[2].place = -1; g_fake.members[2].raceId = 0;
            Call(kNatFrame, ft, fv, 12);  // the race ends: the bar goes, the names come back, a redraw writes names
            Call(kNatMemberText, tt, tv, 2);
            g_fake.members[0].place = 0; g_fake.members[0].raceId = 0x5678;
            g_fake.members[2].place = 1; g_fake.members[2].raceId = 0x5678;
        }
        Check(strcmp(names[0], "Host") == 0, "status: the redraw's name wins outside the status half");
        for (int i = 0; i < 260; ++i) Call(kNatFrame, ft, fv, 12);
        g_fake.members[2].muted = 1;  // muted, its bar not yellow (no redraw since): the name the status half gives back keeps "(muted)"
        g_fake.members[0].place = -1; g_fake.members[0].raceId = 0;
        g_fake.members[2].place = -1; g_fake.members[2].raceId = 0;
        Call(kNatFrame, ft, fv, 12);
        Check(strcmp(names[1], "Me") == 0 && strcmp(names[2], "Bravo 2 (muted)") == 0, "status: the names back when the race ends, (muted) kept");
        g_fake.members[2].muted = 0;
    }
    printf("natives tests: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
