// Offline test of the race bridge (src/race.cpp, RACE_TEST): the host's contract, the guest's
// arming, the racer-creation rewrite, both holds and the pose exchange, against fake game
// functions and memory. Nothing of the game is touched.
#include <windows.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "lobby.h"
#include "race.h"
#include "race_tables.h"
#include "script_abi.h"

void RaceTestSetOriginal(int hook, void* fn);
void* RaceTestHook(int hook);
void RaceTestSetAddresses(uintptr_t scene, uintptr_t courseGraph, uintptr_t seed, uintptr_t setState, uintptr_t setPosition, uintptr_t simTick,
                          uintptr_t racerTypeId);
void RaceTestSetLapAddresses(uintptr_t applyNav, uintptr_t records, uintptr_t finishCount, uintptr_t finishList, uintptr_t raceOver,
                             uintptr_t addFinish, uintptr_t startEndTimer, uintptr_t timerElapsed, uintptr_t timerLimit, uintptr_t tickRate);
int RaceTestPhase();
void* RaceTestRacer(int k);
bool RaceTestReturnFlag();
void RaceTestSetReturnFlag(bool on);
void RaceTestSetRulesAddresses(uintptr_t records, uintptr_t active);
void RaceTestSetHostAi(bool on);
void RaceTestSetMetalVtable(uint32_t (*fn)());
static uint32_t g_fakeMetalVtable = 0;  // what Metal Sonic's core publishes (0 = no Metal Sonic)
static uint32_t FakeMetalVtable() { return g_fakeMetalVtable; }
bool RaceTestAiPuppets();
void RaceTestSetClearItems(uintptr_t fn);
void RaceTestSetItems(bool on);
void RaceTestSetItemConfig(bool config, bool hooks);
void RaceTestSetItemAddresses(uintptr_t alloc, uintptr_t free, uintptr_t projectileType);
void RaceTestSetReactionAddress(uintptr_t play);
enum { kH0905, kH0900, kH090a, kHSetup, kHWaiting, kHWaitingNoIntro, kHTrackIntro, kHCharIntro, kHCountdown, kHRacing, kHRacerCommit, kHWff, kH0912,
       kH1b01, kH0916, kH2003, kH1304, kH0913, kHEventPost, kHRacerMessage, kHExhaust, kHCatchup,
       kHTakeSelect = kHCatchup + 3, kHTakeSource, kHSpeechUpdate, kHRacerTrack, kHRaceEnd, kHRacerUpdate, kHRacerListener, kHOpaStart, kHOpaInput,
       kHPulseSonic, kHPulseAiAi, kHPulseJoe, kHGateFirst };  // gate class c: input kHGateFirst + 2c, tick + 2c + 1
enum { kGateEggman, kGateShadow, kGateAlexKidd, kGateZobio, kGateRyo, kGateKnuckles, kGateAmy, kGateUlala, kGateBeat, kGateBonanza, kGateKindCount };  // race.cpp kGateClasses
enum { kIdle, kArmed, kCreating, kCreated, kSetUp, kHoldA, kIntros, kHoldB, kRunning, kFailed };

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// ---------------------------------------------------------------- fakes: log, lobby, network
void Log(bool, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    wprintf(L"    log: ");
    vwprintf(fmt, ap);
    wprintf(L"\n");
    va_end(ap);
}

static LobbySnapshot g_snap = {};
static uint64_t g_self = 0;
static std::vector<RaceNetMsg> g_sent, g_inbox;
static bool g_active = false;
static int g_sendFails = 0;  // the next sends fail (outgoing queue full)
bool LobbyPost(LobbyCommandKind, int32_t) { return true; }
static int g_progressPosts = 0;
static int32_t g_lastProgress[6];
static int g_testProgressFor = 0, g_testProgressClears = 0;  // test players' progress posted (bit per test player), cleared
static std::vector<uint64_t> g_profilePosts;  // VIEW PROFILE: the Steam ids asked for
bool LobbyPostArgs(LobbyCommandKind kind, const int32_t* args, int count) {
    if (kind == kCmdSetProgress && (count == 5 || count == 6)) {  // (6: with the "viewing results" flag)
        ++g_progressPosts;
        memset(g_lastProgress, 0, sizeof(g_lastProgress));
        memcpy(g_lastProgress, args, count * sizeof(int32_t));
    }
    if (kind == kCmdSetTestProgress && count == 6) {
        if (args[5] == 0) ++g_testProgressClears;
        else if (args[5] > 0 && args[5] < 8 && args[0] >= 0) g_testProgressFor |= 1 << args[5];
    }
    if (kind == kCmdProfile && count == 2) g_profilePosts.push_back((uint64_t)(uint32_t)args[0] | ((uint64_t)(uint32_t)args[1] << 32));
    return true;
}
void LobbyRead(LobbySnapshot& out) { out = g_snap; }
bool RaceNetSend(uint64_t to, const void* data, size_t size, bool reliable) {
    if (g_sendFails > 0) { --g_sendFails; return false; }
    RaceNetMsg m = {};
    m.peer = to;
    m.size = (uint32_t)size;
    m.reliable = reliable;
    memcpy(m.data, data, size);
    g_sent.push_back(m);
    return true;
}
int RaceNetReceive(RaceNetMsg* out, int max) {
    int n = 0;
    while (n < max && !g_inbox.empty()) {
        out[n++] = g_inbox.front();
        g_inbox.erase(g_inbox.begin());
    }
    return n;
}
void RaceNetActive(bool on) { g_active = on; }
uint64_t RaceNetSelf() { return g_self; }

static void Deliver(uint64_t from, const void* data, size_t size, bool reliable = true) {
    RaceNetMsg m = {};
    m.peer = from;
    m.size = (uint32_t)size;
    m.reliable = reliable;
    memcpy(m.data, data, size);
    g_inbox.push_back(m);
}

static int CountSent(WireType type) {
    int n = 0;
    for (const RaceNetMsg& m : g_sent) {
        WireType t;
        uint32_t r;
        if (WirePeek(m.data, m.size, t, r) && t == type) ++n;
    }
    return n;
}

// ---------------------------------------------------------------- fakes: game
static const uint64_t A = 76561198500000002ull, B = 76561198400000001ull;
static uint8_t g_scene[0x1400];  // +0x1374: the event manager
static void* g_scenePtr = g_scene;
static uint8_t g_racerMem[8][0x2200];
static uint8_t g_controllers[8][0x300];
// Course graph: +0x1c nav path count, +0x48 the path array (0x20 bytes each).
static uint8_t g_paths[12][0x20];
static uint8_t g_graph[0x100];
static void* g_courseGraph = g_graph;
// Lap records, finish list, race-over flag, as at 0xcf7a68 / 0xcf7a57 / 0xcf7950 / 0xcf7a6c.
static uint8_t g_records[8][0x90];
static void* g_recordsPtr = g_records;
static uint8_t g_finishCount = 0;
static uint8_t g_finishList[16][8];
static uint32_t g_raceOver = 0;
static uint32_t g_tickRate = 50;
struct NavCall { void* racer; uint32_t nav; };
static std::vector<NavCall> g_navCalls;
static std::vector<std::pair<uint32_t, uint32_t>> g_added;  // AddFinish(index, reason) calls
static int g_timerStarts = 0, g_timerSeconds = 0;
static uint32_t g_humansSeen = 0;  // scene+0x1104 during the last Racing / WaitingForFinishers update
static int g_localIdx = 0;         // which racer the fake race end counts as this game's human
static int g_finishLocalNow = 0;   // the next update puts our car in the finish list with this time
static int g_wffCalls = 0;
// The race-end timer (0xcf7a88: elapsed 0xcf7a8c, length 0xcf7a90), as 0x5b2250 / 0x4cdbf0 use it.
static uint32_t g_timerElapsed = 0, g_timerLimit = 0;
static bool g_timerOn = false;
static void TimerStart(uint32_t seconds) {
    g_timerOn = true;
    g_timerElapsed = 0;
    g_timerLimit = seconds * 50;
}

static uintptr_t PathAt(int i) { return (uintptr_t)g_paths[i]; }
static uint8_t* Tracker(int k) { return g_controllers[k] + 0x80; }
static void Append(uint8_t index, uint32_t reason) {
    g_finishList[g_finishCount][0] = index;
    *(uint32_t*)(g_finishList[g_finishCount] + 4) = reason;
    ++g_finishCount;
}
static bool InList(int index) {
    for (int k = 0; k < g_finishCount; ++k)
        if (g_finishList[k][0] == index) return true;
    return false;
}
static uint32_t* ListReason(int index) {
    for (int k = 0; k < g_finishCount; ++k)
        if (g_finishList[k][0] == index) return (uint32_t*)(g_finishList[k] + 4);
    return nullptr;
}
static int g_created = 0;
struct Seen { uint32_t character; char state[48]; int32_t device; };
static Seen g_seen[8];
static int g_seenLaps = -1;
static uint32_t g_seed = 0;
static int g_setStateCalls = 0;
static void* g_setStateController = nullptr;
static uint32_t g_setStateId = 0;
static int g_setupCalls = 0, g_waitingCalls = 0, g_countdownCalls = 0, g_racingCalls = 0, g_commitCalls = 0, g_teardowns = 0;
static uint32_t g_tick = 5000;
static uint32_t g_racerTypeId = 0x13;  // registered at start-up in the game; any id >= 0x10

// Each controller's top state object (controller +0x228; the state id at +4), as 0x507f00 leaves it once
// the new state has entered.
static uint8_t g_states[8][16];
static void EnterQueuedStates();
static void SetTopState(int k, uint32_t id) {
    *(void**)(g_controllers[k] + 0x228) = g_states[k];
    *(uint32_t*)(g_states[k] + 4) = id;
}
static void __cdecl Fake0905(void*, NativeCall* d) {
    int k = g_created++;
    g_seen[k].character = d->values[0];
    strcpy_s(g_seen[k].state, (const char*)(uintptr_t)d->values[1]);
    g_seen[k].device = (int32_t)d->values[2];
    *(void**)(g_racerMem[k] + 0x9f0) = g_controllers[k];
    SetTopState(k, strstr(g_seen[k].state, "GridHuman") ? 0x17 : 0x18);
    *(int32_t*)(g_racerMem[k] + 0xa1c) = 2;  // active
    *(uint32_t*)(g_racerMem[k] + 0x9e4) = strstr(g_seen[k].state, "GridHuman") ? 0x17 : 0x18;
    *(int32_t*)(g_racerMem[k] + 0x9d8) = k;                 // lap record = creation index (0x49fa2e)
    *(void**)(g_records[k] + 0x8c) = g_racerMem[k];
    *(uint32_t*)(g_records[k] + 0x84) = 4;                  // 3 laps: the start crossing + 3
    d->retValue = (uint32_t)(uintptr_t)g_racerMem[k];
    d->retType = g_racerTypeId;  // like 0x58cb00: the racer's registered type id from [0xd21dc0], not type 7
}
static void __cdecl Fake0900(void*, NativeCall* d) { g_seenLaps = (int)d->values[2]; }
static void __cdecl Fake090a(void*, NativeCall*) { ++g_teardowns; }
static uint32_t g_gridSortSaw[8];
static void __fastcall FakeSetup(void*, void*) {
    ++g_setupCalls;
    for (int k = 0; k < 8; ++k) g_gridSortSaw[k] = *(uint32_t*)(g_racerMem[k] + 0x9e4);
}
static void __fastcall FakeWaiting(void*, void*) { ++g_waitingCalls; EnterQueuedStates(); }
static void __fastcall FakeCountdown(void*, void*) { ++g_countdownCalls; EnterQueuedStates(); }
// The race end as 0x4cdbf0 decides it: over once the finished humans of this game reach scene+0x1104.
static void FakeRaceTick() {
    g_humansSeen = *(uint32_t*)(g_scene + 0x1104);
    if (g_finishLocalNow && !InList(g_localIdx)) {
        *(uint32_t*)(g_records[g_localIdx] + 0x68) = (uint32_t)g_finishLocalNow;
        *(uint32_t*)(g_records[g_localIdx] + 0x60) = 0x1000;
        uint32_t* at = (uint32_t*)(g_records[g_localIdx] + 0x84);  // a real finish counts the last crossing (+0x74 = +0x84)
        if (!*at) *at = 4;
        *(uint32_t*)(g_records[g_localIdx] + 0x74) = *at;
        Append((uint8_t)g_localIdx, 1);
        g_finishLocalNow = 0;
    }
    uint32_t finished = InList(g_localIdx) ? 1 : 0;
    bool timeUp = g_timerOn && g_timerElapsed >= g_timerLimit;
    if (timeUp || (g_humansSeen && g_humansSeen == finished)) {
        for (int k = 0; k < (g_scene[0x1100] ? g_scene[0x1100] : 8); ++k)  // 0x4cd290: every unfinished car gets an estimate
            if (!InList(k)) {
                *(uint32_t*)(g_records[k] + 0x68) = k < 2 ? 0x999999 : 0xa00000 + k * 0x1000;
                Append((uint8_t)k, 6);
            }
        g_raceOver = 1;
    }
}
static void __fastcall FakeRacing(void*, void*) { ++g_racingCalls; EnterQueuedStates(); FakeRaceTick(); }
// The final sort (0x4cc790): the finish list by time, places into +0x80. Not stable: with
// g_tiesReversed, equal times come out in the opposite order of the list's.
static bool g_sortedFake = false, g_tiesReversed = false;
static void FakeSort() {
    int n = g_finishCount;
    for (int i = 1; i < n; ++i)
        for (int j = i; j > 0; --j) {
            uint32_t a = *(uint32_t*)(g_records[g_finishList[j - 1][0]] + 0x68), b = *(uint32_t*)(g_records[g_finishList[j][0]] + 0x68);
            if (a > b || (a == b && g_tiesReversed)) {
                uint8_t t[8];
                memcpy(t, g_finishList[j], 8);
                memcpy(g_finishList[j], g_finishList[j - 1], 8);
                memcpy(g_finishList[j - 1], t, 8);
            } else {
                break;
            }
        }
    for (int i = 0; i < n; ++i) *(int32_t*)(g_records[g_finishList[i][0]] + 0x80) = i;
    g_sortedFake = true;
}
// WaitingForFinishers: 0x5b2250 (only while more than one human: start the timer when our human has
// finished, or count it down), then the lap chain and race end, and the sort in the tick it ends.
static void __fastcall FakeWff(void* self, void*) {
    ++g_wffCalls;
    EnterQueuedStates();
    if (*(uint32_t*)(g_scene + 0x1104) > 1 && self) {
        int32_t* state = (int32_t*)((uint8_t*)self + 0x10);
        if (*state != -1) ++g_timerElapsed;
        else if (InList(g_localIdx)) { TimerStart(30); *state = 0; }
    }
    FakeRaceTick();
    if (g_raceOver && !g_sortedFake) FakeSort();
}
static void __fastcall FakeApplyNav(void* racer, void*, uint32_t nav) {
    g_navCalls.push_back({racer, nav});
    int k = (int)(((uint8_t*)racer - g_racerMem[0]) / sizeof(g_racerMem[0]));
    *(uintptr_t*)(Tracker(k) + 0xac) = PathAt((int)nav);
}
static void __cdecl FakeAddFinish(uint32_t index, uint32_t reason) {
    g_added.push_back({index, reason});
    if (!InList((int)index)) Append((uint8_t)index, reason);
}
// 0912(position) -> results row; the row builder reads each car's human number (+0x9dc) once.
static int32_t g_rowSaw[2] = {-1, -1};
static int g_rowCalls = 0;
static void __cdecl Fake0912(void*, NativeCall*) {
    ++g_rowCalls;
    for (int k = 0; k < 2; ++k) g_rowSaw[k] = *(int32_t*)(g_racerMem[k] + 0x9dc);
}
static void __cdecl FakeStartEndTimer(uint32_t seconds) { ++g_timerStarts; g_timerSeconds = (int)seconds; TimerStart(seconds); }
static void __fastcall FakeCommit(void*, void*, int, void*) { ++g_commitCalls; }
static void __fastcall FakeSetPosition(void* racer, void*, const float* pos4) {
    memcpy((uint8_t*)racer + 0xc0, pos4, 16);
    ((uint8_t*)racer)[0x1e0] = 1;
}
static void __fastcall FakeOther(void*, void*) { EnterQueuedStates(); }
static void __stdcall FakeSeed(uint32_t s) { g_seed = s; }
// 0x507f00 as in the game: it pops every state (top state slot 0) and queues the new one, which enters
// at the controller's next update (here: the next stock phase update, EnterQueuedStates).
static std::vector<std::pair<void*, uint32_t>> g_stateCalls;
static uint32_t g_queuedState[8];
static void __fastcall FakeSetState(void* controller, void*, uint32_t id) {
    ++g_setStateCalls;
    g_setStateController = controller;
    g_setStateId = id;
    g_stateCalls.push_back({controller, id});
    for (int k = 0; k < 8; ++k)
        if (controller == g_controllers[k]) {
            *(void**)(g_controllers[k] + 0x228) = nullptr;
            g_queuedState[k] = id;
        }
}
static void EnterQueuedStates() {
    for (int k = 0; k < 8; ++k)
        if (g_queuedState[k]) {
            SetTopState(k, g_queuedState[k]);
            g_queuedState[k] = 0;
        }
}
// 0x537880(racer, 1): the racer's item clear.
static std::vector<void*> g_itemClears;
static void __fastcall FakeClearItems(void* racer, void*, int) {
    g_itemClears.push_back(racer);
    *(uint32_t*)((uint8_t*)racer + 0xc1c) = 0;
}

typedef void(__cdecl* NativeHook)(void*, NativeCall*);
typedef void(__fastcall* UpdateHook)(void*, void*);
typedef void(__fastcall* CommitHook)(void*, void*, int, void*);
static NativeHook NHook(int i) { return (NativeHook)RaceTestHook(i); }
static UpdateHook UHook(int i) { return (UpdateHook)RaceTestHook(i); }
static void Commit(void* racer) { ((CommitHook)RaceTestHook(kHRacerCommit))(racer, nullptr, 1, nullptr); }

// One script call of 0905(character, stateName, device).
static void Script0905(uint32_t character, const char* state, int32_t device) {
    uint8_t types[3] = {kTypeHash, kTypeLiteralString, kTypeHash};  // as the bridge pushes them
    uint32_t values[3] = {character, (uint32_t)(uintptr_t)state, (uint32_t)device};
    NativeCall d = {0x0905, 1, {}, types, values, 3, 0, 0};
    NHook(kH0905)(nullptr, &d);
}

static void Reset() {
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);  // ends whatever race is in hand
    g_sent.clear();
    g_inbox.clear();
    g_created = 0;
    memset(g_seen, 0, sizeof(g_seen));
    memset(g_racerMem, 0, sizeof(g_racerMem));
    memset(g_controllers, 0, sizeof(g_controllers));
    memset(g_records, 0, sizeof(g_records));
    memset(g_finishList, 0, sizeof(g_finishList));
    g_finishCount = 0;
    g_raceOver = 0;
    g_navCalls.clear();
    g_added.clear();
    g_timerStarts = g_timerSeconds = 0;
    g_humansSeen = 0;
    g_finishLocalNow = 0;
    g_wffCalls = 0;
    g_timerOn = false;
    g_timerElapsed = g_timerLimit = 0;
    g_sendFails = 0;
    *(uint32_t*)(g_scene + 0x1104) = 1;  // this game's one human
    *(uint32_t*)(g_graph + 0x1c) = 12;
    *(uintptr_t*)(g_graph + 0x48) = PathAt(0);
    g_seenLaps = -1;
    g_seed = 0;
    g_setStateCalls = g_setupCalls = g_waitingCalls = g_countdownCalls = g_racingCalls = g_commitCalls = 0;
    g_stateCalls.clear();
    g_itemClears.clear();
    memset(g_states, 0, sizeof(g_states));
    memset(g_queuedState, 0, sizeof(g_queuedState));
    g_sortedFake = g_tiesReversed = false;
}

static void Lobby(uint64_t self, bool host, int racerA, int racerB) {
    g_self = self;
    g_snap = LobbySnapshot{};
    g_snap.state = kLobbyInLobby;
    g_snap.isHost = host;
    g_snap.memberCount = 2;
    g_snap.members[0] = {A, "Alfa", 1, self == A, racerA, 1, 1, -1};
    g_snap.members[1] = {B, "Bravo 2", 0, self == B, racerB, 1, 1, -1};
    g_snap.rules = {0, 3, 0, 1, 1, 1};
}

// ---------------------------------------------------------------- tests
static RaceContract TestHostStart() {
    printf("host start\n");
    Reset();
    RaceUseRacerList(kRacerListRoster);
    Lobby(A, true, 0, 1);  // Sonic, Tails
    RaceContract c = {};
    CHECK(RaceHostStart() == kStartOk);
    CHECK(g_sent.size() == 1 && g_sent[0].reliable && g_sent[0].peer == 0);
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    CHECK(c.humanCount == 2 && c.humans[0] == A && c.humans[1] == B);
    CHECK(c.roster[0] == kRacersRoster[0].hash && c.roster[1] == kRacersRoster[1].hash);
    for (int k = 2; k < 8; ++k)
        for (const RacerEntry& r : kRacersRoster)
            if (r.hash == c.roster[k]) CHECK(r.ai);  // never a NO_AI racer as AI
    CHECK(c.course == 0 && c.courseHash == kCourses[0].hash && c.laps == 3);
    CHECK(RaceMenuPoll() == kRaceStart && RaceLocalIndex() == 0 && RaceContractArmed());
    CHECK(g_active);
    CHECK(RaceHostStart() == kStartBusy);
    // refusals
    Reset();
    Lobby(A, true, 0, 0);  // both players on racer 0: allowed (the user's choice, 2026-10-23); the AI never takes it
    CHECK(RaceHostStart() == kStartOk);
    {
        RaceContract sc = {};
        CHECK(!g_sent.empty() && DecodeContract(g_sent.back().data, g_sent.back().size, sc));
        CHECK(sc.roster[0] == sc.roster[1] && sc.roster[0] == kRacersRoster[0].hash);
        for (int k = 2; k < kGridSlots; ++k) CHECK(sc.roster[k] != sc.roster[0]);
    }
    Reset();
    Lobby(A, true, 0, 22);  // a per-player-appearance racer in the 25-racer list
    CHECK(RaceHostStart() == kStartRacerNotOnline);
    Lobby(A, true, 0, 1);
    g_snap.members[1].ready = 0;
    CHECK(RaceHostStart() == kStartNotReady);
    Lobby(A, true, 0, 1);
    g_snap.members[1].net = 0;  // the guest's game has no Steam certificate yet: the contract couldn't reach it
    CHECK(RaceHostStart() == kStartNetNotReady);
    Lobby(A, true, 0, 1);
    g_snap.members[0].net = 0;
    CHECK(RaceHostStart() == kStartNetNotReady);
    Lobby(A, true, 0, 1);
    g_snap.members[1].place = 2;  // the guest is still on the last race's results ("pos" set): the menu waits
    CHECK(RaceHostStart() == kStartPlayerInRace);
    g_snap.members[0].place = 0;  // (the host's own "pos" doesn't count; the guest is back)
    g_snap.members[1].place = -1;
    CHECK(RaceHostStart() == kStartOk);
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.memberCount = 1;
    CHECK(RaceHostStart() == kStartAlone);
    Lobby(B, false, 0, 1);
    CHECK(RaceHostStart() == kStartNotHost);
    CHECK(g_sent.empty());
    return c;
}

static void TestGuestRace(const RaceContract& c) {
    printf("guest race\n");
    Reset();
    Lobby(B, false, 0, 1);
    uint8_t buf[kWireMax];
    size_t n = EncodeContract(c, buf, sizeof(buf));
    Deliver(B + 1, buf, n);  // not the host: ignored
    CHECK(RaceMenuPoll() == kRaceNone);
    Deliver(A, buf, n);
    CHECK(RaceMenuPoll() == kRaceStart && RaceLocalIndex() == 1);
    RaceLaunched(true);
    CHECK(RaceMenuPoll() == kRaceNone && RaceTestPhase() == kArmed);

    // The bridge's 8 calls: our human at creation index 1, AI everywhere else.
    Script0905(0x11111111, "STATE_Control_StartingGridAI", 0);
    Script0905(c.roster[1], "STATE_Control_StartingGridHuman", 2);
    for (int k = 2; k < 8; ++k) Script0905(0x22222222 + k, "STATE_Control_StartingGridAI", 0);
    CHECK(RaceTestPhase() == kCreated);
    CHECK(g_seen[0].character == c.roster[0] && !strcmp(g_seen[0].state, "STATE_Control_StartingGridAI") && g_seen[0].device == 15);
    CHECK(*(uint32_t*)(g_racerMem[0] + 0x9e4) == 0x18);  // an AI racer until the grid sort
    CHECK(g_seen[1].character == c.roster[1] && !strcmp(g_seen[1].state, "STATE_Control_StartingGridHuman") && g_seen[1].device == 2);
    for (int k = 2; k < 8; ++k) CHECK(g_seen[k].character == c.roster[k] && !strcmp(g_seen[k].state, "STATE_Control_StartingGridAI"));

    uint8_t types[4] = {kTypeHash, kTypeHash, kTypeHash, kTypeHash};  // as the bridge pushes them
    uint32_t values[4] = {1, 0, 7, 0};
    NativeCall d = {0x0900, 1, {}, types, values, 4, 0, 0};
    NHook(kH0900)(nullptr, &d);
    CHECK(g_seenLaps == c.laps);

    UHook(kHSetup)(nullptr, nullptr);
    CHECK(g_setupCalls == 1 && g_seed == c.seed && RaceTestPhase() == kSetUp);
    CHECK(g_gridSortSaw[0] == 0x17 && g_gridSortSaw[1] == 0x17 && g_gridSortSaw[2] == 0x18);  // humans sorted like humans
    CHECK(*(uint32_t*)(g_racerMem[0] + 0x9e4) == 0x18);  // and back to AI afterwards

    // Hold A: the remote car becomes a network car; held until the host's Go.
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(g_waitingCalls == 0 && RaceTestPhase() == kHoldA);
    CHECK(g_setStateCalls == 1 && g_setStateController == g_controllers[0] && g_setStateId == 0x15);
    CHECK(*(uint32_t*)(g_racerMem[0] + 0x9e4) == 0x15);
    CHECK(CountSent(kMsgLoaded) == 1);
    for (int i = 0; i < 5; ++i) UHook(kHWaiting)(nullptr, nullptr);
    CHECK(g_waitingCalls == 0);
    n = EncodeGo(c.race, kStageLoaded, buf, sizeof(buf));
    Deliver(B, buf, n);  // a Go from a guest is not the host's
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(g_waitingCalls == 0);
    Deliver(A, buf, n);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(g_waitingCalls == 1 && RaceTestPhase() == kIntros);

    // Hold B
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(g_countdownCalls == 0 && RaceTestPhase() == kHoldB && CountSent(kMsgLoaded) == 2);
    n = EncodeGo(c.race, kStageCountdown, buf, sizeof(buf));
    Deliver(A, buf, n);
    UHook(kHCountdown)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(g_countdownCalls == 2 && RaceTestPhase() == kRunning);

    // Poses: ours go out at our car's end of tick; the host's car is played back onto racer 0 just
    // before its end of tick. Racer 0 is the remote, racer 1 ours.
    size_t before = g_sent.size();
    for (int k = 0; k < 2; ++k) {  // like the game: position w = 1, a unit rotation
        *(float*)(g_racerMem[k] + 0xcc) = 1.0f;
        *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    }
    for (uint32_t t = 0; t < 40; ++t) {
        ++g_tick;
        RacePose p = {};
        p.race = c.race;
        p.slot = 0;
        p.tick = 1000 + t;
        p.pos[0] = (float)t;
        p.rot[3] = 1;
        p.vel[0] = 50;
        uint8_t pb[kWireMax];
        Deliver(A, pb, EncodePose(p, pb, sizeof(pb)), false);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    CHECK(g_racingCalls == 40 && g_commitCalls == 320);
    int poses = 0;
    for (size_t i = before; i < g_sent.size(); ++i) {
        RacePose p;
        if (DecodePose(g_sent[i].data, g_sent[i].size, p)) { ++poses; CHECK(p.slot == 1 && !g_sent[i].reliable && p.tick > 5000); }
    }
    CHECK(poses == 40);
    float x = *(float*)(g_racerMem[0] + 0xc0);
    printf("  remote car x after 40 ticks: %.2f\n", x);
    CHECK(x > 25 && x < 39);  // a few ticks behind the newest pose (39)
    CHECK(*(float*)(g_racerMem[0] + 0xcc) == 1.0f && *(float*)(g_racerMem[0] + 0x100) == 50.0f && g_racerMem[0][0x1e0] == 1);
    CHECK(*(float*)(g_racerMem[2] + 0xc0) == 0);  // AI cars are left alone
    *(int32_t*)(g_racerMem[0] + 0xa1c) = 1;  // not active (e.g. being respawned): left alone
    *(float*)(g_racerMem[0] + 0xc0) = -7;
    Commit(g_racerMem[0]);
    CHECK(*(float*)(g_racerMem[0] + 0xc0) == -7);

    NativeCall td = {};
    NHook(kH090a)(nullptr, &td);
    CHECK(RaceTestPhase() == kIdle && !g_active && CountSent(kMsgAbort) == 1);
}

static void TestHostHolds(RaceContract c) {
    printf("host holds\n");
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract sent;
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, sent));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);  // the host is creation index 0
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    CHECK(RaceTestPhase() == kCreated && !strcmp(g_seen[1].state, "STATE_Control_StartingGridAI") && g_seen[1].device == 15);
    UHook(kHSetup)(nullptr, nullptr);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldA && CountSent(kMsgGo) == 0);
    uint8_t buf[kWireMax];
    size_t n = EncodeLoaded(sent.race, kStageLoaded, 1, buf, sizeof(buf));
    Deliver(B, buf, n);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 1);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(g_waitingCalls >= 1 && RaceTestPhase() == kIntros);
    // a late or repeated Loaded(1) is answered with Go(1) to that player
    size_t before = g_sent.size();
    Deliver(B, buf, n);
    UHook(kHCountdown)(nullptr, nullptr);  // the host reaches hold B (this tick also pumps)
    bool answered = false;
    for (size_t i = before; i < g_sent.size(); ++i) {
        uint32_t r;
        uint8_t stage;
        if (DecodeGo(g_sent[i].data, g_sent[i].size, r, stage) && stage == kStageLoaded && g_sent[i].peer == B) answered = true;
    }
    CHECK(answered);
    CHECK(RaceTestPhase() == kHoldB && g_countdownCalls == 0);
    n = EncodeLoaded(sent.race, kStageCountdown, 1, buf, sizeof(buf));
    Deliver(B, buf, n);
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 3 && RaceTestPhase() == kRunning && g_countdownCalls == 1);  // Go(1), Go(1) again, Go(2)
    (void)c;
}

static void TestHostPeerLeaves() {
    printf("host: a player leaves during hold A\n");
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldA && CountSent(kMsgGo) == 0);
    g_snap.memberCount = 1;  // the guest left the Steam lobby without a word
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 1 && RaceTestPhase() == kIntros);
}

static void TestStaleAndUnknown(const RaceContract& c) {
    printf("stale and unknown contracts\n");
    // Armed but never launched (the host pressed Back in the same frame): an offline race is untouched.
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    Script0905(0x77, "STATE_Control_StartingGridHuman", 4);
    CHECK(g_seen[0].character == 0x77);
    // Leaving the lobby cancels it.
    g_snap.state = kLobbyIdle;
    CHECK(RaceMenuPoll() == kRaceNone && RaceTestPhase() == kIdle && CountSent(kMsgAbort) == 1);
    // A contract naming a racer this game doesn't have is refused.
    Reset();
    Lobby(B, false, 0, 1);
    RaceContract bad = c;
    bad.roster[5] = 0x12345678;
    uint8_t buf[kWireMax];
    Deliver(A, buf, EncodeContract(bad, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceNone && RaceTestPhase() == kIdle);
    bad = c;  // a per-player-appearance racer as a human
    for (const RacerEntry& r : kRacersRoster)
        if (!strcmp(r.name, "mii")) bad.roster[1] = r.hash;
    Deliver(A, buf, EncodeContract(bad, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceNone);
}

static void TestMismatch(const RaceContract& c) {
    printf("mismatch\n");
    Reset();
    Lobby(B, false, 0, 1);
    uint8_t buf[kWireMax];
    Deliver(A, buf, EncodeContract(c, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    Script0905(0x5, "STATE_Control_StartingGridHuman", 2);  // the script made index 0 our human: not the contract
    CHECK(RaceTestPhase() == kFailed && CountSent(kMsgAbort) == 1);
    CHECK(g_seen[0].character == 0x5);  // passed through unchanged
    Script0905(0x6, "STATE_Control_StartingGridAI", 0);
    CHECK(g_seen[1].character == 0x6);
    CHECK(RaceMenuPoll() == kRaceNone && RaceTestPhase() == kIdle);  // back in the menus: over
}

// ---------------------------------------------------------------- laps and results (M4)
// Host A (creation index 0, this game) and guest B (index 1), up to the running race.
static RaceContract HostRunning(int32_t catchup = 1) {
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules.catchup = catchup;
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    uint8_t buf[kWireMax];
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(c.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(c.race, kStageCountdown, 1, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 2; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;  // a unit rotation
    g_localIdx = 0;
    g_sent.clear();
    return c;
}

static std::vector<RaceFinish> SentFinishes() {
    std::vector<RaceFinish> out;
    for (const RaceNetMsg& m : g_sent) {
        RaceFinish f;
        if (DecodeFinished(m.data, m.size, f)) { CHECK(m.reliable); out.push_back(f); }
    }
    return out;
}

static void TestNav() {
    printf("nav paths\n");
    RaceContract c = HostRunning();
    // Our car's pose carries its track cursor's nav path: the current one, else the last in range.
    *(uintptr_t*)(Tracker(0) + 0x30) = PathAt(3);
    Commit(g_racerMem[0]);
    *(uintptr_t*)(Tracker(0) + 0x30) = 0;
    *(uintptr_t*)(Tracker(0) + 0x80) = PathAt(5);
    Commit(g_racerMem[0]);
    *(uintptr_t*)(Tracker(0) + 0x80) = PathAt(5) + 4;  // not the start of a path
    Commit(g_racerMem[0]);
    *(uint32_t*)(g_graph + 0x1c) = 5;  // path 5 is beyond this course's count
    *(uintptr_t*)(Tracker(0) + 0x80) = PathAt(5);
    Commit(g_racerMem[0]);
    *(uint32_t*)(g_graph + 0x1c) = 12;
    std::vector<uint16_t> navs;
    for (const RaceNetMsg& m : g_sent) {
        RacePose p;
        if (DecodePose(m.data, m.size, p)) navs.push_back(p.nav);
    }
    CHECK(navs.size() == 4 && navs[0] == 3 && navs[1] == 5 && navs[2] == kNavNone && navs[3] == kNavNone);

    // The other car follows the nav path of the pose shown: one game call per change of path.
    auto feed = [&](uint32_t from, uint32_t count, uint16_t nav) {
        for (uint32_t t = from; t < from + count; ++t) {
            ++g_tick;
            RacePose p = {};
            p.race = c.race;
            p.slot = 1;
            p.tick = 2000 + t;
            p.pos[0] = (float)t;
            p.rot[3] = 1;
            p.nav = nav;
            uint8_t pb[kWireMax];
            Deliver(B, pb, EncodePose(p, pb, sizeof(pb)), false);
            UHook(kHRacing)(nullptr, nullptr);
            Commit(g_racerMem[1]);
        }
    };
    feed(0, 20, 2);
    CHECK(g_navCalls.size() == 1 && g_navCalls[0].racer == g_racerMem[1] && g_navCalls[0].nav == 2);
    feed(20, 20, 7);
    CHECK(g_navCalls.size() == 2 && g_navCalls[1].nav == 7);
    *(uintptr_t*)(Tracker(1) + 0xac) = PathAt(1);  // the game moved the forced path itself (a local respawn)
    feed(40, 1, 7);
    CHECK(g_navCalls.size() == 3 && g_navCalls[2].nav == 7);  // put back on the next tick
    feed(41, 20, kNavNone);  // no path known: left alone
    feed(61, 20, 12);        // a path this course doesn't have: left alone
    CHECK(g_navCalls.size() == 3);
    *(int32_t*)(g_racerMem[1] + 0xa1c) = 1;  // being respawned here: no pose, no nav
    feed(81, 20, 4);
    CHECK(g_navCalls.size() == 3);
}

static void TestLocalFinishesFirst() {
    printf("results: this game's player finishes first\n");
    RaceContract c = HostRunning();
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_humansSeen == 2 && *(uint32_t*)(g_scene + 0x1104) == 1);  // raised only during the update
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(!g_raceOver);  // waits for the other player
    std::vector<RaceFinish> mine = SentFinishes();
    CHECK(mine.size() == 1 && mine[0].race == c.race && mine[0].slot == 0 && mine[0].reason == 1 && mine[0].time == 0x00c80000 &&
          mine[0].best == 0x1000);
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    for (int i = 0; i < 5; ++i) UHook(kHWff)(wff, nullptr);
    // Still waiting; the 30 s timer is the game's own here (0x5b2250 starts it for our finish).
    CHECK(!g_raceOver && g_humansSeen == 2 && g_timerStarts == 0 && SentFinishes().size() == 1 && g_wffCalls == 5);
    uint8_t buf[kWireMax];
    RaceFinish wrong = {c.race, 0, 1, 0x00d00000, 0x2000};  // not the sender's slot: ignored
    Deliver(B, buf, EncodeFinished(wrong, buf, sizeof(buf)));
    UHook(kHWff)(wff, nullptr);
    CHECK(!g_raceOver && g_added.empty());
    // The other player's Finished: their record gets their own game's time, and the race ends.
    *(uint32_t*)(g_records[1] + 0x68) = 0x777;
    RaceFinish theirs = {c.race, 1, 1, 0x00d00000, 0x2000};
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    UHook(kHWff)(wff, nullptr);
    CHECK(g_added.size() == 1 && g_added[0].first == 1 && g_added[0].second == 1);
    CHECK(*(uint32_t*)(g_records[1] + 0x68) == 0x00d00000 && *(uint32_t*)(g_records[1] + 0x60) == 0x2000);
    CHECK(*(uint32_t*)(g_records[1] + 0x74) == 4);  // counts as finished: no later crossing here changes the time
    CHECK(g_humansSeen == 1 && g_raceOver && *(uint32_t*)(g_scene + 0x1104) == 1);
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));  // a repeat changes nothing
    UHook(kHWff)(wff, nullptr);
    CHECK(g_added.size() == 1 && SentFinishes().size() == 1);
    NativeCall td = {};
    NHook(kH090a)(nullptr, &td);  // logs the results
    CHECK(RaceTestPhase() == kIdle);
}

static void TestOtherFinishesFirst() {
    printf("results: the other player finishes first\n");
    RaceContract c = HostRunning();
    *(uint32_t*)(g_records[1] + 0x68) = 0x555;  // their car already crossed the line here
    Append(1, 1);
    uint8_t buf[kWireMax];
    RaceFinish theirs = {c.race, 1, 1, 0x00b00000, 0x1800};
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_added.empty() && ListReason(1) && *ListReason(1) == 1 && *(uint32_t*)(g_records[1] + 0x68) == 0x00b00000);
    CHECK(g_humansSeen == 2 && !g_raceOver);  // ours still racing
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_timerStarts == 1 && g_timerSeconds == 30 && *(int32_t*)(wff + 0x10) == 0 && g_humansSeen == 2);
    UHook(kHWff)(wff, nullptr);
    CHECK(g_timerStarts == 1);
    g_finishLocalNow = 0x00c00000;
    UHook(kHWff)(wff, nullptr);  // ours finishes inside this update, which still waits
    CHECK(!g_raceOver && SentFinishes().size() == 1 && SentFinishes()[0].time == 0x00c00000);
    UHook(kHWff)(wff, nullptr);
    CHECK(g_humansSeen == 1 && g_raceOver);
}

static void TestLateAndGone() {
    printf("results: the other player left; a late Finished\n");
    RaceContract c = HostRunning();
    g_snap.memberCount = 1;  // the guest left the Steam lobby
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_humansSeen == 1);  // nobody to wait for
    g_finishLocalNow = 0x100000;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_raceOver);
    uint8_t buf[kWireMax];
    RaceFinish theirs = {c.race, 1, 1, 0x00b00000, 0x1800};
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_added.empty() && *(uint32_t*)(g_records[1] + 0x68) == 0x999999 && g_timerStarts == 0);  // after the end: not used
    // No contract: the race updates are the stock ones, untouched.
    Reset();
    UHook(kHRacing)(nullptr, nullptr);
    UHook(kHWff)(wff, nullptr);
    CHECK(g_humansSeen == 1 && g_wffCalls == 1);
}

// The other player is more than 30 s behind. Their game's timer started when our Finished arrived,
// so ours runs out first; we wait (up to 5 s) for their own estimate, so both screens show the same.
static void TestTimeout() {
    printf("results: the race-end timer runs out\n");
    for (int arrives = 1; arrives >= 0; --arrives) {
        RaceContract c = HostRunning();
        g_finishLocalNow = 0x00c80000;
        UHook(kHRacing)(nullptr, nullptr);
        uint8_t wff[0x20] = {};
        *(int32_t*)(wff + 0x10) = -1;
        for (int i = 0; i < 1500 + 100; ++i) {  // the 30 s timer, and 2 s more
            ++g_tick;
            UHook(kHWff)(wff, nullptr);
        }
        CHECK(g_timerOn && !g_raceOver && g_timerElapsed == g_timerLimit - 1);  // held two ticks short (+1 in the update)
        if (arrives) {
            uint8_t buf[kWireMax];
            RaceFinish theirs = {c.race, 1, 6, 0x01400000, 0x2000};  // their own game's estimate
            Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
            ++g_tick;
            UHook(kHWff)(wff, nullptr);
            CHECK(g_raceOver && *(uint32_t*)(g_records[1] + 0x68) == 0x01400000 && ListReason(1) && *ListReason(1) == 6);
            CHECK(g_added.size() == 1 && g_added[0].second == 6);
        } else {
            for (int i = 0; i < 200 && !g_raceOver; ++i) {  // the rest of the 5 s grace
                ++g_tick;
                UHook(kHWff)(wff, nullptr);
            }
            CHECK(g_raceOver && *(uint32_t*)(g_records[1] + 0x68) == 0x999999);  // this game's own estimate
        }
    }
}

// The race end gave the other player's unfinished car a finish (seen 2026-10-22: reason 1 with a projected time, the
// game's rule for a car on its last lap): a player who didn't finish is DNF (reason 6), as on the console;
// our own real finish stays.
static void TestPlayersDnf() {
    printf("results: a player who didn't finish is DNF\n");
    HostRunning();
    Append(1, 1);  // no Finished from their game, no crossing: not a real finish
    *(uint32_t*)(g_records[0] + 0x74) = *(uint32_t*)(g_records[0] + 0x84) = 4;  // ours crossed the line
    g_finishLocalNow = 0x00c80000;
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    for (int i = 0; i < 1500 + 400 && !g_raceOver; ++i) {
        ++g_tick;
        UHook(kHWff)(wff, nullptr);
    }
    ++g_tick;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver && ListReason(1) && *ListReason(1) == 6 && ListReason(0) && *ListReason(0) != 6);
    // DNF rows last, as on the console (car 1's time was the smallest), and the places follow the list.
    bool dnfSeen = false, ordered = true;
    for (int i = 0; i < g_finishCount; ++i) {
        bool dnf = *(uint32_t*)(g_finishList[i] + 4) == 6;
        ordered = ordered && (dnf || !dnfSeen);
        dnfSeen = dnfSeen || dnf;
        ordered = ordered && *(int32_t*)(g_records[g_finishList[i][0]] + 0x80) == i;
    }
    CHECK(ordered && g_finishList[0][0] == 0);
}

static void TestRecordFinishAndRetry() {
    printf("results: finished by the lap record; queue full\n");
    HostRunning();
    // The lap record says finished (+0x74 = +0x84) before the finish list shows it.
    *(uint32_t*)(g_records[0] + 0x74) = 4;
    *(uint32_t*)(g_records[0] + 0x68) = 0x00a00000;
    g_sendFails = 2;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(SentFinishes().empty());  // the queue was full
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(SentFinishes().empty());
    UHook(kHRacing)(nullptr, nullptr);
    std::vector<RaceFinish> mine = SentFinishes();
    CHECK(mine.size() == 1 && mine[0].reason == 1 && mine[0].time == 0x00a00000);
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(SentFinishes().size() == 1);
    CHECK(g_humansSeen == 2 && !g_raceOver);  // and the race waits for the other player
}

// The results badges: during the results screen's row call only, the humans carry their lobby slot.
static void TestBadges() {
    printf("results badges\n");
    HostRunning();
    *(int32_t*)(g_racerMem[0] + 0x9dc) = 0;  // as the game numbers them on the host: its human 0, the rest AI (8)
    *(int32_t*)(g_racerMem[1] + 0x9dc) = 8;
    static uint8_t module[0x5000];
    memset(module, 0, sizeof(module));
    const uint8_t code[] = {0x32, 0x01, 0x12, 0x09, 0x00, 0x00, 0x96, 0x09, 0x07, 0x01, 0x31, 0x4b, 0x14};
    memcpy(module + 0x4239, code, sizeof(code));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, module + 0x4239 + 8, module};
    uint32_t values[1] = {0};
    uint8_t types[1] = {kTypeInt};
    NativeCall d = {0x0912, 1, {}, types, values, 1, 0, 0};
    NHook(kH0912)(&vm, &d);  // the race isn't over: untouched
    CHECK(g_rowCalls == 1 && g_rowSaw[0] == 0 && g_rowSaw[1] == 8);
    g_raceOver = 1;
    NHook(kH0912)(&vm, &d);
    CHECK(g_rowCalls == 2 && g_rowSaw[0] == 0 && g_rowSaw[1] == 1);  // host 1P, guest 2P
    CHECK(*(int32_t*)(g_racerMem[0] + 0x9dc) == 0 && *(int32_t*)(g_racerMem[1] + 0x9dc) == 8);  // and back
    // The race-win stat reads the same native elsewhere in the module: the real numbers.
    vm.pc = module + 0x53ce + 8;
    NHook(kH0912)(&vm, &d);
    CHECK(g_rowCalls == 3 && g_rowSaw[1] == 8);
    vm.pc = module + 0x4239 + 8;  // the right place, but different code (another mod's results screen)
    module[0x4239 + 6] = 0x97;
    NHook(kH0912)(&vm, &d);
    CHECK(g_rowCalls == 4 && g_rowSaw[1] == 8);
    Reset();  // no online race: untouched
    module[0x4239 + 6] = 0x96;
    g_raceOver = 1;
    *(int32_t*)(g_racerMem[1] + 0x9dc) = 8;
    NHook(kH0912)(&vm, &d);
    CHECK(g_rowCalls == 5 && g_rowSaw[1] == 8);
}

// The results rows' names: right after the row call, the row's name box (1612 at `results` 0x425b) gets a
// human's Steam name; AI rows, other 1612 calls and other screens keep the game's text.
static void TestResultsNames() {
    printf("results rows: the players' names\n");
    HostRunning();
    static uint8_t module[0x5000];
    memset(module, 0, sizeof(module));
    const uint8_t row[] = {0x32, 0x01, 0x12, 0x09, 0x00, 0x00, 0x96, 0x09, 0x07, 0x01, 0x31, 0x4b, 0x14};
    const uint8_t name[] = {0x07, 0x06, 0x51, 0x01, 0x23, 0x3b, 0x32, 0x02, 0x12, 0x16, 0x00, 0x00, 0x9f, 0x09, 0x18};
    memcpy(module + 0x4239, row, sizeof(row));
    memcpy(module + 0x426f - 14, name, sizeof(name));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } rvm = {nullptr, module + 0x4239 + 8, module}, nvm = {nullptr, module + 0x426f, module};
    Append(3, 1);  // 1st an AI, 2nd the guest, 3rd the host
    Append(1, 1);
    Append(0, 1);
    char text[32];
    struct { uint32_t vtable, zero; int32_t capacity; char* text; } box = {0x6a71f8, 0, 32, text};
    uint32_t rv[1];
    uint8_t rt[1] = {kTypeInt};
    NativeCall rd = {0x0912, 1, {}, rt, rv, 1, 0, 0};
    uint32_t nv[2] = {(uint32_t)(uintptr_t)&box, (uint32_t)(uintptr_t)"Metal Sonic"};
    uint8_t nt[2] = {kTypeObject, kTypeLiteralString};
    NativeCall nd = {0x1612, 1, {}, nt, nv, 2, 0, 0};
    auto Row = [&](uint32_t pos) { rv[0] = pos; strcpy_s(text, "-"); NHook(kH0912)(&rvm, &rd); return RaceNativeBefore(&nvm, &nd); };
    CHECK(!Row(1) && strcmp(text, "-") == 0);  // the race isn't over: the game's text
    g_raceOver = 1;
    CHECK(Row(1) && strcmp(text, "Bravo 2") == 0);
    CHECK(Row(2) && strcmp(text, "Alfa") == 0);
    CHECK(!Row(0) && strcmp(text, "-") == 0);  // an AI's row
    CHECK(!Row(7));                            // a place past the finish list
    box.capacity = 5;                          // a smaller box: cut to fit, terminated
    CHECK(Row(1) && strcmp(text, "Brav") == 0);
    box.capacity = 32;
    box.vtable = 0x6a7200;                     // not a text buffer: the game's text
    CHECK(!Row(1) && strcmp(text, "-") == 0);
    box.vtable = 0x6a71f8;
    rv[0] = 1;
    NHook(kH0912)(&rvm, &rd);
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } other = {nullptr, module + 0x4336 + 8, module};
    strcpy_s(text, "-");
    CHECK(!RaceNativeBefore(&other, &nd) && strcmp(text, "-") == 0);  // another 1612 in the module
    module[0x426f - 14] = 0x08;                                       // the right place, other code
    CHECK(!Row(1));
    module[0x426f - 14] = 0x07;
    rvm.pc = module + 0x53ce + 8;  // a 0912 call elsewhere: no row is remembered
    CHECK(!Row(1));
    rvm.pc = module + 0x4239 + 8;
    Reset();  // no online race
    g_raceOver = 1;
    Append(1, 1);
    CHECK(!Row(0) && strcmp(text, "-") == 0);
}

// The pause title: during `pause_menu`'s 0f0f(car) only, that car carries its lobby slot (a guest: "PAUSED 2P").
static void TestPauseTitle() {
    printf("pause title: the lobby's player number\n");
    HostRunning();
    *(int32_t*)(g_racerMem[0] + 0x9dc) = 8;  // as the game numbers them on a guest: the host's car an AI, its own 0
    *(int32_t*)(g_racerMem[1] + 0x9dc) = 0;
    static uint8_t module[0x1000];
    memset(module, 0, sizeof(module));
    const uint8_t code[] = {0x0b, 0x01, 0x3a, 0x32, 0x01, 0x0f, 0x0f, 0x01, 0x00, 0x0b, 0x02, 0x51, 0x00, 0x27};
    memcpy(module + 0x0bf4 - 11, code, sizeof(code));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, module + 0x0bf4, module};
    uint32_t v[1] = {1};
    uint8_t t[1] = {kTypeInt};
    NativeCall d = {0x0f0f, 1, {}, t, v, 1, 0, 0};
    CHECK(!RaceNativeBefore(&vm, &d) && *(int32_t*)(g_racerMem[1] + 0x9dc) == 1);  // read as 2P
    RaceNativeAfter(&vm, &d);
    CHECK(*(int32_t*)(g_racerMem[1] + 0x9dc) == 0);  // and back
    v[0] = 3;  // an AI's car: untouched
    RaceNativeBefore(&vm, &d);
    CHECK(*(int32_t*)(g_racerMem[3] + 0x9dc) != 3);
    RaceNativeAfter(&vm, &d);
    v[0] = 1;
    module[0x0bf4 - 11] = 0x0c;  // the right place, other code
    RaceNativeBefore(&vm, &d);
    CHECK(*(int32_t*)(g_racerMem[1] + 0x9dc) == 0);
    RaceNativeAfter(&vm, &d);
    CHECK(*(int32_t*)(g_racerMem[1] + 0x9dc) == 0);
    module[0x0bf4 - 11] = 0x0b;
    Reset();  // no online race
    *(int32_t*)(g_racerMem[1] + 0x9dc) = 0;
    RaceNativeBefore(&vm, &d);
    CHECK(*(int32_t*)(g_racerMem[1] + 0x9dc) == 0);
    RaceNativeAfter(&vm, &d);
}

// The loading screen's tips: fe_game_bridge's tip draw (1d02 at 0x09de, Roster Pack 0x0a80) starts from the
// contract's seed during our race start only; other 1d02 calls and offline starts keep the game's RNG.
static void TestLoadingTips() {
    printf("loading tips: the race's seed\n");
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract sent;
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, sent));
    static uint8_t module[0x1000];
    memset(module, 0, sizeof(module));
    const uint8_t code[] = {0x55, 0x00, 0x07, 0x03, 0x3a, 0x32, 0x02, 0x02, 0x1d, 0x01, 0x00, 0xc4, 0x00, 0x57, 0x16, 0x07, 0x02, 0x23};
    memcpy(module + 0x09e6 - 13, code, sizeof(code));
    memcpy(module + 0x0a88 - 13, code, sizeof(code));
    memcpy(module + 0x0a0d - 13, code, sizeof(code));  // the standalone Metal Sonic 1.1.2's bridge
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, module + 0x09e6, module};
    uint32_t v[2] = {0, 18};
    uint8_t t[2] = {kTypeInt, kTypeInt};
    NativeCall d = {0x1d02, 1, {}, t, v, 2, 0, 0};
    CHECK(!RaceNativeBefore(&vm, &d) && g_seed == 0);  // armed, the start not yet sent
    RaceLaunched(true);
    CHECK(!RaceNativeBefore(&vm, &d) && g_seed == (sent.seed ^ 0x54495053));
    g_seed = 0;
    vm.pc = module + 0x0a88;  // the Roster Pack's copy
    CHECK(!RaceNativeBefore(&vm, &d) && g_seed == (sent.seed ^ 0x54495053));
    g_seed = 0;
    vm.pc = module + 0x0a0d;  // Metal 1.1.2's copy
    CHECK(!RaceNativeBefore(&vm, &d) && g_seed == (sent.seed ^ 0x54495053));
    g_seed = 0;
    vm.pc = module + 0x0a88 + 0x100;  // another 1d02
    RaceNativeBefore(&vm, &d);
    CHECK(g_seed == 0);
    vm.pc = module + 0x09e6;
    module[0x09e6 - 13] = 0x56;  // the right place, other code
    RaceNativeBefore(&vm, &d);
    CHECK(g_seed == 0);
    module[0x09e6 - 13] = 0x55;
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    CHECK(RaceTestPhase() == kCreated);
    RaceNativeBefore(&vm, &d);
    CHECK(g_seed == (sent.seed ^ 0x54495053));  // the bridge may load the screen after the cars are made
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(g_seed == sent.seed);
    g_seed = 0;
    RaceNativeBefore(&vm, &d);
    CHECK(g_seed == 0);  // set up: the race's own draws are left alone
    Reset();  // offline
    RaceNativeBefore(&vm, &d);
    CHECK(g_seed == 0);
}

// Back to the lobby: the results and pause menus see game type 5 (online) at their item-list calls
// only, and front_main's read of the return key gives the network lobby, once per race.
static int g_1b01Calls = 0, g_0916Calls = 0;
static uint32_t g_storedKey = 0x906825eb, g_storedType = kTypeHash;  // EXIT TO MENU's key, as results writes it
static void __cdecl Fake1b01(void*, NativeCall* d) { ++g_1b01Calls; d->retValue = g_storedKey; d->retType = g_storedType; }
// Single Race, as the game returns it: 0x48c640 always stores type 3 (hash), never 2.
static void __cdecl Fake0916(void*, NativeCall* d) { ++g_0916Calls; d->retValue = 1; d->retType = kTypeHash; }

static uint32_t Type0916(void* vm) {
    NativeCall d = {0x0916, 1, {}, nullptr, nullptr, 0, 0, 0};
    NHook(kH0916)(vm, &d);
    return d.retValue;
}

static NativeCall Read1b01(void* vm, uint32_t key, uint8_t* type, uint32_t* value) {
    *type = kTypeHash;
    *value = key;
    NativeCall d = {0x1b01, 1, {}, type, value, 1, 0, 0};
    NHook(kH1b01)(vm, &d);
    return d;
}

// A Steam join while racing (re\m6-join\FINDINGS.md 3): the race is left as the pause menu's EXIT does it,
// and front_main then opens the online search.
void RaceTestSetJoinAddresses(uintptr_t post, uintptr_t clearGp, uintptr_t clearMission, uintptr_t clear093f, uintptr_t blackboardGet,
                              uintptr_t worldPaused, uintptr_t profiles);
struct PostSeen { uint32_t sender, target, msg, value, type; };
static std::vector<PostSeen> g_posted;
static bool g_postOk = true;
static int g_clears = 0;
static uint32_t g_screen = 0xbed107e7;
static uint8_t g_worldPaused = 0, g_profiles[4 * 0x3af0];
static bool __cdecl FakePostMessage(uint32_t sender, uint32_t target, uint32_t msg, const uint32_t* value, uint32_t zero) {
    CHECK(zero == 0);
    if (g_postOk) g_posted.push_back({sender, target, msg, value[0], value[1]});
    return g_postOk;
}
static void __cdecl FakeClear() { ++g_clears; }
static const uint32_t* __fastcall FakeBlackboard(uintptr_t board, void*, uint32_t key) {
    static uint32_t v[2];
    CHECK(board == 0x6eca14 && key == 0xb0567d36);
    v[0] = g_screen;
    v[1] = kTypeHash;
    return v;
}

static void TestJoinLeavesRace() {
    printf("a Steam join leaves a race\n");
    RaceTestSetJoinAddresses((uintptr_t)&FakePostMessage, (uintptr_t)&FakeClear, (uintptr_t)&FakeClear, (uintptr_t)&FakeClear,
                             (uintptr_t)&FakeBlackboard, (uintptr_t)&g_worldPaused, (uintptr_t)g_profiles);
    static uint8_t game[0x800], front[0x1000];
    const uint8_t inbox[] = {0x51, 0x01, 0x32, 0x01, 0x0e, 0x16, 0x02, 0x00, 0x2f, 0x00};
    const uint8_t frontCode[] = {0x02, 0x2b, 0xc7, 0xd1, 0x72, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0x92, 0x00};
    memcpy(game + 0x0496 - 10, inbox, sizeof(inbox));
    memcpy(front + 0x0433, frontCode, sizeof(frontCode));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } gvm = {nullptr, game + 0x0496, game}, other = {nullptr, game + 0x0600, game},
                                                           fvm = {nullptr, front + 0x0438 + 8, front};
    uint8_t t;
    uint32_t v;
    NativeCall inboxRead = {0x160e, 1, {}, nullptr, nullptr, 1, 0, 0};
    auto Frame = [&]() { ++g_tick; UHook(kHCountdown)(nullptr, nullptr); RaceNativeBefore(&gvm, &inboxRead); };
    // An offline race; Steam up, a join waiting.
    Reset();
    RaceTestSetReturnFlag(false);
    g_snap = LobbySnapshot{};
    g_snap.state = kLobbyIdle;
    g_snap.pendingJoin = 0x0186000012345678ull;
    g_posted.clear();
    g_clears = 0;
    Frame();
    CHECK(g_posted.empty());  // no license picked yet (the attract demo, the title screen)
    uint8_t lt[2] = {kTypeInt, kTypeInt};
    uint32_t lv[2] = {20, 1};
    NativeCall pick = {0x2004, 1, {}, lt, lv, 2, 0, 0};
    g_profiles[1 * 0x3af0 + 0x14] = 1;  // an empty slot: not a license
    RaceNativeBefore(nullptr, &pick);
    Frame();
    CHECK(g_posted.empty());
    g_profiles[1 * 0x3af0 + 0x14] = 0;
    RaceNativeBefore(nullptr, &pick);
    g_worldPaused = 1;  // paused: wait
    Frame();
    CHECK(g_posted.empty());
    g_worldPaused = 0;
    RaceNativeBefore(&other, &inboxRead);  // another 160e: nothing
    CHECK(g_posted.empty());
    Frame();
    CHECK(g_posted.size() == 1 && g_clears == 3);
    CHECK(g_posted[0].sender == 0 && g_posted[0].target == 0xbed107e7 && g_posted[0].msg == 0x682e3dfe && g_posted[0].value == 0 &&
          g_posted[0].type == kTypeInt);
    Frame();
    CHECK(g_posted.size() == 1);  // once
    // front_main after the race: the online search, which takes the join.
    NativeCall d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x7ab1c9a7 && d.retType == kTypeHash);
    // On the results' retry/quit screen: payload 1 (as the results screen quits); not during the race-less loading.
    g_snap.pendingJoin = 0x0186000012345679ull;
    g_screen = 0x603658c4;
    RaceNativeBefore(&gvm, &inboxRead);
    CHECK(g_posted.size() == 2 && g_posted[1].value == 1);
    g_snap.pendingJoin = 0x018600001234567aull;
    g_screen = 0x987b66c0;  // a screen transition
    RaceNativeBefore(&gvm, &inboxRead);
    CHECK(g_posted.size() == 2);
    g_screen = 0xbed107e7;
    g_tick += 100;  // no race update for a while (loading, results)
    RaceNativeBefore(&gvm, &inboxRead);
    CHECK(g_posted.size() == 2);
    *(uint32_t*)(g_scene + 0x1104) = 0;  // no player on the track (the attract demo)
    Frame();
    CHECK(g_posted.size() == 2);
    *(uint32_t*)(g_scene + 0x1104) = 1;
    g_postOk = false;  // the message queue is full: tried again next frame
    Frame();
    g_postOk = true;
    Frame();
    CHECK(g_posted.size() == 3);
    g_snap.pendingJoin = 0;  // no join: front_main as before
    d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x906825eb);
}

static void TestReturnToLobby() {
    printf("return to the lobby\n");
    static uint8_t results[0x5000], pause[0x2000], front[0x1000];
    const uint8_t resultsCode[] = {0x32, 0x00, 0x16, 0x09, 0x00, 0x00, 0x2a, 0x04, 0x32, 0x00, 0x38, 0x09, 0x00, 0x00, 0x2b, 0x04};
    const uint8_t pauseCode[] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x2f, 0x02, 0x51, 0x00, 0x02, 0x3b, 0x66, 0xef, 0x33};
    const uint8_t frontCode[] = {0x02, 0x2b, 0xc7, 0xd1, 0x72, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0x92, 0x00};
    memcpy(results + 0x22fd, resultsCode, sizeof(resultsCode));
    memcpy(pause + 0x0cdd, pauseCode, sizeof(pauseCode));
    memcpy(front + 0x0433, frontCode, sizeof(frontCode));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } rvm = {nullptr, results + 0x22fd + 8, results},
                                                           pvm = {nullptr, pause + 0x0cdd + 8, pause},
                                                           fvm = {nullptr, front + 0x0438 + 8, front};
    uint8_t t;
    uint32_t v;
    Reset();
    RaceTestSetReturnFlag(false);
    // No online race: everything stock.
    CHECK(Type0916(&rvm) == 1 && Type0916(&pvm) == 1);
    NativeCall d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x906825eb && d.retType == kTypeHash);
    // A race the lobby started (the menus sent the start, the script creates the racers).
    HostRunning();
    CHECK(RaceTestReturnFlag());
    CHECK(Type0916(&rvm) == 5 && Type0916(&pvm) == 5);  // results: RETURN TO LOBBY; pause: no RESTART
    {
        NativeCall t5 = {0x0916, 1, {}, nullptr, nullptr, 0, 0, 0};
        NHook(kH0916)(&rvm, &t5);
        CHECK(t5.retType == kTypeHash);  // the game's type kept
    }
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } other = {nullptr, results + 0x25e9 + 8, results};
    memcpy(results + 0x25e9, resultsCode, 8);
    CHECK(Type0916(&other) == 1);  // the same native elsewhere (list timer, finish display): the real type
    results[0x22fd + 10] = 0x39;    // the right place, different code (another mod's results screen)
    CHECK(Type0916(&rvm) == 1);
    results[0x22fd + 10] = 0x38;
    // The race is closed (090a); the flag outlives it until front_main reads the return key.
    NativeCall none = {};
    NHook(kH090a)(nullptr, &none);
    CHECK(RaceTestReturnFlag() && Type0916(&rvm) == 5);
    d = Read1b01(&fvm, 0x1cde073e, &t, &v);  // the other key front_main reads there: untouched
    CHECK(d.retValue == 0x906825eb && RaceTestReturnFlag());
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } elsewhere = {nullptr, front + 0x0500, front};
    d = Read1b01(&elsewhere, 0x72d1c72b, &t, &v);  // the key read somewhere else: untouched
    CHECK(d.retValue == 0x906825eb && RaceTestReturnFlag());
    d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0xb0a0d459 && d.retType == kTypeHash && !RaceTestReturnFlag());  // the lobby, once
    d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x906825eb);
    CHECK(Type0916(&rvm) == 1 && Type0916(&pvm) == 1);  // a later offline race: stock menus
    // Not in the Steam lobby any more (the host left during the race): the network menu's Search.
    RaceTestSetReturnFlag(true);
    g_snap.state = kLobbyError;
    d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x7ab1c9a7 && d.retType == kTypeHash && !RaceTestReturnFlag());
    // An unexpected stored type: the game's own choice, and the flag is spent.
    RaceTestSetReturnFlag(true);
    g_storedType = kTypeInt;
    d = Read1b01(&fvm, 0x72d1c72b, &t, &v);
    CHECK(d.retValue == 0x906825eb && d.retType == kTypeInt && !RaceTestReturnFlag());
    g_storedType = kTypeHash;
    // front_main never read the key (another branch, another mod's front_main): the next offline race
    // clears it at its setup, so its menus are the stock ones.
    Reset();
    RaceTestSetReturnFlag(true);
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(!RaceTestReturnFlag() && Type0916(&rvm) == 1 && Type0916(&pvm) == 1);
    // ...but our own race's setup keeps it, also when its contract failed (an offline race from our lobby).
    HostRunning();
    CHECK(RaceTestReturnFlag());
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridAI", 4);  // racer 0 should be our player: the contract fails
    CHECK(RaceTestPhase() == kFailed);
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(RaceTestReturnFlag() && Type0916(&rvm) == 5);
    // A start the menus could not send never sets it.
    Reset();
    RaceTestSetReturnFlag(false);
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(false);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    CHECK(!RaceTestReturnFlag());
    Reset();
    RaceTestSetReturnFlag(false);
}

// The host's rules: the bridge's difficulty read gives the host's AI setting (lobby 1..3 = 0..2); the
// host's items (and the default hazards) go into this profile's active settings record for the race,
// and the profile's own values are back before any save (1304) and when the race is closed (090a).
static uint8_t g_settings[4][0xac];
static uint32_t g_activeProfile = 1;
static int32_t g_profileDifficulty = 0;  // this PC's own setting (the game returns it as an int, 0x48c520)
static void __cdecl Fake2003(void*, NativeCall* d) { d->retValue = (uint32_t)(d->values[0] == 3 ? g_profileDifficulty : 8); d->retType = kTypeInt; }
static int g_saves = 0;
static uint32_t g_savedItems = 99, g_savedHazards = 99;  // what the record held when the save started
static void __cdecl Fake1304(void*, NativeCall*) {
    ++g_saves;
    g_savedItems = *(uint32_t*)(g_settings[g_activeProfile] + 0xc);
    g_savedHazards = g_settings[g_activeProfile][0x10];
}

struct FakeVm { void* vtable; uint8_t* pc; uint8_t* mod; };
static NativeCall Read2003(FakeVm* vm, uint32_t field, uint8_t* t, uint32_t* v) {
    *t = kTypeInt;
    *v = field;
    NativeCall d = {0x2003, 1, {}, t, v, 1, 0, 0};
    NHook(kH2003)(vm, &d);
    return d;
}

static uint32_t Items() { return *(uint32_t*)(g_settings[g_activeProfile] + 0xc); }
static uint8_t Hazards() { return g_settings[g_activeProfile][0x10]; }

// Egg Hangar's course: the lobby's availability query 2003(73, course) answers by the installed mod; other courses keep the stock answer.
static void TestEggCourse() {
    printf("Egg Hangar's course: never offered without the Egg Hangar mod\n");
    FakeVm vm = {nullptr, nullptr, nullptr};
    uint8_t t[2] = {kTypeInt, kTypeHash};
    const uint32_t egg = 0x2c0dae1f;
    CHECK(sizeof(kCourses) / sizeof(kCourses[0]) == 25 && kCourses[24].hash == egg && strcmp(kCourses[24].name, "doomeggzone_dlc") == 0);
    uint32_t u[2] = {73, egg};
    NativeCall n = {0x2003, 1, {}, t, u, 2, 0, 0};
    NHook(kH2003)(&vm, &n);
    CHECK(n.retValue == 8);  // no complete mod scan yet: the stock answer
    for (int installed = 0; installed < 2; ++installed) {
        RaceUseEggHangar(installed != 0);
        uint32_t v[2] = {73, egg};
        NativeCall d = {0x2003, 1, {}, t, v, 2, 0, 0};
        NHook(kH2003)(&vm, &d);
        CHECK(d.retValue == (installed ? 8u : 0u) && d.retType == kTypeInt);  // with the mod the stock answer (its own), without "no"
        uint32_t w[2] = {73, kCourses[0].hash};
        NativeCall s = {0x2003, 1, {}, t, w, 2, 0, 0};
        NHook(kH2003)(&vm, &s);
        CHECK(s.retValue == 8);  // another course keeps the stock answer
    }
    // 1.1 ENABLE MODIFICATIONS: installed but off in this lobby = not offered; Metal Sonic off = not pickable.
    RaceUseEggHangar(true);
    RaceLobbyMods(false, false);
    uint32_t v[2] = {73, egg};
    NativeCall d = {0x2003, 1, {}, t, v, 2, 0, 0};
    NHook(kH2003)(&vm, &d);
    CHECK(d.retValue == 0);
    RaceUseRacerList(kRacerListRoster);
    int metal = -1;
    for (int i = 0; i < 25; ++i)
        if (RaceRacerHash(i) == 0x9cabac57) metal = i;
    CHECK(metal >= 0 && !RaceRacerOn(metal) && RaceRacerOn(0));
    int taken[1] = {0};
    for (int n = 0; n < 25; ++n) CHECK(RaceTestPlayerRacer(n, taken, 1) != metal);
    RaceLobbyMods(true, true);
    CHECK(RaceRacerOn(metal));
    // The standalone Metal Sonic 1.1.2: the stock 21 + Metal (22nd); the same on / off rule.
    RaceUseRacerList(kRacerListMetal);
    CHECK(RaceRacerHash(21) == 0x9cabac57 && RaceRacerHash(0) == kRacersStock[0].hash && RaceRacerHash(20) == kRacersStock[20].hash);
    CHECK(RaceRacerHash(22) == 0 && RaceRacerOn(21));
    RaceLobbyMods(true, false);
    CHECK(!RaceRacerOn(21) && RaceRacerOn(20));
    for (int n = 0; n < 25; ++n) CHECK(RaceTestPlayerRacer(n, taken, 1) != 21);
    RaceLobbyMods(true, true);
    RaceUseRacerList(kRacerListRoster);
    NativeCall e = {0x2003, 1, {}, t, v, 2, 0, 0};
    NHook(kH2003)(&vm, &e);
    CHECK(e.retValue == 8);
    RaceUseEggHangar(false);
}

static void TestHostRules() {
    printf("host's rules\n");
    static uint8_t rp[0x3000], st[0x3000], mt[0x3000];
    const uint8_t diff[] = {0x51, 0x03, 0x32, 0x01, 0x03, 0x20, 0x01, 0x00, 0xbe, 0x03};
    const uint8_t count[] = {0x51, 0x01, 0x32, 0x01, 0x03, 0x20, 0x01, 0x00, 0xcb, 0x03};
    memcpy(rp + 0x2120, diff, sizeof(diff));   // Roster Pack fe_game_bridge: INT8 3; NAT 2003 at 0x2122
    memcpy(st + 0x207e, diff, sizeof(diff));   // stock: at 0x2080
    memcpy(rp + 0x2199, count, sizeof(count)); // the racer-count read at 0x219b
    memcpy(mt + 0x20a5, diff, sizeof(diff));   // the standalone Metal Sonic 1.1.2's bridge: at 0x20a7
    memcpy(mt + 0x211e, count, sizeof(count)); // its racer count at 0x2120
    FakeVm rvm = {nullptr, rp + 0x212a, rp}, svm = {nullptr, st + 0x2088, st}, cvm = {nullptr, rp + 0x21a3, rp};
    FakeVm mvm = {nullptr, mt + 0x20af, mt}, mcvm = {nullptr, mt + 0x2128, mt};
    uint8_t t;
    uint32_t v;
    memset(g_settings, 0, sizeof(g_settings));
    *(uint32_t*)(g_settings[1] + 0xc) = 1;  // this profile: items without All-Star, hazards off, Hard AI
    g_settings[1][0x10] = 0;
    *(uint32_t*)(g_settings[0] + 0xc) = 7;  // another profile's record: never touched
    g_profileDifficulty = 2;
    // No online race: stock values.
    Reset();
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 2);
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(Items() == 1 && Hazards() == 0);
    // The host's lobby: AI 1 (Beginner), items OFF.
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 1, 0, 1};
    CHECK(RaceHostStart() == kStartOk);
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 2);  // armed but the menus haven't sent the start: stock
    RaceLaunched(true);
    NativeCall d = Read2003(&rvm, 3, &t, &v);
    CHECK(d.retValue == 0 && d.retType == kTypeInt);  // Beginner = Easy
    CHECK(Read2003(&svm, 3, &t, &v).retValue == 0);  // the stock bridge's copy too
    CHECK(Read2003(&mvm, 3, &t, &v).retValue == 0);  // and Metal 1.1.2's
    CHECK(Read2003(&cvm, 1, &t, &v).retValue == 8);  // the racer count: untouched
    CHECK(Read2003(&mcvm, 1, &t, &v).retValue == 8);
    rp[0x2120 + 9] = 0x04;                            // another mod's bridge (different code): untouched
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 2);
    rp[0x2120 + 9] = 0x03;
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    CHECK(Items() == 1 && Hazards() == 0);  // not before SetupRace
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(Items() == 0 && Hazards() == 1);  // the host's items (off), the default hazards, for the race
    CHECK(*(uint32_t*)(g_settings[0] + 0xc) == 7);
    // The results' autosave: the profile's own values are in the record when the save starts.
    g_saves = 0;
    NativeCall none = {};
    NHook(kH1304)(nullptr, &none);
    CHECK(g_saves == 1 && g_savedItems == 1 && g_savedHazards == 0);
    CHECK(Items() == 1 && Hazards() == 0);
    NHook(kH090a)(nullptr, &none);  // closed after the save: nothing more to put back
    CHECK(Items() == 1 && Hazards() == 0);
    NHook(kH1304)(nullptr, &none);  // a later save (front end): the profile's values
    CHECK(g_saves == 2 && g_savedItems == 1);
    // AI 3 (Hard), items ON; the race is closed without a save (quit): put back at 090a.
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 3, 1, 1};
    g_profileDifficulty = 0;
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 2);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 2);  // still the host's while the racers are created
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(Items() == 2 && Hazards() == 1);
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 0);  // set up: reads are stock again
    NHook(kH090a)(nullptr, &none);
    CHECK(Items() == 1 && Hazards() == 0);
    // AI 0 ("off"): only the players' cars (difficulty: Easy, unused).
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 0, 1, 1};
    g_profileDifficulty = 2;
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    CHECK(Read2003(&rvm, 3, &t, &v).retValue == 0);
    CHECK(Read2003(&mcvm, 1, &t, &v).retValue == 2);  // AI OFF on Metal 1.1.2's bridge too
    d = Read2003(&cvm, 1, &t, &v);
    CHECK(d.retValue == 2 && d.retType == kTypeInt);
    // A contract that fails (an offline race from our lobby): this profile's rules.
    Script0905(0x1, "STATE_Control_StartingGridAI", 4);
    CHECK(RaceTestPhase() == kFailed);
    UHook(kHSetup)(nullptr, nullptr);
    CHECK(Items() == 1 && Hazards() == 0);
    // A restart during our race (not shared): it goes on as an offline race with this profile's items.
    HostRunning();
    CHECK(Items() == 2 && Hazards() == 1);
    UHook(kHWaitingNoIntro)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kFailed && Items() == 1 && Hazards() == 0);
    // No active profile: left alone.
    Reset();
    g_activeProfile = 0xffffffff;
    HostRunning();
    g_activeProfile = 1;
    CHECK(Items() == 1 && Hazards() == 0 && *(uint32_t*)(g_settings[0] + 0xc) == 7);
    Reset();
    CHECK(Items() == 1 && Hazards() == 0);
    RaceTestSetReturnFlag(false);
}

// ---------------------------------------------------------------- the host's AI (M5, notes/NPC-SYNC.md)
// Guest B (creation index 1) in a race whose contract says the host drives the AI, up to the running race.
static RaceContract GuestRunning() {
    RaceTestSetHostAi(true);
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c) && (c.flags & kContractHostAi));
    Reset();
    Lobby(B, false, 0, 1);
    uint8_t buf[kWireMax];
    Deliver(A, buf, EncodeContract(c, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridAI", 0);
    Script0905(c.roster[1], "STATE_Control_StartingGridHuman", 2);
    for (int k = 2; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);  // hold A
    Deliver(A, buf, EncodeGo(c.race, kStageLoaded, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(c.race, kStageCountdown, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 8; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;  // a unit rotation
    g_localIdx = 1;
    g_sent.clear();
    return c;
}

// The online pause's AI latch (re\m8-pause\FINDINGS.md): racer+0x1fc8, left set by a race quit from the open pause (the racers
// live for the whole run), is cleared when the next race's cars are created, so its first pause hands the car to the AI.
static void TestPauseLatch() {
    printf("online pause: the AI latch cleared at the race's creation\n");
    Reset();
    for (int k = 0; k < 8; ++k) g_racerMem[k][0x1fc8] = 1;
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    bool clear = true;
    for (int k = 0; k < 8; ++k) clear = clear && g_racerMem[k][0x1fc8] == 0;
    CHECK(clear);
}

// The pause menu's quit question (re\m8-audit A1): the host of an online race with another player in it gets the console's
// AAC0CD27 ("...THIS WILL END THE SESSION FOR OTHER PLAYERS.") at the box function's two text calls (pause_menu 0x16d5 / 0x16e6),
// in place of D5E3E174 / 21C7CA32; a guest, other sites, other keys and offline races keep the game's own.
static void TestQuitBox() {
    printf("quit box: the host's session warning\n");
    HostRunning();
    static uint8_t module[0x2000];
    memset(module, 0, sizeof(module));
    const uint8_t a[] = {0x02, 0x77, 0xa4, 0xc1, 0xf1, 0x0b, 0x01, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x01, 0x00, 0xc4, 0x03};
    const uint8_t b[] = {0x02, 0xe2, 0x82, 0x75, 0x37, 0x0b, 0x01, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x01, 0x00, 0xc5, 0x03};
    memcpy(module + 0x16cd, a, sizeof(a));
    memcpy(module + 0x16de, b, sizeof(b));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, module + 0x16dd, module};
    uint32_t v[2] = {0x1234, 0xd5e3e174};
    uint8_t t[2] = {kTypeHash, kTypeHash};
    NativeCall d = {0x021f, 1, {}, t, v, 2, 0, 0};
    CHECK(!RaceNativeBefore(&vm, &d) && v[1] == 0xaac0cd27);  // first layer
    vm.pc = module + 0x16ee;
    v[1] = 0x21c7ca32;
    CHECK(!RaceNativeBefore(&vm, &d) && v[1] == 0xaac0cd27);  // second layer, the other key
    v[1] = 0x0a3b00ae;  // another box: untouched
    RaceNativeBefore(&vm, &d);
    CHECK(v[1] == 0x0a3b00ae);
    v[1] = 0xd5e3e174;
    module[0x16de] = 0x03;  // the right place, other code
    RaceNativeBefore(&vm, &d);
    CHECK(v[1] == 0xd5e3e174);
    module[0x16de] = 0x02;
    GuestRunning();  // a guest leaving ends only its own race
    vm.pc = module + 0x16dd;
    RaceNativeBefore(&vm, &d);
    CHECK(v[1] == 0xd5e3e174);
    Reset();  // offline
    RaceNativeBefore(&vm, &d);
    CHECK(v[1] == 0xd5e3e174);
}

// One Cars message from `from`: the host's car (slot 0) at x and, with `ai`, the AI 2..7 at x + 100k.
static void HostCars(const RaceContract& c, uint32_t tick, float x, bool ai, uint64_t from = A, int extraSlot = -1, uint8_t fx = kFxNone) {
    RaceCars cars = {};
    cars.race = c.race;
    cars.tick = tick;
    auto add = [&](int slot, float px) {
        RacePose& p = cars.cars[cars.count++];
        p = RacePose{};
        p.race = c.race;
        p.tick = tick;
        p.slot = (uint8_t)slot;
        p.pos[0] = px;
        p.rot[3] = 1;
        p.nav = (uint16_t)(slot % 3);
        p.fx = fx;
    };
    add(0, x);
    if (ai)
        for (int k = 2; k < 8; ++k) add(k, x + 100.0f * k);
    if (extraSlot >= 0 && cars.count < kMaxCars) add(extraSlot, 999.0f);
    uint8_t b[kWireMax];
    size_t n = EncodeCars(cars, b, sizeof(b));
    CHECK(n != 0);
    Deliver(from, b, n, false);
}

static float X(int k) { return *(float*)(g_racerMem[k] + 0xc0); }

// ---------------------------------------------------------------- exhaust effects (re\m6-race-hud\EXHAUST-FINDINGS.md)
// Per car: the exhaust component (vtable 0x6af4e8, +4 racer, +0x70 fired stamp, +0x78 mode) at racer+0x1e64,
// and the boost state at [[racer+0x9f4]+0x3c4].
static uint8_t g_exhaust[8][0x80], g_boostHolder[8][0x400], g_boost[8][0x400];
struct ExhaustSeen { uint8_t flame, otherFlags; float lane; uint32_t level, stamp, status; int calls; } g_exSeen;
static void __fastcall FakeExhaust(uint8_t* e, void*) {
    uint8_t* racer = *(uint8_t**)(e + 4);
    int k = (int)((racer - g_racerMem[0]) / sizeof(g_racerMem[0]));
    uint8_t* b = g_boost[k];
    g_exSeen.flame = b[0x18a];
    g_exSeen.otherFlags = (uint8_t)(b[0x190] | b[0x192] | b[0x193] | b[0x195]);
    memcpy(&g_exSeen.lane, b + 0x2f0, 4);
    g_exSeen.level = *(uint32_t*)(b + 0x334);
    g_exSeen.stamp = *(uint32_t*)(b + 0x184);
    g_exSeen.status = *(uint32_t*)(racer + 0x1d10);
    ++g_exSeen.calls;
}
static void SetUpExhausts() {
    for (int k = 0; k < 8; ++k) {
        memset(g_exhaust[k], 0, sizeof(g_exhaust[k]));
        memset(g_boost[k], 0, sizeof(g_boost[k]));
        *(uint32_t*)g_exhaust[k] = 0x6af4e8;
        *(void**)(g_exhaust[k] + 4) = g_racerMem[k];
        *(uint32_t*)(g_exhaust[k] + 0x78) = 7;
        *(void**)(g_racerMem[k] + 0x1e64) = g_exhaust[k];
        *(void**)(g_racerMem[k] + 0x9f4) = g_boostHolder[k];
        *(void**)(g_boostHolder[k] + 0x3c4) = g_boost[k];
        // This game's own values: what the update must see on cars it simulates, and get back afterwards.
        g_boost[k][0x190] = 1;
        float half = 0.5f;
        for (int i = 0; i < 4; ++i) memcpy(g_boost[k] + 0x2f0 + 4 * i, &half, 4);
        *(uint32_t*)(g_boost[k] + 0x334) = 3;
        *(uint32_t*)(g_boost[k] + 0x184) = 77;
        *(uint32_t*)(g_racerMem[k] + 0x1d10) = 0x18;
    }
}
static void Exhaust(int k) { ((void(__fastcall*)(uint8_t*, void*))RaceTestHook(kHExhaust))(g_exhaust[k], nullptr); }
static bool BoostIntact(int k) {
    float lane;
    memcpy(&lane, g_boost[k] + 0x2f0 + 12, 4);
    return g_boost[k][0x190] == 1 && g_boost[k][0x18a] == 0 && lane == 0.5f && *(uint32_t*)(g_boost[k] + 0x334) == 3 &&
           *(uint32_t*)(g_boost[k] + 0x184) == 77 && *(uint32_t*)(g_racerMem[k] + 0x1d10) == 0x18;
}

static void TestExhaust() {
    printf("exhaust effects: the other cars show their owner's\n");
    RaceContract c = GuestRunning();
    SetUpExhausts();
    // Our own car: its exhaust mode goes out with its pose; the serial changes when its flame fires again.
    *(uint32_t*)(g_exhaust[1] + 0x78) = 2;
    ++g_tick;
    Commit(g_racerMem[1]);
    *(uint32_t*)(g_exhaust[1] + 0x78) = 4;
    *(uint32_t*)(g_exhaust[1] + 0x70) = 1234;  // fired
    ++g_tick;
    Commit(g_racerMem[1]);
    std::vector<uint8_t> fx;
    for (const auto& m : g_sent) {
        RacePose p;
        if (DecodePose(m.data, m.size, p) && p.slot == 1) fx.push_back(p.fx);
    }
    CHECK(fx.size() == 2 && fx[0] == 2 && FxState(fx[1]) == 4 && FxSerial(fx[1]) != FxSerial(fx[0]));
    *(uint32_t*)(g_exhaust[1] + 0x78) = 0x1234;  // not a mode: not known
    ++g_tick;
    Commit(g_racerMem[1]);
    RacePose last;
    CHECK(DecodePose(g_sent.back().data, g_sent.back().size, last) && last.fx == kFxNone);
    // The host's cars arrive with the flame: the update sees it, for that call only.
    for (uint32_t t = 0; t < 20; ++t) {
        ++g_tick;
        HostCars(c, 4000 + t, (float)t, true, A, -1, (uint8_t)(4 | (3 << 3)));
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    g_exSeen = {};
    Exhaust(0);
    CHECK(g_exSeen.calls == 1 && g_exSeen.flame == 1 && g_exSeen.otherFlags == 0 && g_exSeen.lane == 0.0f && !(g_exSeen.status & 8) &&
          g_exSeen.stamp != 77);
    CHECK(BoostIntact(0));  // and every field back
    uint32_t firstStamp = g_exSeen.stamp;
    Exhaust(2);  // the host's AI too
    CHECK(g_exSeen.calls == 2 && g_exSeen.flame == 1 && BoostIntact(2));
    // Charge level 1: all lanes on, level 1, no flame; the same serial keeps the stamp.
    for (uint32_t t = 20; t < 25; ++t) {
        ++g_tick;
        HostCars(c, 4000 + t, (float)t, true, A, -1, (uint8_t)(1 | (3 << 3)));
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    Exhaust(0);
    CHECK(g_exSeen.flame == 0 && g_exSeen.lane == 1.0f && g_exSeen.level == 1 && BoostIntact(0));
    // The flame again with a new serial: a new stamp, so the update fires it again.
    for (uint32_t t = 25; t < 30; ++t) {
        ++g_tick;
        HostCars(c, 4000 + t, (float)t, true, A, -1, (uint8_t)(4 | (4 << 3)));
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    Exhaust(0);
    CHECK(g_exSeen.flame == 1 && g_exSeen.stamp > firstStamp && BoostIntact(0));
    // Off: no lanes, no flags.
    for (uint32_t t = 30; t < 35; ++t) {
        ++g_tick;
        HostCars(c, 4000 + t, (float)t, true, A, -1, kFxOff);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    Exhaust(0);
    CHECK(g_exSeen.flame == 0 && g_exSeen.otherFlags == 0 && g_exSeen.lane == 0.0f && BoostIntact(0));
    // Our own car is simulated here: the update sees this game's values.
    Exhaust(1);
    CHECK(g_exSeen.otherFlags == 1 && g_exSeen.lane == 0.5f && g_exSeen.level == 3 && g_exSeen.stamp == 77 && (g_exSeen.status & 8));
    // An owner that sends no fx (kFxNone): the game's own update, untouched.
    for (uint32_t t = 35; t < 40; ++t) {
        ++g_tick;
        HostCars(c, 4000 + t, (float)t, true);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    Exhaust(0);
    CHECK(g_exSeen.otherFlags == 1 && g_exSeen.lane == 0.5f && g_exSeen.stamp == 77);
    // No online race: untouched.
    Reset();
    Exhaust(0);
    CHECK(g_exSeen.otherFlags == 1 && g_exSeen.stamp == 77);
}

static void TestHostAiGuest() {
    printf("host's AI: guest shows the host's cars\n");
    RaceContract c = GuestRunning();
    // Hold A made the host's car and the 6 AI network cars; ours stays as the script made it.
    CHECK(RaceTestAiPuppets() && g_stateCalls.size() == 7);
    for (auto& s : g_stateCalls) CHECK(s.second == 0x15 && s.first != g_controllers[1]);
    for (int k = 2; k < 8; ++k) CHECK(*(uint32_t*)(g_racerMem[k] + 0x9e4) == 0x15);
    CHECK(*(uint32_t*)(g_racerMem[1] + 0x9e4) == 0x17);
    // The host's cars every tick, its AI every second tick (and an entry for our own car: ignored).
    for (uint32_t t = 0; t < 40; ++t) {
        ++g_tick;
        HostCars(c, 3000 + t, (float)t, t % 2 == 0, A, t == 20 ? 1 : -1);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    printf("  host car x %.2f, AI 5 x %.2f\n", X(0), X(5));
    CHECK(X(0) > 25 && X(0) < 39);
    for (int k = 2; k < 8; ++k) CHECK(fabsf((X(k) - 100.0f * k) - X(0)) < 0.01f);  // the same moment of the host's race
    CHECK(X(1) == 0);  // ours is never written
    CHECK(CountSent(kMsgPose) == 40 && CountSent(kMsgCars) == 0);  // ours still goes out as a Pose
    bool aiNav = false;
    for (const NavCall& n : g_navCalls) aiNav |= n.racer == g_racerMem[4] && n.nav == 1;
    CHECK(aiNav);
    // Cars from another player are not the host's: ignored.
    float x5 = X(5);
    for (uint32_t t = 40; t < 50; ++t) {
        ++g_tick;
        HostCars(c, 3000 + t, 5000.0f, true, B);
        UHook(kHRacing)(nullptr, nullptr);
        Commit(g_racerMem[5]);
    }
    CHECK(X(5) < 1000 && X(5) >= x5);
    // A car that left the network state (the podium, or anything else) is not moved any more.
    SetTopState(3, 0x1a);
    SetTopState(4, 3);
    float x3 = X(3), x4 = X(4);
    for (uint32_t t = 50; t < 60; ++t) {
        ++g_tick;
        HostCars(c, 3000 + t, 60.0f, true);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    CHECK(X(3) == x3 && X(4) == x4 && X(2) > x3 - 100);  // the others still follow
    // A switch to Network21 still queued (top state slot 0): moved as before.
    *(void**)(g_controllers[3] + 0x228) = nullptr;
    ++g_tick;
    HostCars(c, 3060, 61.0f, true);
    UHook(kHRacing)(nullptr, nullptr);
    Commit(g_racerMem[3]);
    CHECK(X(3) != x3);
    SetTopState(3, 0x15);
    SetTopState(4, 0x15);
    // Items a network car picked up here are dropped (an All-Star kept by one locks everyone out);
    // not while it runs an All-Star, never ours.
    *(uint32_t*)(g_racerMem[2] + 0xc1c) = 0xb678e022;
    *(uint32_t*)(g_racerMem[0] + 0xc1c) = 0x1234;  // the host's car
    *(uint32_t*)(g_racerMem[6] + 0xc1c) = 0x1234;
    *(int32_t*)(g_racerMem[6] + 0x1ff0) = 2;         // running an All-Star
    *(uint32_t*)(g_racerMem[1] + 0xc1c) = 0x1234;  // ours
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_itemClears.size() == 2 && *(uint32_t*)(g_racerMem[2] + 0xc1c) == 0 && *(uint32_t*)(g_racerMem[0] + 0xc1c) == 0);
    CHECK(*(uint32_t*)(g_racerMem[6] + 0xc1c) == 0x1234 && *(uint32_t*)(g_racerMem[1] + 0xc1c) == 0x1234);
}

static std::vector<RaceCars> SentCars() {
    std::vector<RaceCars> out;
    for (const RaceNetMsg& m : g_sent) {
        RaceCars c;
        if (DecodeCars(m.data, m.size, c)) { CHECK(!m.reliable); out.push_back(c); }
    }
    return out;
}

static bool SentResults(RaceResults& r, int* count = nullptr) {
    int n = 0;
    for (const RaceNetMsg& m : g_sent)
        if (DecodeResults(m.data, m.size, r)) { CHECK(m.reliable); ++n; }
    if (count) *count = n;
    return n > 0;
}

// CATCH-UP (re\m6-rules\FINDINGS.md B): each game runs the console's human catch-up for its own car
// against every player's car: the leader slowed (racer+0xb00), the last boosted (inputs +0x4c).
void RaceTestSetCatchupAddresses(uintptr_t distance, uintptr_t nearCheck, uintptr_t arena, uintptr_t nearMetres, uintptr_t defaults);
static float g_distance[8];
static bool g_near = false;
static uint8_t g_arena = 0;
static float g_nearMetres = 350.0f, g_scaleDefaults[4] = {1, 1, 1, 1};
static int g_catchupOriginal = 0;
static float __fastcall FakeDistance(uint8_t* racer, void*) { return g_distance[(racer - g_racerMem[0]) / sizeof(g_racerMem[0])]; }
static bool __fastcall FakeNear(void*, void*, float metres) { CHECK(metres == 350.0f); return g_near; }
static float g_live[8];
static float __fastcall FakeNavPosition(uint8_t* racer, void*) { return g_live[(racer - g_racerMem[0]) / sizeof(g_racerMem[0])]; }
void RaceTestSetNavPosition(uintptr_t navPosition);
static void __fastcall FakeCatchupOriginal(uint8_t*, void*) { ++g_catchupOriginal; }

static void TestCatchup() {
    printf("catch-up: the leader slowed, the last boosted, against every player's car\n");
    RaceTestSetCatchupAddresses((uintptr_t)&FakeDistance, (uintptr_t)&FakeNear, (uintptr_t)&g_arena, (uintptr_t)&g_nearMetres,
                                (uintptr_t)&g_scaleDefaults);
    RaceContract c = HostRunning();  // CATCH-UP ON (the lobby's default)
    CHECK(c.catchup == 1);
    UHook(kHRacing)(nullptr, nullptr);  // the race clock runs
    static uint8_t state[0x80], controller[0x300], inputs[0x80], params[0x100], tracker[0x10];
    *(void**)(state + 0x10) = g_racerMem[0];  // this game's car
    *(void**)(state + 0x14) = controller;
    *(void**)(state + 0x18) = inputs;
    *(void**)(state + 0x1c) = tracker;
    *(void**)(controller + 0x250) = params;
    const float p[6] = {50, 150, 0.3f, -100, -300, 0.9f};  // catch-up range min/max, boost; slow-down range min/max, scale
    const uint32_t at[6] = {0x94, 0x98, 0x9c, 0xa0, 0xa4, 0xa8};
    for (int i = 0; i < 6; ++i) memcpy(params + at[i], &p[i], 4);
    auto Run = [&]() { ((void(__fastcall*)(uint8_t*, void*))RaceTestHook(kHCatchup))(state, nullptr); };
    auto Scale = [](int k) { return *(float*)(g_racerMem[k] + 0xb00); };
    auto Boost = [&]() { return *(float*)(inputs + 0x4c); };
    // Last, 200 m behind (past the range): the full boost.
    g_distance[0] = 100;
    g_distance[1] = 300;
    g_catchupOriginal = 0;
    Run();
    CHECK(g_catchupOriginal == 1 && Boost() == 0.3f && Scale(0) == 1.0f);
    g_distance[1] = 200;  // 100 m: halfway through the range
    Run();
    CHECK(fabsf(Boost() - 0.15f) < 1e-5f && Scale(0) == 1.0f);
    g_distance[1] = 120;  // 20 m: none
    Run();
    CHECK(Boost() == 0.0f);
    // Leading by 400 m: the full slow-down; by 200 m: halfway (0.9 * 0.5 + 0.5).
    g_distance[0] = 500;
    g_distance[1] = 100;
    Run();
    CHECK(Scale(0) == 0.9f && *(float*)(g_racerMem[0] + 0xb0c) == 0.9f && Boost() == 0.0f);
    g_distance[0] = 300;
    Run();
    CHECK(fabsf(Scale(0) - 0.95f) < 1e-5f);
    g_distance[0] = 150;  // 50 m: none
    Run();
    CHECK(Scale(0) == 1.0f);
    // The stock function's "keep the defaults" case, an arena, another car's state: defaults or untouched.
    g_distance[0] = 500;
    g_near = true;
    Run();
    CHECK(Scale(0) == 1.0f && Boost() == 0.0f);
    g_near = false;
    g_arena = 1;
    *(float*)(g_racerMem[0] + 0xb00) = 0.5f;
    Run();
    CHECK(Scale(0) == 0.5f);  // untouched
    g_arena = 0;
    *(void**)(state + 0x10) = g_racerMem[1];
    *(float*)(g_racerMem[1] + 0xb00) = 0.5f;
    Run();
    CHECK(Scale(1) == 0.5f);  // not this game's car: untouched
    *(void**)(state + 0x10) = g_racerMem[0];
    // The other player finished: alone on the track, no catch-up.
    Append(1, 1);
    Run();
    CHECK(Scale(0) == 1.0f && Boost() == 0.0f);
    // CATCH-UP OFF: only the game's own (inert) function.
    c = HostRunning(0);
    CHECK(c.catchup == 0);
    UHook(kHRacing)(nullptr, nullptr);
    g_distance[0] = 100;
    g_distance[1] = 300;
    *(float*)(inputs + 0x4c) = 7.0f;
    g_catchupOriginal = 0;
    Run();
    CHECK(g_catchupOriginal == 1 && Boost() == 7.0f);
    // The tick this game's car passes the line (re\m8-flag Q2): its cursor has moved (live 5 m past the line) but its lap record
    // hasn't counted the lap yet (the race update comes after), so 0x4e8740 reads it a lap short (5 m against the other car's 950 m).
    // Measured as of the last race update (cursor 995 m, racer+0xb84) it leads by 45 m: no boost, no slow-down.
    c = HostRunning();
    UHook(kHRacing)(nullptr, nullptr);
    static uint8_t sectors[0x300];
    *(float*)(sectors + 0x2f8) = 0;
    *(void**)(g_scene + 0x10e4) = sectors;
    *(float*)(g_graph + 0x64) = 1000;
    RaceTestSetNavPosition((uintptr_t)&FakeNavPosition);
    for (int k = 0; k < 2; ++k) *(uint32_t*)(g_records[k] + 0x74) = 3;  // both have crossings
    g_distance[0] = 5;
    g_live[0] = 5;
    *(float*)(g_racerMem[0] + 0xb84) = 995;
    g_distance[1] = 950;
    g_live[1] = 950;
    *(float*)(g_racerMem[1] + 0xb84) = 950;
    Run();
    CHECK(Boost() == 0.0f && Scale(0) == 1.0f);
    *(float*)(g_racerMem[0] + 0xb84) = 5;  // counted: really 945 m behind -> the full boost
    Run();
    CHECK(Boost() == 0.3f);
    *(void**)(g_scene + 0x10e4) = nullptr;
    *(float*)(g_graph + 0x64) = 0;
    for (int k = 0; k < 2; ++k) *(uint32_t*)(g_records[k] + 0x74) = 0;
}

// AI RACERS OFF (re\m6-rules\FINDINGS.md A): the bridge asks 2003(1) for the racer count, gets the
// players' number, and makes only their cars; everything else follows the count.
static uint8_t g_bridge[0x3000];
static FakeVm CountSite() {
    const uint8_t count[] = {0x51, 0x01, 0x32, 0x01, 0x03, 0x20, 0x01, 0x00, 0xcb, 0x03};
    memcpy(g_bridge + 0x20f7, count, sizeof(count));  // stock fe_game_bridge: INT8 1; NAT 2003 at 0x20f9
    return FakeVm{nullptr, g_bridge + 0x2101, g_bridge};
}
static void TestAiOff() {
    printf("AI racers off: only the players' cars\n");
    RaceTestSetHostAi(true);
    FakeVm cvm = CountSite();
    uint8_t t;
    uint32_t v;
    // Host.
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 0, 1, 1};
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c) && c.ai == 0);
    RaceLaunched(true);
    CHECK(Read2003(&cvm, 1, &t, &v).retValue == 2);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    CHECK(RaceTestPhase() == kCreating);
    Script0905(0x2, "STATE_Control_StartingGridAI", 0);  // the guest's car (made as an AI)
    CHECK(RaceTestPhase() == kCreated);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 2;
    uint8_t buf[kWireMax];
    UHook(kHWaiting)(nullptr, nullptr);  // hold A: 2 racers is right
    CHECK(RaceTestPhase() == kHoldA);
    Deliver(B, buf, EncodeLoaded(c.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(c.race, kStageCountdown, 1, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 2; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    g_localIdx = 0;
    g_sent.clear();
    // Its Cars messages carry only its own car (no AI); the results only the two players'.
    for (int i = 0; i < 4; ++i) {
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 2; ++k) Commit(g_racerMem[k]);
    }
    std::vector<RaceCars> cars = SentCars();
    CHECK(!cars.empty());
    for (const RaceCars& rc : cars) CHECK(rc.count == 1 && rc.cars[0].slot == 0);
    g_sent.clear();
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    RaceFinish theirs = {c.race, 1, 1, 0x00d00000, 0x2000};
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver && g_finishCount == 2);
    RaceResults r;
    CHECK(SentResults(r));
    CHECK(r.cars[0].reason == 1 && r.cars[0].rank == 0 && r.cars[1].reason == 1 && r.cars[1].rank == 1);
    for (int k = 2; k < 8; ++k) CHECK(r.cars[k].reason == 0 && r.cars[k].rank == kRankNone);
    // A bridge that makes a third car anyway: refused (the contract fails, the race runs as an offline one).
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 0, 1, 1};
    CHECK(RaceHostStart() == kStartOk);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    CHECK(RaceTestPhase() == kFailed);
    // Guest: no AI puppets (the old loop read a car that doesn't exist), the host's car shown, its results.
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.rules = {0, 3, 0, 0, 1, 1};
    CHECK(RaceHostStart() == kStartOk);
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    Reset();
    Lobby(B, false, 0, 1);
    Deliver(A, buf, EncodeContract(c, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    CHECK(Read2003(&cvm, 1, &t, &v).retValue == 2);
    Script0905(0x1, "STATE_Control_StartingGridAI", 0);  // the host's car
    Script0905(c.roster[1], "STATE_Control_StartingGridHuman", 2);
    CHECK(RaceTestPhase() == kCreated);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 2;
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldA && !RaceTestAiPuppets() && g_stateCalls.size() == 1);
    Deliver(A, buf, EncodeGo(c.race, kStageLoaded, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(c.race, kStageCountdown, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 2; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    g_localIdx = 1;
    for (uint32_t i = 0; i < 20; ++i) {
        ++g_tick;
        HostCars(c, 6000 + i, (float)i, false);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 2; ++k) Commit(g_racerMem[k]);
    }
    CHECK(X(0) > 5);  // the host's car moves here
    // The host's results for the two cars, equal times in the other order than this game's sort.
    RaceResults hr = {};
    hr.race = c.race;
    for (RaceResultEntry& e : hr.cars) e.rank = kRankNone;
    hr.cars[0] = {1, 1, 0x00c80000, 0x1000};
    hr.cars[1] = {1, 0, 0x00c80000, 0x1000};
    Deliver(A, buf, EncodeResults(hr, buf, sizeof(buf)));
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    UHook(kHWff)(wff, nullptr);
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver && g_finishCount == 2);
    CHECK(g_finishList[0][0] == 1 && g_finishList[1][0] == 0);  // the host's order
    CHECK(*(int32_t*)(g_records[1] + 0x80) == 0 && *(int32_t*)(g_records[0] + 0x80) == 1);
    CHECK(g_finishList[2][0] == 0 && g_finishList[2][4] == 0);  // nothing written past the two cars
    g_scene[0x1100] = 8;
}

// Drift+ off: the host's Start sets the contract flag while a keyboard player is in the lobby (the user's rule), not otherwise.
static void TestDriftPlusFlag() {
    printf("Drift+ off: the contract flag follows the players' devices\n");
    int32_t inputs[4][2] = {{kInputController, kInputController}, {kInputKeyboard, kInputController}, {kInputController, kInputKeyboard},
                            {kInputUnknown, kInputUnknown}};
    for (int t = 0; t < 4; ++t) {
        Reset();
        Lobby(A, true, 0, 1);
        g_snap.members[0].input = inputs[t][0];
        g_snap.members[1].input = inputs[t][1];
        CHECK(RaceHostStart() == kStartOk);
        RaceContract c = {};
        CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
        CHECK(((c.flags & kContractNoDriftPlus) != 0) == (t == 1 || t == 2));
        RaceCancel(kAbortQuit);
    }
}

static void TestHostAiHost() {
    printf("host's AI: the host sends its cars and the results\n");
    RaceTestSetHostAi(true);
    RaceContract c = HostRunning();
    CHECK(c.flags == kContractHostAi && !RaceTestAiPuppets());
    for (int k = 0; k < 8; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    for (uint32_t t = 0; t < 10; ++t) {
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) {
            *(float*)(g_racerMem[k] + 0xc0) = (float)(t + 10 * k) + (k == 3 && t >= 6 ? 100.0f : 0.0f);  // car 3 respawns at tick 6
            Commit(g_racerMem[k]);
        }
    }
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);  // the last tick's cars go out
    std::vector<RaceCars> cars = SentCars();
    CHECK(cars.size() == 10 && CountSent(kMsgPose) == 0);
    int snaps = 0;
    for (const RaceCars& rc : cars) {
        bool even = rc.tick % 2 == 0;
        CHECK(rc.count == (even ? 7 : 1) && rc.cars[0].slot == 0);
        for (int i = 0; i < rc.count; ++i) {
            CHECK(rc.cars[i].slot != 1);  // never the guest's car
            if (rc.cars[i].flags & kPoseSnap) { ++snaps; CHECK(rc.cars[i].slot == 3); }
        }
    }
    CHECK(snaps == 1);
    // Results: once the race is over, every car's final time and reason, once.
    g_sent.clear();
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(!g_raceOver);
    uint8_t buf[kWireMax];
    RaceFinish theirs = {c.race, 1, 1, 0x00d00000, 0x2000};
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver);
    RaceResults r;
    int count = 0;
    CHECK(SentResults(r, &count) && count == 1 && r.race == c.race);
    CHECK(r.cars[0].reason == 1 && r.cars[0].time == 0x00c80000 && r.cars[0].best == 0x1000);
    CHECK(r.cars[1].reason == 1 && r.cars[1].time == 0x00d00000 && r.cars[1].best == 0x2000);
    for (int k = 2; k < 8; ++k) CHECK(r.cars[k].reason == 6 && r.cars[k].time == 0xa00000u + k * 0x1000);
    // The places: the host and the guest (finished), then the AI that didn't finish (DNF, last as on the console) by
    // their estimates.
    CHECK(r.cars[0].rank == 0 && r.cars[1].rank == 1 && r.cars[2].rank == 2 && r.cars[7].rank == 7);
    UHook(kHWff)(wff, nullptr);
    Commit(g_racerMem[0]);
    CHECK(SentResults(r, &count) && count == 1);
    // The guest far behind: no grace at the timer's end (the host estimates everyone and sends that).
    HostRunning();
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    *(int32_t*)(wff + 0x10) = -1;
    for (int i = 0; i < 1500 + 5 && !g_raceOver; ++i) {
        ++g_tick;
        UHook(kHWff)(wff, nullptr);
    }
    CHECK(g_raceOver && g_timerElapsed >= g_timerLimit);
    CHECK(SentResults(r) && r.cars[1].reason == 6 && r.cars[1].time == 0x999999);
    // Queue full when the race ends: retried at the next car commit.
    c = HostRunning();
    g_finishLocalNow = 0x00c80000;
    UHook(kHRacing)(nullptr, nullptr);
    theirs.race = c.race;
    Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    *(int32_t*)(wff + 0x10) = -1;
    g_sendFails = 1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver && !SentResults(r));
    Commit(g_racerMem[0]);
    CHECK(SentResults(r, &count) && count == 1);
}

// The host's results: its car first, ours last, the AI 2..7 between in order (with `tie`, AI 5 and 6
// have the same time and the host placed 5 first).
static void DeliverResults(const RaceContract& c, uint64_t from, uint32_t guestReason, uint32_t guestTime, bool tie = false) {
    RaceResults r = {};
    r.race = c.race;
    r.cars[0] = {1, 0, 0x00b00000, 0x1000};
    // The host's places as its race end gives them: the finishers by time, then the DNF rows (DnfLast).
    bool guestDone = guestReason != 6;
    r.cars[1] = {(uint8_t)guestReason, (uint8_t)(guestDone ? 4 : 7), guestTime, 0x1100};
    for (int k = 2; k < 8; ++k)
        r.cars[k] = {(uint8_t)(k < 5 ? 1 : 6), (uint8_t)(k < 5 || guestDone ? k - (k < 5 ? 1 : 0) : k - 1), 0x00b80000u + k * 0x10000, 0x1200};
    if (tie) r.cars[6].time = r.cars[5].time;
    uint8_t buf[kWireMax];
    Deliver(from, buf, EncodeResults(r, buf, sizeof(buf)));
}

static void TestHostAiResults() {
    printf("host's AI: the guest's race ends with the host's results\n");
    uint8_t buf[kWireMax];
    uint8_t wff[0x20] = {};
    // Both players finish: the guest waits for the host's results, then shows them for every car.
    RaceContract c = GuestRunning();
    RaceFinish host = {c.race, 0, 1, 0x00b00000, 0x1000};
    Deliver(A, buf, EncodeFinished(host, buf, sizeof(buf)));
    UHook(kHRacing)(nullptr, nullptr);
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);  // the host finished first: our 30 s timer
    CHECK(g_timerStarts == 1);
    g_finishLocalNow = 0x00c00000;
    UHook(kHWff)(wff, nullptr);
    for (int i = 0; i < 100; ++i) {
        ++g_tick;
        UHook(kHWff)(wff, nullptr);
    }
    CHECK(!g_raceOver && g_humansSeen == 2);  // both done, held for the host's results
    std::vector<RaceFinish> mine = SentFinishes();
    CHECK(mine.size() == 1 && mine[0].slot == 1 && mine[0].time == 0x00c00000);
    DeliverResults(c, B, 1, 0x00c00000);  // not from the host: ignored
    UHook(kHWff)(wff, nullptr);
    CHECK(!g_raceOver);
    DeliverResults(c, A, 1, 0x00c00000);
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver);
    for (int k = 2; k < 8; ++k) {
        CHECK(*(uint32_t*)(g_records[k] + 0x68) == 0x00b80000u + k * 0x10000 && *(uint32_t*)(g_records[k] + 0x60) == 0x1200);
        CHECK(ListReason(k) && *ListReason(k) == (uint32_t)(k < 5 ? 1 : 6));
        CHECK(*(uint32_t*)(g_records[k] + 0x74) == *(uint32_t*)(g_records[k] + 0x84));
    }
    CHECK(g_added.size() == 1 + 6);  // the host's Finished, then the 6 AI (ours and the host's were in the list)
    CHECK(SentFinishes().size() == 1);
    // The race-end timer runs out with our player still racing: held until the host's estimate comes.
    c = GuestRunning();
    host.race = c.race;
    Deliver(A, buf, EncodeFinished(host, buf, sizeof(buf)));
    *(int32_t*)(wff + 0x10) = -1;
    for (int i = 0; i < 1500 + 100; ++i) {
        ++g_tick;
        UHook(kHWff)(wff, nullptr);
    }
    CHECK(g_timerOn && !g_raceOver && g_timerElapsed == g_timerLimit - 1);
    DeliverResults(c, A, 6, 0x01400000);
    ++g_tick;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_raceOver && *(uint32_t*)(g_records[1] + 0x68) == 0x01400000 && ListReason(1) && *ListReason(1) == 6);
    CHECK(SentFinishes().empty());  // the host decided our result: not sent back
    // The results never come: after 10 s this game ends with its own.
    c = GuestRunning();
    host.race = c.race;
    Deliver(A, buf, EncodeFinished(host, buf, sizeof(buf)));
    g_finishLocalNow = 0x00c00000;
    *(int32_t*)(wff + 0x10) = -1;
    int ticks = 0;
    for (; ticks < 700 && !g_raceOver; ++ticks) {
        ++g_tick;
        UHook(kHWff)(wff, nullptr);
    }
    CHECK(g_raceOver && ticks > 490 && ticks < 520 && *(uint32_t*)(g_records[4] + 0x68) == 0xa04000);
    // Results after this game's race ended: not used (no second sort).
    DeliverResults(c, A, 1, 0x00c00000);
    UHook(kHWff)(wff, nullptr);
    CHECK(*(uint32_t*)(g_records[4] + 0x68) == 0xa04000);
    // Equal times: this game's sort put them the other way round; the rows follow the host's places.
    for (int reversed = 0; reversed < 2; ++reversed) {
        c = GuestRunning();
        host.race = c.race;
        g_tiesReversed = reversed != 0;
        Deliver(A, buf, EncodeFinished(host, buf, sizeof(buf)));
        g_finishLocalNow = 0x00c00000;
        *(int32_t*)(wff + 0x10) = -1;
        for (int i = 0; i < 5; ++i) UHook(kHWff)(wff, nullptr);
        CHECK(!g_raceOver);
        DeliverResults(c, A, 1, 0x00c00000, true);
        UHook(kHWff)(wff, nullptr);
        CHECK(g_raceOver && g_finishCount == 8);
        for (int r = 0; r < 8; ++r) {
            int k = g_finishList[r][0];
            CHECK(*(int32_t*)(g_records[k] + 0x80) == r);
            CHECK(k == (r == 0 ? 0 : r < 4 ? r + 1 : r == 4 ? 1 : r));  // host, AI 2..4, us, then the DNF AI 5..7 (5 before 6)
        }
    }
    g_tiesReversed = false;
}

static void TestHostAiHandBack() {
    printf("host's AI: the host leaves\n");
    // Before GO (countdown): the AI go back to the grid AI state.
    RaceContract c = GuestRunning();
    g_stateCalls.clear();
    g_snap.members[0] = g_snap.members[1];
    g_snap.memberCount = 1;
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(!RaceTestAiPuppets() && g_stateCalls.size() == 7);  // the AI, and the host's car (M6)
    for (auto& s : g_stateCalls) CHECK(s.second == 0x18);
    for (int k = 2; k < 8; ++k) CHECK(*(uint32_t*)(g_racerMem[k] + 0x9e4) == 0x18);
    // During the race: the AI racing state; no more poses written; the race end doesn't wait.
    c = GuestRunning();
    for (uint32_t t = 0; t < 20; ++t) {
        ++g_tick;
        HostCars(c, 100 + t, (float)t, true);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    g_stateCalls.clear();
    g_snap.members[0] = g_snap.members[1];
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(!RaceTestAiPuppets() && g_stateCalls.size() == 7);
    for (auto& s : g_stateCalls) CHECK(s.second == 3);
    *(float*)(g_racerMem[4] + 0xc0) = -3;
    HostCars(c, 200, 50.0f, true);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    Commit(g_racerMem[4]);
    CHECK(X(4) == -3);
    g_finishLocalNow = 0x00c00000;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_raceOver);  // nobody left to wait for
    // The host leaves after this game's race is over (results, podium): no more poses, but the cars are
    // left in the states they finished in.
    c = GuestRunning();
    g_raceOver = 1;
    g_stateCalls.clear();
    g_snap.members[0] = g_snap.members[1];
    g_snap.memberCount = 1;
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = 0;
    UHook(kHWff)(wff, nullptr);
    CHECK(!RaceTestAiPuppets() && g_stateCalls.empty());
    // A restart (not shared) also hands the AI back.
    GuestRunning();
    g_stateCalls.clear();
    UHook(kHWaitingNoIntro)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kFailed && !RaceTestAiPuppets() && g_stateCalls.size() == 6);
    RaceTestSetHostAi(false);
}

// The 1-player finish panel asks 0913 for creation index 0: on a guest it gets the guest's own car.
static int g_0913Arg = -1;
static void __cdecl Fake0913(void*, NativeCall* d) { g_0913Arg = (int)d->values[0]; }

// ---------------------------------------------------------------- items (M5, notes/ITEMS-SYNC.md)
// The scene's event manager as the game's: the post (0x5c0820) runs an event at once while the window
// has room (+0x12 < 16) and keeps it, else frees it unrun. Executors: kind 0 gives the item (racer
// +0xc1c), and 0xf005c29d fires at once (a nested kind-1 post through the post slot, as AddGadget ->
// 0x538bf0); kinds 1/2 fire (the held item goes; the All-Star sets the racer's All-Star phase 2).
static uint8_t g_mgr[0x100];
static uint32_t g_fakeVtable = 0x6b9a84;
struct Ran {
    int32_t kind;
    int racer;
    uint32_t gadget;
    std::vector<uint8_t> bytes;
    size_t sentBefore;  // messages sent when it ran
    float throwSeen;    // the firing car's controller +0xb8 while it ran
};
static std::vector<Ran> g_ran;
static std::vector<uint8_t*> g_kept;
static int g_freed = 0, g_allocs = 0, g_msgCalls = 0;
static size_t EventSize(int32_t kind) { return kind == 1 || kind == 2 || kind == 3 || kind == 4 ? 0xe0 : kind == 6 || kind == 0xf ? 0xc : kind == 10 || kind == 11 ? 0x10 : 0x14; }
static void* __stdcall FakeAlloc(int32_t kind) {
    if (kind < 0 || kind > 0x11) return nullptr;
    uint8_t* ev = new uint8_t[EventSize(kind)]();
    *(uint32_t**)ev = &g_fakeVtable;
    *(int32_t*)(ev + 4) = kind;
    if (kind >= 1 && kind <= 4) {
        const uint32_t at[4] = {0xa8, 0xb8, 0xc4, 0xd0};
        for (int i = 0; i < 4; ++i) *(uint32_t*)(ev + at[i]) = 0x6b9a88 + i;  // the quantizer vtables
    }
    ++g_allocs;
    return ev;
}
static void __stdcall FakeFree(void* ev) {
    ++g_freed;
    delete[] (uint8_t*)ev;
}
static void* __cdecl FakeProjectileType(uint32_t type) { return type == 0xdead ? nullptr : (void*)&g_fakeVtable; }
typedef void(__fastcall* PostHook)(void*, void*, uint8_t*);
static void GamePost(uint8_t* ev) { ((PostHook)RaceTestHook(kHEventPost))(g_mgr, nullptr, ev); }
static void __fastcall FakePost(void* mgr, void*, uint8_t* ev) {
    uint8_t& n = ((uint8_t*)mgr)[0x12];
    if (n >= 0x10) { FakeFree(ev); return; }
    ++n;
    g_kept.push_back(ev);
    int32_t kind = *(int32_t*)(ev + 4);
    int k = ev[8];
    uint32_t gadget = *(uint32_t*)(ev + 0xc);
    Ran r = {kind, k, gadget, std::vector<uint8_t>(ev, ev + EventSize(kind)), g_sent.size(), *(float*)(g_controllers[k & 7] + 0xb8)};
    g_ran.push_back(r);
    if (kind > 2 || k >= 8) return;
    if (kind == 0) {
        *(uint32_t*)(g_racerMem[k] + 0xc1c) = gadget;
        if (gadget == 0xf005c29d) {  // fired as soon as it is given
            uint8_t* fire = (uint8_t*)FakeAlloc(1);
            fire[8] = (uint8_t)k;
            *(uint32_t*)(fire + 0xc) = gadget;
            GamePost(fire);
        }
    } else {
        *(uint32_t*)(g_racerMem[k] + 0xc1c) = 0;
        if (gadget == 0xb678e022) *(int32_t*)(g_racerMem[k] + 0x1ff0) = 2;
    }
}
static int __fastcall FakeRacerMessage(void*, void*, uint8_t*) { ++g_msgCalls; return 7; }
typedef int(__fastcall* MessageHook)(void*, void*, uint8_t*);
static int Message(int k, uint32_t hash) {
    uint8_t inner[8] = {};
    *(uint32_t*)(inner + 4) = hash;
    uint8_t msg[0x14] = {};
    *(uint8_t**)(msg + 0xc) = inner;
    return ((MessageHook)RaceTestHook(kHRacerMessage))(g_racerMem[k], nullptr, msg);
}

// An event as the game builds it: kind 0 give (gadget, count 0); kinds 1/2 fire with a launch frame.
static uint8_t* GameEvent(int32_t kind, int k, uint32_t gadget) {
    uint8_t* ev = (uint8_t*)FakeAlloc(kind);
    ev[8] = (uint8_t)k;
    *(uint32_t*)(ev + 0xc) = gadget;
    if (kind == 1 || kind == 2) {
        *(uint32_t*)(ev + 0x20) = 0x1234;  // projectile type
        *(uint16_t*)(ev + 0x24) = 0x77;    // serial: the projectile's id on every PC
        for (int i = 0x40; i < 0xa0; ++i) ev[i] = (uint8_t)(i * 3);
        ev[0xdc] = 0x5a;
    }
    return ev;
}

static std::vector<RaceEvent> SentEvents() {
    std::vector<RaceEvent> out;
    for (const RaceNetMsg& m : g_sent) {
        RaceEvent e;
        if (DecodeEvent(m.data, m.size, e)) { CHECK(m.reliable && m.peer == 0); out.push_back(e); }
    }
    return out;
}

// An event of car `slot` from `from`, as its game sends it.
static void DeliverEvent(uint64_t from, const RaceContract& c, int32_t kind, int slot, uint32_t gadget, uint32_t tick, uint8_t bodySlot = 0xff,
                         uint32_t type = 0x1234) {
    RaceEvent e = {};
    e.race = c.race;
    e.tick = tick;
    e.kind = (uint8_t)kind;
    e.slot = (uint8_t)slot;
    e.size = EventBodySize(e.kind);
    if (PseudoEvent((uint8_t)kind)) {
        memcpy(e.body, &gadget, 4);  // (the phase, or the reaction's node)
    } else {
        e.body[0] = bodySlot == 0xff ? (uint8_t)slot : bodySlot;
        memcpy(e.body + 4, &gadget, 4);
        if (kind == 1 || kind == 2) {
            memcpy(e.body + 0x18, &type, 4);
            e.body[0x1c] = 0x77;
            for (int i = 0x40; i < 0xa0; ++i) e.body[i - 8] = (uint8_t)(i * 5);
            float t = 0.25f;
            memcpy(e.body + 0xc, &t, 4);  // the owner's controller +0xb8
        }
    }
    uint8_t b[kWireMax];
    size_t n = EncodeEvent(e, b, sizeof(b));
    CHECK(n != 0);
    Deliver(from, b, n);
}

static void ItemsTick() {
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
}

static void ResetItems() {
    for (uint8_t* ev : g_kept) delete[] ev;
    g_kept.clear();
    g_ran.clear();
    memset(g_mgr, 0, sizeof(g_mgr));
    *(void**)(g_scene + 0x1374) = g_mgr;
    g_freed = g_allocs = g_msgCalls = 0;
}

// A pose of car `slot` at `tick` (its owner's clock), x along the track.
static void DeliverPose(uint64_t from, const RaceContract& c, int slot, uint32_t tick, float x) {
    RacePose p = {};
    p.race = c.race;
    p.slot = (uint8_t)slot;
    p.tick = tick;
    p.pos[0] = x;
    p.rot[3] = 1;
    p.nav = kNavNone;
    uint8_t b[kWireMax];
    Deliver(from, b, EncodePose(p, b, sizeof(b)), false);
}

static void TestItemsHost() {
    printf("items: host\n");
    RaceTestSetHostAi(true);
    RaceTestSetItems(true);
    RaceContract c = HostRunning();
    CHECK(c.flags == (kContractHostAi | kContractItems));
    ResetItems();
    ItemsTick();
    // Our car fires: it runs here and goes out first (reliable), with the bytes taken before it ran,
    // the quantizer vtables cleared and our controller's +0xb8 at +0x14.
    *(float*)(g_controllers[0] + 0xb8) = 0.75f;
    GamePost(GameEvent(1, 0, 0x5f724da9));
    std::vector<RaceEvent> sent = SentEvents();
    CHECK(g_ran.size() == 1 && g_ran[0].kind == 1 && g_ran[0].racer == 0 && g_ran[0].sentBefore == 1);
    CHECK(sent.size() == 1 && sent[0].kind == 1 && sent[0].slot == 0 && sent[0].tick == g_tick && sent[0].size == 0xd8);
    if (sent.size() == 1) {
        const RaceEvent& e = sent[0];
        CHECK(e.body[0] == 0 && *(uint32_t*)(e.body + 4) == 0x5f724da9 && *(uint32_t*)(e.body + 0x18) == 0x1234 && e.body[0x1c] == 0x77);
        CHECK(*(float*)(e.body + 0xc) == 0.75f);
        for (uint32_t v : {0xa8u, 0xb8u, 0xc4u, 0xd0u}) CHECK(*(uint32_t*)(e.body + v - 8) == 0);
        CHECK(e.body[0x50 - 8] == (uint8_t)(0x50 * 3) && e.body[0xdc - 8] == 0x5a);
    }
    // The host's AI: its gives and fires go out too.
    GamePost(GameEvent(0, 3, 0x7711877d));
    sent = SentEvents();
    CHECK(g_ran.size() == 2 && sent.size() == 2 && sent[1].kind == 0 && sent[1].slot == 3 && *(uint32_t*)(sent[1].body + 4) == 0x7711877d);
    // The guest's car picked something up here: that's the guest's game's to decide. Dropped, not run, not sent.
    int freed = g_freed;
    GamePost(GameEvent(0, 1, 0x7711877d));
    GamePost(GameEvent(2, 1, 0x7711877d));
    CHECK(g_ran.size() == 2 && g_freed == freed + 2 && SentEvents().size() == 2);
    // A full window frees the event unrun: nothing to send.
    g_mgr[0x12] = 0x10;
    GamePost(GameEvent(1, 0, 0x5f724da9));
    CHECK(g_ran.size() == 2 && g_freed == freed + 3 && SentEvents().size() == 2);
    g_mgr[0x12] = 0;
    // An item fired as soon as it is given: the give goes out before the fire it causes.
    GamePost(GameEvent(0, 0, 0xf005c29d));
    sent = SentEvents();
    CHECK(sent.size() == 4 && sent[2].kind == 0 && sent[3].kind == 1 && *(uint32_t*)(sent[3].body + 4) == 0xf005c29d);
    CHECK(g_ran.size() == 4 && g_ran[2].kind == 0 && g_ran[3].kind == 1);
    // Other kinds (hits, homing) run here as before and are not sent.
    uint8_t* hit = (uint8_t*)FakeAlloc(5);
    hit[8] = 1;
    GamePost(hit);
    CHECK(g_ran.size() == 5 && SentEvents().size() == 4);
    // Item boxes: none for the guest's car here (1, as the stock early-outs); ours and other messages as stock.
    CHECK(Message(1, 0x06cd52a4) == 1 && g_msgCalls == 0);
    CHECK(Message(0, 0x06cd52a4) == 7 && Message(1, 0x19aedfcd) == 7 && Message(3, 0x06cd52a4) == 7 && g_msgCalls == 3);
    // The guest's car keeps what its owner gave it (no more item drops for network cars).
    *(uint32_t*)(g_racerMem[1] + 0xc1c) = 0x7711877d;
    ItemsTick();
    CHECK(g_itemClears.empty() && *(uint32_t*)(g_racerMem[1] + 0xc1c) == 0x7711877d);

    // From the guest: its car's fire runs here (no poses yet: at once), as a new event with its bytes,
    // our vtables kept, the owner's controller value lent for the call; never sent back.
    g_sent.clear();
    size_t ran = g_ran.size();
    *(float*)(g_controllers[1] + 0xb8) = 0.5f;
    DeliverEvent(B, c, 1, 1, 0x5f724da9, 7000);
    ItemsTick();
    CHECK(g_ran.size() == ran + 1 && SentEvents().empty());
    if (g_ran.size() == ran + 1) {
        const Ran& r = g_ran.back();
        CHECK(r.kind == 1 && r.racer == 1 && r.gadget == 0x5f724da9 && r.throwSeen == 0.25f);
        CHECK(*(uint32_t*)(r.bytes.data() + 0x20) == 0x1234 && r.bytes[0x24] == 0x77 && r.bytes[0x50] == (uint8_t)(0x50 * 5));
        const uint32_t at[4] = {0xa8, 0xb8, 0xc4, 0xd0};
        for (int i = 0; i < 4; ++i) CHECK(*(uint32_t*)(r.bytes.data() + at[i]) == 0x6b9a88u + i);
        CHECK(*(uint32_t**)r.bytes.data() == &g_fakeVtable && *(uint32_t*)(r.bytes.data() + 0x14) == 0);  // head ours; +0x14 not copied
    }
    CHECK(*(float*)(g_controllers[1] + 0xb8) == 0.5f);  // given back
    // Refused: a car the guest doesn't own (ours, an AI), a body naming another car, the roulette, a
    // projectile type this game lacks, an unknown kind.
    DeliverEvent(B, c, 1, 0, 0x5f724da9, 7001);
    DeliverEvent(B, c, 0, 3, 0x7711877d, 7001);
    DeliverEvent(B, c, 0, 1, 0x7711877d, 7001, 0);
    DeliverEvent(B, c, 0, 1, 0x85d1e53c, 7001);
    DeliverEvent(B, c, 1, 1, 0x5f724da9, 7001, 0xff, 0xdead);
    DeliverEvent(B, c, 0, 1, 0x12345678, 7001);
    for (int32_t kind : {3, 4, 5, 6, 10, 11}) DeliverEvent(B, c, kind, 1, 0x5f724da9, 7001);  // kinds each game decides itself
    ItemsTick();
    CHECK(g_ran.size() == ran + 1);
    // A give to the guest's car holding another item: that one goes first (it would refuse the new one).
    *(uint32_t*)(g_racerMem[1] + 0xc1c) = 0x11a9bc48;
    DeliverEvent(B, c, 0, 1, 0x7711877d, 7002);
    ItemsTick();
    CHECK(g_itemClears.size() == 1 && g_itemClears[0] == g_racerMem[1] && g_ran.back().kind == 0 && g_ran.back().gadget == 0x7711877d);
    // Its fire-at-once item: the fire our copy makes is dropped (the owner's own fire comes from it).
    ran = g_ran.size();
    freed = g_freed;
    DeliverEvent(B, c, 0, 1, 0xf005c29d, 7003);
    ItemsTick();
    CHECK(g_ran.size() == ran + 1 && g_ran.back().kind == 0 && g_freed == freed + 1 && SentEvents().empty());
    // The window nearly full (12 of 16): our player's own items keep their room; it runs next tick.
    g_mgr[0x12] = 12;
    ran = g_ran.size();
    DeliverEvent(B, c, 1, 1, 0x7711877d, 7004);
    ItemsTick();
    CHECK(g_ran.size() == ran);
    g_mgr[0x12] = 0;
    ItemsTick();
    CHECK(g_ran.size() == ran + 1);

    // Timing: once the guest's car is shown from its poses, its events run when it is shown at that tick.
    for (uint32_t t = 0; t < 40; ++t) {
        DeliverPose(B, c, 1, 9000 + t, (float)t);
        ItemsTick();
        Commit(g_racerMem[1]);
    }
    ran = g_ran.size();
    DeliverEvent(B, c, 1, 1, 0x53eb2f26, 9039);  // its newest tick: shown a few ticks from now
    int waited = 0;
    for (uint32_t t = 40; t < 60 && g_ran.size() == ran; ++t) {
        DeliverPose(B, c, 1, 9000 + t, (float)t);
        ItemsTick();
        Commit(g_racerMem[1]);
        ++waited;
    }
    printf("  the guest's fire ran %d ticks after it arrived (when its car was shown at that moment)\n", waited);
    CHECK(g_ran.size() == ran + 1 && waited >= 2 && waited <= 12);
    // Far ahead of what is shown: it runs at most 0.5 s after arrival (counted late).
    ran = g_ran.size();
    DeliverEvent(B, c, 0, 1, 0x7711877d, 9500);
    waited = 0;
    for (uint32_t t = 60; t < 100 && g_ran.size() == ran; ++t) {
        DeliverPose(B, c, 1, 9000 + t, (float)t);
        ItemsTick();
        Commit(g_racerMem[1]);
        ++waited;
    }
    CHECK(g_ran.size() == ran + 1 && waited >= 25 && waited <= 27);

    // All-Star: ours goes out phase by phase (not the 0 -> 1 of the start); the guest's copy runs until its owner's ends.
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 1;
    ItemsTick();
    CHECK(SentEvents().empty());
    for (int32_t phase : {2, 3, 4, 5, 1}) {
        *(int32_t*)(g_racerMem[0] + 0x1ff0) = phase;
        ItemsTick();
    }
    sent = SentEvents();
    CHECK(sent.size() == 5);
    const uint32_t phases[5] = {2, 3, 4, 5, 1};
    for (size_t i = 0; i < sent.size() && i < 5; ++i) CHECK(sent[i].kind == kEventAllStarPhase && sent[i].slot == 0 && *(uint32_t*)sent[i].body == phases[i]);
    *(uint32_t*)(g_racerMem[1] + 0xc1c) = 0;
    DeliverEvent(B, c, 1, 1, 0xb678e022, 9050);  // (its car is shown past that moment)
    ItemsTick();
    float* length = (float*)(g_racerMem[1] + 0x1ff0 + 0x30);
    CHECK(*(int32_t*)(g_racerMem[1] + 0x1ff0) == 2 && length[0] == 60.0f && length[3] == 60.0f);
    DeliverEvent(B, c, kEventAllStarPhase, 1, 5, 9051);  // the owner's is over
    ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 0);  // still phase 2 here: wait (its first tick would clear the flag)
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 1);
    g_racerMem[1][0x1ff0 + 0x4b] = 0;
    ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 0);  // once
    // The guest leaves in its next All-Star: its copy here ends; its later events don't run.
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    ran = g_ran.size();
    DeliverEvent(B, c, 1, 1, 0x7711877d, 99999);
    uint8_t b[kWireMax];
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    ItemsTick();
    ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 1 && g_ran.size() == ran);
    // Its car is this game's AI now (M6): it keeps what it holds and uses it like the other AI.
    *(uint32_t*)(g_racerMem[1] + 0xc1c) = 0xb678e022;
    size_t clears = g_itemClears.size();
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 1;
    ItemsTick();
    CHECK(g_itemClears.size() == clears && *(uint32_t*)(g_racerMem[1] + 0xc1c) == 0xb678e022);
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);  // the summary line
    RaceTestSetItems(false);
    RaceTestSetHostAi(false);
}

static void TestItemsGuest() {
    printf("items: guest with the host's AI\n");
    RaceTestSetItems(true);
    RaceContract c = GuestRunning();
    CHECK(c.flags == (kContractHostAi | kContractItems));
    ResetItems();
    // The host's car shown from its Cars; the host's events for its AI run with its car's clock.
    for (uint32_t t = 0; t < 30; ++t) {
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);
        ItemsTick();
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    DeliverEvent(A, c, 0, 4, 0x26e96da5, 3029);
    int waited = 0;
    for (uint32_t t = 30; t < 50 && g_ran.empty(); ++t) {
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);
        ItemsTick();
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
        ++waited;
    }
    CHECK(g_ran.size() == 1 && g_ran[0].racer == 4 && g_ran[0].gadget == 0x26e96da5 && waited >= 2 && waited <= 12);
    // Our car's events go out; the AI's own pickups here are the host's to decide (dropped).
    GamePost(GameEvent(1, 1, 0x53eb2f26));
    int freed = g_freed;
    GamePost(GameEvent(0, 5, 0x7711877d));
    std::vector<RaceEvent> sent = SentEvents();
    CHECK(sent.size() == 1 && sent[0].slot == 1 && g_freed == freed + 1 && g_ran.size() == 2);
    CHECK(Message(5, 0x06cd52a4) == 1 && Message(1, 0x06cd52a4) == 7);
    // The host leaves: the AI drive themselves here again, and their items are this game's (not sent).
    uint8_t b[kWireMax];
    Deliver(A, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    ItemsTick();
    CHECK(!RaceTestAiPuppets());
    g_sent.clear();
    GamePost(GameEvent(0, 5, 0x7711877d));
    CHECK(g_ran.size() == 3 && g_ran.back().racer == 5 && SentEvents().empty());
    CHECK(Message(5, 0x06cd52a4) == 7);
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);
    RaceTestSetItems(false);
    RaceTestSetHostAi(false);
}

// Driver reactions: the owner sends each reaction node its cars' drivers start; the others play it on their
// copy of the car once it can react (else dropped after 0.5 s), and that copy's own reactions never pop.
struct FakeAnim {
    uint8_t ca[0x100];
    uint8_t runner[0x40];
    uint8_t inst[2][0x100];
    uint8_t node[2][0x10];
    uint8_t rc[0xc0];
};
static FakeAnim g_anim[8];
static std::vector<std::pair<void*, uint32_t>> g_played;
static bool g_playResult = true;
static bool __fastcall FakePlayReaction(void* ca, void*, uint32_t node) {
    g_played.push_back({ca, node});
    if (g_playResult) ((uint8_t*)ca)[0] |= 0x40;
    return g_playResult;
}

// Car k with an animation whose active list holds `newest` then `older` (0: none), base state 4, a free instance.
static void Anim(int k, uint32_t newest, uint32_t older = 0) {
    FakeAnim& a = g_anim[k];
    memset(&a, 0, sizeof(a));
    *(void**)(a.ca + 0xc4) = a.runner;
    *(int32_t*)a.runner = 4;
    *(uint32_t*)(a.runner + 0x1c) = 1;
    *(void**)(a.runner + 0x20) = newest ? a.inst[0] : nullptr;
    *(void**)(a.inst[0] + 0x88) = a.node[0];
    *(uint32_t*)(a.node[0] + 0xc) = newest;
    if (older) {
        *(void**)(a.inst[0] + 0x8c) = a.inst[1];
        *(void**)(a.inst[1] + 0x88) = a.node[1];
        *(uint32_t*)(a.node[1] + 0xc) = older;
    }
    *(void**)(g_racerMem[k] + 0x2044) = a.ca;
    *(void**)(g_racerMem[k] + 0x2040) = a.rc;
}

static std::vector<RaceEvent> SentReactions() {
    std::vector<RaceEvent> out;
    for (const RaceEvent& e : SentEvents())
        if (e.kind == kEventReaction) out.push_back(e);
    return out;
}

static void TestReactions() {
    printf("reactions: host sends, plays the guest's\n");
    RaceTestSetHostAi(true);
    RaceTestSetItems(true);
    RaceTestSetReactionAddress((uintptr_t)&FakePlayReaction);
    RaceContract c = HostRunning();
    ResetItems();
    g_played.clear();
    for (int k = 0; k < 8; ++k) Anim(k, 0);
    ItemsTick();
    // Our driver starts a Showboat (the flag rises; the newest active node is the reaction, an older one behind it).
    Anim(0, 0x1b13cf01, 0x46770a3a);
    g_anim[0].ca[0] = 0x40;
    ItemsTick();
    std::vector<RaceEvent> sent = SentReactions();
    CHECK(sent.size() == 1 && sent[0].slot == 0 && *(uint32_t*)sent[0].body == 0x1b13cf01 && sent[0].tick == g_tick);
    ItemsTick();  // still playing: once
    CHECK(SentReactions().size() == 1);
    g_anim[0].ca[0] = 0;
    ItemsTick();
    *(uint32_t*)(g_anim[0].node[0] + 0xc) = 0x12345678;  // a node outside the reaction set (and the older one gone)
    *(void**)(g_anim[0].inst[0] + 0x8c) = nullptr;
    g_anim[0].ca[0] = 0x40;
    ItemsTick();
    CHECK(SentReactions().size() == 1);
    // The host's AI too; not the guest's car (its own game sends that one).
    Anim(3, 0xf27a8e21);
    g_anim[3].ca[0] = 0x40;
    Anim(1, 0x7cad4e8c);
    g_anim[1].ca[0] = 0x40;
    ItemsTick();
    sent = SentReactions();
    CHECK(sent.size() == 2 && sent[1].slot == 3 && *(uint32_t*)sent[1].body == 0xf27a8e21);
    // No animation object, or none of its runner: nothing (no crash).
    *(void**)(g_racerMem[5] + 0x2044) = nullptr;
    *(void**)(g_anim[4].ca + 0xc4) = nullptr;
    g_anim[4].ca[0] = 0x40;
    *(void**)(g_racerMem[4] + 0x2044) = g_anim[4].ca;
    ItemsTick();
    CHECK(SentReactions().size() == 2);

    // From the guest: its car plays it here, and its own reactions never pop (timer held at 1.0).
    Anim(1, 0);
    float* timer = (float*)(g_anim[1].rc + 0xb0);
    DeliverEvent(B, c, kEventReaction, 1, 0x9db1d546, 7000);
    ItemsTick();
    CHECK(g_played.size() == 1 && g_played[0].first == g_anim[1].ca && g_played[0].second == 0x9db1d546);
    CHECK(timer[0] == 1.0f && timer[3] == 1.0f);
    CHECK(*(float*)(g_anim[0].rc + 0xb0) == 0);  // our own car's reactions are left alone
    // Busy (a reaction plays): it waits, then plays when the car can react.
    DeliverEvent(B, c, kEventReaction, 1, 0xa7a4b00f, 7001);
    ItemsTick();
    CHECK(g_played.size() == 1);
    g_anim[1].ca[0] = 0;
    ItemsTick();
    CHECK(g_played.size() == 2 && g_played[1].second == 0xa7a4b00f);
    // Not driving (base state 5), or no free animation instance: waits, and is dropped after 0.5 s.
    g_anim[1].ca[0] = 0;
    *(int32_t*)g_anim[1].runner = 5;
    DeliverEvent(B, c, kEventReaction, 1, 0x68bd232b, 7002);
    for (int t = 0; t < 10; ++t) ItemsTick();
    CHECK(g_played.size() == 2);
    *(int32_t*)g_anim[1].runner = 4;
    *(uint32_t*)(g_anim[1].runner + 0x1c) = 0;
    for (int t = 0; t < 20; ++t) ItemsTick();
    *(uint32_t*)(g_anim[1].runner + 0x1c) = 1;
    ItemsTick();
    CHECK(g_played.size() == 2);  // dropped (waited past 25 ticks)
    // A newer one replaces one still waiting.
    g_anim[1].ca[0] = 0x40;
    DeliverEvent(B, c, kEventReaction, 1, 0x7e91a0e2, 7003);
    DeliverEvent(B, c, kEventReaction, 1, 0x2066a85d, 7004);
    ItemsTick();
    g_anim[1].ca[0] = 0;
    ItemsTick();
    CHECK(g_played.size() == 3 && g_played[2].second == 0x2066a85d);
    // The guest may not decide our car or the AI: refused.
    DeliverEvent(B, c, kEventReaction, 0, 0x1b13cf01, 7005);
    DeliverEvent(B, c, kEventReaction, 3, 0x1b13cf01, 7005);
    g_anim[0].ca[0] = 0;
    g_anim[3].ca[0] = 0;
    ItemsTick();
    ItemsTick();
    CHECK(g_played.size() == 3);
    // A node outside the reaction set (any other animation of the character) is dropped, never played.
    DeliverEvent(B, c, kEventReaction, 1, 0x12345678, 7006);
    ItemsTick();
    ItemsTick();
    CHECK(g_played.size() == 3);
    g_anim[1].ca[0] = 0;  // (the last played one is over)
    DeliverEvent(B, c, kEventReaction, 1, 0x1b13cf01, 7007);  // a real one still plays after it
    ItemsTick();
    CHECK(g_played.size() == 4 && g_played[3].second == 0x1b13cf01);
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);  // the summary lines
    for (int k = 0; k < 8; ++k) {
        *(void**)(g_racerMem[k] + 0x2040) = nullptr;
        *(void**)(g_racerMem[k] + 0x2044) = nullptr;
    }
    RaceTestSetItems(false);
    RaceTestSetHostAi(false);
}

// The host's ItemSync=0 (the hooks are in, no flag), or a host whose item hooks are off (another mod has
// the slots): no item flag, and the M5 behaviour (network cars' items dropped, nothing mirrored).
// Guest with the host's AI: the host's AI reactions play on this game's copies; a departed host's pending one is dropped.
static void TestReactionsGuest() {
    printf("reactions: guest plays the host's AI reactions\n");
    RaceTestSetItems(true);
    RaceTestSetReactionAddress((uintptr_t)&FakePlayReaction);
    RaceContract c = GuestRunning();
    ResetItems();
    g_played.clear();
    for (int k = 0; k < 8; ++k) Anim(k, 0);
    for (uint32_t t = 0; t < 30; ++t) {
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);
        ItemsTick();
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    DeliverEvent(A, c, kEventReaction, 4, 0x1b13cf01, 3029);  // the host's AI 4 showboats
    int waited = 0;
    for (uint32_t t = 30; t < 50 && g_played.empty(); ++t) {
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);
        ItemsTick();
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
        ++waited;
    }
    CHECK(g_played.size() == 1 && g_played[0].first == g_anim[4].ca && g_played[0].second == 0x1b13cf01 && waited <= 12);
    CHECK(*(float*)(g_anim[4].rc + 0xb0) == 1.0f && *(float*)(g_anim[0].rc + 0xb0) == 1.0f);  // the puppets' own never pop
    CHECK(*(float*)(g_anim[1].rc + 0xb0) == 0);  // ours are left alone
    // One from the host for its own car; the host then leaves while another waits for its car: dropped, not played.
    g_anim[0].ca[0] = 0x40;  // busy: it waits
    DeliverEvent(A, c, kEventReaction, 0, 0x9db1d546, 3049);
    HostCars(c, 3050, 50.0f, true);
    ItemsTick();
    uint8_t b[kWireMax];
    Deliver(A, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    ItemsTick();
    g_anim[0].ca[0] = 0;
    for (int t = 0; t < 3; ++t) ItemsTick();
    CHECK(g_played.size() == 1 && !RaceTestAiPuppets());
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);
    for (int k = 0; k < 8; ++k) {
        *(void**)(g_racerMem[k] + 0x2040) = nullptr;
        *(void**)(g_racerMem[k] + 0x2044) = nullptr;
    }
    RaceTestSetItems(false);
    RaceTestSetHostAi(false);
}

static void TestItemsOff(bool config, bool hooks) {
    printf("items: off (ItemSync=%d, item hooks %s)\n", config, hooks ? "in" : "off");
    RaceTestSetHostAi(true);
    RaceTestSetItemConfig(config, hooks);
    RaceContract c = HostRunning();
    CHECK(c.flags == kContractHostAi);
    ResetItems();
    ItemsTick();
    GamePost(GameEvent(0, 1, 0x7711877d));
    CHECK(g_ran.size() == 1 && SentEvents().empty());
    CHECK(Message(1, 0x06cd52a4) == 7);
    DeliverEvent(B, c, 1, 1, 0x5f724da9, 7000);
    ItemsTick();
    CHECK(g_ran.size() == 1);
    ItemsTick();
    CHECK(g_itemClears.size() == 1 && g_itemClears[0] == g_racerMem[1]);  // as in M5: the network car's item is dropped
    NativeCall d = {};
    NHook(kH090a)(nullptr, &d);
    RaceTestSetItems(false);
    RaceTestSetHostAi(false);
}

static void TestFinishPanel() {
    printf("finish panel: this game's car\n");
    static uint8_t module[0x5000];
    memset(module, 0, sizeof(module));
    const uint8_t code[] = {0x51, 0x00, 0x32, 0x01, 0x13, 0x09, 0x00, 0x00, 0xd8, 0x07, 0x07, 0x01, 0x51, 0x00, 0x23};
    memcpy(module + 0x37f8, code, sizeof(code));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, module + 0x3802, module};
    auto ask = [&](void* v) {
        uint8_t t[1] = {kTypeInt};  // INT8 0 pushes type 2
        uint32_t val[1] = {0};
        NativeCall d = {0x0913, 1, {}, t, val, 1, 0, 0};
        NHook(kH0913)(v, &d);
        CHECK(t[0] == kTypeInt);
        return g_0913Arg;
    };
    RaceTestSetHostAi(true);
    GuestRunning();
    CHECK(ask(&vm) == 1);  // the guest's own car
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } other = {nullptr, module + 0x352b + 8, module};
    memcpy(module + 0x352b - 2, code, sizeof(code));
    CHECK(ask(&other) == 0);  // the same native elsewhere: untouched
    module[0x37f8 + 12] = 0x52;  // the right place, different code (another mod's results)
    CHECK(ask(&vm) == 0);
    module[0x37f8 + 12] = 0x51;
    HostRunning();
    CHECK(ask(&vm) == 0);  // the host's own car is index 0 already
    Reset();
    CHECK(ask(&vm) == 0);  // no online race
    RaceTestSetHostAi(false);
}

// ---------------------------------------------------------------- online pause (M6, re\m6-pause\FINDINGS.md)
static void TestOnlinePause() {
    printf("online pause: the pause menu's online path\n");
    static uint8_t pause[0x2000];
    struct Site { uint32_t pc; uint8_t code[16]; };
    const Site sites[] = {
        {0x04c4, {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xb8, 0x00, 0x51, 0x05, 0x4b, 0x14, 0x21, 0x00, 0x32, 0x00}},
        {0x067e, {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xf4, 0x00, 0x51, 0x05, 0x4a, 0x02, 0x0e, 0x42, 0xd2, 0x86}},
        {0x0842, {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x61, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x15, 0x00, 0x02, 0x81}},
        {0x098d, {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x94, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x0c, 0x00, 0x32, 0x00}},
    };
    for (const Site& s : sites) memcpy(pause + s.pc - 8, s.code, 16);
    const uint8_t endRead[] = {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0xf4, 0x00, 0x4f};
    memcpy(pause + 0x068e - 13, endRead, sizeof(endRead));
    typedef struct { void* vtable; uint8_t* pc; uint8_t* mod; } Vm;
    Vm open = {nullptr, pause + 0x04c4, pause}, close = {nullptr, pause + 0x067e, pause}, unpauseHud = {nullptr, pause + 0x0842, pause},
       unpause = {nullptr, pause + 0x098d, pause}, endKey = {nullptr, pause + 0x068e, pause};
    uint8_t t;
    uint32_t v;
    // Offline: the stock pause everywhere.
    Reset();
    RaceTestSetReturnFlag(false);
    CHECK(Type0916(&open) == 1 && Type0916(&unpause) == 1);
    // Our race: the open site sees 5 (0928: the AI drives our car, nothing stops), then the menu's other sites.
    HostRunning();
    CHECK(Type0916(&unpause) == 1);  // no pause open yet
    CHECK(Type0916(&open) == 5 && Type0916(&close) == 5 && Type0916(&unpauseHud) == 5 && Type0916(&unpause) == 5);
    {
        NativeCall d = {0x0916, 1, {}, nullptr, nullptr, 0, 0, 0};
        NHook(kH0916)(&open, &d);
        CHECK(d.retType == kTypeHash);  // the game's type kept
    }
    // A results key left at 1 by an earlier race: read 0 there until this race is over.
    g_storedKey = 1;
    g_storedType = kTypeInt;
    CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 0);
    // This game's car finished (driven by the AI behind the menu): the menu closes at once, as on the console, before the
    // results make their scenes; also in a first race, whose key the results haven't written yet.
    g_storedKey = 0;
    Append(0, 1);
    CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 1);
    g_finishCount = 0;
    g_storedKey = 1;
    CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 0);
    // This game's results start (its car finished, driven by the AI behind the menu): the menu closes now, as on the
    // console, before the results make the title and button scenes it shares (re\m7-results-title\FINDINGS.md). Only at
    // the results' own RESULTS_START write.
    {
        static uint8_t results[0x2000];
        const uint8_t start[] = {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x51, 0x01, 0x32, 0x02, 0x03, 0x1b, 0x00, 0x00, 0xc6, 0x02};
        memcpy(results + 0x1ade, start, sizeof(start));
        Vm elsewhere = {nullptr, results + 0x1aee, results}, resultsStart = {nullptr, results + 0x1aed, results};
        NativeCall w = {0x1b03, 1, {}, nullptr, nullptr, 0, 0, 0};
        RaceNativeBefore(&elsewhere, &w);
        CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 0);
        RaceNativeBefore(&resultsStart, &w);
        CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 1);
    }
    g_raceOver = 1;
    CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 1);  // the race ended with the menu open: it closes
    CHECK(Type0916(&open) == 1);  // no new online pause after the race end
    g_raceOver = 0;
    // The race is closed (090a, RETURN TO LOBBY): the latch goes; the next offline race pauses as before.
    Type0916(&open);
    NativeCall none = {};
    NHook(kH090a)(nullptr, &none);
    RaceTestSetReturnFlag(false);
    CHECK(Type0916(&open) == 1 && Type0916(&unpause) == 1 && Type0916(&close) == 1);
    CHECK(Read1b01(&endKey, 0x86d2420e, &t, &v).retValue == 1);
    g_storedKey = 0x906825eb;
    g_storedType = kTypeHash;
}

// ---------------------------------------------------------------- name tags (M6, re\m6-race-hud\FINDINGS.md (b))
// 0x47f3a0 (stdcall(element hash, text object)): reads the text at once through the object's vtbl[0] (sprintf "%s").
void RaceTestSetTextAddress(uintptr_t setElementText);
static int g_textCalls = 0;
static uint32_t g_textElement = 0;
static char g_textSeen[64];
static bool g_textFound = true;
static bool __stdcall FakeSetElementText(uint32_t element, void* text) {
    ++g_textCalls;
    g_textElement = element;
    const uint32_t* obj = (const uint32_t*)text;
    CHECK(obj[0] == 0x6a71f8 && (int32_t)obj[2] >= 2);
    strncpy_s(g_textSeen, (const char*)(uintptr_t)obj[3], _TRUNCATE);
    return g_textFound;
}

static NativeCall HudCall(void* vm, uint32_t id, int32_t arg, uint32_t ret, uint32_t retType, uint8_t* t, uint32_t* v) {
    *t = kTypeInt;
    *v = (uint32_t)arg;
    NativeCall d = {id, 1, {}, t, v, arg >= 0 ? 1u : 0u, ret, retType};
    RaceNativeAfter(vm, &d);
    return d;
}

static void TestNameTags() {
    printf("name tags: the HUD tags the other players' cars\n");
    static uint8_t hud[0x7000];
    const uint8_t gate[] = {0x32, 0x00, 0x38, 0x09, 0x00, 0x00, 0x18, 0x04, 0x51, 0x01, 0x48, 0x14, 0x0c, 0x00};
    const uint8_t human[] = {0x07, 0x01, 0x39, 0x32, 0x01, 0x11, 0x09, 0x00, 0x00, 0x8b, 0x09, 0x14, 0x17, 0x00};
    memcpy(hud + 0x36b6, gate, sizeof(gate));
    memcpy(hud + 0x5b1a, human, sizeof(human));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } gvm = {nullptr, hud + 0x36b6 + 8, hud}, hvm = {nullptr, hud + 0x5b1d + 8, hud},
                                                           elsewhere = {nullptr, hud + 0x3c94 + 8, hud};
    uint8_t t;
    uint32_t v;
    Reset();
    CHECK(HudCall(&gvm, 0x0938, -1, 1, kTypeInt, &t, &v).retValue == 1);  // no online race: stock
    CHECK(HudCall(&hvm, 0x0911, 1, 0, kTypeInt, &t, &v).retValue == 0);
    HostRunning();
    NativeCall d = HudCall(&gvm, 0x0938, -1, 1, kTypeInt, &t, &v);
    CHECK(d.retValue == 2 && d.retType == kTypeInt);  // the gate: tags on
    CHECK(HudCall(&gvm, 0x0938, -1, 1, kTypeHash, &t, &v).retType == kTypeHash);  // type kept
    CHECK(HudCall(&hvm, 0x0911, 1, 0, kTypeInt, &t, &v).retValue == 1);  // the guest's car: human for the tags
    CHECK(HudCall(&hvm, 0x0911, 0, 1, kTypeInt, &t, &v).retValue == 1);  // ours: as the game said
    CHECK(HudCall(&hvm, 0x0911, 3, 0, kTypeInt, &t, &v).retValue == 0);  // an AI: no tag
    CHECK(HudCall(&elsewhere, 0x0911, 1, 0, kTypeInt, &t, &v).retValue == 0);  // 0911 elsewhere: untouched
    hud[0x36b6 + 9] = 0x02;  // the right place, other code (another mod's hud)
    CHECK(HudCall(&gvm, 0x0938, -1, 1, kTypeInt, &t, &v).retValue == 1);
    hud[0x36b6 + 9] = 0x01;
    // The tag's text: the player's Steam name instead of "2P" (TagFunc's 021f at hud 0x5c62).
    const uint8_t text[] = {0x07, 0x0c, 0x3c, 0x0b, 0x02, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x00, 0x00, 0xd3, 0x09, 0x18};
    memcpy(hud + 0x5c62 - 6, text, sizeof(text));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } tvm = {nullptr, hud + 0x5c62 + 8, hud};
    uint8_t tt[2] = {kTypeHash, kTypeHash};
    uint32_t tv[2] = {0x384c35c0, 0x8c88e2d8};  // LFT_TXT_01, "2P"
    NativeCall d2 = {0x021f, 1, {}, tt, tv, 2, 0, 0};
    g_textCalls = 0;
    CHECK(RaceNativeBefore(&tvm, &d2) && g_textCalls == 1 && g_textElement == 0x384c35c0);
    CHECK(strcmp(g_textSeen, "Bravo 2") == 0);
    tv[1] = 0x1c49c7c3;  // "1P": this game's own player is never tagged here
    CHECK(!RaceNativeBefore(&tvm, &d2) && g_textCalls == 1);
    tv[1] = 0x12345678;  // not a player label
    CHECK(!RaceNativeBefore(&tvm, &d2));
    tv[1] = 0x8c88e2d8;
    g_textFound = false;  // the element isn't there: the game's own label
    CHECK(!RaceNativeBefore(&tvm, &d2) && g_textCalls == 2);
    g_textFound = true;
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } other = {nullptr, hud + 0x6000 + 8, hud};
    CHECK(!RaceNativeBefore(&other, &d2));  // 021f elsewhere
    // Egg Hangar's hud (the same code, later: gate 0x36d4, human 0x5b7f, text 0x5cc4): the tags and their text there too.
    static uint8_t egg[0x7000];
    memcpy(egg + 0x36d4, gate, sizeof(gate));
    memcpy(egg + 0x5b7c, human, sizeof(human));
    memcpy(egg + 0x5cc4 - 6, text, sizeof(text));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } egate = {nullptr, egg + 0x36d4 + 8, egg}, ehuman = {nullptr, egg + 0x5b7f + 8, egg},
                                                           etext = {nullptr, egg + 0x5cc4 + 8, egg};
    CHECK(HudCall(&egate, 0x0938, -1, 1, kTypeInt, &t, &v).retValue == 2);
    CHECK(HudCall(&ehuman, 0x0911, 1, 0, kTypeInt, &t, &v).retValue == 1);
    tv[1] = 0x8c88e2d8;
    int calls = g_textCalls;
    CHECK(RaceNativeBefore(&etext, &d2) && g_textCalls == calls + 1 && strcmp(g_textSeen, "Bravo 2") == 0);
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } stockPlace = {nullptr, egg + 0x36b6 + 8, egg};
    CHECK(HudCall(&stockPlace, 0x0938, -1, 1, kTypeInt, &t, &v).retValue == 1);  // (the stock place in Egg's hud: other code)
    g_snap.memberCount = 1;  // the guest left: no tags (its car is an AI now)
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(HudCall(&gvm, 0x0938, -1, 1, kTypeInt, &t, &v).retValue == 1);
    CHECK(HudCall(&hvm, 0x0911, 1, 0, kTypeInt, &t, &v).retValue == 0);
    CHECK(!RaceNativeBefore(&tvm, &d2));
}

// ---------------------------------------------------------------- the progress bar (M6, re\m6-race-hud\FINDINGS.md (a))
void RaceTestSetBarAddresses(uintptr_t localPlayers, uintptr_t iconHash, uintptr_t setImage);
struct FakeElement {
    void** vtable;
    uint8_t body[0xe0];
    FakeElement* parent;  // +0xe4: a marker's node
    bool shown;
};
static int g_shows = 0, g_hides = 0;
static void __fastcall ElementShow(FakeElement* e, void*) { e->shown = true; ++g_shows; }
static void __fastcall ElementHide(FakeElement* e, void*) { e->shown = false; ++g_hides; }
static void* g_elementVtable[0x60 / 4];
static uint32_t g_localPlayers = 1;
static uint32_t __cdecl FakeIconHash(void* racer) { return 0x1c0 + (uint32_t)((uint8_t(*)[0x2200])racer - g_racerMem); }
static uint32_t g_imageElement = 0, g_image = 0, g_imageSet = 0, g_imageSets = 0;
static bool __fastcall FakeSetImage(uintptr_t sets, void*, uint32_t element, uint32_t image, uint32_t set) {
    g_imageSets = (uint32_t)sets; g_imageElement = element; g_image = image; g_imageSet = set;
    return true;
}

static void TestProgressBar() {
    printf("progress bar: the other players' cars show their player markers\n");
    static uint8_t bar[0x400];
    static FakeElement rivalMarker, rivalHead, rivalNode, humanMarker, humanHead, humanText, humanNode;
    for (FakeElement* e : {&rivalMarker, &rivalHead, &rivalNode, &humanMarker, &humanHead, &humanText, &humanNode}) e->vtable = g_elementVtable;
    g_elementVtable[0x48 / 4] = (void*)&ElementShow;
    g_elementVtable[0x50 / 4] = (void*)&ElementHide;
    rivalMarker.parent = &rivalNode;
    humanMarker.parent = &humanNode;
    RaceTestSetBarAddresses((uintptr_t)&g_localPlayers, (uintptr_t)&FakeIconHash, (uintptr_t)&FakeSetImage);
    uint8_t* R = bar + 3 * 0x1c;   // rival entry of list index 1
    uint8_t* U = bar + 11 * 0x1c;  // human entry of creation index 1
    auto init = [&](void* racer1) {  // what the bar's init leaves for a car that isn't category 1
        memset(bar, 0, sizeof(bar));
        bar[0x379] = 1;
        *(void**)(R + 0) = &rivalMarker; *(void**)(R + 4) = &rivalHead; *(void**)(R + 0xc) = racer1; *(void**)(R + 0x10) = &rivalNode;
        *(uint32_t*)(R + 0x18) = 0xc72c4bc8; R[0x15] = 1;
        *(void**)(U + 0) = &humanMarker; *(void**)(U + 4) = &humanHead; *(void**)(U + 8) = &humanText; *(void**)(U + 0x10) = &humanNode;
        *(uint32_t*)(U + 0x18) = 0x1650a39;
        rivalNode.shown = true;
        humanNode.shown = false;
    };
    NativeCall frame = {0x1f15, 1, {}, nullptr, nullptr, 0, 0, 0}, built = {0x1f16, 1, {}, nullptr, nullptr, 0, 0, 0};
    Reset();
    *(void**)(g_scene + 0x1174) = bar;
    init(g_racerMem[1]);
    CHECK(!RaceNativeBefore(nullptr, &frame) && *(void**)(R + 0) == &rivalMarker);  // no online race: untouched
    HostRunning();
    init(g_racerMem[1]);
    g_shows = g_hides = 0;
    CHECK(!RaceNativeBefore(nullptr, &frame));  // the game's own update still runs
    CHECK(*(void**)(R + 0) == &humanMarker && *(void**)(R + 4) == &humanHead && *(void**)(R + 8) == &humanText);
    CHECK(*(void**)(R + 0x10) == &humanNode && *(uint32_t*)(R + 0x18) == 0x1650a39 && R[0x15] == 0);
    CHECK(*(void**)(R + 0xc) == g_racerMem[1] && *(void**)(U + 0) == &humanMarker);  // the racer kept; the human entry untouched
    CHECK(!rivalNode.shown && humanNode.shown && g_shows == 1 && g_hides == 1);
    CHECK(g_imageSets == 0xc282a8 && g_imageElement == (uint32_t)(uintptr_t)&humanHead && g_image == 0x1c1 && g_imageSet == 0x3a6dfc06);
    RaceNativeBefore(nullptr, &frame);
    CHECK(g_shows == 1 && g_hides == 1);  // once
    init(g_racerMem[1]);  // the bar is built again (1f16): applied again after it
    RaceNativeAfter(nullptr, &built);
    CHECK(*(void**)(R + 0) == &humanMarker && humanNode.shown && !rivalNode.shown);
    // The player leaves: the car is a computer racer's again, with its own marker.
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    RaceNativeBefore(nullptr, &frame);
    CHECK(*(void**)(R + 0) == &rivalMarker && *(void**)(R + 0x10) == &rivalNode && *(uint32_t*)(R + 0x18) == 0xc72c4bc8 && R[0x15] == 1);
    CHECK(rivalNode.shown && !humanNode.shown);
    // No 2P marker in this HUD (or a stale node): the car keeps its rival marker.
    HostRunning();
    init(g_racerMem[1]);
    *(void**)(U + 0x10) = &rivalNode;
    RaceNativeBefore(nullptr, &frame);
    CHECK(*(void**)(R + 0) == &rivalMarker);
    // Split screen (2 local players: other sprite names): left alone.
    HostRunning();
    init(g_racerMem[1]);
    g_localPlayers = 2;
    RaceNativeBefore(nullptr, &frame);
    CHECK(*(void**)(R + 0) == &rivalMarker);
    g_localPlayers = 1;
    *(void**)(g_scene + 0x1174) = nullptr;
}

// ---------------------------------------------------------------- a player leaves mid-race (M6)
static const uint64_t C3 = 76561198000000003ull;

// Guest B (creation index 1) in a 3-player race (A hosts, C is index 2) with the host's AI, running.
static RaceContract Guest3Running() {
    RaceTestSetHostAi(true);
    Reset();
    Lobby(A, true, 0, 1);
    g_snap.members[2] = {C3, "C", 0, 0, 3, 1, 1, -1};
    g_snap.memberCount = 3;
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c) && c.humanCount == 3);
    Reset();
    Lobby(B, false, 0, 1);
    g_snap.members[2] = {C3, "C", 0, 0, 3, 1, 1, -1};
    g_snap.memberCount = 3;
    uint8_t buf[kWireMax];
    Deliver(A, buf, EncodeContract(c, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridAI", 0);
    Script0905(c.roster[1], "STATE_Control_StartingGridHuman", 2);
    for (int k = 2; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(c.race, kStageLoaded, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(c.race, kStageCountdown, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 8; ++k) *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    g_localIdx = 1;
    g_sent.clear();
    return c;
}

static int StateCallsFor(int k, uint32_t id) {
    int n = 0;
    for (auto& s : g_stateCalls) n += s.first == g_controllers[k] && s.second == id;
    return n;
}

static void TestCarLeft() {
    printf("a player leaves mid-race: their car goes on as an AI\n");
    // Host with the host's AI: it drives the car and sends it with its others; its items are the host's.
    RaceTestSetHostAi(true);
    RaceTestSetItems(true);
    RaceContract c = HostRunning();
    ResetItems();
    ItemsTick();  // racing
    GamePost(GameEvent(0, 1, 0x5f724da9));  // still the guest's car: left to the guest
    CHECK(SentEvents().empty() && g_freed == 1);
    g_stateCalls.clear();
    g_snap.memberCount = 1;  // the guest left
    ItemsTick();
    CHECK(g_stateCalls.size() == 1 && StateCallsFor(1, 3) == 1);
    CHECK(*(uint32_t*)(g_racerMem[1] + 0x9e4) == 0x18);  // its initial state as created (an AI racer)
    g_stateCalls.clear();
    ItemsTick();
    CHECK(g_stateCalls.empty());  // once
    g_sent.clear();
    for (uint32_t t = 0; t < 4; ++t) {
        ItemsTick();
        for (int k = 0; k < 8; ++k) {
            *(float*)(g_racerMem[k] + 0xc0) = (float)(t + 10 * k);
            Commit(g_racerMem[k]);
        }
    }
    ItemsTick();
    int withCar1 = 0;
    for (const RaceCars& rc : SentCars()) {
        for (int i = 0; i < rc.count; ++i) withCar1 += rc.cars[i].slot == 1;
        CHECK(rc.cars[0].slot == 0);
    }
    CHECK(withCar1 == 2);  // every 2nd tick, like the AI
    g_sent.clear();
    GamePost(GameEvent(0, 1, 0x5f724da9));
    std::vector<RaceEvent> ev = SentEvents();
    CHECK(ev.size() == 1 && ev[0].slot == 1 && *(uint32_t*)(g_racerMem[1] + 0xc1c) == 0x5f724da9);
    CHECK(Message(1, 0x06cd52a4) == 7);  // its item boxes are this game's now
    RaceTestSetItems(false);
    // Each game drives its own AI (HostRunsAI=0): the car goes on as this game's AI.
    RaceTestSetHostAi(false);
    c = HostRunning();
    UHook(kHRacing)(nullptr, nullptr);
    g_stateCalls.clear();
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_stateCalls.size() == 1 && StateCallsFor(1, 3) == 1);
    float x1 = X(1);
    uint8_t buf[kWireMax];
    DeliverPose(B, c, 1, 50, 777.0f);  // a late pose of the player who left: not shown
    UHook(kHRacing)(nullptr, nullptr);
    Commit(g_racerMem[1]);
    CHECK(X(1) == x1);
    // Before GO (countdown): the AI grid state.
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(c.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    g_stateCalls.clear();
    g_snap.memberCount = 1;
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(g_stateCalls.size() == 1 && StateCallsFor(1, 0x18) == 1);
    // Noticed in the very first race update after GO: the racing state, not the grid's.
    HostRunning();
    g_stateCalls.clear();
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_stateCalls.size() == 1 && StateCallsFor(1, 3) == 1);
    // The player had finished (their time is in): their car stays where it finished.
    c = HostRunning();
    UHook(kHRacing)(nullptr, nullptr);
    RaceFinish fin = {c.race, 1, 1, 0x00b00000, 0x1800};
    Deliver(B, buf, EncodeFinished(fin, buf, sizeof(buf)));
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(InList(1));
    g_stateCalls.clear();
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_stateCalls.empty());
    // After this game's race is over: the car stays as it is.
    HostRunning();
    g_raceOver = 1;
    g_stateCalls.clear();
    g_snap.memberCount = 1;
    uint8_t wff[0x20] = {};
    UHook(kHWff)(wff, nullptr);
    CHECK(g_stateCalls.empty());

    // Guest, 3 players, the host's AI: C leaves; its car follows the host's game like the AI.
    c = Guest3Running();
    CHECK(StateCallsFor(2, 0x15) == 1 && *(uint32_t*)(g_racerMem[2] + 0x9e4) == 0x15);  // C's car: a network car since hold A
    for (uint32_t t = 0; t < 20; ++t) {
        ++g_tick;
        DeliverPose(C3, c, 2, 9000 + t, 5.0f);
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);  // (its AI 3..7, and slot 2: not the host's car yet)
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    CHECK(fabsf(X(2) - 5.0f) < 0.01f);  // C's own poses
    g_stateCalls.clear();
    g_snap.memberCount = 2;  // C left
    for (uint32_t t = 20; t < 60; ++t) {
        ++g_tick;
        DeliverPose(C3, c, 2, 9000 + t, 5.0f);  // (late: ignored)
        HostCars(c, 3000 + t, (float)t, t % 2 == 0);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    printf("  host car x %.2f, C's car x %.2f\n", X(0), X(2));
    CHECK(g_stateCalls.empty());  // still a network car here
    CHECK(fabsf((X(2) - 200.0f) - X(0)) < 0.01f);  // at the same moment of the host's race as its car
    // The host leaves too: this game drives the host's car, the AI and C's car.
    g_stateCalls.clear();
    g_snap.members[0] = g_snap.members[1];
    g_snap.memberCount = 1;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(!RaceTestAiPuppets() && g_stateCalls.size() == 7);
    for (int k = 0; k < 8; ++k) CHECK(StateCallsFor(k, 3) == (k == 1 ? 0 : 1));
    float x2 = X(2);
    HostCars(c, 4000, 50.0f, true);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    Commit(g_racerMem[2]);
    CHECK(X(2) == x2);
    RaceTestSetHostAi(false);
}

// ---------------------------------------------------------------- HUD toasts (M7, re\m7-join-race\FINDINGS.md Q2)
void RaceTestSetToastAddresses(uintptr_t idle, uintptr_t play, uintptr_t sceneText);
static uint8_t g_toastElement[0x120];  // the HUD ticker's text element: +0x108 -> its bound text buffer (FakeFindText, below)
static uint32_t g_toastObj[4] = {0x6a71f8, 0, 64, 0};
static char g_toastBuf[64];
static bool g_toastIdle = true;
static bool __stdcall FakeElementIdle(uint32_t element) { CHECK(element == 0xca4107ae); return g_toastIdle; }
struct PlaySeen { uint32_t element, from, to, c, loop; };
static std::vector<PlaySeen> g_plays;
static void __stdcall FakePlayElement(uint32_t element, uint32_t from, uint32_t to, uint32_t c, uint32_t loop) { g_plays.push_back({element, from, to, c, loop}); }
static const char* g_sceneFmt = nullptr;
static uint32_t g_sceneKeySeen = 0;
static const char* __fastcall FakeSceneText(uintptr_t sets, void*, uint32_t scene, uint32_t key) {
    CHECK(sets == 0xc282a8 && scene == 0x91c494ea);
    g_sceneKeySeen = key;
    return g_sceneFmt;
}

static void TestToasts() {
    printf("HUD toasts: joins and leaves while racing\n");
    RaceTestSetToastAddresses((uintptr_t)&FakeElementIdle, (uintptr_t)&FakePlayElement, (uintptr_t)&FakeSceneText);
    *(uint32_t**)(g_toastElement + 0x108) = g_toastObj;
    g_toastObj[3] = (uint32_t)(uintptr_t)g_toastBuf;
    static uint8_t hud[0x7000];
    const uint8_t gate[] = {0x32, 0x00, 0x38, 0x09, 0x00, 0x00, 0x18, 0x04, 0x51, 0x01, 0x48, 0x14, 0x0c, 0x00};
    memcpy(hud + 0x36b6, gate, sizeof(gate));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } gvm = {nullptr, hud + 0x36b6 + 8, hud}, elsewhere = {nullptr, hud + 0x3c94 + 8, hud};
    uint8_t t;
    uint32_t v;
    auto HudFrame = [&](void* vm) { HudCall(vm, 0x0938, -1, 1, kTypeInt, &t, &v); };
    g_toastIdle = true;
    g_sceneFmt = nullptr;
    g_plays.clear();
    g_toastBuf[0] = 0;
    RaceContract c = HostRunning();
    HudFrame(&gvm);
    CHECK(g_plays.empty() && g_toastBuf[0] == 0);  // nothing to say
    // A new member joins the lobby while we race: the ticker says so (English fallback: the scene gave no text).
    g_snap.memberCount = 3;
    g_snap.members[2] = {B + 1, "Joiner", 0, 0, -1, 0, 1, -1};
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    HudFrame(&elsewhere);  // 0938 elsewhere: nothing
    CHECK(g_plays.empty());
    HudFrame(&gvm);
    CHECK(g_plays.size() == 1 && g_plays[0].element == 0xc49502fc && g_plays[0].from == 0x3e3460a8 && g_plays[0].to == 0xf5fda6b0 && g_plays[0].loop == 1);
    CHECK(strcmp(g_toastBuf, "Joiner HAS JOINED THE LOBBY.") == 0 && g_sceneKeySeen == 0x8da10d88);
    HudFrame(&gvm);
    CHECK(g_plays.size() == 1);  // once
    // Several at once: the other player's quit (left the game) comes first, then the joiner leaving the lobby; a
    // busy ticker waits; the scene's own (localized) text is used when there is one; non-ASCII name characters are '?'.
    g_snap.memberCount = 2;
    uint8_t b[kWireMax];
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    g_toastIdle = false;
    HudFrame(&gvm);
    CHECK(g_plays.size() == 1);
    g_toastIdle = true;
    g_sceneFmt = "{0} A QUITTE LA PARTIE.";
    HudFrame(&gvm);
    CHECK(g_plays.size() == 2 && strcmp(g_toastBuf, "Bravo 2 A QUITTE LA PARTIE.") == 0 && g_sceneKeySeen == 0x993d0c07);
    g_sceneFmt = nullptr;
    HudFrame(&gvm);
    CHECK(g_plays.size() == 3 && strcmp(g_toastBuf, "Joiner HAS LEFT THE LOBBY.") == 0);
    HudFrame(&gvm);
    CHECK(g_plays.size() == 3);
    g_snap.memberCount = 2;
    g_snap.members[1] = {B + 2, "J\xc3\xb6rg \xe2\x98\x85", 0, 0, -1, 0, 1, -1};  // the quitter is gone from the lobby too; a new member with non-ASCII letters
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    HudFrame(&gvm);
    CHECK(g_plays.size() == 4 && (strcmp(g_toastBuf, "Bravo 2 HAS LEFT THE LOBBY.") == 0));
    g_sceneFmt = "{0} A REJOINT LE SALON \xc3\x89.";  // a non-ASCII letter in the scene's text: one '?'
    HudFrame(&gvm);
    CHECK(g_plays.size() == 5 && strcmp(g_toastBuf, "J?rg ? A REJOINT LE SALON ?.") == 0);
    g_sceneFmt = nullptr;
    // A player who finished and then quits "returned to the lobby" (not "left the game").
    c = HostRunning();
    g_plays.clear();
    g_toastBuf[0] = 0;
    uint8_t fb[kWireMax];
    RaceFinish theirs = {c.race, 1, 1, 0x00d00000, 0x2000};
    Deliver(B, fb, EncodeFinished(theirs, fb, sizeof(fb)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    Deliver(B, fb, EncodeAbort(c.race, kAbortQuit, fb, sizeof(fb)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    HudFrame(&gvm);
    CHECK(g_plays.size() == 1 && strcmp(g_toastBuf, "Bravo 2 HAS RETURNED TO THE LOBBY.") == 0);
    // A ticker buffer we don't recognise: the toasts are dropped, nothing written.
    g_toastObj[0] = 0x12345678;
    g_snap.memberCount = 3;
    g_snap.members[2] = {B + 3, "Late", 0, 0, -1, 0, 1, -1};
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    HudFrame(&gvm);
    CHECK(g_plays.size() == 1 && strcmp(g_toastBuf, "Bravo 2 HAS RETURNED TO THE LOBBY.") == 0);
    g_toastObj[0] = 0x6a71f8;
    NativeCall td = {};
    NHook(kH090a)(nullptr, &td);
}

// ---------------------------------------------------------------- the console's session messages (M7, re\m7-messages\FINDINGS.md)
void RaceTestSetDialogAddresses(uintptr_t helper, uintptr_t create, uintptr_t set, uintptr_t release, uintptr_t none);
static uint8_t g_arrayHelper[8];  // +4: the array type id
static uint32_t g_fakeArray[16];  // the array object: +0 count, +8 the elements
static uint32_t g_arrayElems[3][2];
static int g_arrayCreates = 0, g_arraySets = 0, g_releases = 0;
static bool g_arrayOk = true;
static bool __fastcall FakeArrayCreate(uintptr_t helper, void*, uint32_t* out, uint32_t count) {
    CHECK(helper == (uintptr_t)g_arrayHelper && count == 3);
    ++g_arrayCreates;
    if (!g_arrayOk) return false;
    g_fakeArray[0] = count;
    g_fakeArray[2] = (uint32_t)(uintptr_t)g_arrayElems;
    out[0] = (uint32_t)(uintptr_t)g_fakeArray;
    out[1] = *(uint32_t*)(g_arrayHelper + 4);
    memset(g_arrayElems, 0xee, sizeof(g_arrayElems));
    return true;
}
static void __fastcall FakeArraySet(uintptr_t helper, void*, uint32_t* arr, uint32_t index, const uint32_t* elem) {
    CHECK(helper == (uintptr_t)g_arrayHelper && arr[0] == (uint32_t)(uintptr_t)g_fakeArray && arr[1] == *(uint32_t*)(g_arrayHelper + 4) && index < 3);
    ++g_arraySets;
    g_arrayElems[index][0] = elem[0];
    g_arrayElems[index][1] = elem[1];
}
static void __fastcall FakeRelease(uint32_t* arr, void*) {
    CHECK(arr[0] == (uint32_t)(uintptr_t)g_fakeArray);
    ++g_releases;
}
static uint32_t g_none[2] = {0, 0};

static void TestSessionDialogs() {
    printf("session messages: the console's dialogs when the others are gone\n");
    RaceTestSetJoinAddresses((uintptr_t)&FakePostMessage, (uintptr_t)&FakeClear, (uintptr_t)&FakeClear, (uintptr_t)&FakeClear,
                             (uintptr_t)&FakeBlackboard, (uintptr_t)&g_worldPaused, (uintptr_t)g_profiles);
    *(uint32_t*)(g_arrayHelper + 4) = 0x29;
    RaceTestSetDialogAddresses((uintptr_t)g_arrayHelper, (uintptr_t)&FakeArrayCreate, (uintptr_t)&FakeArraySet, (uintptr_t)&FakeRelease,
                               (uintptr_t)g_none);
    static uint8_t game[0x800], dialogs[0x800];
    const uint8_t inbox[] = {0x51, 0x01, 0x32, 0x01, 0x0e, 0x16, 0x02, 0x00, 0x2f, 0x00};
    const uint8_t close[] = {0x02, 0x1f, 0x96, 0x4b, 0x42, 0x32, 0x01, 0x06, 0x02, 0x01, 0x00, 0x5e, 0x01};
    memcpy(game + 0x0496 - 10, inbox, sizeof(inbox));
    memcpy(dialogs + 0x751 - 13, close, sizeof(close));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } gvm = {nullptr, game + 0x0496, game}, dvm = {nullptr, dialogs + 0x751, dialogs},
                                                           elsewhere = {nullptr, dialogs + 0x600, dialogs};
    NativeCall inboxRead = {0x160e, 1, {}, nullptr, nullptr, 1, 0, 0};
    uint8_t ht = kTypeHash;
    uint32_t hv = 0x424b961f;
    NativeCall hide = {0x0206, 1, {}, &ht, &hv, 1, 0, 0};
    auto Frame = [&]() { ++g_tick; UHook(kHRacing)(nullptr, nullptr); RaceNativeBefore(&gvm, &inboxRead); };
    auto Posts = [&](uint32_t target, uint32_t msg) { int n = 0; for (const PostSeen& p : g_posted) if (p.target == target && p.msg == msg) ++n; return n; };
    auto DialogPosts = [&]() { return Posts(0x694fe697, 0xdfea0f93); };
    auto QuitPosts = [&]() { return Posts(0xbed107e7, 0x682e3dfe); };
    g_screen = 0xbed107e7;
    g_worldPaused = 0;
    g_postOk = true;
    g_arrayOk = true;
    uint8_t b[kWireMax];
    NativeCall td = {};
    // Host: the only other player quits the race -> "the other player(s) lost" (RETURN TO LOBBY), posted once as [def, none, none].
    RaceContract c = HostRunning();
    g_posted.clear();
    g_clears = g_arrayCreates = g_arraySets = g_releases = 0;
    Frame();
    CHECK(DialogPosts() == 0);
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 1 && g_arrayCreates == 1 && g_arraySets == 3 && g_releases == 1);
    CHECK(g_arrayElems[0][0] == 0xcb5d217e && g_arrayElems[0][1] == kTypeHash && g_arrayElems[1][0] == 0 && g_arrayElems[1][1] == 0 &&
          g_arrayElems[2][0] == 0 && g_arrayElems[2][1] == 0);
    CHECK(g_posted.back().sender == 0 && g_posted.back().value == (uint32_t)(uintptr_t)g_fakeArray && g_posted.back().type == 0x29);
    Frame();
    Frame();
    CHECK(DialogPosts() == 1 && QuitPosts() == 0);
    // 0206 elsewhere, or on another scene: nothing. The OK path: the race is left like the pause menu's EXIT, once.
    RaceNativeBefore(&elsewhere, &hide);
    hv = 0x12345678;
    RaceNativeBefore(&dvm, &hide);
    hv = 0x424b961f;
    CHECK(QuitPosts() == 0);
    RaceNativeBefore(&dvm, &hide);
    CHECK(QuitPosts() == 1 && g_clears == 3 && g_posted.back().value == 0 && g_posted.back().type == kTypeInt);
    RaceNativeBefore(&dvm, &hide);
    Frame();
    CHECK(QuitPosts() == 1 && DialogPosts() == 1);
    // The quit message queue full at the answer: retried at the next frame.
    c = HostRunning();
    g_posted.clear();
    g_clears = 0;
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 1);
    g_postOk = false;
    RaceNativeBefore(&dvm, &hide);
    CHECK(QuitPosts() == 0 && g_clears == 0);
    g_postOk = true;
    Frame();
    CHECK(QuitPosts() == 1 && g_clears == 3);
    // The dialog post refused (queue full) -> retried; paused -> waits; the array not made -> retried.
    c = HostRunning();
    g_posted.clear();
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    g_postOk = false;
    Frame();
    CHECK(DialogPosts() == 0);
    g_postOk = true;
    g_worldPaused = 1;
    Frame();
    CHECK(DialogPosts() == 0);
    g_worldPaused = 0;
    g_arrayOk = false;
    int releases = g_releases;
    Frame();
    CHECK(DialogPosts() == 0 && g_releases == releases);  // (nothing to release)
    g_arrayOk = true;
    Frame();
    CHECK(DialogPosts() == 1);
    // The race is closed with the dialog still up (left another way): the dialog is closed with it.
    NHook(kH090a)(nullptr, &td);
    CHECK(Posts(0x694fe697, 0x3cb781ef) == 1 && g_posted.back().value == 0 && g_posted.back().type == 0);
    // Not once the race is over (the results screen has its own RETURN TO LOBBY).
    c = HostRunning();
    g_posted.clear();
    g_raceOver = 1;
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 0);
    g_raceOver = 0;
    NHook(kH090a)(nullptr, &td);
    CHECK(Posts(0x694fe697, 0x3cb781ef) == 0);
    // After a pause and CONTINUE the message still comes (the pause menu is closed again); while the pause menu is
    // open, neither the post nor the answer's quit goes out.
    static uint8_t pause[0x1000];
    const uint8_t openCode[] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xb8, 0x00, 0x51, 0x05, 0x4b, 0x14, 0x21, 0x00, 0x32, 0x00};
    const uint8_t unpauseCode[] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x94, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x0c, 0x00, 0x32, 0x00};
    const uint8_t pollCode[] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xf4, 0x00, 0x51, 0x05, 0x4a, 0x02, 0x0e, 0x42, 0xd2, 0x86};  // the race-end poll, every frame
    memcpy(pause + 0x04c4 - 8, openCode, sizeof(openCode));
    memcpy(pause + 0x098d - 8, unpauseCode, sizeof(unpauseCode));
    memcpy(pause + 0x067e - 8, pollCode, sizeof(pollCode));
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } pvm = {nullptr, pause + 0x04c4, pause}, uvm = {nullptr, pause + 0x098d, pause},
                                                           pollvm = {nullptr, pause + 0x067e, pause};
    auto PauseSiteCall = [&](void* vm) { NativeCall type = {0x0916, 1, {}, nullptr, nullptr, 0, 0, 0}; NHook(kH0916)(vm, &type); return type.retValue; };
    c = HostRunning();
    g_posted.clear();
    g_clears = 0;
    CHECK(PauseSiteCall(&pvm) == 5);  // the online pause opens
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 0);
    CHECK(PauseSiteCall(&uvm) == 5);  // CONTINUE
    Frame();
    CHECK(DialogPosts() == 1);
    PauseSiteCall(&pvm);  // Esc under the message: the answer's quit waits for the menu to close
    for (int i = 0; i < 3; ++i) CHECK(PauseSiteCall(&pollvm) == 5);  // the menu's race-end poll, every frame it is up
    RaceNativeBefore(&dvm, &hide);
    Frame();
    CHECK(QuitPosts() == 0);
    PauseSiteCall(&pollvm);
    Frame();
    CHECK(QuitPosts() == 0);
    PauseSiteCall(&uvm);
    Frame();
    CHECK(QuitPosts() == 1 && g_clears == 3);
    // The answer during the race end's handover (the race over, the game screen still up): the quit waits for the
    // results' retry/quit screen; a Steam join asking in the same race doesn't quit twice.
    c = HostRunning();
    g_posted.clear();
    g_clears = 0;
    Deliver(B, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 1);
    g_raceOver = 1;
    RaceNativeBefore(&dvm, &hide);
    Frame();
    CHECK(QuitPosts() == 0);
    g_screen = 0x603658c4;
    RaceNativeBefore(&gvm, &inboxRead);
    CHECK(QuitPosts() == 1 && g_posted.back().value == 1);
    g_snap.pendingJoin = 0x0186000012345699ull;  // a Steam join now: the race is already being left
    RaceNativeBefore(&gvm, &inboxRead);
    CHECK(QuitPosts() == 1);
    g_snap.pendingJoin = 0;
    g_screen = 0xbed107e7;
    g_raceOver = 0;
    NHook(kH090a)(nullptr, &td);
    CHECK(Posts(0x694fe697, 0x3cb781ef) == 0);  // answered: the OK closed it
    // Guest: the host gone from the lobby -> "the host lost"; the host's quit while still in the lobby -> "the host ended the match".
    c = GuestRunning();
    g_posted.clear();
    g_snap.members[0] = g_snap.members[1];
    g_snap.memberCount = 1;
    Frame();
    CHECK(DialogPosts() == 1 && g_arrayElems[0][0] == 0x8374a07b);
    NHook(kH090a)(nullptr, &td);
    c = GuestRunning();
    g_posted.clear();
    Deliver(A, b, EncodeAbort(c.race, kAbortQuit, b, sizeof(b)));
    Frame();
    CHECK(DialogPosts() == 1 && g_arrayElems[0][0] == 0x168056c7);
    NHook(kH090a)(nullptr, &td);
    g_posted.clear();
}

// ---------------------------------------------------------------- the lobby's race bar: our progress (M7)
void RaceTestSetProgressAddress(uintptr_t distance);
static float g_fakeDistance = 0;
static float __fastcall FakeRecordDistance(void*, void*) { return g_fakeDistance; }

static void TestProgress() {
    printf("race progress: place, lap and distance for the lobby's race bar\n");
    RaceTestSetProgressAddress((uintptr_t)&FakeRecordDistance);
    RaceContract c = HostRunning();
    CHECK(RaceLastId() == c.race);
    *(float*)(g_graph + 100) = 1000.0f;  // the lap's length
    *(int32_t*)(g_records[0] + 0x80) = 2;  // place (0-based)
    *(int32_t*)(g_records[0] + 0x7c) = 1;  // lap shown
    g_fakeDistance = 1500.0f;  // half of 3 laps
    g_progressPosts = 0;
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_progressPosts == 1 && g_lastProgress[0] == 2 && g_lastProgress[1] == 1 && g_lastProgress[2] == 500 && g_lastProgress[3] == 3 &&
          (uint32_t)g_lastProgress[4] == c.race);
    for (int t = 0; t < 49; ++t) { ++g_tick; UHook(kHRacing)(nullptr, nullptr); }
    CHECK(g_progressPosts == 1);  // once a second
    *(int32_t*)(g_records[0] + 0x7c) = 2;
    g_fakeDistance = 2400.0f;
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_progressPosts == 2 && g_lastProgress[1] == 2 && g_lastProgress[2] == 800);
    g_fakeDistance = 99999.0f;  // past the end (the finish line): capped
    g_tick += 50;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_progressPosts == 3 && g_lastProgress[2] == 1000);
    g_tick += 50;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_progressPosts == 3);  // unchanged: not re-sent
    // This game's results start (RESULTS_START, results 0x1ade + 15): one more record with the "viewing results" flag (the lobby
    // bar's VIEWING RESULTS for a player waiting there), once; the close then clears the record.
    {
        static uint8_t results[0x2000];
        const uint8_t start[] = {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x51, 0x01, 0x32, 0x02, 0x03, 0x1b, 0x00, 0x00, 0xc6, 0x02};
        memcpy(results + 0x1ade, start, sizeof(start));
        struct { void* vtable; uint8_t* pc; uint8_t* mod; } vm = {nullptr, results + 0x1aed, results};
        NativeCall w = {0x1b03, 1, {}, nullptr, nullptr, 0, 0, 0};
        RaceNativeBefore(&vm, &w);
        CHECK(g_progressPosts == 4 && g_lastProgress[5] == 1 && g_lastProgress[2] == 1000 && g_lastProgress[0] == 2 && (uint32_t)g_lastProgress[4] == c.race);
        RaceNativeBefore(&vm, &w);
        CHECK(g_progressPosts == 4);  // once
    }
    NativeCall td = {};
    NHook(kH090a)(nullptr, &td);  // the race closed: "not racing"
    CHECK(g_progressPosts == 5 && g_lastProgress[0] == -1 && RaceLastId() == c.race);
    *(float*)(g_graph + 100) = 0;
}

// ---------------------------------------------------------------- a full lobby: 8 humans, no AI (M7)
static void Lobby8(uint64_t self, bool host) {
    g_self = self;
    g_snap = LobbySnapshot{};
    g_snap.state = kLobbyInLobby;
    g_snap.isHost = host;
    g_snap.memberCount = 8;
    for (int i = 0; i < 8; ++i) {
        uint64_t id = i == 0 ? A : B + (uint64_t)(i - 1);
        LobbyMember& m = g_snap.members[i];
        m = LobbyMember{};
        m.id = id;
        sprintf_s(m.name, "P%d", i + 1);
        m.isHost = i == 0;
        m.isMe = id == self;
        m.racer = i;
        m.ready = 1;
        m.net = 1;
        m.place = -1;  // (not racing)
    }
    g_snap.rules = {0, 3, 0, 1, 1, 1};
}

static void TestEightHumans() {
    printf("eight humans: a full lobby races with no AI\n");
    RaceTestSetHostAi(true);
    RaceTestSetItems(false);
    Reset();
    RaceUseRacerList(kRacerListRoster);
    Lobby8(A, true);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    CHECK(c.humanCount == 8 && c.humans[0] == A && c.humans[7] == B + 6 && c.flags == kContractHostAi);
    for (int k = 0; k < 8; ++k) CHECK(c.roster[k] == kRacersRoster[k].hash);  // every grid place is a player's racer
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    for (int k = 1; k < 8; ++k) CHECK(g_seen[k].character == c.roster[k] && g_seen[k].device == 15);
    UHook(kHSetup)(nullptr, nullptr);
    for (int k = 0; k < 8; ++k) CHECK(g_gridSortSaw[k] == 0x17);  // every car sorted as a human
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldA && g_setStateCalls == 7 && !RaceTestAiPuppets());  // the 7 other players' cars: network cars
    for (int k = 1; k < 8; ++k) CHECK(*(uint32_t*)(g_racerMem[k] + 0x9e4) == 0x15);
    uint8_t buf[kWireMax];
    for (int k = 1; k < 8; ++k) Deliver(c.humans[k], buf, EncodeLoaded(c.race, kStageLoaded, (uint8_t)k, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 1 && RaceTestPhase() == kIntros);
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldB);
    for (int k = 1; k < 8; ++k) Deliver(c.humans[k], buf, EncodeLoaded(c.race, kStageCountdown, (uint8_t)k, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 2 && RaceTestPhase() == kRunning);
    // The host's Cars message carries only its own car (no AI to drive); player 7's poses land on racer 7.
    for (int k = 0; k < 8; ++k) {
        *(float*)(g_racerMem[k] + 0xcc) = 1.0f;
        *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    }
    g_sent.clear();
    for (uint32_t t = 0; t < 40; ++t) {
        ++g_tick;
        RacePose p = {};
        p.race = c.race;
        p.slot = 7;
        p.tick = 1000 + t;
        p.pos[0] = (float)t;
        p.rot[3] = 1;
        p.vel[0] = 50;
        Deliver(c.humans[7], buf, EncodePose(p, buf, sizeof(buf)), false);
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    std::vector<RaceCars> cars = SentCars();
    CHECK(cars.size() >= 39 && CountSent(kMsgPose) == 0);
    for (const RaceCars& rc : cars) CHECK(rc.count == 1 && rc.cars[0].slot == 0);
    float x = *(float*)(g_racerMem[7] + 0xc0);
    CHECK(x > 25 && x < 39);
    CHECK(*(float*)(g_racerMem[3] + 0xc0) == 0);  // another player's car with no poses yet: left alone
    // Player 8 leaves: the host drives that car as one of its AI and sends it with its own.
    g_snap.memberCount = 7;
    g_sent.clear();
    for (int t = 0; t < 4; ++t) {
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    cars = SentCars();
    bool adopted = false;
    for (const RaceCars& rc : cars)
        if (rc.count == 2 && (rc.cars[0].slot == 7 || rc.cars[1].slot == 7)) adopted = true;
    CHECK(adopted && g_setStateId == 3 && g_setStateController == g_controllers[7]);
}

// ---------------------------------------------------------------- the intros (re\m6-intro\FINDINGS.md; the vote is gone)
void RaceTestSetIntroAddresses(uintptr_t flowRecord, uintptr_t flowAdvance, uintptr_t findText);
static uint8_t g_flowRec[0x20];  // the current flow record: +4 its state
static uint8_t* g_flowRecPtr = g_flowRec;
static int g_advances = 0;
static void __cdecl FakeFlowAdvance() {  // 0x39 -> 0x3a -> 0x3b, as the stock records chain; the phase timer restarts
    ++g_advances;
    int32_t& st = *(int32_t*)(g_flowRec + 4);
    st = st == 0x39 ? 0x3a : st == 0x3a ? 0x3b : st;
    g_timerElapsed = 0;
    g_timerLimit = 250;
}
static uint8_t g_inputRoot[0x20];  // [kInputPlayers] -> this; +0x14 -> P
static uint8_t* g_inputRootPtr = g_inputRoot;
static uint8_t g_player[0x1000];   // P: +0x960 device index, +0xc device table, +0x4c keyboard
static uint8_t g_device[0x100];    // the pad: +0xf7 / +0xf3 flag bytes
static uint8_t g_keyboard[0x520];  // vtable at +0; the per-scancode flag bytes at +0x4f8 (bit 4 = pressed this poll)
static void* g_kbVtable[0x30 / 4];
static int __fastcall FakeKeyPressed(void* kb, void*, uint32_t key) { return (((uint8_t*)kb)[0x4f8 + key] & 0x10) ? 1 : 0; }
static void PressEnter(bool on) { g_keyboard[0x4f8 + 0x1c] = on ? 0x10 : 0; }
static bool IntroPressFake(bool course) {
    return (g_device[course ? 0xf7 : 0xf3] & 0x10) != 0 || (g_keyboard[0x4f8 + 0x1c] & 0x10) != 0;
}
static uint8_t g_element[0x120];   // the CONTINUE text element: +0x104 -> its descriptor; descriptor +0x18 -> the string
static uint8_t g_desc[0x20];
static bool g_elementPresent = true;
static uint8_t* __fastcall FakeFindText(uintptr_t table, void*, uint32_t hash) {
    CHECK(table == 0xc28340);
    if (hash == 0xca4107ae) return g_toastElement;
    return g_elementPresent && hash == 0xd80223a4 ? g_element : nullptr;
}
static int g_cues = 0;
static uint32_t g_lastCue = 0;
static void __cdecl FakePlayCue(uint32_t cue, int32_t, int32_t) { ++g_cues; g_lastCue = cue; }
// The announcer's speech queue (vtable 0x6a464c-like, its update in slot 1; +0x16 line count, +0x24 run state) and the
// course intro's music (scene +0x1334, a sound object whose slot 0x6c is "is playing").
void RaceTestSetIntroSoundAddresses(uintptr_t voiceQueue, uintptr_t voiceVtable, uintptr_t voiceUpdate, uintptr_t soundVtable, uintptr_t isPlaying);
static void __fastcall FakeVoiceUpdate(void*, void*) {}
static uintptr_t g_voiceVt[2] = {0, (uintptr_t)&FakeVoiceUpdate};
static uint8_t g_voice[0x30];
static uint8_t* g_voicePtr = g_voice;
static bool g_musicPlaying = true;
static bool __fastcall FakeIsPlaying(void* self, void*) { return self && g_musicPlaying; }
static uintptr_t g_soundVt[0x70 / 4] = {};
static uint8_t g_music[0x10];
static void IntroSounds(int lines, bool music) {  // the announcer's lines still queued (running), the music playing
    *(uintptr_t*)g_voice = (uintptr_t)g_voiceVt;
    *(uint16_t*)(g_voice + 0x16) = (uint16_t)lines;
    g_voice[0x24] = 1;
    *(uintptr_t*)g_music = (uintptr_t)g_soundVt;
    g_soundVt[0x6c / 4] = (uintptr_t)&FakeIsPlaying;
    g_musicPlaying = music;
    *(uint8_t**)(g_scene + 0x1334) = g_music;
}
static int g_gateSeen = -1;
static int g_stockSkips = 0;
// The stock intro updates (re\m6-intro\FINDINGS.md): the course intro raises its gate from 0 to 1 on its first tick and
// skips on a press while the gate is 1 (the timer only counts); the racer intro never writes its gate, skips on a press
// while it is 1, and ends at its timer. Both advance through the flow advance.
static void __fastcall FakeTrackIntro(void* self, void*) {
    uint8_t* s = (uint8_t*)self;
    g_gateSeen = s ? s[0x10] : -1;
    ++g_timerElapsed;
    if (s && s[0x10] == 0) s[0x10] = 1;
    if (s && s[0x10] == 1 && IntroPressFake(true)) { ++g_stockSkips; FakeFlowAdvance(); }
    EnterQueuedStates();
}
static void __fastcall FakeCharIntro(void* self, void*) {
    uint8_t* s = (uint8_t*)self;
    g_gateSeen = s ? s[0x10] : -1;
    ++g_timerElapsed;
    if (g_timerElapsed >= g_timerLimit || (s && s[0x10] == 1 && IntroPressFake(false))) { ++g_stockSkips; FakeFlowAdvance(); }
    EnterQueuedStates();
}

static void GuestAtIntros(const RaceContract& c) {  // a guest race up to the intros (hold A released)
    Reset();
    Lobby(B, false, 0, 1);
    uint8_t buf[kWireMax];
    Deliver(A, buf, EncodeContract(c, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    Script0905(0x11111111, "STATE_Control_StartingGridAI", 0);
    Script0905(c.roster[1], "STATE_Control_StartingGridHuman", 2);
    for (int k = 2; k < 8; ++k) Script0905(0x22222222 + k, "STATE_Control_StartingGridAI", 0);
    uint8_t types[4] = {kTypeHash, kTypeHash, kTypeHash, kTypeHash};
    uint32_t values[4] = {1, 0, 7, 0};
    NativeCall d = {0x0900, 1, {}, types, values, 4, 0, 0};
    NHook(kH0900)(nullptr, &d);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(c.race, kStageLoaded, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kIntros);
    *(int32_t*)(g_flowRec + 4) = 0x39;
    g_timerElapsed = 0;
    g_timerLimit = 250;
    g_advances = g_cues = g_textCalls = g_stockSkips = 0;
    PressEnter(false);
    memset(g_device, 0, sizeof(g_device));
    IntroSounds(2, true);  // both announcer lines queued, the music playing
}

static void TestIntros(const RaceContract& c) {
    printf("intros: each screen's own skip, the course intro's end with the announcer and the music, the longest wait\n");
    RaceTestSetHostAi(false);
    RaceTestSetItems(false);
    *(uint8_t**)(g_inputRoot + 0x14) = g_player;
    *(int32_t*)(g_player + 0x960) = 0;
    *(uint8_t**)(g_player + 0xc) = g_device;
    *(uint8_t**)(g_player + 0x4c) = g_keyboard;
    *(void**)g_keyboard = g_kbVtable;
    g_kbVtable[0x28 / 4] = (void*)&FakeKeyPressed;
    uint8_t track[0x20] = {}, racer[0x20] = {};
    GuestAtIntros(c);
    // The course intro is the stock one: its gate as the stock update leaves it (0 -> 1 on its first tick), CONTINUE as the game
    // shows it (no text written), nothing sent.
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_gateSeen == 0 && track[0x10] == 1 && g_advances == 0);
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_gateSeen == 1 && track[0x10] == 1 && g_textCalls == 0 && g_advances == 0);
    // Our Enter: the stock skip, on this screen only (the announcer's lines still queued); nothing sent, no sound of ours.
    PressEnter(true);
    UHook(kHTrackIntro)(track, nullptr);
    PressEnter(false);
    CHECK(g_stockSkips == 1 && g_advances == 1 && *(int32_t*)(g_flowRec + 4) == 0x3a && g_cues == 0);
    CHECK(CountSent(kMsgIntroVote) == 0);
    *(int32_t*)(g_flowRec + 4) = 0x39;  // a course-intro tick before the flush: no advance of ours on top
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1);
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    // The racer intro is the stock one: its gate as it is; it plays its own 5 s, then the countdown (hold B).
    racer[0x10] = 1;
    UHook(kHCharIntro)(racer, nullptr);
    CHECK(g_gateSeen == 1 && racer[0x10] == 1 && g_advances == 1 && *(int32_t*)(g_flowRec + 4) == 0x3a);
    for (int t = 0; t < 300 && *(int32_t*)(g_flowRec + 4) == 0x3a; ++t) UHook(kHCharIntro)(racer, nullptr);
    CHECK(g_advances == 2 && g_stockSkips == 2 && *(int32_t*)(g_flowRec + 4) == 0x3b && g_cues == 0);  // its timer's own advance
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldB);
    // A press in the course intro's first tick: the stock skip too (nothing held off or consumed).
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    PressEnter(true);
    UHook(kHTrackIntro)(track, nullptr);
    PressEnter(false);
    CHECK(g_stockSkips == 1 && g_advances == 1);
    // A press at the racer intro (the pad's id 0x1f): the stock skip, on this screen only; hold B then waits for the others.
    GuestAtIntros(c);
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    racer[0x10] = 1;
    g_device[0xf3] = 0x10;
    UHook(kHCharIntro)(racer, nullptr);
    g_device[0xf3] = 0;
    CHECK(g_advances == 1 && g_stockSkips == 1 && *(int32_t*)(g_flowRec + 4) == 0x3b);
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldB);

    // No press: the course intro waits for the announcer AND the music, even when the announcer finishes first; a short tune
    // (the music first, then the announcer) waits for the announcer; the racer intro runs on its own timer.
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    for (int t = 0; t < 600; ++t) UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 0);
    *(uint16_t*)(g_voice + 0x16) = 0;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 0);  // the music still plays
    g_musicPlaying = false;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1 && *(int32_t*)(g_flowRec + 4) == 0x3a && g_cues == 0 && g_stockSkips == 0);
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    g_musicPlaying = false;  // House of the Dead's 4 s tune: over before the announcer
    for (int t = 0; t < 300; ++t) UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 0);
    *(uint16_t*)(g_voice + 0x16) = 0;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1 && g_cues == 0);
    racer[0x10] = 1;
    for (int t = 0; t < 10; ++t) UHook(kHCharIntro)(racer, nullptr);
    CHECK(g_advances == 1 && racer[0x10] == 1);
    // A stopped announcer queue (run state 0) never drains: it counts as finished. No queue, no music: finished too.
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    g_voice[0x24] = 0;
    g_musicPlaying = false;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1);
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    g_voicePtr = nullptr;
    *(uint8_t**)(g_scene + 0x1334) = nullptr;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1);
    g_voicePtr = g_voice;
    // Another object where the queue or the music should be (its vtable differs): not waited for.
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    *(uintptr_t*)g_voice = 0x12345678;
    *(uintptr_t*)g_music = 0x12345678;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1);
    // Neither ever ends (a paused game, a stuck stream): the longest wait, 1250 ticks.
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    for (int t = 0; t < 1249; ++t) UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 0 && g_timerElapsed == 1249);
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1 && g_cues == 0);
    // An old build's vote (wire v8 still has the message): ignored.
    GuestAtIntros(c);
    memset(track, 0, sizeof(track));
    {
        uint8_t buf[kWireMax];
        Deliver(A, buf, EncodeIntroVote(c.race, 0, buf, sizeof(buf)));
    }
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 0 && g_textCalls == 0);

    // Offline race (no contract): the stock updates run untouched (the gate as it is, a press is the stock skip's).
    NativeCall td = {};
    NHook(kH090a)(nullptr, &td);
    *(int32_t*)(g_flowRec + 4) = 0x39;
    g_advances = g_stockSkips = 0;
    track[0x10] = 1;
    PressEnter(true);
    UHook(kHTrackIntro)(track, nullptr);
    PressEnter(false);
    CHECK(g_gateSeen == 1 && g_stockSkips == 1 && g_advances == 1);
}

// ---------------------------------------------------------------- test players (debugging, lobby.h)
static const uint64_t T1 = kTestPlayerBase + 1, T2 = kTestPlayerBase + 2;
static void LobbyWithTestPlayers(uint64_t self, bool host) {  // Alfa hosts, Bravo 2, two test players (as steam_mp.cpp adds them)
    Lobby(self, host, 0, 1);
    g_snap.memberCount = 4;
    g_snap.members[2] = {T1, "Test Player 1", 0, 0, 2, 1, 1, -1};
    g_snap.members[3] = {T2, "Test Player 2", 0, 0, 3, 1, 1, -1};
}

static void TestTestPlayers() {
    printf("test players: counted as players, driven by the host, never waited for\n");
    RaceTestSetHostAi(true);
    RaceTestSetItems(false);
    // Host: the contract names them; their cars are the host's AI from hold A; only the real guest is waited for.
    Reset();
    RaceUseRacerList(kRacerListRoster);
    LobbyWithTestPlayers(A, true);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c));
    CHECK(c.humanCount == 4 && c.humans[2] == T1 && c.humans[3] == T2 && c.roster[2] == kRacersRoster[2].hash);
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldA);
    uint8_t buf[kWireMax];
    Deliver(B, buf, EncodeLoaded(c.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 1 && RaceTestPhase() == kIntros);
    // The course intro: test players cast nothing (each screen skips for itself); it ends with the announcer and the music.
    *(int32_t*)(g_flowRec + 4) = 0x39;
    g_timerElapsed = 0;
    g_advances = g_cues = 0;
    IntroSounds(2, true);
    uint8_t track[0x20] = {};
    PressEnter(false);
    memset(g_device, 0, sizeof(g_device));
    g_sent.clear();
    for (int t = 0; t < 120; ++t) UHook(kHTrackIntro)(track, nullptr);
    CHECK(CountSent(kMsgIntroVote) == 0 && g_advances == 0);
    *(uint16_t*)(g_voice + 0x16) = 0;
    g_musicPlaying = false;
    UHook(kHTrackIntro)(track, nullptr);
    CHECK(g_advances == 1 && g_cues == 0);
    // Hold B: released once the real guest is there; the race runs and the host's Cars carry the test players' cars.
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kHoldB);
    Deliver(B, buf, EncodeLoaded(c.race, kStageCountdown, 1, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(CountSent(kMsgGo) == 1 && RaceTestPhase() == kRunning);  // (the sent list was cleared after the first)
    CHECK(g_seed == (c.seed ^ 0x474f474fu));  // the shared RNG reseeded at the release (the intros' draws may differ per PC)
    for (int k = 0; k < 8; ++k) {
        *(float*)(g_racerMem[k] + 0xcc) = 1.0f;
        *(float*)(g_racerMem[k] + 0xdc) = 1.0f;
    }
    g_sent.clear();
    g_testProgressFor = g_testProgressClears = 0;
    for (int t = 0; t < 4; ++t) {
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        for (int k = 0; k < 8; ++k) Commit(g_racerMem[k]);
    }
    int seen = 0;
    for (const RaceCars& rc : SentCars())
        for (int i = 0; i < rc.count; ++i) seen |= 1 << rc.cars[i].slot;
    CHECK((seen & 0xd) == 0xd && !(seen & 2));  // its own car and the test players', not the guest's
    // The lobby's race bar: the host reports its test players' progress (they have no game to), cleared at the end.
    CHECK(g_testProgressFor == 0x6 && g_testProgressClears == 0);  // test players 1 and 2
    // Test Player 1's car crosses the line first: a player's finish (no Finished can come): "RACE ENDS IN" starts.
    g_timerStarts = 0;
    Append(2, 1);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    uint8_t wff[0x20] = {};
    *(int32_t*)(wff + 0x10) = -1;
    UHook(kHWff)(wff, nullptr);
    CHECK(g_timerStarts == 1 && g_timerSeconds == 30 && !g_raceOver);
    {
        NativeCall end = {};
        NHook(kH090a)(nullptr, &end);
    }
    CHECK(g_testProgressClears == 1);

    Reset();
}

// ---------------------------------------------------------------- the shared announcer (M7, re\m7-announcer\FINDINGS.md)
void RaceTestSetAnnouncer(bool hooks, uintptr_t commentMan, uintptr_t play, uintptr_t flush, uintptr_t child, uintptr_t request,
                          uintptr_t wrongWay);
typedef uint32_t(__fastcall* TakeHook)(uint8_t* self, void*, uint32_t arg);
typedef void(__fastcall* QueueHook)(void* queue, void*, uint32_t ms);
// A sound group: +4 its hash, +0x14 its take count (the list object), +0x18 -> its takes (8 bytes each: +0 hash).
struct FakeGroup {
    uint8_t mem[0x20];
    uint8_t items[8][8];
};
static FakeGroup g_groups[4];
static uint8_t* __fastcall FakeGroupChild(uint8_t* takes, void*, int i) { return *(uint8_t**)(takes + 4) + i * 8; }
static void MakeGroup(FakeGroup& gr, uint32_t hash, std::vector<uint32_t> takes) {
    memset(&gr, 0, sizeof(gr));
    *(uint32_t*)(gr.mem + 4) = hash;
    for (size_t i = 0; i < takes.size(); ++i) *(uint32_t*)gr.items[i] = takes[i];
    *(int32_t*)(gr.mem + 0x14) = (int32_t)takes.size();
    *(uint8_t**)(gr.mem + 0x18) = gr.items[0];
}
static int g_stockPicks = 0;
static uint32_t __fastcall FakeSelectStock(uint8_t* group, void*, uint32_t) { ++g_stockPicks; return **(uint32_t**)(group + 0x18); }  // its first take
static uint32_t __fastcall FakeSourceStock(uint8_t* take, void*, uint32_t) { return *(uint32_t*)(take + 0xc); }
static uint8_t g_cm[0x40], g_entries[4][0x40];
static uint8_t* g_entryList[4];
static uint8_t* g_cmPtr = g_cm;
static uint32_t g_saidTake = 0;
static int g_flushes = 0, g_speechUpdates = 0, g_wrongWayCalls = 0, g_wrongWayIndex = -1;
// 0x4bbec0: a line through the game's gates, here straight to the resolver: the group's take pick, then its source.
static void __cdecl FakeCommentRequest(uint32_t cue, int32_t, int32_t) {
    for (FakeGroup& gr : g_groups)
        if (*(uint32_t*)(gr.mem + 4) == cue) {
            uint8_t take[0x10] = {};
            *(uint32_t*)(take + 4) = ((TakeHook)RaceTestHook(kHTakeSelect))(gr.mem, nullptr, 0);
            ((TakeHook)RaceTestHook(kHTakeSource))(take, nullptr, 0);
        }
}
static void __cdecl FakeWrongWayCall(int32_t index) { ++g_wrongWayCalls; g_wrongWayIndex = index; }
static bool g_playFull = false;  // the speech queue has no room (0x46d6b0 refuses before it resolves)
static int g_playRefused = 0;
static bool __fastcall FakeCommentPlay(uint8_t* cm, void*, uint8_t* entry) {  // 0x46d6b0: the entry's group through the resolver
    CHECK(cm == g_cm);
    if (g_playFull) {
        ++g_playRefused;
        return false;
    }
    g_saidTake = 0;
    for (FakeGroup& gr : g_groups)
        if (*(uint32_t*)(gr.mem + 4) == *(uint32_t*)(entry + 4)) g_saidTake = ((TakeHook)RaceTestHook(kHTakeSelect))(gr.mem, nullptr, 0);
    return g_saidTake != 0;
}
static void __cdecl FakeFlush() { ++g_flushes; }
static void __fastcall FakeSpeechUpdate(void*, void*, uint32_t) { ++g_speechUpdates; }
static uint32_t TestMix32(uint32_t x) {  // as race.cpp Mix32
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}

static void TestAnnouncer() {
    printf("announcer: the same intro takes everywhere; the host's lines said on the guests\n");
    RaceTestSetAnnouncer(true, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeCommentRequest, (uintptr_t)&FakeWrongWayCall);
    RaceTestSetOriginal(kHTakeSelect, (void*)&FakeSelectStock);
    RaceTestSetOriginal(kHTakeSource, (void*)&FakeSourceStock);
    RaceTestSetOriginal(kHSpeechUpdate, (void*)&FakeSpeechUpdate);
    MakeGroup(g_groups[0], 0x1111, {0xa1, 0xa2, 0xa3, 0xa4});
    MakeGroup(g_groups[1], 0x2222, {0xa4, 0xa3, 0xa2, 0xa1});  // the same takes, in another order
    MakeGroup(g_groups[2], 0x3333, {0xb1, 0xb2});
    MakeGroup(g_groups[3], 0xd95d80cd, {0xc1, 0xc2});         // LAP2
    for (int i = 0; i < 4; ++i) {
        memset(g_entries[i], 0, sizeof(g_entries[i]));
        *(uint32_t*)(g_entries[i] + 4) = *(uint32_t*)(g_groups[i].mem + 4);
        *(int32_t*)(g_entries[i] + 0x24) = 2500 + i;
        g_entryList[i] = g_entries[i];
    }
    *(uint32_t*)(g_entries[3] + 0x20) = 30000;  // LAP2's cooldown
    *(uint8_t***)(g_cm + 0x14) = g_entryList;
    *(int32_t*)(g_cm + 0x18) = 4;
    TakeHook select = (TakeHook)RaceTestHook(kHTakeSelect), source = (TakeHook)RaceTestHook(kHTakeSource);
    QueueHook update = (QueueHook)RaceTestHook(kHSpeechUpdate);
    uint8_t buf[kWireMax];

    // The course intro: the race's take (the smallest mix of the seed and its hash), whatever its list's order; no stock pick.
    RaceTestSetHostAi(true);
    Reset();
    Lobby(A, true, 0, 1);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract c = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, c) && (c.flags & kContractAnnouncer));
    GuestAtIntros(c);
    g_stockPicks = 0;
    uint32_t want = 0;
    for (uint32_t t : {0xa1u, 0xa2u, 0xa3u, 0xa4u})
        if (!want || TestMix32(c.seed ^ t) < TestMix32(c.seed ^ want)) want = t;
    CHECK(select(g_groups[0].mem, nullptr, 0) == want && select(g_groups[1].mem, nullptr, 0) == want && g_stockPicks == 0);
    CHECK(CountSent(kMsgAnnounce) == 0);
    // With the announcer group, the speech queue's update slot holds our hook: the course intro still waits for the
    // announcer's lines (the independent check's finding: it took that for "no queue" and never waited).
    g_voiceVt[1] = (uintptr_t)RaceTestHook(kHSpeechUpdate);
    g_musicPlaying = false;
    {
        uint8_t track[0x20] = {};
        for (int t = 0; t < 50; ++t) UHook(kHTrackIntro)(track, nullptr);
        CHECK(g_advances == 0);
        *(uint16_t*)(g_voice + 0x16) = 0;
        UHook(kHTrackIntro)(track, nullptr);
        CHECK(g_advances == 1);
    }
    g_voiceVt[1] = (uintptr_t)&FakeVoiceUpdate;

    // Host: each line it queues goes to everyone (its stock take; a single-take entry is its own cue). Its own player's
    // lap calls are held (their entry looks busy for good); the first car to reach a crossing gets the lap call, after a
    // flush, passed on; another player's wrong-way report is called.
    for (int k = 0; k < 8; ++k) *(uint32_t*)(g_records[k] + 0x74) = 1;
    c = HostRunning();
    CHECK(*(uint32_t*)(g_entries[3] + 0x20) == 0x7fffffff && g_entries[3][0x35] == 1);
    uint8_t take[0x10] = {};
    *(uint32_t*)(take + 4) = select(g_groups[0].mem, nullptr, 0);
    *(uint32_t*)(take + 0xc) = 0xcd01;
    CHECK(*(uint32_t*)(take + 4) == 0xa1 && source(take, nullptr, 0) == 0xcd01);
    *(uint32_t*)(take + 4) = 0x3333;
    source(take, nullptr, 0);
    *(uint32_t*)(g_records[5] + 0x74) = 2;  // car 5 starts lap 2 first
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    *(uint32_t*)(g_records[0] + 0x74) = 2;  // the host's car later: no call
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    std::vector<RaceAnnounce> said;
    for (const RaceNetMsg& m : g_sent) {
        RaceAnnounce a;
        if (DecodeAnnounce(m.data, m.size, a)) { CHECK(m.reliable && m.peer == 0 && a.race == c.race); said.push_back(a); }
    }
    CHECK(said.size() == 3 && said[0].cue == 0x1111 && said[0].take == 0xa1 && said[0].wait == 2500 && !said[0].flush);
    CHECK(said[1].cue == 0x3333 && said[1].take == 0x3333 && said[1].wait == 2502 && !said[1].flush);
    CHECK(said[2].cue == 0xd95d80cd && said[2].take == 0xc1 && said[2].flush);
    CHECK(*(uint32_t*)(g_entries[3] + 0x20) == 0x7fffffff && g_entries[3][0x35] == 1);  // still held for the host's own
    Deliver(B, buf, EncodeWrongWay(c.race, 1, buf, sizeof(buf)));
    Deliver(B, buf, EncodeWrongWay(c.race, 0, buf, sizeof(buf)));  // not its car: ignored
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_wrongWayCalls == 1 && g_wrongWayIndex == 1);
    NativeCall end = {};
    NHook(kH090a)(nullptr, &end);
    CHECK(*(uint32_t*)(g_entries[3] + 0x20) == 30000 && g_entries[3][0x35] == 0);  // the race over: released

    // Guest: its own commentary muted from the countdown on; the host's line said with the host's take, after the
    // host's flush; a take this PC lacks is its own pick; the host gone: its own commentary again.
    c = GuestRunning();
    CHECK(g_scene[0x1350] == 1);
    RaceAnnounce h = {c.race, 100, 0x1111, 0xa3, 4000, 1};
    Deliver(A, buf, EncodeAnnounce(h, buf, sizeof(buf)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    g_flushes = g_speechUpdates = 0;
    update(g_voice, nullptr, 16);
    CHECK(g_saidTake == 0xa3 && g_flushes == 1 && g_speechUpdates == 1 && *(int32_t*)(g_entries[0] + 0x24) == 4000 && g_entries[0][0x35] == 1);
    update(g_voice, nullptr, 16);
    CHECK(g_speechUpdates == 2 && g_flushes == 1);  // said once
    // The speech queue here is full (this game's own voices at the finish): the host's lines wait for room, in order (the
    // first refused, the next not tried), and are said once there is; one that waited 10 s goes unsaid.
    {
        RaceAnnounce w1 = {c.race, 101, 0x1111, 0xa2, 4000, 0}, w2 = {c.race, 102, 0x3333, 0xb2, 4000, 0};
        Deliver(A, buf, EncodeAnnounce(w1, buf, sizeof(buf)));
        Deliver(A, buf, EncodeAnnounce(w2, buf, sizeof(buf)));
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        g_playFull = true;
        g_playRefused = 0;
        update(g_voice, nullptr, 16);
        update(g_voice, nullptr, 16);
        CHECK(g_playRefused == 2);  // (only the first line tried, once per update)
        g_playFull = false;
        g_saidTake = 0;
        update(g_voice, nullptr, 16);
        CHECK(g_saidTake == 0xb2);  // both said, in order (the last one's take shows)
        g_saidTake = 0;
        update(g_voice, nullptr, 16);
        CHECK(g_saidTake == 0);  // nothing left
        Deliver(A, buf, EncodeAnnounce(w1, buf, sizeof(buf)));
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
        g_playFull = true;
        update(g_voice, nullptr, 16);
        g_tick += 501;
        update(g_voice, nullptr, 16);  // 10 s: dropped
        g_playFull = false;
        g_saidTake = 0;
        update(g_voice, nullptr, 16);
        CHECK(g_saidTake == 0);
    }
    h.take = 0xff;
    h.flush = 0;
    Deliver(A, buf, EncodeAnnounce(h, buf, sizeof(buf)));
    Deliver(B + 5, buf, EncodeAnnounce(h, buf, sizeof(buf)));  // not the host: ignored
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    g_stockPicks = 0;
    update(g_voice, nullptr, 16);
    CHECK(g_saidTake == 0xa1 && g_stockPicks == 1);
    // This game's car turns the wrong way: the host is told, once.
    g_sent.clear();
    g_racerMem[1][0xa28] = 1;
    for (int t = 0; t < 3; ++t) {
        ++g_tick;
        UHook(kHRacing)(nullptr, nullptr);
    }
    CHECK(CountSent(kMsgWrongWay) == 1 && g_sent.back().peer == A);
    g_racerMem[1][0xa28] = 0;
    g_snap.members[0] = g_snap.members[1];  // the host left the lobby
    g_snap.memberCount = 1;
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_scene[0x1350] == 0);
    // A host without the announcer hooks: its guests keep their own commentary.
    RaceTestSetAnnouncer(false, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeCommentRequest, (uintptr_t)&FakeWrongWayCall);
    c = GuestRunning();
    CHECK(!(c.flags & kContractAnnouncer) && g_scene[0x1350] == 0);
    Reset();
}

// ---------------------------------------------------------------- fair call-outs (M7, re\m7-announcer2\FINDINGS.md §1, §2)
void RaceTestSetFairAddresses(uintptr_t overtakeCall, uintptr_t overtakeCooldown, uintptr_t alertCooldown, uintptr_t placeList, uintptr_t raceClock,
                              uintptr_t commentOff, uintptr_t raceMode, uintptr_t calloutsLive, uintptr_t voiceWin, uintptr_t voiceLose,
                              uintptr_t genericAlert);
void RaceTestSetFair(bool hooks, float roll, int coin);
int RaceTestMentions(int k);
static float g_overtakeCd = 45.0f, g_alertCd = 50.0f;
static uint8_t g_placeList[8][8];
static uint32_t g_raceClock = 100, g_generic = 0;
static uint8_t g_commentOff = 0, g_calloutsLive = 1, g_offSeen = 0xff;
static int32_t g_raceMode = 1;
static uint8_t g_trackers[8][0x100], g_voices[8][0x800];
static float g_cdSeen = -1.0f;
struct OvertakeSeen { int32_t index; uint8_t gained; int32_t place; };
static std::vector<OvertakeSeen> g_overtakes;
struct RequestSeen { uint32_t cue; int32_t wait; };
static std::vector<RequestSeen> g_requests;
static std::vector<std::pair<int, bool>> g_voiceLines;  // (car, won)
// 0x4c4840 inside slot 3: the cooldown it would speak from, then the car's place into its tracker.
static void __fastcall FakeTrack(uint8_t* racer, void*) {
    g_cdSeen = g_overtakeCd;
    int k = (int)((racer - g_racerMem[0]) / sizeof(g_racerMem[0]));
    *(int32_t*)g_trackers[k] = *(int32_t*)(g_records[k] + 0x80);
}
static void __cdecl FakeOvertakeCall(int32_t index, uint8_t gained, int32_t place) {  // 0x4bc180: speaks from 45 s
    g_overtakes.push_back({index, gained, place});
    if (g_overtakeCd >= 45.0f) g_overtakeCd = 0.0f;
}
static uint8_t g_queueRecords[8][0x14];
static uint8_t* g_queueSlots[8];
static uint8_t g_sounderPool[8][4];
// 0x4bbec0 here: the request logged and its line appended to the fake speech queue (g_voice: slots +0x10, cap +0x14, count
// +0x16) with a sounder from the queue's pool, as 0x46e2e0 picks it: the first one no queued record uses (a line that has
// ended gives its sounder to the next).
static void __cdecl FakeRequestLog(uint32_t cue, int32_t wait, int32_t) {
    g_requests.push_back({cue, wait});
    uint16_t& count = *(uint16_t*)(g_voice + 0x16);
    *(uint16_t*)(g_voice + 0x14) = 8;
    if (count >= 8) return;
    *(uint8_t***)(g_voice + 0x10) = g_queueSlots;
    void* sounder = nullptr;
    for (int p = 0; p < 8 && !sounder; ++p) {
        bool used = false;
        for (int i = 0; i < count; ++i) used = used || *(void**)g_queueRecords[i] == g_sounderPool[p];
        if (!used) sounder = g_sounderPool[p];
    }
    g_queueSlots[count] = g_queueRecords[count];
    *(void**)g_queueRecords[count] = sounder;
    ++count;
}
static void QueueDone() { *(uint16_t*)(g_voice + 0x16) = 0; }  // the announcer has said everything
static void __fastcall FakeVoiceWin(uint8_t* voice, void*, int32_t) { g_voiceLines.push_back({(int)((voice - g_voices[0]) / sizeof(g_voices[0])), true}); }
static void __fastcall FakeVoiceLose(uint8_t* voice, void*, int32_t) { g_voiceLines.push_back({(int)((voice - g_voices[0]) / sizeof(g_voices[0])), false}); }
static uint8_t g_muteSeen = 0xff;
static void __fastcall FakeRaceEndEnter(void*, void*) { g_offSeen = g_commentOff; g_muteSeen = g_scene[0x1350]; }
static uint32_t Cue(int k, int idx) { return 0xc0000000u | (uint32_t)(k << 8) | (uint32_t)idx; }  // car k's voice cue idx
static void Track(int k) { ((UpdateHook)RaceTestHook(kHRacerTrack))(g_racerMem[k], nullptr); }
// Car r passes from place prev to cur; the car now at prev is o.
static void SetPass(int r, int32_t prev, int32_t cur, int o) {
    *(int32_t*)g_trackers[r] = prev;
    *(int32_t*)(g_records[r] + 0x80) = cur;
    g_placeList[prev][0] = (uint8_t)o;
    g_overtakes.clear();
    g_cdSeen = -1.0f;
}
static void FinishOrder(std::vector<int> cars, int dnf = -1) {
    g_finishCount = (uint8_t)cars.size();
    for (size_t i = 0; i < cars.size(); ++i) {
        g_finishList[i][0] = (uint8_t)cars[i];
        *(uint32_t*)(g_finishList[i] + 4) = cars[i] == dnf ? 6 : 1;
    }
    g_requests.clear();
    g_voiceLines.clear();
    g_offSeen = 0xff;
}

static void TestFairCalls() {
    printf("fair call-outs: passes, homing warnings, the finish, fresh recordings\n");
    RaceTestSetAnnouncer(true, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeRequestLog, (uintptr_t)&FakeWrongWayCall);
    RaceTestSetFairAddresses((uintptr_t)&FakeOvertakeCall, (uintptr_t)&g_overtakeCd, (uintptr_t)&g_alertCd, (uintptr_t)g_placeList,
                             (uintptr_t)&g_raceClock, (uintptr_t)&g_commentOff, (uintptr_t)&g_raceMode, (uintptr_t)&g_calloutsLive,
                             (uintptr_t)&FakeVoiceWin, (uintptr_t)&FakeVoiceLose, (uintptr_t)&g_generic);
    RaceTestSetOriginal(kHRacerTrack, (void*)&FakeTrack);
    RaceTestSetOriginal(kHRaceEnd, (void*)&FakeRaceEndEnter);
    RaceTestSetOriginal(kHTakeSelect, (void*)&FakeSelectStock);
    RaceTestSetOriginal(kHTakeSource, (void*)&FakeSourceStock);
    RaceTestSetHostAi(true);
    RaceTestSetFair(true, 0.0f, 0);  // every chance taken; the stock's coin on the passer's side
    RaceContract c = HostRunning();
    memset(g_trackers, 0, sizeof(g_trackers));
    memset(g_voices, 0, sizeof(g_voices));
    for (int k = 0; k < 8; ++k) {
        *(int32_t*)(g_racerMem[k] + 0xa1c) = 2;  // racing
        *(uint8_t**)(g_racerMem[k] + 0x2040) = g_trackers[k];
        *(uint8_t**)(g_racerMem[k] + 0x9f4) = g_voices[k];
        for (int idx : {9, 0x10, 0x11, 0x12, 0x13, 0x14}) *(uint32_t*)(g_voices[k] + 0x72c + idx * 4) = Cue(k, idx);
    }
    g_overtakeCd = 45.0f;

    // The guest's car 1 passes computer car 4 for 6th (place 5): the stock's pick quiet (it saw 0 s), car 1's GAIN said (chance
    // 1 - 0.65 * 5/7 = 0.54 with roll 0).
    SetPass(1, 6, 5, 4);
    Track(1);
    CHECK(g_cdSeen == 0.0f && g_overtakes.size() == 1 && g_overtakes[0].index == 1 && g_overtakes[0].gained == 1 && g_overtakes[0].place == 5);
    CHECK(g_overtakeCd == 0.0f && RaceTestMentions(1) == 1 && *(int32_t*)g_trackers[1] == 5);
    // Due again 90 s later: a roll above the chance: no line (the computer racer's side isn't the stock coin's; its window is
    // under 4 for LOST anyway).
    g_tick += 4500;
    g_overtakeCd = 45.0f;
    RaceTestSetFair(true, 0.6f, 0);
    SetPass(1, 6, 5, 4);
    Track(1);
    CHECK(g_cdSeen == 0.0f && g_overtakes.empty() && g_overtakeCd == 45.0f);
    // The coin on the computer racer's side near the front (car 1 to 2nd, car 3 now 3rd): the player's chance first
    // (1 - 0.65 / 7 = 0.91 > 0.6: GAIN for car 1).
    RaceTestSetFair(true, 0.6f, 1);
    SetPass(1, 2, 1, 3);
    Track(1);
    CHECK(g_overtakes.size() == 1 && g_overtakes[0].index == 1 && g_overtakes[0].gained == 1 && g_overtakes[0].place == 1);
    // Just mentioned: (b) 0.3 x 0.91 = 0.27 < 0.6: the player's line not said; the coin's computer side then is, as the
    // stock (LOST for car 3 at place 2: the passer's new place 1 < 4).
    g_overtakeCd = 45.0f;
    SetPass(1, 2, 1, 3);
    Track(1);
    CHECK(g_overtakes.size() == 1 && g_overtakes[0].index == 3 && g_overtakes[0].gained == 0 && g_overtakes[0].place == 2);
    CHECK(RaceTestMentions(3) == 0);  // (computer racers aren't counted)
    // Two computer racers: the stock's own pick, untouched (it saw 45 s).
    g_overtakeCd = 45.0f;
    SetPass(4, 5, 4, 5);
    Track(4);
    CHECK(g_cdSeen == 45.0f && g_overtakes.empty());
    // Not due (30 s since the last line): untouched.
    g_overtakeCd = 30.0f;
    SetPass(1, 6, 5, 4);
    Track(1);
    CHECK(g_cdSeen == 30.0f && g_overtakes.empty());
    // Not a pass: the same car at the place it left (m7w: "car 2 passed car 2"), or the race over: the stock's pick, untouched.
    g_overtakeCd = 45.0f;
    SetPass(1, 6, 5, 1);
    Track(1);
    CHECK(g_cdSeen == 45.0f && g_overtakes.empty());
    g_raceOver = 1;
    SetPass(1, 6, 5, 4);
    Track(1);
    CHECK(g_cdSeen == 45.0f && g_overtakes.empty());
    g_raceOver = 0;
    // A duel (f): car 1 passes the host's car 0 for 3rd; the coin on the host's LOST: 1 - 0.65 * 3/7 = 0.72, x1.5: always.
    g_tick += 4500;
    g_overtakeCd = 45.0f;
    RaceTestSetFair(true, 0.99f, 1);
    SetPass(1, 3, 2, 0);
    Track(1);
    CHECK(g_overtakes.size() == 1 && g_overtakes[0].index == 0 && g_overtakes[0].gained == 0 && g_overtakes[0].place == 3);
    // Passed into last place (TRAIL): as the stock for its local player: always on its coin's side (LOST), never on the
    // passer's (a computer racer at 7th: outside its window).
    g_overtakeCd = 45.0f;
    RaceTestSetFair(true, 0.99f, 0);
    SetPass(6, 7, 6, 1);
    Track(6);
    CHECK(g_cdSeen == 0.0f && g_overtakes.empty());
    RaceTestSetFair(true, 0.99f, 1);
    SetPass(6, 7, 6, 1);
    Track(6);
    CHECK(g_cdSeen == 0.0f && g_overtakes.size() == 1 && g_overtakes[0].index == 1 && g_overtakes[0].gained == 0 && g_overtakes[0].place == 7);

    // Homing warnings (d): a lock on the guest's car 60 m away: its own warning, 1 s; the same lock again: nothing; the cooldown.
    static uint8_t homing[0x100], near_[0x100];
    *(float*)(homing + 0xc0) = 60.0f;
    *(float*)(near_ + 0xc0) = 20.0f;
    memset(g_racerMem[1] + 0xc0, 0, 12);
    g_requests.clear();
    g_alertCd = 50.0f;
    RaceTestSetFair(true, 0.5f, 0);
    *(uintptr_t*)(g_racerMem[1] + 0x1cfc) = (uintptr_t)homing;
    Commit(g_racerMem[1]);
    CHECK(g_requests.size() == 1 && g_requests[0].cue == Cue(1, 9) && g_requests[0].wait == 1000 && g_alertCd == 0.0f);
    g_alertCd = 50.0f;
    Commit(g_racerMem[1]);
    CHECK(g_requests.size() == 1);
    // A new lock 20 m away: too close (as the stock); one on the host's own car: the stock's.
    *(uintptr_t*)(g_racerMem[1] + 0x1cfc) = (uintptr_t)near_;
    Commit(g_racerMem[1]);
    int hostMentions = RaceTestMentions(0);
    *(uintptr_t*)(g_racerMem[0] + 0x1cfc) = (uintptr_t)homing;
    Commit(g_racerMem[0]);
    CHECK(g_requests.size() == 1 && RaceTestMentions(0) == hostMentions);
    // The stock warned about the host's own car just now (its cooldown 0 at the lock change): a mention all the same.
    g_alertCd = 0.0f;
    *(uintptr_t*)(g_racerMem[0] + 0x1cfc) = (uintptr_t)near_;
    Commit(g_racerMem[0]);
    CHECK(g_requests.size() == 1 && RaceTestMentions(0) == hostMentions + 1);
    g_alertCd = 50.0f;
    // The generic warning a quarter of the time; not while the stock's own line keeps the cooldown down.
    g_generic = 0x7777;
    RaceTestSetFair(true, 0.1f, 0);
    *(uintptr_t*)(g_racerMem[1] + 0x1cfc) = 0;
    Commit(g_racerMem[1]);
    *(uintptr_t*)(g_racerMem[1] + 0x1cfc) = (uintptr_t)homing;
    Commit(g_racerMem[1]);
    CHECK(g_requests.size() == 2 && g_requests[1].cue == 0x7777);
    g_alertCd = 10.0f;
    *(uintptr_t*)(g_racerMem[1] + 0x1cfc) = (uintptr_t)near_ + 0x10;  // another homing thing, 70 m away (its +0xc0 is near_ + 0xd0)
    *(float*)(near_ + 0xd0) = 70.0f;
    Commit(g_racerMem[1]);
    CHECK(g_requests.size() == 2);
    g_generic = 0;

    // Place lines (user, 2026-10-22): each player's place the moment they cross the line (a race tick sees them in the finish
    // list), RESULT1-3 (25 s) or RESULT4 (35 s) by finish time; computer racers none. A player crossing while the announcer
    // still says the last place line is skipped (user: the first one's line, not a list).
    RaceTestSetFair(true, 0.0f, 0);
    g_requests.clear();
    QueueDone();
    auto finish = [](int k, uint32_t t, uint32_t reason) {
        *(uint32_t*)(g_records[k] + 0x68) = t;
        Append((uint8_t)k, reason);
    };
    finish(3, 0x1000, 1);  // a computer racer wins: no line
    finish(1, 0x2000, 1);  // the guest's car, at this game's view of its crossing: not yet (its own time comes with its Finished)
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.empty());
    {
        uint8_t buf[kWireMax];
        RaceFinish theirs = {c.race, 1, 1, 0x2000, 0x1000};
        Deliver(B, buf, EncodeFinished(theirs, buf, sizeof(buf)));
    }
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.size() == 1 && g_requests[0].cue == Cue(1, 0x11) && g_requests[0].wait == 25000);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.size() == 1);  // (once)
    finish(5, 0x3000, 1);
    finish(0, 0x4000, 1);  // the host's own car, 4th, while the guest's line is still said: skipped
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.size() == 1);
    // The race's end (the host's car was the last player: its finish ended the race, so its line is the race end's): the stock's
    // lines held with the game's commentary switch for the call (its voices still play), put back after; the guest's line still
    // said: the host's skipped.
    g_requests.clear();
    g_voiceLines.clear();
    g_muteSeen = 0xff;
    UHook(kHRaceEnd)(nullptr, nullptr);
    CHECK(g_muteSeen == 1 && g_scene[0x1350] == 0 && g_requests.empty() && g_voiceLines.empty());
    // Mode 3 (not a race of ours): the stock's.
    g_raceMode = 3;
    g_muteSeen = 0xff;
    UHook(kHRaceEnd)(nullptr, nullptr);
    CHECK(g_muteSeen == 0 && g_requests.empty());
    g_raceMode = 1;

    // (g) No recording twice while the line has another: the stock's pick first, then the ones not said yet, then the stock's.
    MakeGroup(g_groups[0], 0x1111, {0xa1, 0xa2, 0xa3, 0xa4});
    TakeHook select = (TakeHook)RaceTestHook(kHTakeSelect);
    std::vector<uint32_t> picks;
    for (int i = 0; i < 5; ++i) picks.push_back(select(g_groups[0].mem, nullptr, 0));
    CHECK(picks[0] == 0xa1 && picks[4] == 0xa1);
    std::vector<uint32_t> first(picks.begin(), picks.begin() + 4);
    std::sort(first.begin(), first.end());
    CHECK(first == std::vector<uint32_t>({0xa1, 0xa2, 0xa3, 0xa4}));

    // (j) Test players are players, although their cars are the host's AI from hold A (adopted, like a departed player's: in
    // game on m7r their passes went to the stock pick).
    Reset();
    LobbyWithTestPlayers(A, true);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract t = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, t));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    {
        uint8_t buf[kWireMax];
        UHook(kHWaiting)(nullptr, nullptr);
        Deliver(B, buf, EncodeLoaded(t.race, kStageLoaded, 1, buf, sizeof(buf)));
        UHook(kHWaiting)(nullptr, nullptr);
        UHook(kHCountdown)(nullptr, nullptr);
        Deliver(B, buf, EncodeLoaded(t.race, kStageCountdown, 1, buf, sizeof(buf)));
        UHook(kHCountdown)(nullptr, nullptr);
    }
    CHECK(RaceTestPhase() == kRunning);
    for (int k = 0; k < 8; ++k) {
        *(uint8_t**)(g_racerMem[k] + 0x2040) = g_trackers[k];
        *(uint8_t**)(g_racerMem[k] + 0x9f4) = g_voices[k];
    }
    RaceTestSetFair(true, 0.0f, 1);
    g_overtakeCd = 45.0f;
    SetPass(5, 4, 3, 3);  // computer car 5 passes Test Player 2's car 3 for 4th
    Track(5);
    CHECK(g_cdSeen == 0.0f && g_overtakes.size() == 1 && g_overtakes[0].index == 3 && g_overtakes[0].gained == 0 && g_overtakes[0].place == 4);
    // Test Player 1 (car 2) finishes 1st: RESULT1. The announcer done, the host's car finishes 2nd: RESULT2; Test Player 2 right
    // after it: skipped. The race ends with the guest not finished: FINFAIL.
    g_requests.clear();
    QueueDone();
    *(uint32_t*)(g_records[2] + 0x68) = 0x1000;
    Append(2, 1);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.size() == 1 && g_requests[0].cue == Cue(2, 0x10) && g_requests[0].wait == 25000);
    // The line ends (in the queue's update), and the next line queued gets its sounder: the announcer has finished all the same.
    QueueDone();
    ((QueueHook)RaceTestHook(kHSpeechUpdate))(g_voice, nullptr, 16);
    FakeRequestLog(0x9999, 1000, 0);
    g_requests.pop_back();
    *(uint32_t*)(g_records[0] + 0x68) = 0x2000;
    Append(0, 1);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    *(uint32_t*)(g_records[3] + 0x68) = 0x2100;
    Append(3, 1);
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    CHECK(g_requests.size() == 2 && g_requests[1].cue == Cue(0, 0x11));
    QueueDone();
    Append(1, 6);
    g_requests.clear();
    UHook(kHRaceEnd)(nullptr, nullptr);
    CHECK(g_requests.size() == 1 && g_requests[0].cue == Cue(1, 0x14) && g_requests[0].wait == 35000);

    // Without the group (another mod holds a slot): the stock's pick, untouched.
    RaceTestSetFair(false, -1.0f, -1);
    g_overtakeCd = 45.0f;
    SetPass(1, 6, 5, 4);
    Track(1);
    CHECK(g_cdSeen == 45.0f && g_overtakes.empty());
    (void)c;
    Reset();
}

// Driver voices, log only (re\m7-vo-triggers\FINDINGS.md): each car's lines counted from its cooldowns (voice +0x4d0, 7 floats)
// once per frame, before the speech queue's update; a line that reached the queue has its group (voice +0x4b4) as a record's
// sounder.
int RaceTestVoiceLines(int k, int kind, bool queued);
static uint8_t g_voiceGroups[8][7][0x100];
static void TestVoices() {
    printf("driver voices: every car's lines counted from their cooldowns\n");
    RaceTestSetOriginal(kHSpeechUpdate, (void*)&FakeSpeechUpdate);
    RaceTestSetAnnouncer(false, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeRequestLog, (uintptr_t)&FakeWrongWayCall);  // (counting only: no lines shared)
    RaceTestSetHostAi(true);
    RaceContract c = HostRunning();
    memset(g_voices, 0, sizeof(g_voices));
    memset(g_voiceGroups, 0, sizeof(g_voiceGroups));
    for (int k = 0; k < 8; ++k) {
        *(uint8_t**)(g_racerMem[k] + 0x9f4) = g_voices[k];
        for (int i = 0; i < 7; ++i) {
            *(uint8_t**)(g_voices[k] + 0x4b4 + i * 4) = g_voiceGroups[k][i];
            *(int32_t*)(g_voiceGroups[k][i] + 0xb0) = 4;
            g_voiceGroups[k][i][0xc0] = g_voiceGroups[k][i][0xcc] = 1;
            *(float*)(g_voices[k] + 0x4d0 + i * 4) = 20.0f;
        }
    }
    *(uint16_t*)(g_voice + 0x14) = 8;
    *(uint16_t*)(g_voice + 0x16) = 0;
    *(uint8_t***)(g_voice + 0x10) = g_queueSlots;
    QueueHook update = (QueueHook)RaceTestHook(kHSpeechUpdate);
    update(g_voice, nullptr, 16);  // the first look: nothing counted
    auto cool = [](int k, int i) -> float& { return *(float*)(g_voices[k] + 0x4d0 + i * 4); };
    for (int k = 0; k < 8; ++k)
        for (int i = 0; i < 7; ++i) CHECK(RaceTestVoiceLines(k, i, false) == 0);
    // The guest's car 1 says NEG (its cooldown set to 0, two ticks since) and the line is queued; AI car 3's POS fires out of
    // range (cooldown 0, nothing queued); the others tick on.
    for (int k = 0; k < 8; ++k)
        for (int i = 0; i < 7; ++i) cool(k, i) += 0.04f;
    cool(1, 1) = 0.04f;
    cool(3, 0) = 0.0f;
    g_queueSlots[0] = g_queueRecords[0];
    *(void**)g_queueRecords[0] = g_voiceGroups[1][1];
    *(uint16_t*)(g_voice + 0x16) = 1;
    int updates = g_speechUpdates;
    update(g_voice, nullptr, 16);
    CHECK(g_speechUpdates == updates + 1);
    CHECK(RaceTestVoiceLines(1, 1, false) == 1 && RaceTestVoiceLines(1, 1, true) == 1);
    CHECK(RaceTestVoiceLines(3, 0, false) == 1 && RaceTestVoiceLines(3, 0, true) == 0);
    CHECK(RaceTestVoiceLines(0, 1, false) == 0 && RaceTestVoiceLines(1, 0, false) == 0);
    // The next frames: rising cooldowns count nothing more; a write back to 20 s (the vehicle's setup) isn't a line.
    cool(1, 1) += 0.5f;
    cool(3, 0) += 0.5f;
    cool(5, 4) = 20.0f;
    update(g_voice, nullptr, 16);
    CHECK(RaceTestVoiceLines(1, 1, false) == 1 && RaceTestVoiceLines(3, 0, false) == 1 && RaceTestVoiceLines(5, 4, false) == 0);
    // The host's own car's INTRO, queued (a record further down the queue).
    cool(0, 6) = 0.0f;
    g_queueSlots[1] = g_queueRecords[1];
    *(void**)g_queueRecords[1] = g_voiceGroups[0][6];
    *(uint16_t*)(g_voice + 0x16) = 2;
    update(g_voice, nullptr, 16);
    CHECK(RaceTestVoiceLines(0, 6, false) == 1 && RaceTestVoiceLines(0, 6, true) == 1);
    // The music queue (the same update) and a frame with no voice of a car: nothing counted, no crash.
    static uint8_t music[0x30];
    cool(2, 1) = 0.0f;
    update(music, nullptr, 16);
    CHECK(RaceTestVoiceLines(2, 1, false) == 0);
    *(uint8_t**)(g_racerMem[2] + 0x9f4) = nullptr;
    update(g_voice, nullptr, 16);
    CHECK(RaceTestVoiceLines(2, 1, false) == 0);
    *(uint16_t*)(g_voice + 0x16) = 0;
    NativeCall none = {};
    NHook(kH090a)(nullptr, &none);  // the summary at the race's close
    CHECK(RaceTestPhase() == kIdle);
    (void)c;
    Reset();
}

// The players' racers' lines on every screen: each player's game sends its racer's lines (Voice); the others mute that
// racer's own lines (cooldowns held at 0) and say the sent ones through 0x54d0a0; an intro line waits for the racer intro.
void RaceTestSetVoiceLineAddress(uintptr_t fn);
struct LineSeen { uint8_t* voice; uint32_t kind; uint8_t mode; int32_t wait; };
static std::vector<LineSeen> g_linesSeen;
// 0x54d0a0 here: speaks only from a 20 s cooldown, sets it to 0, queues the car's group with the wait.
static void __fastcall FakeDriverLine(uint8_t* voice, void*, uint32_t kind, uint8_t mode, int32_t wait) {
    g_linesSeen.push_back({voice, kind, mode, wait});
    float& c = *(float*)(voice + 0x4d0 + kind * 4);
    if (c < 20.0f) return;
    c = 0.0f;
    uint16_t& count = *(uint16_t*)(g_voice + 0x16);
    if (count >= 8) return;
    g_queueSlots[count] = g_queueRecords[count];
    *(void**)g_queueRecords[count] = *(void**)(voice + 0x4b4 + kind * 4);
    *(int32_t*)(g_queueRecords[count] + 4) = wait;
    ++count;
}
// A sound group's mode setter (0x474a00: +0x28) and Play (its vtable slot 14), for the intro lines said straight.
void RaceTestSetGroupPlayAddresses(uintptr_t setMode, uintptr_t play);
static std::vector<uint8_t*> g_groupPlays;
static int32_t __fastcall FakeGroupSetMode(uint8_t* group, void*, uint8_t mode) { group[0x28] = mode; return 0; }
static int g_playRefuse = 0;  // the next that many Plays fail to start (the sound system's few streams busy)
// The groups' manager (group +0x78): +0x6c its 4 streamers, +0x70 their count; a streamer +0x7c nonzero while in use.
static uint8_t g_soundMgr[0x80], g_streamers[4][0x80];
static uint8_t* g_streamerList[4];
static void StreamersFree(int n) {  // n of the 4 free
    for (int i = 0; i < 4; ++i) {
        g_streamerList[i] = g_streamers[i];
        *(uint32_t*)(g_streamers[i] + 0x7c) = i < n ? 0 : 1;
    }
    *(uint8_t***)(g_soundMgr + 0x6c) = g_streamerList;
    *(int32_t*)(g_soundMgr + 0x70) = 4;
}
static int32_t __fastcall FakeGroupPlay(uint8_t* group, void*) {  // a started line takes a free streamer
    g_groupPlays.push_back(group);
    if (g_playRefuse > 0) { --g_playRefuse; return 1; }
    for (int i = 0; i < 4; ++i)
        if (!*(uint32_t*)(g_streamers[i] + 0x7c)) { *(uint32_t*)(g_streamers[i] + 0x7c) = 1; break; }
    return 0;
}
static uintptr_t g_groupVt[16] = {};
static void VoicesOn(int cars) {  // every car's voice: 7 groups at 20 s, an empty speech queue
    memset(g_voices, 0, sizeof(g_voices));
    memset(g_voiceGroups, 0, sizeof(g_voiceGroups));
    g_groupVt[14] = (uintptr_t)&FakeGroupPlay;
    RaceTestSetGroupPlayAddresses((uintptr_t)&FakeGroupSetMode, (uintptr_t)&FakeGroupPlay);
    for (int k = 0; k < cars; ++k) {
        *(uint8_t**)(g_racerMem[k] + 0x9f4) = g_voices[k];
        for (int i = 0; i < 7; ++i) {
            *(uintptr_t**)g_voiceGroups[k][i] = g_groupVt;
            *(uint8_t**)(g_voiceGroups[k][i] + 0x78) = g_soundMgr;
            *(uint8_t**)(g_voices[k] + 0x4b4 + i * 4) = g_voiceGroups[k][i];
            *(float*)(g_voices[k] + 0x4d0 + i * 4) = 20.0f;
        }
    }
    *(uintptr_t*)g_voice = (uintptr_t)g_voiceVt;
    *(uint16_t*)(g_voice + 0x14) = 8;
    *(uint16_t*)(g_voice + 0x16) = 0;
    *(uint8_t***)(g_voice + 0x10) = g_queueSlots;
    g_linesSeen.clear();
    g_groupPlays.clear();
    StreamersFree(4);
    g_playRefuse = 0;
}
static std::vector<RaceVoice> SentVoices() {
    std::vector<RaceVoice> out;
    for (const RaceNetMsg& m : g_sent) {
        RaceVoice v;
        if (DecodeVoice(m.data, m.size, v)) { CHECK(m.reliable); out.push_back(v); }
    }
    return out;
}

static void TestVoiceMirror() {
    printf("driver voices: each player's racer's lines on every screen\n");
    RaceTestSetAnnouncer(true, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeRequestLog, (uintptr_t)&FakeWrongWayCall);
    RaceTestSetOriginal(kHSpeechUpdate, (void*)&FakeSpeechUpdate);
    RaceTestSetVoiceLineAddress((uintptr_t)&FakeDriverLine);
    RaceTestSetHostAi(true);
    RaceContract c = HostRunning();
    VoicesOn(8);
    QueueHook update = (QueueHook)RaceTestHook(kHSpeechUpdate);
    auto cool = [](int k, int i) -> float& { return *(float*)(g_voices[k] + 0x4d0 + i * 4); };
    update(g_voice, nullptr, 16);
    // The guest's racer (car 1) is muted here: its shared kinds held at 0 (the podium's RCE_WON / RCE_LST left), nothing counted.
    CHECK(cool(1, 0) == 0.0f && cool(1, 1) == 0.0f && cool(1, 4) == 0.0f && cool(1, 5) == 0.0f && cool(1, 6) == 0.0f);
    CHECK(cool(1, 2) == 20.0f && cool(1, 3) == 20.0f && cool(0, 0) == 20.0f && cool(4, 1) == 20.0f);
    // The host's own racer's drift line (POS, 3D, queued with its 3000 ms): sent to everyone; a computer racer's NEG: not sent.
    g_sent.clear();
    cool(0, 0) = 0.02f;
    g_voiceGroups[0][0][0x28] = 1;
    g_queueSlots[0] = g_queueRecords[0];
    *(void**)g_queueRecords[0] = g_voiceGroups[0][0];
    *(int32_t*)(g_queueRecords[0] + 4) = 3000;
    cool(4, 1) = 0.02f;
    *(uint16_t*)(g_voice + 0x16) = 1;
    update(g_voice, nullptr, 16);
    std::vector<RaceVoice> sent = SentVoices();
    CHECK(sent.size() == 1 && sent[0].race == c.race && sent[0].slot == 0 && sent[0].kind == 0 && sent[0].mode == 1 && sent[0].wait == 3000);
    CHECK(RaceTestVoiceLines(0, 0, true) == 1 && RaceTestVoiceLines(4, 1, false) == 1);
    // The guest's NEG for its own racer: said here through the game's line function (3D, its wait), the gate opened for it and
    // closed again; the line is queued.
    *(uint16_t*)(g_voice + 0x16) = 0;
    uint8_t buf[kWireMax];
    RaceVoice v = {c.race, 1, 1, 1, 3000};
    Deliver(B, buf, EncodeVoice(v, buf, sizeof(buf)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);  // (messages are taken in the race's ticks)
    CHECK(g_linesSeen.empty());
    update(g_voice, nullptr, 16);
    CHECK(g_linesSeen.size() == 1 && g_linesSeen[0].voice == g_voices[1] && g_linesSeen[0].kind == 1 && g_linesSeen[0].mode == 1 &&
          g_linesSeen[0].wait == 3000);
    CHECK(*(uint16_t*)(g_voice + 0x16) == 1 && *(void**)g_queueRecords[0] == g_voiceGroups[1][1] && cool(1, 1) == 0.0f);
    CHECK(RaceTestVoiceLines(1, 1, false) == 0);  // (a line said for its owner isn't one of this game's own)
    // One naming another car than the sender's, one for the podium's kinds, one from another race: not said.
    g_linesSeen.clear();
    v.slot = 0;
    Deliver(B, buf, EncodeVoice(v, buf, sizeof(buf)));
    v.slot = 1;
    v.kind = 2;
    CHECK(EncodeVoice(v, buf, sizeof(buf)) == 0);
    v.kind = 1;
    v.race = c.race + 1;
    Deliver(B, buf, EncodeVoice(v, buf, sizeof(buf)));
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_linesSeen.empty());
    // The guest leaves: its racer is a computer racer here now (adopted): no longer muted, its lines the stock's.
    v.race = c.race;
    {
        uint8_t b2[kWireMax];
        Deliver(B, b2, EncodeAbort(c.race, kAbortQuit, b2, sizeof(b2)));
    }
    ++g_tick;
    UHook(kHRacing)(nullptr, nullptr);
    cool(1, 0) = 20.0f;
    update(g_voice, nullptr, 16);
    CHECK(cool(1, 0) == 20.0f);
    g_sent.clear();
    cool(0, 1) = 0.0f;  // and with no other player left, the host's own lines aren't sent
    update(g_voice, nullptr, 16);
    CHECK(SentVoices().empty());

    // Guest at its course intro: the host's intro line waits for this game's racer intro (its enter flushes the queue), then
    // is said with a longer wait (several may come at once); the guest's own racer intro line is sent.
    GuestAtIntros(c);
    VoicesOn(8);
    update(g_voice, nullptr, 16);
    v = {c.race, 0, 6, 0, 3000};
    Deliver(A, buf, EncodeVoice(v, buf, sizeof(buf)));
    uint8_t track[0x20] = {}, racer[0x20] = {};
    UHook(kHTrackIntro)(track, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_linesSeen.empty() && cool(0, 6) == 0.0f);  // (the host's racer muted here)
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    UHook(kHCharIntro)(racer, nullptr);
    g_sent.clear();
    cool(1, 6) = 0.0f;  // this game's racer intro line (the stock's, at the racer intro's enter)
    g_queueSlots[0] = g_queueRecords[0];
    *(void**)g_queueRecords[0] = g_voiceGroups[1][6];
    *(int32_t*)(g_queueRecords[0] + 4) = 3000;
    *(uint16_t*)(g_voice + 0x16) = 1;
    update(g_voice, nullptr, 16);
    // (said at once, straight, not through the queue: the lines play together and the countdown's flush can't cut them)
    CHECK(g_linesSeen.empty() && g_groupPlays.size() == 1 && g_groupPlays[0] == g_voiceGroups[0][6]);
    CHECK(g_voiceGroups[0][6][0x28] == 0 && g_voiceGroups[0][6][0xac] == 1);
    sent = SentVoices();
    CHECK(sent.size() == 1 && sent[0].slot == 1 && sent[0].kind == 6 && sent[0].mode == 0 && sent[0].wait == 3000);
    // The podium's lines stay each screen's own: the host's racer's RCE_WON here isn't muted.
    CHECK(cool(0, 2) == 20.0f);
    // A group that isn't a sound group as this exe has it (another Play): not played, counted as not heard.
    g_groupPlays.clear();
    v = {c.race, 0, 6, 0, 3000};
    static uintptr_t otherVt[16] = {};
    *(uintptr_t**)g_voiceGroups[0][6] = otherVt;
    Deliver(A, buf, EncodeVoice(v, buf, sizeof(buf)));
    UHook(kHCharIntro)(racer, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.empty());
    *(uintptr_t**)g_voiceGroups[0][6] = g_groupVt;

    // Host with two test players: at its racer intro their racers say their intro lines here, at once, and on every screen.
    Reset();
    LobbyWithTestPlayers(A, true);
    CHECK(RaceHostStart() == kStartOk);
    RaceContract t = {};
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, t));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(t.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kIntros);
    VoicesOn(8);
    g_groupPlays.clear();
    g_sent.clear();
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    g_timerElapsed = 0;
    UHook(kHCharIntro)(racer, nullptr);
    CHECK(g_groupPlays.empty());  // (started after the speech queue's update: this game's own line first)
    sent = SentVoices();
    CHECK(sent.size() == 2 && sent[0].slot == 2 && sent[1].slot == 3 && sent[0].kind == 6 && sent[0].mode == 0);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 2 && g_groupPlays[0] == g_voiceGroups[2][6] && g_groupPlays[1] == g_voiceGroups[3][6]);
    UHook(kHCharIntro)(racer, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 2);  // (once)
    // The sound system refuses a start (only a few streams at once in game): tried again until it starts; a line starts only
    // while two streamers are free (one stays for the game); still waiting at the countdown's release: dropped.
    Reset();
    LobbyWithTestPlayers(A, true);
    CHECK(RaceHostStart() == kStartOk);
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, t));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(t.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    VoicesOn(8);
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    g_timerElapsed = 0;
    g_playRefuse = 1;  // car 2's first try
    UHook(kHCharIntro)(racer, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 2 && g_groupPlays[0] == g_voiceGroups[2][6] && g_groupPlays[1] == g_voiceGroups[3][6]);
    StreamersFree(1);  // the other lines and the music hold three: no try
    for (int f = 0; f < 5; ++f) update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 2);
    StreamersFree(2);  // one more free: car 2 again, refused again; then started
    g_playRefuse = 1;
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 3 && g_groupPlays[2] == g_voiceGroups[2][6]);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 4 && g_groupPlays[3] == g_voiceGroups[2][6]);
    for (int f = 0; f < 20; ++f) update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 4);  // (done)
    // A line still waiting when the countdown is released: dropped (the race's music and voices need the streams).
    Reset();
    LobbyWithTestPlayers(A, true);
    CHECK(RaceHostStart() == kStartOk);
    CHECK(DecodeContract(g_sent[0].data, g_sent[0].size, t));
    RaceLaunched(true);
    Script0905(0x1, "STATE_Control_StartingGridHuman", 4);
    for (int k = 1; k < 8; ++k) Script0905(0x2, "STATE_Control_StartingGridAI", 0);
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(t.race, kStageLoaded, 1, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    VoicesOn(8);
    StreamersFree(1);
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    UHook(kHCharIntro)(racer, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.empty());
    UHook(kHCountdown)(nullptr, nullptr);
    Deliver(B, buf, EncodeLoaded(t.race, kStageCountdown, 1, buf, sizeof(buf)));
    UHook(kHCountdown)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kRunning);
    StreamersFree(4);
    update(g_voice, nullptr, 16);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.empty());
    // Guest: the host's test player's intro line is said; one of its other lines, or from someone else, isn't.
    Reset();
    LobbyWithTestPlayers(B, false);
    Deliver(A, buf, EncodeContract(t, buf, sizeof(buf)));
    CHECK(RaceMenuPoll() == kRaceStart);
    RaceLaunched(true);
    Script0905(0x11111111, "STATE_Control_StartingGridAI", 0);
    Script0905(t.roster[1], "STATE_Control_StartingGridHuman", 2);
    for (int k = 2; k < 8; ++k) Script0905(0x22222222 + k, "STATE_Control_StartingGridAI", 0);
    {
        uint8_t types[4] = {kTypeHash, kTypeHash, kTypeHash, kTypeHash};
        uint32_t values[4] = {1, 0, 7, 0};
        NativeCall d = {0x0900, 1, {}, types, values, 4, 0, 0};
        NHook(kH0900)(nullptr, &d);
    }
    UHook(kHSetup)(nullptr, nullptr);
    g_scene[0x1100] = 8;
    UHook(kHWaiting)(nullptr, nullptr);
    Deliver(A, buf, EncodeGo(t.race, kStageLoaded, buf, sizeof(buf)));
    UHook(kHWaiting)(nullptr, nullptr);
    CHECK(RaceTestPhase() == kIntros);
    VoicesOn(8);
    g_groupPlays.clear();
    *(int32_t*)(g_flowRec + 4) = 0x3a;
    UHook(kHCharIntro)(racer, nullptr);
    v = {t.race, 2, 6, 0, 3000};
    Deliver(A, buf, EncodeVoice(v, buf, sizeof(buf)));
    v.kind = 0;
    Deliver(A, buf, EncodeVoice(v, buf, sizeof(buf)));
    UHook(kHCharIntro)(racer, nullptr);
    update(g_voice, nullptr, 16);
    CHECK(g_groupPlays.size() == 1 && g_groupPlays[0] == g_voiceGroups[2][6] && g_linesSeen.empty());
    Reset();
}

// ---------------------------------------------------------------- fair item lines (M7, re\m7-item-lines\FINDINGS.md)
void RaceTestSetLineAddresses(uintptr_t hitCooldown, uintptr_t raceSeconds, uintptr_t genericHit, uintptr_t allStarHitCall, uintptr_t opaLineCall,
                              uintptr_t opaVtable);
void RaceTestSetLineHooks(bool lines, bool opa);
void RaceTestSetPulseHooks(bool on);
void RaceTestSetGateHooks(bool on);
void RaceTestSetProjectileSerial(uintptr_t address);
static float g_hitCd = 45.0f, g_raceSeconds = 10.0f, g_hitCdSeen = -1.0f;
static uint32_t g_genericHit = 0;
static std::vector<int32_t> g_allStarHits;
static std::vector<std::pair<int32_t, uint32_t>> g_opaLines;
static uint8_t g_listenerOffSeen = 0xff, g_opaOffSeen = 0xff;
static int32_t g_opaWeaponSeen = -1;
// 0x4f3990 (racer slot 4): the car's update, whose HIT test reads the cooldown. 0x4ee020 (the listener): the message's handling.
static void __fastcall FakeRacerUpdate(uint8_t*, void*) { g_hitCdSeen = g_hitCd; }
static int __fastcall FakeListener(uint8_t*, void*, int32_t*) { g_listenerOffSeen = g_commentOff; return 5; }
static void __cdecl FakeAllStarHit(int32_t index) { g_allStarHits.push_back(index); }
static void __cdecl FakeOpaLine(int32_t index, uint32_t cue) { g_opaLines.push_back({index, cue}); }
// 0x5e9b10 (Opa-Opa slot 11): fires the weapon at +0x184 and says its line; 0x5e9580: the ram's start (weapon 1).
static void __fastcall FakeOpaStart(uint8_t* obj, void*) { g_opaOffSeen = g_commentOff; g_opaWeaponSeen = *(int32_t*)(obj + 0x184); }
static std::vector<uint8_t*> g_opaRams;
static void __fastcall FakeOpaRam(uint8_t* obj, void*) { g_opaRams.push_back(obj); }
void RaceTestSetOpaRamAddress(uintptr_t fn);
typedef int(__fastcall* ListenerHook)(uint8_t*, void*, int32_t*);
static int Listen(int k, int attacker, int32_t kind = 0x28) {  // a message to car k's listener (racer+0x2c0): kind, +8 the sender's racer
    int32_t msg[4] = {kind, 0, (int32_t)(uintptr_t)g_racerMem[attacker], 0};
    return ((ListenerHook)RaceTestHook(kHRacerListener))(g_racerMem[k] + 0x2c0, nullptr, msg);
}
static void RacerUpdate(int k) { ((UpdateHook)RaceTestHook(kHRacerUpdate))(g_racerMem[k], nullptr); }
static void OpaStart(uint8_t* obj) { ((UpdateHook)RaceTestHook(kHOpaStart))(obj, nullptr); }
// 0x5ea2e0 (Opa-Opa slot 7): the computer racer's input for the All-Star: here always "fire" (and junk in the other bytes).
static void __fastcall FakeOpaInput(uint8_t*, void*, uint8_t* out) { memset(out, 0x55, 12); out[8] = out[9] = 1; }
static void OpaInput(uint8_t* obj, uint8_t* out) { ((void(__fastcall*)(uint8_t*, void*, uint8_t*))RaceTestHook(kHOpaInput))(obj, nullptr, out); }
// The pulse classes' input slots share one hook function (it tells the class by the object's vtable).
static void PulseInput(uint8_t* obj, uint8_t* out) { ((void(__fastcall*)(uint8_t*, void*, uint8_t*))RaceTestHook(kHPulseAiAi))(obj, nullptr, out); }
// The scene update's Opa-Opa phase-4 tick for one object (it runs AFTER the race update in the game's frame, 0x4967c0): it asks
// the input slot and fires when "down" is set and the gate allows (the shot timer past the interval for weapons 0/2, then reset
// to 0; the ram's pulse at 0 for weapon 1, then 2.5), as 0x5e9ee0 / 0x5e9890 do. Returns true when it fired.
static bool OpaSceneTick(uint8_t* obj) {
    uint8_t out[12];
    OpaInput(obj, out);
    int32_t w = *(int32_t*)(obj + 0x184);
    if (!out[8]) return false;
    if ((w == 0 || w == 2) && *(float*)(obj + 0x160) > 3.0f) { *(float*)(obj + 0x160) = 0.0f; return true; }
    if (w == 1 && *(float*)(obj + 0x154) <= 0.001f) { *(float*)(obj + 0x154) = 2.5f; return true; }
    return false;
}
// A guest's report from B: its car spins (other 1) / stopped (0), or an All-Star's touch by car `other`.
static void ReportHit(const RaceContract& c, uint8_t slot, uint8_t kind = kLineHit, uint8_t other = 1) {
    RaceItemLine l = {c.race, kind, slot, other};
    uint8_t buf[kWireMax];
    size_t n = EncodeItemLine(l, buf, sizeof(buf));
    CHECK(n != 0);
    Deliver(B, buf, n);
}
static void ReportSpinEnd(const RaceContract& c, uint8_t slot) { ReportHit(c, slot, kLineHit, 0); }
static uint32_t PhaseBodyOf(const RaceEvent& e) { uint32_t b; memcpy(&b, e.body, 4); return b; }

// ---- The gate All-Stars (re\m7-allstar-shots2\FINDINGS.md): the stock phase-4 ticks as the disassembly reads them, asking the
// (hooked) input slot as the game does; a human owner's input comes from its controller (slot 6), a computer racer's from slot 7.
static uint32_t Bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int TestAnim(const uint8_t* racer) {  // 0x59fe90
    const uint8_t* a = *(const uint8_t* const*)(racer + 0x2044);
    const uint8_t* d = *(const uint8_t* const*)(a + 0xcc);
    return (*(const uint16_t*)(d + 0xe) >> 10) & 0xf;
}
static void SetTestAnim(uint8_t* racer, int anim) {
    uint8_t* a = *(uint8_t**)(racer + 0x2044);
    uint8_t* d = *(uint8_t**)(a + 0xcc);
    *(uint16_t*)(d + 0xe) = (uint16_t)(anim << 10);
}
static bool g_fakePress = false, g_fakeHuman = false;  // the stock computer input presses (steer 0.25, vertical -0.5); the owner is a human
static std::vector<int> g_rockets;    // Eggman: each rocket's pattern index
static std::vector<float> g_aims;     // Shadow: steer, vertical per spear; Alex Kidd: the steering per shot
static std::vector<int> g_punches;    // Knuckles: each punch's level
typedef void(__fastcall* GateInputFn)(uint8_t*, void*, uint8_t*);
typedef void(__fastcall* GateTickFn)(uint8_t*, void*, float*);
static void __fastcall FakeGateInput(uint8_t*, void*, uint8_t* out) {
    memset(out, 0, 12);
    if (!g_fakePress) return;
    out[8] = out[9] = 1;
    *(float*)out = 0.25f;
    *(float*)(out + 4) = -0.5f;
}
static void GateInput(int cls, uint8_t* obj, uint8_t* out) { ((GateInputFn)RaceTestHook(kHGateFirst + 2 * cls))(obj, nullptr, out); }
static void GateTick(int cls, uint8_t* obj, float dt) { ((GateTickFn)RaceTestHook(kHGateFirst + 2 * cls + 1))(obj, nullptr, &dt); }
static void ChooseInput(int cls, uint8_t* obj, uint8_t* out) {  // 0x5a0080
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    uint8_t* ctrl = *(uint8_t**)(racer + 0x9f0);
    if (g_fakeHuman && ctrl && *(int32_t*)(ctrl + 0x18) == 1) {
        memset(out, 0, 12);
        memcpy(out, ctrl + 0x5c, 4);
        memcpy(out + 4, ctrl + 0x58, 4);
        int32_t s = *(int32_t*)(ctrl + 0x24);
        out[8] = s == 2 || s == 3;
        out[9] = s == 2;
        return;
    }
    GateInput(cls, obj, out);
}
static void __fastcall FakeEggmanTick(uint8_t* obj, void*, float* dt) {  // 0x5eb050
    uint8_t out[12];
    ChooseInput(kGateEggman, obj, out);
    float& t = *(float*)(obj + 0x134);
    t += *dt;
    if (t >= *(float*)(obj + 0x138) && out[9] && TestAnim(*(uint8_t**)(obj + 0x10)) < 6) {
        int32_t& p = *(int32_t*)(obj + 0x130);
        if (--p < 0) p = 7;
        g_rockets.push_back(p);
        *(float*)(obj + 0x138) = 0.75f;
        t = 0.0f;
    }
}
static void __fastcall FakeShadowTick(uint8_t* obj, void*, float* dt) {  // 0x5dd7f0 / 0x5dd1b0
    uint8_t out[12];
    ChooseInput(kGateShadow, obj, out);
    float& t = *(float*)(obj + 0x174);
    t -= *dt;
    if (out[9] == 1 && t <= 0.0f) {
        g_aims.push_back(*(float*)out);
        g_aims.push_back(*(float*)(out + 4));
        t = 0.5f;
    }
}
static void __fastcall FakeAlexTick(uint8_t* obj, void*, float* dt) {  // 0x5d5e20
    uint8_t out[12];
    ChooseInput(kGateAlexKidd, obj, out);
    uint8_t* veh = *(uint8_t**)(*(uint8_t**)(obj + 0x10) + 0x9f4);
    float& steer = *(float*)(veh + 0x984);
    if (*(int32_t*)(veh + 0x980)) {
        float s = *(float*)out;
        if (s != 0.0f) {
            steer += 0.9f * *dt * s;
            steer = steer > 0.9f ? 0.9f : steer < -0.9f ? -0.9f : steer;
        } else {
            steer *= 1.0f - 0.9f * *dt;
        }
    }
    float i = *(float*)(obj + 0x134), cool = (1.0f - i) * 1.5f + i * 0.5f;
    float& c = *(float*)(obj + 0x130);
    c -= *dt;
    if (c <= 0.0f && out[9]) {
        g_aims.push_back(steer);
        c = cool;
    }
    float& mash = *(float*)(obj + 0x138);
    mash = out[9] ? (mash + 0.33f > 1.0f ? 1.0f : mash + 0.33f) : mash * 0.95f;
    *(float*)(obj + 0x134) = (1.0f - 2.0f * *dt) * i + 2.0f * *dt * mash;
}
static void __fastcall FakeZobioTick(uint8_t* obj, void*, float* dt) {  // 0x5e14c0
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    float& c = *(float*)(obj + 0x138);
    c -= *dt;
    uint8_t out[12];
    ChooseInput(kGateZobio, obj, out);
    if (out[9] && TestAnim(racer) == 2 && c <= 0.0f) SetTestAnim(racer, 6);
}
static void __fastcall FakeRyoTick(uint8_t* obj, void*, float*) {  // 0x5da570
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    uint8_t out[12];
    ChooseInput(kGateRyo, obj, out);
    if (out[9] && TestAnim(racer) != 3) SetTestAnim(racer, 3);
}
static void __fastcall FakeKnucklesTick(uint8_t* obj, void*, float* dt) {  // 0x5dc160 / 0x5dbd50
    uint8_t out[12];
    ChooseInput(kGateKnuckles, obj, out);
    bool held = out[8] != 0;
    float& cool = *(float*)(obj + 0x134);
    float& charge = *(float*)(obj + 0x138);
    int32_t& level = *(int32_t*)(obj + 0x130);
    if (cool > 0.0f) { cool -= *dt; return; }
    if (held) {
        charge += *dt;
        level = charge >= 3.0f ? 3 : charge >= 1.25f ? 2 : charge > 0.0f ? 1 : level;
        return;
    }
    if (charge <= 0.0f) return;
    if (charge < 0.25f) { charge += *dt; return; }
    g_punches.push_back(level);
    level = 0;
    charge = 0.0f;
    cool = 0.5f;
}
static uint32_t g_fakeSerial = 1000;  // the game's projectile serial [0xcdd048]: every new object bumps it
static bool g_ulalaDead = false;      // Ulala's target car gone: the shot goes out with the anim kept
static std::vector<float> g_beatAim;  // Beat: the steer input the spray saw per held tick
static void __fastcall FakeAmyTick(uint8_t* obj, void*, float* dt) {  // 0x5d8870
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    float& timer = *(float*)(obj + 0x144);
    if (timer > 0.0f) {
        timer -= *dt;
        if (timer < 0.0f) { *(int32_t*)(obj + 0x140) = 0; timer = 0.0f; }
    }
    uint8_t out[12];
    ChooseInput(kGateAmy, obj, out);
    int anim = TestAnim(racer);
    if (!out[9] || (anim != 2 && anim != 7)) return;
    int32_t swing = *(int32_t*)(obj + 0x140);
    if (swing) {
        SetTestAnim(racer, swing);
    } else {
        uint8_t& flip = *(uint8_t*)(obj + 0x13a);
        SetTestAnim(racer, flip ? 3 : 4);
        flip = !flip;
    }
}
static bool g_ulalaOtherObject = false;  // something else made an object during Ulala's tick (no shot)
static void __fastcall FakeUlalaTick(uint8_t* obj, void*, float*) {  // 0x5e09e0
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    if (g_ulalaOtherObject) ++g_fakeSerial;
    uint8_t out[12];
    ChooseInput(kGateUlala, obj, out);
    if (!out[9] || TestAnim(racer) != 2) return;
    if (!g_ulalaDead) SetTestAnim(racer, 6);
    ++g_fakeSerial;  // the shot object
}
static void __fastcall FakeBeatTick(uint8_t* obj, void*, float*) {  // 0x5db390
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    uint8_t out[12];
    ChooseInput(kGateBeat, obj, out);
    bool held = out[8] != 0;
    int anim = TestAnim(racer);
    if (held && anim == 2) SetTestAnim(racer, 3);
    if (!held && anim == 3) SetTestAnim(racer, 2);
    *(float*)(obj + 0x130) = held ? 0.9f : 1.1f;
    if (held) g_beatAim.push_back(*(float*)out);
}
static std::vector<float> g_bombs;  // Bonanza Bros: each bomb's orientation (the aim angle at the throw)
static int g_clipPos[8];            // the Attack clip's position per car (the stock reads the layer-3 clip; 14 = the throw)
static void __fastcall FakeBonanzaTick(uint8_t* obj, void*, float* arg) {  // slot 23 0x5bd470, called by the blimp's update with the blimp
    if ((uint8_t*)arg != *(uint8_t**)(obj + 0x174)) return;
    uint8_t* racer = *(uint8_t**)(obj + 0x10);
    int k = (int)(racer - g_racerMem[0]) / 0x2200;
    uint8_t out[12];
    ChooseInput(kGateBonanza, obj, out);
    int32_t& state = *(int32_t*)(obj + 0x1a4);
    float& aim = *(float*)(obj + 0x150);
    uint8_t& thrown = *(uint8_t*)(obj + 0x186);
    float steer = *(float*)out;
    if (state != 5 && steer != 0.0f) {
        aim -= 0.02f * 10.0f * steer;
        aim = aim > 0.3490659f ? 0.3490659f : aim < -0.3490659f ? -0.3490659f : aim;
    }
    if (state >= 2 && state <= 4) {
        ++g_clipPos[k];
        if (g_clipPos[k] >= 14 && !thrown) { g_bombs.push_back(aim); thrown = 1; }
        if (g_clipPos[k] >= 20) state = 0;
    } else if (state == 0) {
        thrown = 0;  // ("RevengeReady" restarts)
    }
    if (out[9] && state == 0 && *(void**)(obj + 0x1a0)) {  // "Attack" plays (the clip restarts) only while nothing is thrown; state 3 either way
        if (!thrown) g_clipPos[k] = 0;
        state = 3;
    }
}
static void DeliverGate(uint64_t from, const RaceContract& c, int slot, uint32_t head, uint32_t a, uint32_t b, uint32_t tick) {
    RaceEvent e = {};
    e.race = c.race;
    e.tick = tick;
    e.kind = kEventAllStarGate;
    e.slot = (uint8_t)slot;
    e.size = EventBodySize(kEventAllStarGate);
    memcpy(e.body, &head, 4);
    memcpy(e.body + 4, &a, 4);
    memcpy(e.body + 8, &b, 4);
    uint8_t buf[kWireMax];
    size_t n = EncodeEvent(e, buf, sizeof(buf));
    CHECK(n != 0);
    Deliver(from, buf, n);
}

static void TestItemLines() {
    printf("fair item lines: hits reported by their own game, the host's pick; Opa-Opa's All-Star weapon\n");
    RaceTestSetAnnouncer(true, (uintptr_t)&g_cmPtr, (uintptr_t)&FakeCommentPlay, (uintptr_t)&FakeFlush, (uintptr_t)&FakeGroupChild,
                         (uintptr_t)&FakeRequestLog, (uintptr_t)&FakeWrongWayCall);
    RaceTestSetFairAddresses((uintptr_t)&FakeOvertakeCall, (uintptr_t)&g_overtakeCd, (uintptr_t)&g_alertCd, (uintptr_t)g_placeList,
                             (uintptr_t)&g_raceClock, (uintptr_t)&g_commentOff, (uintptr_t)&g_raceMode, (uintptr_t)&g_calloutsLive,
                             (uintptr_t)&FakeVoiceWin, (uintptr_t)&FakeVoiceLose, (uintptr_t)&g_generic);
    RaceTestSetLineAddresses((uintptr_t)&g_hitCd, (uintptr_t)&g_raceSeconds, (uintptr_t)&g_genericHit, (uintptr_t)&FakeAllStarHit,
                             (uintptr_t)&FakeOpaLine, 0x6bb3d8);
    RaceTestSetOriginal(kHRacerUpdate, (void*)&FakeRacerUpdate);
    RaceTestSetOriginal(kHRacerListener, (void*)&FakeListener);
    RaceTestSetOriginal(kHOpaStart, (void*)&FakeOpaStart);
    RaceTestSetOpaRamAddress((uintptr_t)&FakeOpaRam);
    RaceTestSetOriginal(kHTakeSelect, (void*)&FakeSelectStock);
    RaceTestSetOriginal(kHTakeSource, (void*)&FakeSourceStock);
    RaceTestSetOriginal(kHSpeechUpdate, (void*)&FakeSpeechUpdate);
    RaceTestSetHostAi(true);
    RaceTestSetItems(true);
    RaceTestSetFair(true, 0.0f, 0);
    RaceTestSetLineHooks(true, true);
    RaceContract c = HostRunning();
    CHECK((c.flags & (kContractHostAi | kContractItems | kContractAnnouncer)) == (kContractHostAi | kContractItems | kContractAnnouncer));
    ResetItems();
    memset(g_voices, 0, sizeof(g_voices));
    for (int k = 0; k < 8; ++k) {
        *(int32_t*)(g_racerMem[k] + 0xa1c) = 2;
        *(uint8_t**)(g_racerMem[k] + 0x9f4) = g_voices[k];
        *(uint32_t*)(g_voices[k] + 0x72c + 6 * 4) = Cue(k, 6);
        *(int32_t*)(g_records[k] + 0x80) = k;  // places: the host's car leads, the guest's is 2nd
    }
    g_requests.clear();
    g_hitCd = 45.0f;
    g_raceSeconds = 10.0f;
    g_commentOff = 0;
    g_calloutsLive = 1;
    g_genericHit = 0;
    g_allStarHits.clear();
    g_opaLines.clear();

    // Slot 4: while the host picks the lines, the stock's pick sees 0 s for every car; the cooldown is put back.
    g_hitCdSeen = -1.0f;
    RacerUpdate(0);
    CHECK(g_hitCdSeen == 0.0f && g_hitCd == 45.0f);
    RacerUpdate(1);
    CHECK(g_hitCdSeen == 0.0f && g_hitCd == 45.0f);
    // The host's own car spins: its HIT line (the leader: chance 1), the cooldown starts over, a mention; still spinning next
    // tick: nothing more (one decision per spin).
    int m0 = RaceTestMentions(0);
    g_racerMem[0][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 1 && g_requests[0].cue == Cue(0, 6) && g_requests[0].wait == 2500 && g_hitCd == 0.0f && RaceTestMentions(0) == m0 + 1);
    ItemsTick();
    CHECK(g_requests.size() == 1);
    g_racerMem[0][0xba0] = 0;
    // The guest's car spins (its game's report) inside the 45 s: no line yet, the cooldown untouched; when the 45 s are up
    // while it still spins, its line (as the stock's test, which reads the flag; 2nd: chance 1 - 0.65 / 7 = 0.91 above the
    // roll 0.5), a mention; then nothing more for this spin.
    g_hitCd = 10.0f;
    ReportHit(c, 1);
    ItemsTick();
    CHECK(g_requests.size() == 1 && g_hitCd == 10.0f);
    g_hitCd = 45.0f;
    RaceTestSetFair(true, 0.5f, 0);
    ItemsTick();
    CHECK(g_requests.size() == 2 && g_requests[1].cue == Cue(1, 6) && g_hitCd == 0.0f && RaceTestMentions(1) == 1);
    g_hitCd = 45.0f;
    ItemsTick();
    CHECK(g_requests.size() == 2 && g_hitCd == 45.0f);
    ReportSpinEnd(c, 1);
    ItemsTick();
    // A report about a car that isn't the sender's is ignored; the puppet's own spin flag on this screen doesn't count.
    ReportHit(c, 0);
    g_racerMem[1][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 2 && g_hitCd == 45.0f);
    g_racerMem[1][0xba0] = 0;
    // Players before computer racers (c): computer car 4 spins in the tick the guest's report arrives: the guest's line (its
    // chance 0.91 x 0.3 just mentioned = 0.27 above the roll 0); the computer racer's spin was not decided: when the 45 s are
    // up again while it still spins, its line.
    RaceTestSetFair(true, 0.0f, 0);
    g_racerMem[4][0xba0] = 1;
    ReportHit(c, 1);
    ItemsTick();
    CHECK(g_requests.size() == 3 && g_requests[2].cue == Cue(1, 6) && RaceTestMentions(1) == 2);
    g_hitCd = 45.0f;
    ItemsTick();
    CHECK(g_requests.size() == 4 && g_requests[3].cue == Cue(4, 6) && g_hitCd == 0.0f);
    g_racerMem[4][0xba0] = 0;
    ReportSpinEnd(c, 1);
    ItemsTick();
    // The player's roll fails: the computer racer's line, as the stock (always); no mention; the player's spin is decided.
    g_hitCd = 45.0f;
    RaceTestSetFair(true, 0.99f, 0);
    g_racerMem[4][0xba0] = 1;
    ReportHit(c, 1);
    ItemsTick();
    CHECK(g_requests.size() == 5 && g_requests[4].cue == Cue(4, 6) && g_hitCd == 0.0f && RaceTestMentions(1) == 2);
    g_racerMem[4][0xba0] = 0;
    g_hitCd = 45.0f;
    ItemsTick();
    CHECK(g_requests.size() == 5 && g_hitCd == 45.0f);
    ReportSpinEnd(c, 1);
    ItemsTick();
    // The player's roll fails with no computer racer spinning: no line, the cooldown stays.
    ReportHit(c, 1);
    ItemsTick();
    CHECK(g_requests.size() == 5 && g_hitCd == 45.0f);
    ReportSpinEnd(c, 1);
    ItemsTick();
    // The generic line a quarter of the time; a racer without lines of its own (Metal Sonic): none, the cooldown reset anyway.
    g_genericHit = 0x6666;
    RaceTestSetFair(true, 0.1f, 0);
    g_racerMem[0][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_requests[5].cue == 0x6666 && g_hitCd == 0.0f);
    g_racerMem[0][0xba0] = 0;
    g_hitCd = 45.0f;
    *(uint32_t*)(g_voices[5] + 0x72c + 6 * 4) = 0;
    g_racerMem[5][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 0.0f);
    g_racerMem[5][0xba0] = 0;
    g_genericHit = 0;
    // A car not active (respawning) doesn't count.
    g_hitCd = 45.0f;
    *(int32_t*)(g_racerMem[4] + 0xa1c) = 1;
    g_racerMem[4][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 45.0f);
    *(int32_t*)(g_racerMem[4] + 0xa1c) = 2;
    g_racerMem[4][0xba0] = 0;
    ItemsTick();
    // The stock's own gates: the race clock under 7 s, the game's switch, the call-outs off: no line, the cooldown kept.
    g_hitCd = 45.0f;
    g_raceSeconds = 5.0f;
    g_racerMem[0][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 45.0f);
    g_racerMem[0][0xba0] = 0;
    ItemsTick();
    g_raceSeconds = 10.0f;
    g_commentOff = 1;
    ReportHit(c, 1);
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 45.0f);
    g_commentOff = 0;
    g_calloutsLive = 0;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 45.0f);
    g_calloutsLive = 1;
    ReportSpinEnd(c, 1);
    ItemsTick();

    // ALLSTM: the puppet touched by an All-Star on this screen: the stock's line held with the game's switch for the call,
    // back after; the host's own car touched, or another message kind: untouched.
    g_listenerOffSeen = 0xff;
    CHECK(Listen(1, 0) == 5 && g_listenerOffSeen == 1 && g_commentOff == 0);
    g_listenerOffSeen = 0xff;
    Listen(0, 1);
    CHECK(g_listenerOffSeen == 0);
    g_listenerOffSeen = 0xff;
    Listen(1, 0, 0x10);
    CHECK(g_listenerOffSeen == 0);
    // The guest's report: the stock's line about the All-Star user (its record); a player's counted as a mention.
    ReportHit(c, 1, kLineAllStarHit, 3);
    ItemsTick();
    CHECK(g_allStarHits.size() == 1 && g_allStarHits[0] == 3 && RaceTestMentions(3) == 0);
    int m0b = RaceTestMentions(0);
    ReportHit(c, 1, kLineAllStarHit, 0);
    ItemsTick();
    CHECK(g_allStarHits.size() == 2 && g_allStarHits[1] == 0 && RaceTestMentions(0) == m0b + 1);
    ReportHit(c, 0, kLineAllStarHit, 3);  // not the sender's car
    ItemsTick();
    CHECK(g_allStarHits.size() == 2);

    // Without the group (another mod holds a slot): the stock's pick, untouched; a report dropped.
    RaceTestSetLineHooks(false, true);
    g_hitCdSeen = -1.0f;
    RacerUpdate(0);
    CHECK(g_hitCdSeen == 45.0f);
    g_listenerOffSeen = 0xff;
    Listen(1, 0);
    CHECK(g_listenerOffSeen == 0);
    ReportHit(c, 1);
    g_racerMem[0][0xba0] = 1;
    ItemsTick();
    CHECK(g_requests.size() == 6 && g_hitCd == 45.0f);
    g_racerMem[0][0xba0] = 0;
    ReportHit(c, 1, kLineAllStarHit, 3);
    ItemsTick();
    CHECK(g_allStarHits.size() == 2);
    RaceTestSetLineHooks(true, true);
    ItemsTick();

    // Opa-Opa's weapon, the owner's side (the host's car 0 holds the All-Star; re\m7-opa\FINDINGS.md: the menu runs in phase 3,
    // +0x184 holds the last All-Star's value, then 4, then the cursor; final at the change to 4). Phase 3 goes out without a
    // weapon whatever +0x184 says; phase 4 carries the final one; "none" (4) too.
    static uint8_t opa0[0x200], opa1[0x200];
    memset(opa0, 0, sizeof(opa0));
    memset(opa1, 0, sizeof(opa1));
    *(uint32_t*)opa0 = 0x6bb3d8;
    *(uint8_t**)(opa0 + 0x10) = g_racerMem[0];
    *(int32_t*)(opa0 + 0x184) = 1;  // the last All-Star's
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = opa0;
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 3;
    ItemsTick();
    std::vector<RaceEvent> sent = SentEvents();
    CHECK(sent.size() == 1 && sent[0].kind == kEventAllStarPhase && sent[0].slot == 0 && PhaseBodyOf(sent[0]) == 3);
    *(int32_t*)(opa0 + 0x184) = 4;  // the menu opens
    ItemsTick();
    *(int32_t*)(opa0 + 0x184) = 2;  // the cursor
    ItemsTick();
    CHECK(SentEvents().size() == 1);
    *(float*)(opa0 + 0x160) = 100000.0f;  // (slot 11 sets the shot timer as the All-Star becomes active)
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    sent = SentEvents();
    CHECK(sent.size() == 2 && AllStarPhaseOf(PhaseBodyOf(sent[1])) == 4 && AllStarWeaponOf(PhaseBodyOf(sent[1])) == 2);
    ItemsTick();
    CHECK(SentEvents().size() == 2);
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 5;
    ItemsTick();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    sent = SentEvents();
    CHECK(sent.size() == 4 && AllStarPhaseOf(PhaseBodyOf(sent[2])) == 5 && PhaseBodyOf(sent[3]) == 0);
    // Nothing chosen (the menu never reached its default): phase 4 says "none".
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 3;
    ItemsTick();
    *(int32_t*)(opa0 + 0x184) = 4;
    *(float*)(opa0 + 0x160) = 100000.0f;
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    sent = SentEvents();
    CHECK(sent.size() == 2 && AllStarWeaponOf(PhaseBodyOf(sent[1])) == kWeaponNone);
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    // The host's own car's object: its own choice, untouched by the hook.
    *(int32_t*)(opa0 + 0x184) = 2;
    g_opaWeaponSeen = -1;
    g_opaOffSeen = 0xff;
    OpaStart(opa0);
    CHECK(g_opaWeaponSeen == 2 && g_opaOffSeen == 0);
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = nullptr;

    // The other side: the guest's car 1's All-Star copy here. (A) Its owner's phase 4 (weapon 2) arrives before the copy
    // becomes active: the copy's start fires it instead of this screen's draw (0), the line through the stock.
    *(uint32_t*)opa1 = 0x6bb3d8;
    *(uint8_t**)(opa1 + 0x10) = g_racerMem[1];
    *(int32_t*)(opa1 + 0x184) = 0;
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = opa1;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    g_opaRams.clear();
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9000);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 2), 9001);
    ItemsTick();
    g_opaWeaponSeen = -1;
    g_opaOffSeen = 0xff;
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    CHECK(g_opaWeaponSeen == 2 && g_opaOffSeen == 0 && *(int32_t*)(opa1 + 0x184) == 2 && g_opaLines.empty() && g_opaRams.empty());
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9002);  // over there: forgotten
    ItemsTick();
    // (B) The copy becomes active first: it gets 4 (none) instead of its draw (fires nothing); the owner's weapon 1 then goes
    // in late, with the ram's start (what slot 11 does for weapon 1) and the line about it.
    *(int32_t*)(opa1 + 0x184) = 0;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9003);
    ItemsTick();
    g_opaWeaponSeen = -1;
    g_opaOffSeen = 0xff;
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    CHECK(g_opaWeaponSeen == kWeaponNone && g_opaOffSeen == 0 && *(int32_t*)(opa1 + 0x184) == kWeaponNone && g_opaLines.empty());
    *(float*)(opa1 + 0x160) = 0.0f;  // the blank copy's fire flag reset the shot timer meanwhile
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 1), 9004);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == 1 && g_opaRams.size() == 1 && g_opaRams[0] == opa1 && *(float*)(opa1 + 0x160) == 100000.0f);
    CHECK(g_opaLines.size() == 1 && g_opaLines[0].first == 1 && g_opaLines[0].second == 0xefc87eef);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 1), 9005);  // the same again: nothing more
    ItemsTick();
    CHECK(g_opaLines.size() == 1 && g_opaRams.size() == 1);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9006);
    ItemsTick();
    // (C) Late, weapon 0: no ram, its line; (D) late, "none": nothing fired or said.
    *(int32_t*)(opa1 + 0x184) = 2;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9007);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 0), 9008);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == 0 && g_opaRams.size() == 1 && g_opaLines.size() == 2 && g_opaLines[1].second == 0x131930b8);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9009);
    ItemsTick();
    *(int32_t*)(opa1 + 0x184) = 2;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9010);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kWeaponNone), 9011);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == kWeaponNone && g_opaRams.size() == 1 && g_opaLines.size() == 2);
    // A copy no longer active when the late weapon arrives (its All-Star aborted here): nothing written.
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9012);
    ItemsTick();
    *(int32_t*)(opa1 + 0x184) = 2;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9013);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 1;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 1), 9014);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == kWeaponNone && g_opaRams.size() == 1 && g_opaLines.size() == 2);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9015);
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = nullptr;

    // Metal Sonic's All-Star (re\m9-metal-allstar\FINDINGS.md): its plates close once elapsed (+0x20) >= length (+0x30); a copy's
    // 60 s length never gets there, so the owner sends its time-up and the copy's length becomes its elapsed at the owner's tick.
    RaceTestSetMetalVtable(&FakeMetalVtable);
    alignas(16) static uint8_t metal0[0x220], metal1[0x220], notMetal[0x220];
    memset(metal0, 0, sizeof(metal0));
    memset(metal1, 0, sizeof(metal1));
    memset(notMetal, 0, sizeof(notMetal));
    g_fakeMetalVtable = 0x12345670;
    *(uint32_t*)metal0 = *(uint32_t*)metal1 = 0x12345670;
    *(uint32_t*)notMetal = 0x11111110;  // (another class: not Metal's)
    float* el0 = (float*)(g_racerMem[0] + 0x1ff0 + 0x20);
    float* len0 = (float*)(g_racerMem[0] + 0x1ff0 + 0x30);
    auto metalPhases = [&]() {
        std::vector<RaceEvent> all = SentEvents(), out;
        for (const RaceEvent& e : all)
            if (e.kind == kEventAllStarPhase) out.push_back(e);
        return out;
    };
    // Owner (the host's car 0): phase 4 running, then its time out: one phase-4 message with the time-up, never twice.
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = metal0;
    for (int i = 0; i < 4; ++i) { el0[i] = 1.0f; len0[i] = 12.0f; }
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 3;
    ItemsTick();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    ItemsTick();
    CHECK(metalPhases().size() == 2 && !(PhaseBodyOf(metalPhases()[1]) & kAllStarTimeUp));
    for (int i = 0; i < 4; ++i) el0[i] = 12.0f;
    ItemsTick();
    CHECK(metalPhases().size() == 3 && PhaseBodyOf(metalPhases()[2]) == (4u | kAllStarTimeUp));
    ItemsTick();
    CHECK(metalPhases().size() == 3);
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 5;
    ItemsTick();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    // The next All-Star: a time-up again (one per All-Star).
    for (int i = 0; i < 4; ++i) el0[i] = 0.0f;
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    for (int i = 0; i < 4; ++i) el0[i] = 12.5f;
    ItemsTick();
    CHECK(metalPhases().size() == 7 && (PhaseBodyOf(metalPhases()[6]) & kAllStarTimeUp));
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    // Not Metal's class, or no Metal Sonic core: never a time-up.
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = notMetal;
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    ItemsTick();
    CHECK(metalPhases().size() == 1 && !(PhaseBodyOf(metalPhases()[0]) & kAllStarTimeUp));
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = metal0;
    g_fakeMetalVtable = 0;
    g_sent.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    ItemsTick();
    ItemsTick();
    CHECK(metalPhases().size() == 1 && !(PhaseBodyOf(metalPhases()[0]) & kAllStarTimeUp));
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    g_fakeMetalVtable = 0x12345670;
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = nullptr;
    for (int i = 0; i < 4; ++i) el0[i] = len0[i] = 0.0f;
    // Copy (the guest's car 1 here, its 60 s copy length): the owner's time-up shortens it to the elapsed so far, all lanes.
    float* el1 = (float*)(g_racerMem[1] + 0x1ff0 + 0x20);
    float* len1 = (float*)(g_racerMem[1] + 0x1ff0 + 0x30);
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = metal1;
    for (int i = 0; i < 4; ++i) { el1[i] = 11.5f + i; len1[i] = 60.0f; }
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon), 9016);
    ItemsTick();
    CHECK(len1[0] == 60.0f);  // (no time-up yet: the copy keeps running)
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon, true), 9017);
    ItemsTick();
    CHECK(len1[0] == 11.5f && len1[1] == 12.5f && len1[2] == 13.5f && len1[3] == 14.5f);
    // The owner's end arrives while this copy's plates are still closing (phase 4 here): no S+0x4b for kMetalCloseGrace ticks
    // (the copy ends itself); still phase 4 after that: ended as any copy.
    g_racerMem[1][0x1ff0 + 0x4b] = 0;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(5, kNoWeapon), 9018);
    ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 0);
    for (int t = 0; t < 98; ++t) ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 0);
    for (int t = 0; t < 3; ++t) ItemsTick();
    CHECK(g_racerMem[1][0x1ff0 + 0x4b] == 1);
    g_racerMem[1][0x1ff0 + 0x4b] = 0;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 5;  // (the game's own time-up)
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9024);
    ItemsTick();
    // Regression (the bug): a copy with no time-up keeps its 60 s length, and the owner's end cuts it with the plates open.
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = metal1;
    for (int i = 0; i < 4; ++i) { el1[i] = 12.0f; len1[i] = 60.0f; }
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon), 9025);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(5, kNoWeapon), 9026);
    ItemsTick();
    CHECK(len1[0] == 60.0f && g_racerMem[1][0x1ff0 + 0x4b] == 1);
    g_racerMem[1][0x1ff0 + 0x4b] = 0;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9027);
    ItemsTick();
    // Still transforming here (phase 3) when it arrives: it waits for phase 4.
    for (int i = 0; i < 4; ++i) { el1[i] = 0.0f; len1[i] = 60.0f; }
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon, true), 9019);
    ItemsTick();
    CHECK(len1[0] == 60.0f);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    for (int i = 0; i < 4; ++i) el1[i] = 0.5f;
    ItemsTick();
    CHECK(len1[0] == 0.5f && len1[3] == 0.5f);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9020);
    ItemsTick();
    // A copy that isn't Metal's class here: nothing written.
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = notMetal;
    for (int i = 0; i < 4; ++i) { el1[i] = 3.0f; len1[i] = 60.0f; }
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon, true), 9021);
    ItemsTick();
    CHECK(len1[0] == 60.0f);
    // Over there before the copy got to phase 4: dropped, nothing written later.
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = metal1;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon, true), 9022);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9023);
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    ItemsTick();
    CHECK(len1[0] == 60.0f);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = nullptr;
    for (int i = 0; i < 4; ++i) el1[i] = len1[i] = 0.0f;
    g_fakeMetalVtable = 0;

    // Opa-Opa's shots (re\m7-allstar-shots\FINDINGS.md §3). Owner (the host's car 0, All-Star active): the gate's reset of the
    // shot timer (+0x160 = 0) sends one shot; weapon 1: the ram's pulse rising to 2.5.
    RaceTestSetOriginal(kHOpaInput, (void*)&FakeOpaInput);
    auto shots = [&]() {
        std::vector<RaceEvent> all = SentEvents(), out;
        for (const RaceEvent& e : all)
            if (e.kind == kEventAllStarShot) out.push_back(e);
        return out;
    };
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = opa0;
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    *(int32_t*)(opa0 + 0x184) = 0;
    *(float*)(opa0 + 0x160) = 100000.0f;
    g_sent.clear();
    ItemsTick();
    CHECK(shots().empty());
    *(float*)(opa0 + 0x160) = 0.0f;  // the gate fired this tick
    ItemsTick();
    CHECK(shots().size() == 1 && shots()[0].slot == 0 && PhaseBodyOf(shots()[0]) == AllStarShotBody(0, 1));
    *(float*)(opa0 + 0x160) = 0.02f;  // the next tick: the timer runs again
    ItemsTick();
    CHECK(shots().size() == 1);
    *(float*)(opa0 + 0x160) = 0.0f;
    ItemsTick();
    CHECK(shots().size() == 2 && PhaseBodyOf(shots()[1]) == AllStarShotBody(0, 2));
    ItemsTick();  // a tick with no time passing keeps the 0: not a new shot
    CHECK(shots().size() == 2);
    *(int32_t*)(opa0 + 0x184) = 1;  // the ram: its pulse
    *(float*)(opa0 + 0x154) = 2.5f;
    ItemsTick();
    CHECK(shots().size() == 3 && AllStarShotWeapon(PhaseBodyOf(shots()[2])) == 1);
    ItemsTick();
    CHECK(shots().size() == 3);  // (the pulse stays 2.5 for a moment: one shot)
    *(float*)(opa0 + 0x154) = 1.0f;
    ItemsTick();
    *(float*)(opa0 + 0x154) = 2.5f;
    ItemsTick();
    CHECK(shots().size() == 4);
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = nullptr;
    // Copy (the guest's car 1, All-Star active, weapon 2 in): its input slot outputs nothing of its own (the computer racer's
    // random fire is thrown away); a sent shot opens the gate for the tick and says fire; once fired, the shot is done.
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = opa1;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    *(int32_t*)(opa1 + 0x184) = 0;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9100);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 2), 9101);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    *(float*)(opa1 + 0x160) = 1.0f;
    uint8_t in[12];
    OpaInput(opa1, in);
    CHECK(*(int32_t*)(opa1 + 0x184) == 2 && memcmp(in, "\0\0\0\0\0\0\0\0\0\0\0\0", 12) == 0);
    CHECK(!OpaSceneTick(opa1) && *(float*)(opa1 + 0x160) == 1.0f);  // the scene tick fires nothing of its own
    // The frame: the race update (the shot queued, the gate opened: timer 100000), then the scene update (the input says fire,
    // the gate fires: timer 0), then the next race update reads the proof and the gate isn't opened again: ONE fire per shot.
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarShotBody(2, 1), 9102);
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 100000.0f);
    OpaInput(opa1, in);
    CHECK(in[8] == 1 && in[9] == 1 && in[0] == 0);
    CHECK(OpaSceneTick(opa1) && *(float*)(opa1 + 0x160) == 0.0f);
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 0.0f);  // (not forced again)
    OpaInput(opa1, in);
    CHECK(in[8] == 0 && in[9] == 0);
    *(float*)(opa1 + 0x160) = 0.02f;  // the timer runs on
    CHECK(!OpaSceneTick(opa1));
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 0.02f);  // (nothing due: the timer isn't touched)
    // A shot arriving while the copy's weapon isn't in yet (it became active first, weapon 4): it waits, and fires once the
    // owner's weapon arrives (the late apply, then the gate opened in the same race update, the scene tick fires).
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9103);
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    *(int32_t*)(opa1 + 0x184) = 0;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9104);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    *(float*)(opa1 + 0x160) = 1.0f;
    CHECK(*(int32_t*)(opa1 + 0x184) == kWeaponNone);
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarShotBody(0, 1), 9105);
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 1.0f);  // not forced: the weapon isn't in
    OpaInput(opa1, in);
    CHECK(in[8] == 0 && !OpaSceneTick(opa1));
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 0), 9106);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == 0 && *(float*)(opa1 + 0x160) == 100000.0f);
    CHECK(OpaSceneTick(opa1) && *(float*)(opa1 + 0x160) == 0.0f);
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 0.0f && !OpaSceneTick(opa1));
    // A shot the copy couldn't fire in 90 ticks (its All-Star not active here) is dropped.
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarShotBody(0, 2), 9107);
    ItemsTick();
    g_tick += 95;
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    *(float*)(opa1 + 0x160) = 1.0f;
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x160) == 1.0f && !OpaSceneTick(opa1));
    // Weapon 1 (the ram): the pulse is zeroed for the tick, the scene tick's fire sets it to 2.5, read as the proof next.
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9108);
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 3;
    *(int32_t*)(opa1 + 0x184) = 2;
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(3, kNoWeapon), 9109);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, 1), 9110);
    ItemsTick();
    OpaStart(opa1);
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    *(float*)(opa1 + 0x154) = 1.7f;
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarShotBody(1, 1), 9111);
    ItemsTick();
    CHECK(*(int32_t*)(opa1 + 0x184) == 1 && *(float*)(opa1 + 0x154) == 0.0f);
    CHECK(OpaSceneTick(opa1) && *(float*)(opa1 + 0x154) == 2.5f);
    ItemsTick();
    CHECK(*(float*)(opa1 + 0x154) == 2.5f && !OpaSceneTick(opa1));  // (done: not zeroed again)
    // The host's own object (not remote): its input is its own.
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = opa0;
    OpaInput(opa0, in);
    CHECK(in[0] == 0x55 && in[8] == 1);
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = nullptr;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = nullptr;

    // The pulse All-Stars (Super Sonic 0x6b7c50 +0x178 = 2.5, AiAi 0x6b8380 +0x1b0 = 1.5, B.D. Joe 0x6b8008 +0x150 = 1.5): the same
    // frame as Opa-Opa's shots, the pulse as the gate. Owner (the host's car 0, Super Sonic active): the pulse rising to 2.5 sends
    // one shot; staying there sends none; falling and rising again sends the next.
    RaceTestSetOriginal(kHPulseSonic, (void*)&FakeOpaInput);
    RaceTestSetOriginal(kHPulseAiAi, (void*)&FakeOpaInput);
    RaceTestSetOriginal(kHPulseJoe, (void*)&FakeOpaInput);
    RaceTestSetPulseHooks(true);
    static uint8_t sonic0[0x200], aiai1[0x200];
    memset(sonic0, 0, sizeof(sonic0));
    memset(aiai1, 0, sizeof(aiai1));
    *(uint32_t*)sonic0 = 0x6b7c50;
    *(uint8_t**)(sonic0 + 0x10) = g_racerMem[0];
    *(uint32_t*)aiai1 = 0x6b8380;
    *(uint8_t**)(aiai1 + 0x10) = g_racerMem[1];
    auto pulses = [&]() {
        std::vector<RaceEvent> all = SentEvents(), out;
        for (const RaceEvent& e : all)
            if (e.kind == kEventAllStarShot && AllStarPulseClass(PhaseBodyOf(e)) >= 0) out.push_back(e);
        return out;
    };
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = sonic0;
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 4;
    g_sent.clear();
    ItemsTick();
    CHECK(pulses().empty());
    *(float*)(sonic0 + 0x178) = 2.5f;
    ItemsTick();
    CHECK(pulses().size() == 1 && pulses()[0].slot == 0 && PhaseBodyOf(pulses()[0]) == AllStarPulseBody(0, 1));
    ItemsTick();
    CHECK(pulses().size() == 1);
    *(float*)(sonic0 + 0x178) = 1.2f;
    ItemsTick();
    *(float*)(sonic0 + 0x178) = 2.5f;
    ItemsTick();
    CHECK(pulses().size() == 2 && PhaseBodyOf(pulses()[1]) == AllStarPulseBody(0, 2));
    // Its own input stays its own.
    PulseInput(sonic0, in);
    CHECK(in[0] == 0x55 && in[8] == 1);
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();
    *(uint8_t**)(g_racerMem[0] + 0x1ff0 + 0x40) = nullptr;
    // Copy (the guest's car 1, AiAi active): its input outputs nothing of its own; a sent shot zeroes the pulse for the tick and
    // says fire, the scene tick's fire sets 1.5, the next race update reads it: one fire per shot. A shot of another class
    // (Super Sonic's) for an AiAi copy never fires; one too old is dropped.
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = aiai1;
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 4;
    *(float*)(aiai1 + 0x1b0) = 0.7f;
    PulseInput(aiai1, in);
    CHECK(memcmp(in, "\0\0\0\0\0\0\0\0\0\0\0\0", 12) == 0);
    auto aiaiScene = [&]() {  // 0x5906f0: the pulse starts when the input presses and it is <= 0.001
        uint8_t o[12];
        PulseInput(aiai1, o);
        if (!o[9] || *(float*)(aiai1 + 0x1b0) > 0.001f) return false;
        *(float*)(aiai1 + 0x1b0) = 1.5f;
        return true;
    };
    CHECK(!aiaiScene());
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(4, kNoWeapon), 9200);
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarPulseBody(1, 1), 9201);
    ItemsTick();
    CHECK(*(float*)(aiai1 + 0x1b0) == 0.0f);
    CHECK(aiaiScene() && *(float*)(aiai1 + 0x1b0) == 1.5f);
    ItemsTick();
    CHECK(*(float*)(aiai1 + 0x1b0) == 1.5f && !aiaiScene());
    DeliverEvent(B, c, kEventAllStarShot, 1, AllStarPulseBody(0, 2), 9202);  // Super Sonic's pulse: not this copy's class
    ItemsTick();
    *(float*)(aiai1 + 0x1b0) = 0.9f;
    ItemsTick();
    CHECK(*(float*)(aiai1 + 0x1b0) == 0.9f && !aiaiScene());
    g_tick += 95;
    ItemsTick();
    CHECK(*(float*)(aiai1 + 0x1b0) == 0.9f);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9203);
    ItemsTick();
    *(int32_t*)(g_racerMem[1] + 0x1ff0) = 0;
    *(uint8_t**)(g_racerMem[1] + 0x1ff0 + 0x40) = nullptr;

    // ---- The gate All-Stars (Eggman, Shadow, Alex Kidd, Zobio, Ryo, Knuckles): the owner's proof read right after the class's tick
    // sends the shot (with its payload); a copy's due shot opens the gate before its tick and its input says "fire"; a copy never
    // fires on its own. Owner = the host's car 0, copy = the guest's car 1 (as seen by the host).
    printf("gate All-Stars: Eggman's rockets, Shadow's spears, Alex Kidd's shots, Zobio's zombie, Ryo's lift, Knuckles' punches mirrored\n");
    RaceTestSetGateHooks(true);
    for (int cls = 0; cls < kGateKindCount; ++cls) RaceTestSetOriginal(kHGateFirst + 2 * cls, (void*)&FakeGateInput);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateAmy + 1, (void*)&FakeAmyTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateUlala + 1, (void*)&FakeUlalaTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateBeat + 1, (void*)&FakeBeatTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateBonanza + 1, (void*)&FakeBonanzaTick);
    RaceTestSetProjectileSerial((uintptr_t)&g_fakeSerial);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateEggman + 1, (void*)&FakeEggmanTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateShadow + 1, (void*)&FakeShadowTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateAlexKidd + 1, (void*)&FakeAlexTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateZobio + 1, (void*)&FakeZobioTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateRyo + 1, (void*)&FakeRyoTick);
    RaceTestSetOriginal(kHGateFirst + 2 * kGateKnuckles + 1, (void*)&FakeKnucklesTick);
    static uint8_t gate0[0x200], gate1[0x200], ctrl0[0x100], veh0[0xa00], veh1[0xa00], anim0[0x100], anim1[0x100], animData0[0x20], animData1[0x20];
    uint8_t* saved[6] = {*(uint8_t**)(g_racerMem[0] + 0x9f0), *(uint8_t**)(g_racerMem[0] + 0x9f4), *(uint8_t**)(g_racerMem[0] + 0x2044),
                         *(uint8_t**)(g_racerMem[1] + 0x9f0), *(uint8_t**)(g_racerMem[1] + 0x9f4), *(uint8_t**)(g_racerMem[1] + 0x2044)};
    memset(ctrl0, 0, sizeof(ctrl0));
    memset(veh0, 0, sizeof(veh0));
    memset(veh1, 0, sizeof(veh1));
    memset(anim0, 0, sizeof(anim0));
    memset(anim1, 0, sizeof(anim1));
    memset(animData0, 0, sizeof(animData0));
    memset(animData1, 0, sizeof(animData1));
    *(uint8_t**)(anim0 + 0xcc) = animData0;
    *(uint8_t**)(anim1 + 0xcc) = animData1;
    *(uint8_t**)(g_racerMem[0] + 0x2044) = anim0;
    *(uint8_t**)(g_racerMem[1] + 0x2044) = anim1;
    *(uint8_t**)(g_racerMem[0] + 0x9f4) = veh0;
    *(uint8_t**)(g_racerMem[1] + 0x9f4) = veh1;
    *(uint8_t**)(g_racerMem[0] + 0x9f0) = ctrl0;
    *(int32_t*)(ctrl0 + 0x18) = 1;  // a local human's controller
    auto gates = [&]() {
        std::vector<RaceEvent> all = SentEvents(), out;
        for (const RaceEvent& e : all)
            if (e.kind == kEventAllStarGate) out.push_back(e);
        return out;
    };
    auto gateWord = [](const RaceEvent& e, int i) { uint32_t v; memcpy(&v, e.body + 4 * i, 4); return v; };
    auto activate = [&](uint8_t* obj, int k, uint32_t vtable) {
        memset(obj, 0, 0x200);
        *(uint32_t*)obj = vtable;
        *(uint8_t**)(obj + 0x10) = g_racerMem[k];
        *(uint8_t**)(g_racerMem[k] + 0x1ff0 + 0x40) = obj;
        *(int32_t*)(g_racerMem[k] + 0x1ff0) = 4;
    };
    auto deactivate = [&](int k) {
        *(int32_t*)(g_racerMem[k] + 0x1ff0) = 0;
        *(uint8_t**)(g_racerMem[k] + 0x1ff0 + 0x40) = nullptr;
    };
    const float dt = 0.02f;
    // Eggman, owner (a computer racer: the stock input): no shot while the input doesn't press; the press fires (pattern 0 -> 7) and
    // the shot is sent with the pattern; the restarted timer sends nothing more.
    g_sent.clear();
    g_rockets.clear();
    g_fakePress = g_fakeHuman = false;
    activate(gate0, 0, 0x6bb510);
    *(float*)(gate0 + 0x138) = 0.75f;
    SetTestAnim(g_racerMem[0], 2);
    for (int i = 0; i < 40; ++i) GateTick(kGateEggman, gate0, dt);
    CHECK(gates().empty() && g_rockets.empty());
    g_fakePress = true;
    GateTick(kGateEggman, gate0, dt);
    CHECK(g_rockets.size() == 1 && g_rockets[0] == 7 && gates().size() == 1 && gates()[0].slot == 0 && AllStarGateClass(gateWord(gates()[0], 0)) == kGateEggman &&
          AllStarGateFlags(gateWord(gates()[0], 0)) == 0 && gateWord(gates()[0], 1) == 7);
    GateTick(kGateEggman, gate0, dt);
    CHECK(g_rockets.size() == 1 && gates().size() == 1);
    g_fakePress = false;
    deactivate(0);
    // Eggman, copy: the stock input pressing every tick fires nothing; the owner's shot fires once with the owner's pattern; in a hit
    // reaction (anim 6) the shot waits; one not fired in time is dropped.
    g_rockets.clear();
    activate(gate1, 1, 0x6bb510);
    *(float*)(gate1 + 0x138) = 0.75f;
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 50; ++i) GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateEggman, 1, 0), 3, 0, 9201);
    ItemsTick();
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 1 && g_rockets[0] == 3 && *(float*)(gate1 + 0x134) == 0.0f);
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 1);
    DeliverGate(B, c, 1, AllStarGateHead(kGateEggman, 9, 0), 99, 0, 9201);  // a pattern out of range (never from this build): the first one
    ItemsTick();
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 2 && g_rockets[1] == 0);
    SetTestAnim(g_racerMem[1], 6);
    DeliverGate(B, c, 1, AllStarGateHead(kGateEggman, 2, 0), 5, 0, 9201);
    ItemsTick();
    GateTick(kGateEggman, gate1, dt);
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 2);
    SetTestAnim(g_racerMem[1], 2);
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 3 && g_rockets[2] == 5);
    deactivate(1);
    DeliverGate(B, c, 1, AllStarGateHead(kGateEggman, 3, 0), 1, 0, 9201);
    ItemsTick();
    g_tick += 95;
    ItemsTick();  // (dropped: its All-Star wasn't active here)
    activate(gate1, 1, 0x6bb510);
    *(float*)(gate1 + 0x138) = 0.75f;
    GateTick(kGateEggman, gate1, dt);
    CHECK(g_rockets.size() == 3);
    deactivate(1);
    g_fakePress = false;
    // Shadow, owner: a human's spear is sent with its controller's steer and vertical (slot 6); a computer racer's with the input
    // our hook saw (slot 7).
    g_aims.clear();
    g_fakeHuman = true;
    activate(gate0, 0, 0x6b7bc8);
    *(float*)(ctrl0 + 0x5c) = 0.3f;
    *(float*)(ctrl0 + 0x58) = -0.2f;
    *(int32_t*)(ctrl0 + 0x24) = 0;
    for (int i = 0; i < 5; ++i) GateTick(kGateShadow, gate0, dt);
    CHECK(g_aims.empty() && gates().size() == 1);
    *(int32_t*)(ctrl0 + 0x24) = 2;
    GateTick(kGateShadow, gate0, dt);
    CHECK(g_aims.size() == 2 && *(float*)(gate0 + 0x174) == 0.5f && gates().size() == 2 && AllStarGateClass(gateWord(gates()[1], 0)) == kGateShadow &&
          gateWord(gates()[1], 1) == Bits(0.3f) && gateWord(gates()[1], 2) == Bits(-0.2f));
    *(int32_t*)(ctrl0 + 0x24) = 0;
    g_fakeHuman = false;
    g_fakePress = true;
    *(float*)(gate0 + 0x174) = 0.0f;
    GateTick(kGateShadow, gate0, dt);
    CHECK(gates().size() == 3 && gateWord(gates()[2], 1) == Bits(0.25f) && gateWord(gates()[2], 2) == Bits(-0.5f));
    GateTick(kGateShadow, gate0, 0.0f);  // a tick without time passing keeps the 0.5: no second spear
    CHECK(gates().size() == 3);
    g_fakePress = false;
    deactivate(0);
    // Shadow, copy: fires only the owner's spear, with the owner's aim.
    g_aims.clear();
    activate(gate1, 1, 0x6b7bc8);
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateShadow, gate1, dt);
    CHECK(g_aims.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateShadow, 1, 0), Bits(0.6f), Bits(0.1f), 9201);
    ItemsTick();
    GateTick(kGateShadow, gate1, dt);
    CHECK(g_aims.size() == 2 && g_aims[0] == 0.6f && g_aims[1] == 0.1f && *(float*)(gate1 + 0x174) == 0.5f);
    GateTick(kGateShadow, gate1, dt);
    CHECK(g_aims.size() == 2);
    deactivate(1);
    g_fakePress = false;
    // Alex Kidd, owner (human): the steering builds up from the controller; the shot is sent with the steering it aimed by.
    g_aims.clear();
    g_fakeHuman = true;
    activate(gate0, 0, 0x6b7700);
    *(int32_t*)(veh0 + 0x980) = 1;
    *(float*)(veh0 + 0x984) = 0.0f;
    *(float*)(ctrl0 + 0x5c) = 1.0f;
    *(int32_t*)(ctrl0 + 0x24) = 0;
    for (int i = 0; i < 10; ++i) GateTick(kGateAlexKidd, gate0, dt);
    CHECK(g_aims.empty() && fabsf(*(float*)(veh0 + 0x984) - 0.18f) < 1e-4f);
    *(int32_t*)(ctrl0 + 0x24) = 2;
    GateTick(kGateAlexKidd, gate0, dt);
    float aimed = *(float*)(veh0 + 0x984);
    CHECK(g_aims.size() == 1 && g_aims[0] == aimed && gates().size() == 4 && AllStarGateClass(gateWord(gates()[3], 0)) == kGateAlexKidd && gateWord(gates()[3], 1) == Bits(aimed));
    *(int32_t*)(ctrl0 + 0x24) = 0;
    g_fakeHuman = false;
    deactivate(0);
    // Alex Kidd, copy: the owner's steering is planted so the tick's shrink leaves exactly it for the shot.
    g_aims.clear();
    activate(gate1, 1, 0x6b7700);
    *(int32_t*)(veh1 + 0x980) = 1;
    *(float*)(veh1 + 0x984) = -0.4f;
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateAlexKidd, gate1, dt);
    CHECK(g_aims.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateAlexKidd, 1, 0), Bits(0.198f), 0, 9201);
    ItemsTick();
    GateTick(kGateAlexKidd, gate1, dt);
    CHECK(g_aims.size() == 1 && fabsf(g_aims[0] - 0.198f) < 1e-5f && *(float*)(gate1 + 0x130) > 0.0f);
    deactivate(1);
    g_fakePress = false;
    // Zobio: the owner's press in its ready pose (anim 2 -> 6) is sent; the copy fires only the owner's, and only in its ready pose.
    activate(gate0, 0, 0x6b7de8);
    SetTestAnim(g_racerMem[0], 2);
    g_fakePress = true;
    GateTick(kGateZobio, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 6 && gates().size() == 5 && AllStarGateClass(gateWord(gates()[4], 0)) == kGateZobio);
    g_fakePress = false;
    deactivate(0);
    activate(gate1, 1, 0x6b7de8);
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateZobio, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2);
    SetTestAnim(g_racerMem[1], 4);
    DeliverGate(B, c, 1, AllStarGateHead(kGateZobio, 1, 0), 0, 0, 9201);
    ItemsTick();
    GateTick(kGateZobio, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 4);
    SetTestAnim(g_racerMem[1], 2);
    GateTick(kGateZobio, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 6);
    deactivate(1);
    g_fakePress = false;
    // Ryo: the owner's lift (anim -> 3) is sent once; the copy lifts only at the owner's.
    activate(gate0, 0, 0x6b7a30);
    SetTestAnim(g_racerMem[0], 2);
    g_fakePress = true;
    GateTick(kGateRyo, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 3 && gates().size() == 6 && AllStarGateClass(gateWord(gates()[5], 0)) == kGateRyo);
    GateTick(kGateRyo, gate0, dt);
    CHECK(gates().size() == 6);
    g_fakePress = false;
    deactivate(0);
    activate(gate1, 1, 0x6b7a30);
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateRyo, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2);
    DeliverGate(B, c, 1, AllStarGateHead(kGateRyo, 1, 0), 0, 0, 9201);
    ItemsTick();
    GateTick(kGateRyo, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 3);
    deactivate(1);
    g_fakePress = false;
    // Knuckles, owner: the hold's start (level 1) is sent once per charge, the punch with its level; a new hold after the cooldown
    // sends a new start.
    g_punches.clear();
    activate(gate0, 0, 0x6b7b40);
    g_fakePress = true;
    GateTick(kGateKnuckles, gate0, 0.5f);
    CHECK(*(int32_t*)(gate0 + 0x130) == 1 && gates().size() == 7 && AllStarGateFlags(gateWord(gates()[6], 0)) == kGateFlagHold &&
          AllStarGateClass(gateWord(gates()[6], 0)) == kGateKnuckles);
    GateTick(kGateKnuckles, gate0, 0.5f);
    GateTick(kGateKnuckles, gate0, 0.5f);
    CHECK(*(int32_t*)(gate0 + 0x130) == 2 && gates().size() == 7);
    g_fakePress = false;
    GateTick(kGateKnuckles, gate0, 0.5f);
    CHECK(g_punches.size() == 1 && g_punches[0] == 2 && *(float*)(gate0 + 0x134) == 0.5f && gates().size() == 8 &&
          AllStarGateFlags(gateWord(gates()[7], 0)) == 0 && gateWord(gates()[7], 1) == 2);
    GateTick(kGateKnuckles, gate0, 0.0f);  // a tick without time passing keeps the 0.5: no second punch
    CHECK(gates().size() == 8);
    GateTick(kGateKnuckles, gate0, 0.5f);
    GateTick(kGateKnuckles, gate0, 0.5f);
    g_fakePress = true;
    GateTick(kGateKnuckles, gate0, 0.5f);
    CHECK(gates().size() == 9 && AllStarGateFlags(gateWord(gates()[8], 0)) == kGateFlagHold);
    g_fakePress = false;
    deactivate(0);
    // Knuckles, copy: the stock input holding every tick charges nothing (no phantom punch at the end); the owner's hold start
    // makes it hold (the wind-up), the owner's punch lands with the owner's level, then nothing more.
    g_punches.clear();
    activate(gate1, 1, 0x6b7b40);
    g_fakePress = true;
    for (int i = 0; i < 10; ++i) GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(*(float*)(gate1 + 0x138) == 0.0f && g_punches.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateKnuckles, 1, kGateFlagHold), 0, 0, 9201);
    ItemsTick();
    GateTick(kGateKnuckles, gate1, 0.5f);
    GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(*(float*)(gate1 + 0x138) == 1.0f && *(int32_t*)(gate1 + 0x130) == 1 && g_punches.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateKnuckles, 1, 0), 3, 0, 9201);
    ItemsTick();
    GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(g_punches.size() == 1 && g_punches[0] == 3 && *(float*)(gate1 + 0x134) == 0.5f);
    for (int i = 0; i < 5; ++i) GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(g_punches.size() == 1 && *(float*)(gate1 + 0x138) == 0.0f);
    // The owner's All-Star ends while its hold is mirrored: the hold ends with it (no charge, no punch afterwards).
    DeliverGate(B, c, 1, AllStarGateHead(kGateKnuckles, 2, kGateFlagHold), 0, 0, 9201);
    ItemsTick();
    GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(*(float*)(gate1 + 0x138) == 0.5f);
    DeliverEvent(B, c, kEventAllStarPhase, 1, AllStarPhaseBody(0, kNoWeapon), 9201);
    ItemsTick();
    for (int i = 0; i < 3; ++i) GateTick(kGateKnuckles, gate1, 0.5f);
    CHECK(g_punches.size() == 1 && *(float*)(gate1 + 0x138) == 0.0f);
    deactivate(1);
    g_fakePress = false;
    // Amy, owner: a swing from the ready pose is sent with its anim (the alternating pair without a target zone, the zone's anim with
    // one); nothing outside the ready poses. Copy: swings only at the owner's, with the owner's anim; waits in a hit reaction.
    size_t before = gates().size();
    activate(gate0, 0, 0x6b7810);
    SetTestAnim(g_racerMem[0], 2);
    g_fakePress = true;
    GateTick(kGateAmy, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 4 && gates().size() == before + 1 && AllStarGateClass(gateWord(gates()[before], 0)) == kGateAmy && gateWord(gates()[before], 1) == 4);
    GateTick(kGateAmy, gate0, dt);  // mid-swing (anim 4): no new swing
    CHECK(gates().size() == before + 1);
    SetTestAnim(g_racerMem[0], 7);
    *(int32_t*)(gate0 + 0x140) = 5;  // a target in the right zone
    *(float*)(gate0 + 0x144) = 1.5f;
    GateTick(kGateAmy, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 5 && gates().size() == before + 2 && gateWord(gates()[before + 1], 1) == 5);
    g_fakePress = false;
    deactivate(0);
    activate(gate1, 1, 0x6b7810);
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateAmy, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2);
    DeliverGate(B, c, 1, AllStarGateHead(kGateAmy, 1, 0), 3, 0, 9201);
    ItemsTick();
    GateTick(kGateAmy, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 3 && *(int32_t*)(gate1 + 0x140) == 3);
    SetTestAnim(g_racerMem[1], 6);  // a hit reaction: the stock refuses, the shot waits
    DeliverGate(B, c, 1, AllStarGateHead(kGateAmy, 2, 0), 5, 0, 9201);
    ItemsTick();
    GateTick(kGateAmy, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 6);
    SetTestAnim(g_racerMem[1], 7);
    GateTick(kGateAmy, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 5);
    deactivate(1);
    g_fakePress = false;
    // Ulala, owner: a shot from the ready pose (anim 6/7/8) is sent; with the target car gone the anim stays 2 and only the new
    // object tells. Copy: fires at the owner's shots only, from its ready pose.
    before = gates().size();
    activate(gate0, 0, 0x6b7d60);
    SetTestAnim(g_racerMem[0], 2);
    g_fakePress = true;
    GateTick(kGateUlala, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 6 && gates().size() == before + 1 && AllStarGateClass(gateWord(gates()[before], 0)) == kGateUlala && gateWord(gates()[before], 1) == 6);
    SetTestAnim(g_racerMem[0], 2);
    g_ulalaDead = true;
    GateTick(kGateUlala, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 2 && gates().size() == before + 2 && gateWord(gates()[before + 1], 1) == 2);
    g_ulalaDead = false;
    g_fakePress = false;
    GateTick(kGateUlala, gate0, dt);  // no press, no object: nothing
    CHECK(gates().size() == before + 2);
    g_ulalaOtherObject = true;  // another object made during the tick, with no press: not a shot
    GateTick(kGateUlala, gate0, dt);
    CHECK(gates().size() == before + 2);
    g_fakeHuman = true;  // a human owner: the press from the controller
    *(int32_t*)(ctrl0 + 0x24) = 0;
    GateTick(kGateUlala, gate0, dt);
    CHECK(gates().size() == before + 2);
    *(int32_t*)(ctrl0 + 0x24) = 2;
    g_ulalaDead = true;
    GateTick(kGateUlala, gate0, dt);  // the human's press, the target gone: the shot (no anim change) is told by the new object
    CHECK(gates().size() == before + 3 && gateWord(gates()[before + 2], 1) == 2);
    *(int32_t*)(ctrl0 + 0x24) = 0;
    g_ulalaDead = false;
    g_ulalaOtherObject = false;
    g_fakeHuman = false;
    deactivate(0);
    activate(gate1, 1, 0x6b7d60);
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 30; ++i) GateTick(kGateUlala, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2);
    SetTestAnim(g_racerMem[1], 4);
    DeliverGate(B, c, 1, AllStarGateHead(kGateUlala, 1, 0), 6, 2, 9201);
    ItemsTick();
    GateTick(kGateUlala, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 4);  // (not in the ready pose: waits)
    SetTestAnim(g_racerMem[1], 2);
    GateTick(kGateUlala, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 6);
    GateTick(kGateUlala, gate1, dt);
    SetTestAnim(g_racerMem[1], 2);
    uint32_t serial = g_fakeSerial;
    GateTick(kGateUlala, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2 && g_fakeSerial == serial);  // (once)
    deactivate(1);
    g_fakePress = false;
    // Beat, owner: the spray's held and released edges go out with the steer (a computer Beat always holds: one "held" at its start).
    // Copy: sprays only while its owner holds, aimed by the owner's steer; the stock's always-held input is overridden.
    before = gates().size();
    g_beatAim.clear();
    *(int32_t*)(g_racerMem[0] + 0x1ff0) = 0;
    ItemsTick();  // (the last All-Star over: the owner's phase message; a new one starts its holds afresh)
    activate(gate0, 0, 0x6b7ab8);
    ItemsTick();
    SetTestAnim(g_racerMem[0], 2);
    g_fakePress = true;
    GateTick(kGateBeat, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 3 && gates().size() == before + 1 && AllStarGateClass(gateWord(gates()[before], 0)) == kGateBeat &&
          AllStarGateFlags(gateWord(gates()[before], 0)) == kGateFlagHold && gateWord(gates()[before], 1) == Bits(0.25f));
    GateTick(kGateBeat, gate0, dt);
    CHECK(gates().size() == before + 1);
    g_fakePress = false;
    GateTick(kGateBeat, gate0, dt);
    CHECK(TestAnim(g_racerMem[0]) == 2 && gates().size() == before + 2 && AllStarGateFlags(gateWord(gates()[before + 1], 0)) == kGateFlagRelease);
    g_fakePress = true;
    GateTick(kGateBeat, gate0, dt);
    CHECK(gates().size() == before + 3 && AllStarGateFlags(gateWord(gates()[before + 2], 0)) == kGateFlagHold);
    g_fakePress = false;
    deactivate(0);
    g_beatAim.clear();
    activate(gate1, 1, 0x6b7ab8);
    SetTestAnim(g_racerMem[1], 2);
    g_fakePress = true;
    for (int i = 0; i < 10; ++i) GateTick(kGateBeat, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2 && *(float*)(gate1 + 0x130) == 1.1f && g_beatAim.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateBeat, 1, kGateFlagHold), Bits(0.4f), 0, 9201);
    ItemsTick();
    GateTick(kGateBeat, gate1, dt);
    GateTick(kGateBeat, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 3 && *(float*)(gate1 + 0x130) == 0.9f && g_beatAim.size() == 2 && g_beatAim[1] == 0.4f);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBeat, 1, kGateFlagRelease), 0, 0, 9201);
    ItemsTick();
    GateTick(kGateBeat, gate1, dt);
    CHECK(TestAnim(g_racerMem[1]) == 2 && *(float*)(gate1 + 0x130) == 1.1f && g_beatAim.size() == 2);
    deactivate(1);
    g_fakePress = false;
    // Bonanza Bros, owner (a computer racer: the stock input presses and steers): the blimp's Attack goes out, the aim as it turns,
    // the bomb's throw with the aim now and before; nothing from a blimp that isn't the All-Star's own.
    before = gates().size();
    g_bombs.clear();
    static uint8_t blimp0[0x600], blimp1[0x600], other[0x600], bonAnim[16];
    auto blimpTick = [&](uint8_t* obj, uint8_t* blimp) { ((GateTickFn)RaceTestHook(kHGateFirst + 2 * kGateBonanza + 1))(obj, nullptr, (float*)blimp); };
    activate(gate0, 0, 0x6b7920);
    *(uint8_t**)(gate0 + 0x174) = blimp0;
    *(uint8_t**)(gate0 + 0x1a0) = bonAnim;
    g_fakePress = true;  // (press and steer 0.25)
    blimpTick(gate0, other);
    CHECK(gates().size() == before && *(int32_t*)(gate0 + 0x1a4) == 0);  // another blimp: the stock returns at once, nothing sent
    blimpTick(gate0, blimp0);
    CHECK(*(int32_t*)(gate0 + 0x1a4) == 3 && gates().size() >= before + 1 && AllStarGateFlags(gateWord(gates()[before], 0)) == kGateFlagAttack &&
          AllStarGateClass(gateWord(gates()[before], 0)) == kGateBonanza);
    for (int i = 0; i < 13; ++i) blimpTick(gate0, blimp0);  // the clip to 13; the aim turns each call: sent every call while the attack runs and it moves
    size_t aims = 0;
    for (size_t i = before + 1; i < gates().size(); ++i) aims += AllStarGateFlags(gateWord(gates()[i], 0)) == kGateFlagAim ? 1 : 0;
    CHECK(aims >= 3 && aims <= 14 && aims == gates().size() - before - 1 && g_bombs.empty());
    blimpTick(gate0, blimp0);  // the clip reaches 14: the throw
    CHECK(g_bombs.size() == 1 && *(uint8_t*)(gate0 + 0x186) == 1 && AllStarGateFlags(gateWord(gates().back(), 0)) == 0 &&
          gateWord(gates().back(), 1) == Bits(g_bombs[0]));
    size_t afterThrow = gates().size();
    for (int i = 0; i < 10; ++i) blimpTick(gate0, blimp0);  // the clip ends (state 0), the aim stops at the clamp: no more throws, few aims
    CHECK(g_bombs.size() == 1);
    g_fakePress = false;
    deactivate(0);
    // Bonanza Bros, copy: the stock input pressing and steering changes nothing on its own (no attack, no turning); the owner's
    // Attack starts the clip here, the owner's aim steers the blimp, the throw is held past the clip's 14 until the owner's bomb
    // arrives, then thrown once with the owner's aim; a clip that ends without the owner's throw throws nothing.
    g_bombs.clear();
    activate(gate1, 1, 0x6b7920);
    *(uint8_t**)(gate1 + 0x174) = blimp1;
    *(uint8_t**)(gate1 + 0x1a0) = bonAnim;
    g_fakePress = true;
    for (int i = 0; i < 20; ++i) blimpTick(gate1, blimp1);
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 0 && *(float*)(gate1 + 0x150) == 0.0f && g_bombs.empty());
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 1, kGateFlagAttack), Bits(0.0f), 0, 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 3);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 1, kGateFlagAim), Bits(0.2f), 0, 9201);
    ItemsTick();
    for (int i = 0; i < 16; ++i) blimpTick(gate1, blimp1);  // past the clip's 14: held, no bomb
    CHECK(g_bombs.empty() && *(float*)(gate1 + 0x150) == 0.2f && *(int32_t*)(gate1 + 0x1a4) == 3);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 2, 0), Bits(0.3f), Bits(0.25f), 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);
    CHECK(g_bombs.size() == 1 && g_bombs[0] == 0.3f && *(uint8_t*)(gate1 + 0x186) == 1);
    for (int i = 0; i < 10; ++i) blimpTick(gate1, blimp1);  // the clip ends; a new attack on the copy's own needs the owner's
    CHECK(g_bombs.size() == 1 && *(int32_t*)(gate1 + 0x1a4) == 0);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 3, kGateFlagAttack), 0, 0, 9201);
    ItemsTick();
    for (int i = 0; i < 25; ++i) blimpTick(gate1, blimp1);  // an attack whose owner never throws: the clip ends with no bomb
    CHECK(g_bombs.size() == 1 && *(int32_t*)(gate1 + 0x1a4) == 0);
    // The owner's next Attack arrives while the copy's clip is in its last call: the press waits for the ready state (a press in
    // the call that ends the clip would set state 3 without the Attack's anim, the thrown byte held), then plays with the clip restarted.
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 4, kGateFlagAttack), 0, 0, 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 3);
    for (int i = 0; i < 19; ++i) blimpTick(gate1, blimp1);  // the clip at 19
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 3 && g_clipPos[1] == 19);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 5, kGateFlagAttack), 0, 0, 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);  // the clip ends in this call: no press yet
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 0 && g_clipPos[1] == 20);
    blimpTick(gate1, blimp1);  // ready: the press, the clip restarts (the anim plays)
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 3 && g_clipPos[1] == 0 && *(uint8_t*)(gate1 + 0x186) == 0);
    // A throw arriving for that attack goes once its clip is there; a stale throw is dropped by the next Attack.
    for (int i = 0; i < 15; ++i) blimpTick(gate1, blimp1);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 6, 0), Bits(-0.1f), Bits(-0.1f), 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);
    CHECK(g_bombs.size() == 2 && g_bombs[1] == -0.1f && *(float*)(gate1 + 0x150) == -0.1f);
    for (int i = 0; i < 10; ++i) blimpTick(gate1, blimp1);  // the clip ends
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 0);
    // An Attack and its throw taken in one pass (a late burst): the throw waits for its own attack to start here, then goes at the
    // clip's 14 with its aim.
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 7, kGateFlagAttack), 0, 0, 9201);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 8, 0), Bits(0.15f), Bits(0.15f), 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);
    CHECK(*(int32_t*)(gate1 + 0x1a4) == 3 && g_clipPos[1] == 0 && g_bombs.size() == 2);
    for (int i = 0; i < 13; ++i) blimpTick(gate1, blimp1);
    CHECK(g_bombs.size() == 2);
    blimpTick(gate1, blimp1);
    CHECK(g_bombs.size() == 3 && g_bombs[2] == 0.15f);
    for (int i = 0; i < 10; ++i) blimpTick(gate1, blimp1);
    // A throw that never went here (the copy's clip was over) is dropped by the owner's next Attack: that attack throws nothing of its own.
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 9, 0), Bits(0.05f), Bits(0.05f), 9201);
    ItemsTick();
    blimpTick(gate1, blimp1);  // state 0: nothing to throw from
    CHECK(g_bombs.size() == 3);
    DeliverGate(B, c, 1, AllStarGateHead(kGateBonanza, 10, kGateFlagAttack), 0, 0, 9201);
    ItemsTick();
    for (int i = 0; i < 25; ++i) blimpTick(gate1, blimp1);
    CHECK(g_bombs.size() == 3 && *(int32_t*)(gate1 + 0x1a4) == 0);
    (void)afterThrow;
    deactivate(1);
    g_fakePress = false;
    *(uint8_t**)(g_racerMem[0] + 0x9f0) = saved[0];
    *(uint8_t**)(g_racerMem[0] + 0x9f4) = saved[1];
    *(uint8_t**)(g_racerMem[0] + 0x2044) = saved[2];
    *(uint8_t**)(g_racerMem[1] + 0x9f0) = saved[3];
    *(uint8_t**)(g_racerMem[1] + 0x9f4) = saved[4];
    *(uint8_t**)(g_racerMem[1] + 0x2044) = saved[5];

    // Guest: its own car's spin flag is reported to the host as it changes (spinning, then not); an All-Star's touch on its own
    // car is reported with the user's car, one on another car not; its slot 4 untouched (no hold on a guest).
    c = GuestRunning();
    CHECK(g_scene[0x1350] == 1);
    g_sent.clear();
    g_racerMem[1][0xba0] = 1;
    ItemsTick();
    ItemsTick();
    CHECK(CountSent(kMsgItemLine) == 1 && g_sent.back().peer == A);
    {
        RaceItemLine l;
        CHECK(DecodeItemLine(g_sent.back().data, g_sent.back().size, l) && l.kind == kLineHit && l.slot == 1 && l.other == 1);
    }
    g_racerMem[1][0xba0] = 0;
    ItemsTick();
    CHECK(CountSent(kMsgItemLine) == 2);
    {
        RaceItemLine l;
        CHECK(DecodeItemLine(g_sent.back().data, g_sent.back().size, l) && l.kind == kLineHit && l.slot == 1 && l.other == 0);
    }
    g_sent.clear();
    g_listenerOffSeen = 0xff;
    CHECK(Listen(1, 0) == 5 && g_listenerOffSeen == 0 && CountSent(kMsgItemLine) == 1);
    {
        RaceItemLine l;
        CHECK(DecodeItemLine(g_sent.back().data, g_sent.back().size, l) && l.kind == kLineAllStarHit && l.slot == 1 && l.other == 0);
    }
    Listen(2, 0);
    Listen(1, 1);  // its own All-Star: not a hit by another
    CHECK(CountSent(kMsgItemLine) == 1);
    g_hitCd = 45.0f;
    g_hitCdSeen = -1.0f;
    RacerUpdate(1);
    CHECK(g_hitCdSeen == 45.0f);
    Reset();
}

// ---------------------------------------------------------------- VIEW PROFILE / GAMER CARD (M7, re\m7-profile\FINDINGS.md)
// The results' profile cursor: the rows whose car is a player's (built by 0912 at the row site), Up/Down between them, F1
// asks the Steam worker for that player's profile; the footer; the 60 s list limit at our module's 0916 site.
static void TestProfileCursor() {
    printf("results: the profile cursor (VIEW PROFILE / GAMER CARD)\n");
    RaceContract c = HostRunning();
    RaceTestSetReturnFlag(true);
    static uint8_t module[0x7000];
    memset(module, 0, sizeof(module));
    const uint8_t row[] = {0x32, 0x01, 0x12, 0x09, 0x00, 0x00, 0x96, 0x09, 0x07, 0x01, 0x31, 0x4b, 0x14};
    const uint8_t start[] = {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x51, 0x01, 0x32, 0x02, 0x03, 0x1b, 0x00, 0x00, 0xc6, 0x02};
    const uint8_t list[] = {0x32, 0x00, 0x16, 0x09, 0x00, 0x00, 0x9d, 0x04, 0x51, 0x05, 0x4a, 0x14, 0x08, 0x00, 0x57, 0x09};
    memcpy(module + 0x4239, row, sizeof(row));
    memcpy(module + 0x1ade, start, sizeof(start));
    memcpy(module + 0x25e9 + 12, list, sizeof(list));  // (our module: the stock's 0x25e9 moved 12 bytes down)
    struct { void* vtable; uint8_t* pc; uint8_t* mod; } rvm = {nullptr, module + 0x4239 + 8, module}, svm = {nullptr, module + 0x1ade + 15, module},
                                                      lvm = {nullptr, module + 0x25e9 + 12 + 8, module}, stockList = {nullptr, module + 0x25e9 + 8, module};
    // Not our results yet (the race isn't over): nothing.
    CHECK(RaceProfileNative(0, 0, 0, 0) == -1 && RaceProfileNative(1, 0, 0, 0) == -1 && RaceProfileNative(3, 0, 0, 0) == 0);
    Append(3, 1);  // 1st a computer racer, 2nd the guest, 3rd the host
    Append(1, 1);
    Append(0, 1);
    g_raceOver = 1;
    uint32_t rv[1];
    uint8_t rt[1] = {kTypeInt};
    NativeCall rd = {0x0912, 1, {}, rt, rv, 1, 0, 0};
    for (uint32_t pos = 0; pos < 3; ++pos) {
        rv[0] = pos;
        NHook(kH0912)(&rvm, &rd);
    }
    CHECK(RaceProfileNative(0, 0, 0, 0) == -1);  // the results haven't started
    NativeCall sd = {0x1b03, 1, {}, nullptr, nullptr, 0, 0, 0};
    CHECK(!RaceNativeBefore(&svm, &sd));
    g_profilePosts.clear();
    // The first frame: the cursor lands on the first row with a profile (the 2nd row, the guest's), to be highlighted; the
    // footer gets GAMER CARD. The next frame changes nothing.
    CHECK(RaceProfileNative(0, 0, 0, 0) == 1 && RaceProfileNative(1, 0, 0, 0) == -1 && RaceProfileNative(2, 0, 0, 0) == 1 && RaceProfileNative(3, 0, 0, 0) == 1);
    CHECK(RaceProfileNative(0, 0, 0, 0) == 1 && RaceProfileNative(1, 0, 0, 0) == -1 && RaceProfileNative(2, 0, 0, 0) == -1 && RaceProfileNative(3, 0, 0, 0) == 0);
    // Down: the host's row (3rd); down again: stays; up: back; up: stays (the 1st row is a computer racer's).
    CHECK(RaceProfileNative(0, 0, 1, 0) == 2 && RaceProfileNative(1, 0, 0, 0) == 1 && RaceProfileNative(2, 0, 0, 0) == 2);
    CHECK(RaceProfileNative(0, 0, 1, 0) == 2 && RaceProfileNative(1, 0, 0, 0) == -1 && RaceProfileNative(2, 0, 0, 0) == -1);
    CHECK(RaceProfileNative(0, 1, 0, 0) == 1 && RaceProfileNative(1, 0, 0, 0) == 2 && RaceProfileNative(2, 0, 0, 0) == 1);
    CHECK(RaceProfileNative(0, 1, 0, 0) == 1 && RaceProfileNative(1, 0, 0, 0) == -1 && RaceProfileNative(2, 0, 0, 0) == -1);
    // F1: the guest's Steam profile asked for (its id as two dwords); again at once: not twice within half a second.
    CHECK(RaceProfileNative(0, 0, 0, 1) == 1 && g_profilePosts.size() == 1 && g_profilePosts[0] == B);
    CHECK(RaceProfileNative(0, 0, 0, 1) == 1 && g_profilePosts.size() == 1);
    // The footer is set again every 50 frames (the screen's own footer call may come after ours).
    int footers = 0;
    for (int i = 0; i < 100; ++i) {
        RaceProfileNative(0, 0, 0, 0);
        footers += RaceProfileNative(3, 0, 0, 0) == 1;
    }
    CHECK(footers == 2);
    // The list's time limit: type 5 at our module's site (60 s), the stock's at the stock site (the module without our call).
    NativeCall t = {0x0916, 1, {}, nullptr, nullptr, 0, 0, 0};
    NHook(kH0916)(&lvm, &t);
    CHECK(t.retValue == 5);
    t.retValue = 0;
    NHook(kH0916)(&stockList, &t);
    CHECK(t.retValue == 1);
    // The race closed: nothing.
    NativeCall end = {};
    NHook(kH090a)(nullptr, &end);
    CHECK(RaceProfileNative(0, 0, 0, 0) == -1 && RaceProfileNative(3, 0, 0, 0) == 0);
    (void)c;
    Reset();
}

// The `results` module's call sites race.cpp checks, against the module our build substitutes (menus\out\results.smosbin):
// every site after 0x25e1 moved by the profile cursor's 12-byte insertion (the independent check of m7zh found three that hadn't).
static void TestResultsModuleSites(const char* path) {
    printf("results module: every call site matches the built module\n");
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); ++g_fail; return; }
    static uint8_t module[0x10000];
    size_t n = fread(module, 1, sizeof(module), f);
    fclose(f);
    CHECK(n > 0x6600 && n < sizeof(module) && memcmp(module, "smosbinL", 8) == 0);
    RaceTestSite sites[16];
    int count = RaceTestResultsSites(sites, 16);
    CHECK(count == 6);
    for (int i = 0; i < count; ++i) {
        const RaceTestSite& s = sites[i];
        bool ok = (int)s.pc + s.from >= 0 && s.pc + s.from + s.size <= n && memcmp(module + s.pc + s.from, s.code, s.size) == 0;
        if (!ok) printf("  %s at PC %#x: the module's bytes differ\n", s.name, s.pc);
        CHECK(ok);
    }
}

int main(int argc, char** argv) {
    RaceTestSetAddresses((uintptr_t)&g_scenePtr, (uintptr_t)&g_courseGraph, (uintptr_t)&FakeSeed, (uintptr_t)&FakeSetState,
                         (uintptr_t)&FakeSetPosition, (uintptr_t)&g_tick, (uintptr_t)&g_racerTypeId);
    RaceTestSetOriginal(kH0905, (void*)&Fake0905);
    RaceTestSetOriginal(kH0900, (void*)&Fake0900);
    RaceTestSetOriginal(kH090a, (void*)&Fake090a);
    RaceTestSetOriginal(kHSetup, (void*)&FakeSetup);
    RaceTestSetOriginal(kHWaiting, (void*)&FakeWaiting);
    RaceTestSetOriginal(kHWaitingNoIntro, (void*)&FakeWaiting);
    RaceTestSetOriginal(kHTrackIntro, (void*)&FakeTrackIntro);
    RaceTestSetOriginal(kHCharIntro, (void*)&FakeCharIntro);
    RaceTestSetIntroSoundAddresses((uintptr_t)&g_voicePtr, (uintptr_t)g_voiceVt, (uintptr_t)&FakeVoiceUpdate, (uintptr_t)g_soundVt,
                                   (uintptr_t)&FakeIsPlaying);
    RaceTestSetIntroAddresses((uintptr_t)&g_flowRecPtr, (uintptr_t)&FakeFlowAdvance, (uintptr_t)&FakeFindText);
    RaceTestSetProgressAddress((uintptr_t)&FakeRecordDistance);
    RaceTestSetOriginal(kHCountdown, (void*)&FakeCountdown);
    RaceTestSetOriginal(kHRacing, (void*)&FakeRacing);
    RaceTestSetOriginal(kHRacerCommit, (void*)&FakeCommit);
    RaceTestSetOriginal(kHWff, (void*)&FakeWff);
    RaceTestSetOriginal(kH0912, (void*)&Fake0912);
    RaceTestSetOriginal(kH1b01, (void*)&Fake1b01);
    RaceTestSetOriginal(kH0916, (void*)&Fake0916);
    RaceTestSetOriginal(kH2003, (void*)&Fake2003);
    RaceTestSetOriginal(kH1304, (void*)&Fake1304);
    RaceTestSetOriginal(kH0913, (void*)&Fake0913);
    RaceTestSetRulesAddresses((uintptr_t)g_settings, (uintptr_t)&g_activeProfile);
    RaceTestSetClearItems((uintptr_t)&FakeClearItems);
    RaceTestSetLapAddresses((uintptr_t)&FakeApplyNav, (uintptr_t)&g_recordsPtr, (uintptr_t)&g_finishCount, (uintptr_t)g_finishList,
                            (uintptr_t)&g_raceOver, (uintptr_t)&FakeAddFinish, (uintptr_t)&FakeStartEndTimer, (uintptr_t)&g_timerElapsed,
                            (uintptr_t)&g_timerLimit, (uintptr_t)&g_tickRate);
    RaceTestSetOriginal(kHEventPost, (void*)&FakePost);
    RaceTestSetOriginal(kHRacerMessage, (void*)&FakeRacerMessage);
    RaceTestSetOriginal(kHExhaust, (void*)&FakeExhaust);
    RaceTestSetOriginal(kHCatchup, (void*)&FakeCatchupOriginal);
    RaceTestSetItemAddresses((uintptr_t)&FakeAlloc, (uintptr_t)&FakeFree, (uintptr_t)&FakeProjectileType);
    RaceTestSetTextAddress((uintptr_t)&FakeSetElementText);
    RaceTestSetItems(false);   // M3-M5 cases: no item flag (config.ini [Race] ItemSync=0)
    RaceTestSetHostAi(false);  // M3/M4 cases: each game drives its own AI (config.ini [Race] HostRunsAI=0)
    RaceContract c = TestHostStart();
    TestGuestRace(c);
    TestIntros(c);
    TestHostHolds(c);
    TestHostPeerLeaves();
    TestStaleAndUnknown(c);
    TestMismatch(c);
    TestNav();
    TestLocalFinishesFirst();
    TestOtherFinishesFirst();
    TestLateAndGone();
    TestTimeout();
    TestPlayersDnf();
    TestRecordFinishAndRetry();
    TestBadges();
    TestResultsNames();
    TestPauseTitle();
    TestQuitBox();
    TestPauseLatch();
    TestLoadingTips();
    TestReturnToLobby();
    TestJoinLeavesRace();
    TestSessionDialogs();
    TestToasts();
    TestProgress();
    TestHostRules();
    TestEggCourse();
    TestAiOff();
    TestCatchup();
    TestHostAiGuest();
    TestExhaust();
    TestDriftPlusFlag();
    TestHostAiHost();
    TestHostAiResults();
    TestHostAiHandBack();
    TestFinishPanel();
    TestCarLeft();
    TestNameTags();
    TestProgressBar();
    TestOnlinePause();
    TestItemsHost();
    TestItemsGuest();
    TestReactions();
    TestReactionsGuest();
    TestItemsOff(false, true);
    TestItemsOff(true, false);
    TestEightHumans();
    TestTestPlayers();
    TestAnnouncer();
    TestFairCalls();
    TestVoices();
    TestVoiceMirror();
    TestItemLines();
    TestProfileCursor();
    if (argc > 1) TestResultsModuleSites(argv[1]);
    printf("race bridge: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
