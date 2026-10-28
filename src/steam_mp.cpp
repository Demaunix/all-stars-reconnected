// Steam Multiplayer for Sonic & SEGA All-Stars Racing (ChaoGarage runtime).
// Runs Steamworks inside the game process: lobbies created, searched and joined on request from
// the multiplayer menus (lobby.h mailbox), rich presence, warm/cold joins, and the mod-set check.
//
// Threading: every Steamworks call happens on one worker thread owned by this DLL.
// SsasrModInitialize (main thread, before the game's CRT) only checks files, binds
// steam_api.dll, reads the command line and starts that thread, so a Steam failure
// never disables the player's other mods: this runtime logs it and stays inactive.
#include <windows.h>
#include <bcrypt.h>
#include <delayimp.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include <climits>
#include <string>
#include <utility>
#include "mod_api.h"
#include "steam/steam_api.h"
#include "parse.h"
#include "background.h"
#include "script_subst.h"
#include "log.h"
#include "modset.h"
#include "lobby.h"
#include "natives.h"
#include "race_wire.h"
#include "voice_chat.h"
#include "search_rows.h"
#include "race.h"
#include "fingerprint.h"

#define MOD_NAME L"Steam Multiplayer"
#define MOD_VERSION "1.1.1-dp9"
static const uint32_t kAppId = 34190;
// sdk/redistributable_bin/steam_api.dll from Steamworks SDK 1.65 (32-bit).
static const char kSteamApiSha256[] = "1cb8b7cb77923762d4e49b27e44295369a79e93b5d4282c5e840b268d28fe771";

// ---------------------------------------------------------------- logging
static CRITICAL_SECTION g_logLock;
static bool g_logReady = false;
static std::wstring g_logPath;
static void(__cdecl* g_hostLog)(const wchar_t*) = nullptr;

static std::wstring Widen(const char* utf8) {
    if (!utf8) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

// Writes to mods\Steam Multiplayer\SteamMultiplayer.log; `host` also mirrors the line to SsasrMods.log.
static void LogV(bool host, const wchar_t* fmt, va_list ap) {
    if (!g_logReady) return;
    wchar_t msg[1024];
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    SYSTEMTIME t;
    GetSystemTime(&t);
    wchar_t line[1200];
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u UTC [%5lu] %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, line, -1, &s[0], n, nullptr, nullptr);
    EnterCriticalSection(&g_logLock);
    HANDLE h = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD k;
        WriteFile(h, s.data(), (DWORD)s.size(), &k, nullptr);
        CloseHandle(h);
    }
    if (host && g_hostLog) {
        std::wstring m = MOD_NAME L": ";
        m += msg;
        g_hostLog(m.c_str());
    }
    LeaveCriticalSection(&g_logLock);
}

void Log(bool host, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    LogV(host, fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------- steam_api.dll binding
// The delay-load helper resolves imports by the bare name "steam_api.dll". This hook makes it use
// the exact module this runtime loaded by full path after checking its hash, never another copy.
static HMODULE g_steamApi = nullptr;

static FARPROC WINAPI DelayHook(unsigned notify, PDelayLoadInfo info) {
    if (notify == dliNotePreLoadLibrary && g_steamApi && _stricmp(info->szDll, "steam_api.dll") == 0) return (FARPROC)g_steamApi;
    return nullptr;
}
extern "C" const PfnDliHook __pfnDliNotifyHook2 = DelayHook;

// Binds every steam_api import now, so a missing export is a logged failure here and never a
// delay-load exception later inside the worker.
static bool BindSteamApi() {
    __try {
        return SUCCEEDED(__HrLoadAllImportsForDll("steam_api.dll"));
    } __except (GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND) ||
                        GetExceptionCode() == VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND)
                    ? EXCEPTION_EXECUTE_HANDLER
                    : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// ---------------------------------------------------------------- helpers
static bool FileSha256(const std::wstring& path, std::string& hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    static unsigned char buf[64 * 1024];  // only used on the main thread during initialization
    for (;;) {
        if (!ok) break;
        DWORD got = 0;
        if (!ReadFile(f, buf, sizeof(buf), &got, nullptr)) { ok = false; break; }
        if (!got) break;  // end of file
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buf, got, 0));
    }
    unsigned char digest[32];
    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (!ok) return false;
    hex.clear();
    for (unsigned char b : digest) {
        char two[3];
        sprintf_s(two, "%02x", b);
        hex += two;
    }
    return true;
}

static uint64_t LobbyFromCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    uint64_t id = LobbyFromArgs(argc, argv);
    if (argv) LocalFree(argv);
    return id;
}

static const wchar_t* InitResultName(ESteamAPIInitResult r) {
    switch (r) {
    case k_ESteamAPIInitResult_OK: return L"OK";
    case k_ESteamAPIInitResult_FailedGeneric: return L"FailedGeneric";
    case k_ESteamAPIInitResult_NoSteamClient: return L"NoSteamClient";
    case k_ESteamAPIInitResult_VersionMismatch: return L"VersionMismatch";
    default: return L"?";
    }
}

static std::wstring PersonaOf(CSteamID id) {
    return Widen(SteamFriends()->GetFriendPersonaName(id));
}

// ---------------------------------------------------------------- mod-set check
// Scanned once in SsasrModInitialize (the loader starts all of mods\*\mod.ini or none); read-only after.
static std::vector<ModEntry> g_mods;
static std::string g_modsEncoded;
static bool g_modsOk = false;
// ENABLE MODIFICATIONS (1.1): the toggleable mods (modset.h ModToggleable) the host turned off for the lobby it creates next
// (LobbySetHostMods, from the menu's question; none until it is answered, as 1.0.0).
static volatile LONG g_hostMetalOff, g_hostEggOff;
static std::vector<std::string> HostModsOff() {
    std::vector<std::string> off;
    if (InterlockedCompareExchange(&g_hostMetalOff, 0, 0)) off.push_back("metal-sonic");
    if (InterlockedCompareExchange(&g_hostEggOff, 0, 0)) off.push_back("egg-hangar");
    return off;
}
static int g_testPlayers = 0;  // config.ini [Debug] TestPlayers: pretend players in the lobbies this game hosts (lobby.h)
static bool g_testSameRacer = false;  // config.ini [Debug] TestPlayerRacer=same: they all take the host's racer
static std::wstring g_configIni;  // the mod's config.ini (the voice chat reads its [Voice] section on the worker)
const wchar_t* LobbyConfigPath() { return g_configIni.c_str(); }
static volatile LONG g_overlayOpen = 0;  // Steam's overlay is open (worker callback, read by the game thread)

// Lobby data keys. "proto" marks our lobbies (search filter); "mods" holds the host's mod list
// (modset.h EncodeMods); "members" is kept current by the host so searchers see the player count.
static const char kModsKey[] = "mods";
static const char kProtoKey[] = "asr_proto";
static const char kProto[] = "1";

// ---------------------------------------------------------------- mailbox (game thread <-> worker)
static CRITICAL_SECTION g_boxLock;
static bool g_steamUp = false;  // worker accepts commands
static LobbySnapshot g_snap = {};
struct Command { LobbyCommandKind kind; int32_t arg; int32_t args[kCommandArgs]; };
static Command g_cmds[16];
static int g_cmdCount = 0;
static HANDLE g_wake = nullptr;  // auto-reset: the game thread queued something for the worker

// A session command (host, search, join, leave) shows its first state in the snapshot at once (the
// worker applies it up to ~30 ms later), so a menu never reads the previous search's results or
// error as the answer to a new request. Member-data commands (racer, ready, rules) change no state.
static bool Post(const Command& c) {
    EnterCriticalSection(&g_boxLock);
    bool ok = g_steamUp && g_cmdCount < (int)(sizeof(g_cmds) / sizeof(g_cmds[0]));
    if (ok) {
        g_cmds[g_cmdCount++] = c;
        if (IsSessionCommand(c.kind)) {
            g_snap.state = FirstState(c.kind);
            if (c.kind == kCmdJoinLobby) g_snap.pendingJoin = 0;  // taken (the worker clears its copy)
            g_snap.error = kLobbyErrNone;
            if (c.kind == kCmdSearch) g_snap.rowCount = 0;
            if (c.kind != kCmdJoinRow) g_snap.memberCount = 0;
            ++g_snap.revision;
        }
    }
    LeaveCriticalSection(&g_boxLock);
    if (ok && g_wake) SetEvent(g_wake);
    return ok;
}

bool LobbyPost(LobbyCommandKind kind, int32_t arg) {
    Command c = {};
    c.kind = kind;
    c.arg = arg;
    return Post(c);
}

bool LobbyOverlayOpen() { return InterlockedCompareExchange(&g_overlayOpen, 0, 0) != 0; }

static volatile LONG g_lobbyFrameAt = 0;  // GetTickCount | 1 of the last lobby frame (0: none yet)
void LobbyNoteLobbyFrame() { InterlockedExchange(&g_lobbyFrameAt, (LONG)(GetTickCount() | 1)); }
static volatile LONG g_localFlag = -1;
void LobbyNoteFlag(int32_t code) { InterlockedExchange(&g_localFlag, code); }
static volatile LONG g_localInput = kInputUnknown;
void LobbyNoteInput(int32_t kind) { InterlockedExchange(&g_localInput, kind); }
static const char* InputData(int32_t kind) { return kind == kInputKeyboard ? "k" : kind == kInputController ? "c" : ""; }
// (g_mods is written once at start-up, before any menu runs.)
bool LobbyModInstalled(const char* id) { return g_modsOk && ModListed(g_mods, id); }
void LobbySetHostMods(bool metalOn, bool eggOn) {
    InterlockedExchange(&g_hostMetalOff, metalOn ? 0 : 1);
    InterlockedExchange(&g_hostEggOff, eggOn ? 0 : 1);
    Log(false, L"ENABLE MODIFICATIONS: the next lobby uses Metal Sonic %s, Egg Hangar %s", metalOn ? L"if installed" : L"off",
        eggOn ? L"if installed" : L"off");
}

bool LobbyPostArgs(LobbyCommandKind kind, const int32_t* args, int count) {
    if (count < 0 || count > kCommandArgs) return false;
    Command c = {};
    c.kind = kind;
    for (int i = 0; i < count; ++i) c.args[i] = args[i];
    return Post(c);
}

void LobbyRead(LobbySnapshot& out) {
    EnterCriticalSection(&g_boxLock);
    out = g_snap;
    LeaveCriticalSection(&g_boxLock);
}

// ---------------------------------------------------------------- race message pipe
// Two bounded queues under their own lock. The game thread never blocks on Steam: a full outbound
// queue refuses the message (poses are sent again next tick; the holds resend Loaded every 2 s and the
// host answers each with Go once released), a full inbound queue drops the oldest pose first.
static CRITICAL_SECTION g_raceLock;
static RaceNetMsg g_raceOut[64];
static int g_raceOutCount = 0;
static RaceNetMsg g_raceIn[256];
static int g_raceInCount = 0;
static volatile LONG g_raceActive = 0;
static volatile uint64_t g_selfId = 0;

bool RaceNetSend(uint64_t to, const void* data, size_t size, bool reliable) {
    if (!data || !size || size > kRaceMsgMax) return false;
    EnterCriticalSection(&g_raceLock);
    bool ok = g_steamUp && g_raceOutCount < (int)(sizeof(g_raceOut) / sizeof(g_raceOut[0]));
    if (ok) {
        RaceNetMsg& m = g_raceOut[g_raceOutCount++];
        m.peer = to;
        m.size = (uint32_t)size;
        m.reliable = reliable;
        memcpy(m.data, data, size);
    }
    LeaveCriticalSection(&g_raceLock);
    if (ok && g_wake) SetEvent(g_wake);
    return ok;
}

int RaceNetReceive(RaceNetMsg* out, int max) {
    EnterCriticalSection(&g_raceLock);
    int n = g_raceInCount < max ? g_raceInCount : max;
    memcpy(out, g_raceIn, n * sizeof(RaceNetMsg));
    memmove(g_raceIn, g_raceIn + n, (g_raceInCount - n) * sizeof(RaceNetMsg));
    g_raceInCount -= n;
    LeaveCriticalSection(&g_raceLock);
    return n;
}

void RaceNetActive(bool on) {
    InterlockedExchange(&g_raceActive, on ? 1 : 0);
    if (g_wake) SetEvent(g_wake);
}

uint64_t RaceNetSelf() { return g_selfId; }

static int TakeRaceOut(RaceNetMsg* out, int max) {
    EnterCriticalSection(&g_raceLock);
    int n = g_raceOutCount < max ? g_raceOutCount : max;
    memcpy(out, g_raceOut, n * sizeof(RaceNetMsg));
    memmove(g_raceOut, g_raceOut + n, (g_raceOutCount - n) * sizeof(RaceNetMsg));
    g_raceOutCount -= n;
    LeaveCriticalSection(&g_raceLock);
    return n;
}

static void PutRaceIn(uint64_t from, const void* data, uint32_t size, bool reliable) {
    EnterCriticalSection(&g_raceLock);
    const int cap = (int)(sizeof(g_raceIn) / sizeof(g_raceIn[0]));
    if (g_raceInCount == cap) {  // full: drop the oldest unreliable message, else the oldest
        int drop = 0;
        for (int i = 0; i < cap; ++i)
            if (!g_raceIn[i].reliable) { drop = i; break; }
        memmove(g_raceIn + drop, g_raceIn + drop + 1, (cap - drop - 1) * sizeof(RaceNetMsg));
        --g_raceInCount;
    }
    RaceNetMsg& m = g_raceIn[g_raceInCount++];
    m.peer = from;
    m.size = size;
    m.reliable = reliable;
    memcpy(m.data, data, size);
    LeaveCriticalSection(&g_raceLock);
}

static void ClearRaceQueues() {
    EnterCriticalSection(&g_raceLock);
    g_raceOutCount = 0;
    g_raceInCount = 0;
    LeaveCriticalSection(&g_raceLock);
}

static int TakeCommands(Command* out, int max) {
    EnterCriticalSection(&g_boxLock);
    int n = g_cmdCount < max ? g_cmdCount : max;
    memcpy(out, g_cmds, n * sizeof(Command));
    memmove(g_cmds, g_cmds + n, (g_cmdCount - n) * sizeof(Command));
    g_cmdCount -= n;
    LeaveCriticalSection(&g_boxLock);
    return n;
}

static void SetSteamUp(bool up) {
    EnterCriticalSection(&g_boxLock);
    g_steamUp = up;
    if (!up) g_cmdCount = 0;
    g_snap.state = up ? kLobbyIdle : kLobbyOff;
    ++g_snap.revision;
    LeaveCriticalSection(&g_boxLock);
    if (!up) ClearRaceQueues();
}

static void CopyName(char (&dst)[kNameLen], const char* src) {
    strncpy_s(dst, src ? src : "", _TRUNCATE);
}

// ---------------------------------------------------------------- session (worker thread only)
// One Steam operation (create, join, public search) is outstanding at a time. A menu command that
// arrives meanwhile never gets dropped: a pending public search is cancelled, and a pending create
// or join (which Steam can't cancel) is abandoned - its lobby is left as soon as it arrives - and
// the new command runs right after it. Leave abandons it too. A friends search asks Steam for each
// friend's lobby data and finishes when all answered or after kFriendSearchMs. The worker loop
// publishes the snapshot after applying each batch of commands.
class Session {
public:
    explicit Session(uint64_t coldJoin) : m_coldJoin(coldJoin) {}

    void Start() {
        ESteamNetworkingAvailability auth = SteamNetworkingSockets()->InitAuthentication();  // asks Steam for the certificate now
        if (auth != k_ESteamNetworkingAvailability_Current) Log(false, L"race connections: waiting for this game's Steam certificate (status %d)", (int)auth);
        NoteAuth(auth);
        m_online = SteamUser()->BLoggedOn();
        if (!m_online) Log(true, L"Steam is not connected to the Steam servers (offline mode?): online games can't be found or hosted until it is");
        m_voice.Configure(g_configIni.c_str());
        Publish();
        if (m_coldJoin) {
            Log(true, L"cold join: launched with +connect_lobby %llu; joined once this player has picked a license", m_coldJoin);
            RememberJoin(m_coldJoin);
        }
    }

    void Apply(const Command& c) {
        if (m_stopping) return;
        if (c.kind == kCmdSetLocal) {  // member data: kept, and written to whatever lobby we are in
            m_localRacer = c.args[0];
            m_localReady = c.args[1] ? 1 : 0;
            WriteLocalData();
            return;
        }
        if (c.kind == kCmdSetRules) {
            LobbyRules r = {c.args[0], c.args[1], c.args[2], c.args[3], c.args[4], c.args[5]};
            m_localRules = r;
            WriteRules();
            return;
        }
        if (c.kind == kCmdSetProgress) {  // member data: never cancels a search or waits behind a create/join
            memcpy(m_progress, c.args, sizeof(m_progress));
            WriteProgress();
            return;
        }
        if (c.kind == kCmdSetTestProgress) {  // host: its test players' race progress (lobby data, kept here)
            if (c.args[5] == 0) {
                for (auto& p : m_testProgress) p[0] = -1;
            } else if (c.args[5] >= 1 && c.args[5] <= kMaxMembers - 1) {
                memcpy(m_testProgress[c.args[5] - 1], c.args, sizeof(m_testProgress[0]));
            }
            m_testProgressDirty = true;  // (written once per pump by Tick: one lobby data write for all of them)
            return;
        }
        if (c.kind == kCmdProfile) {  // VIEW PROFILE / GAMER CARD (the results' cursor): that player's profile page in the overlay
            CSteamID id((uint64)(uint32_t)c.args[0] | ((uint64)(uint32_t)c.args[1] << 32));
            if (id.IsValid() && id.BIndividualAccount() && !IsTestPlayer(id.ConvertToUint64())) {
                SteamFriends()->ActivateGameOverlayToUser("steamid", id);
                Log(false, L"view profile: the Steam overlay was asked for %s's profile%s", PersonaOf(id).c_str(),
                    SteamUtils()->IsOverlayEnabled() ? L"" : L" (Steam says its overlay is off in this game)");
            }
            return;
        }
        if (c.kind == kCmdInvite) {  // INVITE FRIENDS: Steam's own invite list for this lobby
            if (m_lobby.IsValid() && m_state == kLobbyInLobby) {
                SteamFriends()->ActivateGameOverlayInviteDialog(m_lobby);
                Log(false, L"invite friends: the Steam overlay's invite dialog was asked for%s",
                    SteamUtils()->IsOverlayEnabled() ? L"" : L" (Steam says its overlay is off in this game)");
            }
            return;
        }
        if (c.kind == kCmdKick) {  // KICK (host): that member out of the lobby, for good
            Kick((uint64_t)(uint32_t)c.args[0] | ((uint64_t)(uint32_t)c.args[1] << 32));
            return;
        }
        if (c.kind == kCmdMute) {  // MUTE (the lobby popup): this game stops or starts playing that member's voice (until the game closes)
            uint64_t id = (uint64_t)(uint32_t)c.args[0] | ((uint64_t)(uint32_t)c.args[1] << 32);
            int r = m_voice.ToggleMuted(id);
            Log(true, L"voice: %s %s%s", PersonaOf(CSteamID((uint64)id)).c_str(), r == 1 ? L"is muted here" : r == 0 ? L"is heard again here" : L"not muted (16 players muted already)",
                m_voice.Mode() == kVoiceOff ? L" (voice chat is off in config.ini)" : L"");
            return;
        }
        if (c.kind == kCmdLeave) {
            m_hasDeferred = false;
            Leave();
            return;
        }
        if (m_pending == Op::Listing) CancelListing();
        if (Busy()) {  // a create or join is in flight: run this command when it has been answered
            m_abandon = true;
            m_deferred = c;
            m_hasDeferred = true;
            if (c.kind == kCmdJoinLobby && m_pendingJoin == ((uint64_t)(uint32_t)c.args[0] | ((uint64_t)(uint32_t)c.args[1] << 32)))
                m_pendingJoin = 0;  // taken (it waits in m_deferred; Back drops it)
            SetState((LobbyState)FirstState(c.kind));
            Log(false, L"menu command %d waits for the pending Steam request", (int)c.kind);
            return;
        }
        Run(c);
    }

    // Called every pump: finishes a friends search whose answers are complete or overdue.
    void Tick() {
        // Away from the lobby screen (no lobby frame for a moment: loading, racing, results, another menu): not ready
        // (user, 2026-10-22: the check means "at the lobby"). The natives post ready again on the next lobby frame.
        if (m_lobby.IsValid() && m_localReady && LobbyFrameStale((uint32_t)InterlockedCompareExchange(&g_lobbyFrameAt, 0, 0), GetTickCount())) {
            m_localReady = 0;
            WriteLocalData();
            Log(false, L"away from the lobby screen: shown as not ready until back");
        }
        if (m_testProgressDirty) {
            m_testProgressDirty = false;
            WriteTestProgress();
        }
        if (m_lobby.IsValid() && (int32_t)InterlockedCompareExchange(&g_localInput, 0, 0) != m_postedInput) WriteLocalData();
        // After a failed certificate request Steam doesn't retry on its own (isteamnetworkingsockets.h,
        // InitAuthentication): ask again every 30 s until it is in.
        if (!m_netReady && GetTickCount() - m_authAsked > 30000) {
            m_authAsked = GetTickCount();
            NoteAuth(SteamNetworkingSockets()->InitAuthentication());
        }
        if (m_state == kLobbySearching && m_listDone && (m_waiting.empty() || GetTickCount() - m_searchStart > kFriendSearchMs)) {
            if (!m_waiting.empty()) Log(false, L"search: %u friend lobby(s) did not answer in time", (unsigned)m_waiting.size());
            m_waiting.clear();
            FinishSearch();
            Publish();
        }
        PollNewHost();
    }

    // No new operations after this; results still in flight are left when they arrive.
    void Stop() {
        m_stopping = true;
        m_voice.Shutdown();
        LeaveCurrent();
        SteamFriends()->ClearRichPresence();
    }

    // Race messages: send what the game queued, hand over what arrived. Only lobby members count.
    // Voice chat (voice_chat.h; M7): every loop, with the lobby's other real members as the peers (the only senders heard, the
    // only recipients); talking while in a lobby, the race included.
    void PumpVoice() {
        uint64_t peers[kMaxMembers];
        int n = 0;
        for (const LobbyMember& m : m_members)
            if (!m.isMe && !IsTestPlayer(m.id) && n < kMaxMembers) peers[n++] = m.id;
        m_voice.Pump(peers, n, m_lobby.IsValid() && m_state == kLobbyInLobby, LobbyOverlayOpen());
    }

    void PumpRace() {
        RaceNetMsg out[16];
        int n;
        while ((n = TakeRaceOut(out, 16)) > 0) {
            for (int i = 0; i < n; ++i) {
                if (!m_lobby.IsValid()) continue;  // not in a lobby: nobody to send to
                const RaceNetMsg& m = out[i];
                int flags = m.reliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_UnreliableNoNagle;
                flags |= k_nSteamNetworkingSend_AutoRestartBrokenSession;
                for (const LobbyMember& member : m_members) {
                    if (member.isMe || IsTestPlayer(member.id) || (m.peer && member.id != m.peer)) continue;
                    SteamNetworkingIdentity to;
                    to.SetSteamID64(member.id);
                    EResult r = SteamNetworkingMessages()->SendMessageToUser(to, m.data, m.size, flags, kRaceChannel);
                    if (r != k_EResultOK && m.reliable) Log(false, L"race message to %llu not sent (EResult %d)", member.id, (int)r);
                    NotePeer(member.id);
                }
            }
        }
        SteamNetworkingMessage_t* in[32];
        while ((n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kRaceChannel, in, 32)) > 0) {
            for (int i = 0; i < n; ++i) {
                uint64_t from = in[i]->m_identityPeer.GetSteamID64();
                uint32_t size = (uint32_t)in[i]->m_cbSize;
                if (IsMember(from) && size > 0 && size <= kRaceMsgMax) {
                    PutRaceIn(from, in[i]->m_pData, size, (in[i]->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0);
                    NotePeer(from);
                } else if (!m_strangerLogged) {
                    Log(false, L"race message from %llu ignored (not in our lobby, or %u bytes)", from, size);
                    m_strangerLogged = true;
                }
                in[i]->Release();
            }
        }
    }

    bool Busy() const { return m_pending != Op::None; }

private:
    enum class Op { None, Creating, Joining, Listing };
    static const DWORD kFriendSearchMs = 3000;

    void SetState(LobbyState s, LobbyError e = kLobbyErrNone) {
        m_state = s;
        m_error = e;
    }

    void Run(const Command& c) {
        // Steam offline (M7): without Steam's servers nothing can be found, hosted or joined; the menus get the notice at once
        // instead of a request that fails later with a misleading error.
        if (!m_online && (c.kind == kCmdHost || c.kind == kCmdSearch || c.kind == kCmdJoinRow || c.kind == kCmdJoinLobby)) {
            m_online = SteamUser()->BLoggedOn();  // (asked live: a connection made without a callback seen, e.g. just after the start)
            if (!m_online) {
                Log(true, L"menu command %d refused: Steam is not connected to the Steam servers", (int)c.kind);
                // A remembered Steam join is spent by its refusal, as by a failed join: the menus would take it again every frame.
                if (c.kind == kCmdJoinLobby && m_pendingJoin == ((uint64_t)(uint32_t)c.args[0] | ((uint64_t)(uint32_t)c.args[1] << 32))) m_pendingJoin = 0;
                SetState(kLobbyError, kLobbyErrSteamOff);
                return;
            }
            Log(true, L"Steam is connected to the Steam servers (asked)");
        }
        switch (c.kind) {
        case kCmdHost: Host(c.arg == kModeFriends ? kModeFriends : kModeAnyone); break;
        case kCmdSearch: Search(c.arg == kModeFriends ? kModeFriends : kModeAnyone); break;
        case kCmdJoinRow: {
            if (c.arg < 0 || c.arg >= m_rows.count) {
                Log(false, L"join refused: no row %d", c.arg);
                SetState(kLobbyError, kLobbyErrJoin);
                break;
            }
            const LobbyRow row = m_rows.rows[c.arg];
            if (row.otherVersion) {  // listed so the player sees the game; joinable only with the host's build
                Log(true, L"can't join %s's game: the host runs another Steam Multiplayer version (you run %hs)", Widen(row.host).c_str(), MOD_VERSION);
                SetState(kLobbyError, kLobbyErrVersion);
                break;
            }
            if (row.otherMods && !ModsMatch(CSteamID(row.lobby), true)) {  // (the differences go to the log, as at a join)
                SetState(kLobbyError, kLobbyErrMods);
                break;
            }
            Join(row.lobby, L"menu");
            m_joinPublic = m_pending == Op::Joining && m_mode != kModeFriends && !row.friendRow;  // a stranger: only public seats (PRIVATE SLOTS)
            break;
        }
        case kCmdJoinLobby: {
            uint64_t lobby = (uint64_t)(uint32_t)c.args[0] | ((uint64_t)(uint32_t)c.args[1] << 32);
            if (lobby == m_pendingJoin) m_pendingJoin = 0;
            m_mode = kModeFriends;  // (a friend's game: every seat counts)
            Join(lobby, L"Steam");
            break;
        }
        default: break;
        }
    }

    // After a create/join result: the command that waited for it.
    void RunDeferred() {
        if (!m_hasDeferred || m_stopping || Busy()) return;
        m_hasDeferred = false;
        Run(m_deferred);
    }

    // A public search's result can simply be ignored (no lobby to leave).
    void CancelListing() {
        m_listed.Cancel();
        m_pending = Op::None;
        m_listDone = true;
        Log(false, L"public lobby search cancelled");
    }

    void Host(LobbyMode mode) {
        if (Busy()) { Log(false, L"create refused: busy"); return; }
        LeaveCurrent();
        ELobbyType type = mode == kModeFriends ? k_ELobbyTypeFriendsOnly : k_ELobbyTypePublic;
        SteamAPICall_t call = SteamMatchmaking()->CreateLobby(type, kMaxMembers);
        if (call == k_uAPICallInvalid) {
            Log(true, L"CreateLobby could not be issued");
            SetState(kLobbyError, kLobbyErrCreate);
            return;
        }
        m_mode = mode;
        m_pending = Op::Creating;
        m_abandon = false;
        m_created.Set(call, this, &Session::OnLobbyCreated);
        SetState(kLobbyCreating);
        Log(false, L"CreateLobby(%s, %d) requested, call %llu", mode == kModeFriends ? L"friends-only" : L"public", kMaxMembers, call);
    }

    // Play with Friends: the friends' current lobbies (their games' lobby ids, each asked for its data). Play with Anyone
    // (user, 2026-10-22): Steam's public lobby list AND the friends' lobbies, so a friend's friends-only game shows there
    // too, and every other mod player's public game with it. Both finish in Tick() once every answer is in (or overdue).
    void Search(LobbyMode mode) {
        if (Busy()) { Log(false, L"search refused: busy"); return; }
        LeaveCurrent();
        m_mode = mode;
        m_rows.Clear();
        m_waiting.clear();
        m_searchStart = GetTickCount();
        m_listDone = mode == kModeFriends;  // (no public list in Play with Friends)
        SetState(kLobbySearching);
        if (!g_modsOk) Log(true, L"search: this game's own mod list could not be read, so every game found will be marked \"other mods\" and refused");
        int n = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
        for (int i = 0; i < n; ++i) {
            FriendGameInfo_t g;
            CSteamID f = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
            if (!SteamFriends()->GetFriendGamePlayed(f, &g) || g.m_gameID.AppID() != kAppId || !g.m_steamIDLobby.IsLobby()) continue;
            uint64_t lobby = g.m_steamIDLobby.ConvertToUint64();
            if (std::find(m_waiting.begin(), m_waiting.end(), lobby) != m_waiting.end()) continue;  // two friends, one game
            if (SteamMatchmaking()->RequestLobbyData(g.m_steamIDLobby)) m_waiting.push_back(lobby);
        }
        Log(false, L"%s search: %u friend lobby(s) asked for data", mode == kModeFriends ? L"friends" : L"public", (unsigned)m_waiting.size());
        if (mode == kModeFriends) return;  // Tick() and OnLobbyDataUpdate finish it
        // Every lobby of this game is one of ours (the stock game makes none); a lobby of another mod build is listed and
        // marked (AddRow), so no "asr_proto" filter: it hid them.
        SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
        SteamAPICall_t call = SteamMatchmaking()->RequestLobbyList();
        if (call == k_uAPICallInvalid) {
            Log(true, L"lobby search could not be issued");
            m_waiting.clear();
            SetState(kLobbyError, kLobbyErrSearch);
            return;
        }
        m_pending = Op::Listing;
        m_listed.Set(call, this, &Session::OnLobbyList);
        Log(false, L"public lobby search requested, call %llu", call);
    }

    // Adds a row for one of our lobbies whose data is known (SearchRows: not full, not our own; a stranger's only with a public
    // seat free, PRIVATE SLOTS; a friend's always; a lobby of another build marked). viaFriend: found through a friend's game.
    void AddRow(CSteamID lobby, bool viaFriend) {
        ISteamMatchmaking* mm = SteamMatchmaking();
        const char* proto = mm->GetLobbyData(lobby, kProtoKey);
        if (!proto || !proto[0]) return;  // not a Steam Multiplayer lobby
        const char* version = mm->GetLobbyData(lobby, "asr_mp");
        LobbyRow row = {};
        row.lobby = lobby.ConvertToUint64();
        CopyName(row.host, mm->GetLobbyData(lobby, "host"));
        row.members = atoi(mm->GetLobbyData(lobby, "members"));
        row.maxMembers = mm->GetLobbyMemberLimit(lobby);
        if (row.maxMembers <= 0) row.maxMembers = kMaxMembers;
        row.hostId = _strtoui64(mm->GetLobbyData(lobby, "hostid"), nullptr, 10);
        row.racing = atoi(mm->GetLobbyData(lobby, "racing")) != 0;
        const char* flag = mm->GetLobbyData(lobby, "flag");
        row.flag = flag && flag[0] ? atoi(flag) : -1;  // (an older build's lobby: no flag)
        row.otherVersion = strcmp(proto, kProto) != 0 || !version || strcmp(version, MOD_VERSION) != 0;
        row.otherMods = !row.otherVersion && !ModsMatch(lobby, false);  // (the console's compatibility column: the join would be refused)
        std::vector<ModEntry> needs;
        if (DecodeMods(mm->GetLobbyData(lobby, kModsKey), needs)) {  // the row's mod icons, each marked by how this game has it
            row.mods = ModStateFor(g_mods, needs, "egg-hangar") | ModStateFor(g_mods, needs, "metal-sonic") << 2;
            row.otherModsRest = row.otherMods && (!g_modsOk || ModsDifferBeyondIcons(g_mods, needs));
        } else {
            row.otherModsRest = row.otherMods;
        }
        // PRIVATE SLOTS (the host's rules): seats kept for friends and invites, as on the console (8 seats, 0-7 private).
        // A stranger's game is listed only while one of its public seats is free; a friend's always (as in Play with Friends).
        int seats = PublicSeats(mm->GetLobbyData(lobby, "rules"));
        bool listed = m_rows.Add(row, viaFriend || m_mode == kModeFriends, seats, m_lobby.ConvertToUint64());
        if (!listed && !viaFriend && m_mode != kModeFriends && row.members < row.maxMembers && row.members >= seats)
            Log(false, L"  %s's game: its %d public seat(s) are taken (the rest are private)", Widen(row.host).c_str(), seats);
    }

    void FinishSearch() {
        SetState(kLobbyResults);
        Log(false, L"search finished: %d game(s)", m_rows.count);
        for (int i = 0; i < m_rows.count; ++i) {
            const LobbyRow& r = m_rows.rows[i];
            Log(false, L"  %s (%d/%d) lobby %llu%s%s%s%s", Widen(r.host).c_str(), r.members, r.maxMembers, r.lobby, r.friendRow ? L", a friend's" : L"",
                r.racing ? L", racing" : L"", r.otherVersion ? L", another mod version (join refused)" : L"", r.otherMods ? L", other mods (join refused)" : L"");
        }
    }

    void Join(uint64_t lobby, const wchar_t* how) {
        if (m_stopping) return;
        if (Busy()) {
            Log(false, L"%s join of %llu refused: busy", how, lobby);
            return;
        }
        if (m_lobby.ConvertToUint64() == lobby) {
            Log(false, L"%s join ignored: already in lobby %llu", how, lobby);
            return;
        }
        LeaveCurrent();
        SteamAPICall_t call = SteamMatchmaking()->JoinLobby(CSteamID(lobby));
        if (call == k_uAPICallInvalid) {
            Log(true, L"JoinLobby(%llu) could not be issued", lobby);
            SetState(kLobbyError, kLobbyErrJoin);
            return;
        }
        m_pending = Op::Joining;
        m_pendingLobby = lobby;
        m_joinPublic = false;  // (set by a public search's row join)
        m_abandon = false;
        m_entered.Set(call, this, &Session::OnLobbyEntered);
        SetState(kLobbyJoining);
        Log(false, L"%s join: JoinLobby(%llu) requested, call %llu", how, lobby, call);
    }

    void Leave() {
        if (m_pending == Op::Listing) CancelListing();
        if (m_pending == Op::Creating || m_pending == Op::Joining) m_abandon = true;  // left when it arrives
        m_waiting.clear();
        LeaveCurrent();
        if (m_state != kLobbyError) SetState(kLobbyIdle);
    }

    void LeaveCurrent() {
        if (!m_lobby.IsValid()) return;
        CloseRaceSessions();
        SteamMatchmaking()->LeaveLobby(m_lobby);
        Log(false, L"left lobby %llu", m_lobby.ConvertToUint64());
        RaceLobbyMods(true, true);  // (outside a lobby every installed mod is in use, as offline)
        m_lobby = CSteamID();
        m_host = CSteamID();
        m_hostGone = CSteamID();
        m_hostChanges = 0;
        m_isHost = false;
        m_progress[0] = -1;
        for (auto& p : m_testProgress) p[0] = -1;
        m_members.clear();
        m_rules = kNoRules;
        SteamFriends()->ClearRichPresence();
        m_presence[0] = 0;
    }

    // ---- host migration (M7). Steam keeps a lobby whose owner left and makes another member its owner
    // (isteammatchmaking.h, GetLobbyOwner), so the lobby goes on with that player as host, as the console
    // planned. The new owner may not be known the moment the old one leaves: Tick keeps asking for a while.
    // The game's own lobby data (version, protocol, mods) stays with the lobby; the new host republishes its
    // name and the rules its panel holds (the old host's, copied there) through the lobby script.
    bool AdoptNewHost() {
        if (!m_lobby.IsValid() || !m_hostGone.IsValid()) return false;
        CSteamID owner = SteamMatchmaking()->GetLobbyOwner(m_lobby);
        if (!owner.IsValid() || owner == m_hostGone) return false;
        m_host = owner;
        m_hostGone = CSteamID();
        ++m_hostChanges;
        m_isHost = owner == SteamUser()->GetSteamID();
        if (m_isHost) {
            Log(true, L"the host left; this game is the lobby's host now (handover %d)", m_hostChanges);
            PublishHostData();
            m_localRules = m_rules;  // the rules as the lobby has them (the old host's), not ones this game hosted with earlier
            WriteRules();
        } else {
            Log(true, L"the host left; %s is the lobby's host now (handover %d)", PersonaOf(owner).c_str(), m_hostChanges);
        }
        RefreshLobby();
        return true;
    }

    // The host's lobby data: the mod version and protocol every joiner checks, the name searchers see, the mod list.
    void PublishHostData() {
        ISteamMatchmaking* mm = SteamMatchmaking();
        mm->SetLobbyData(m_lobby, "asr_mp", MOD_VERSION);
        mm->SetLobbyData(m_lobby, kProtoKey, kProto);
        mm->SetLobbyData(m_lobby, "host", SteamFriends()->GetPersonaName());
        char hostId[24];
        sprintf_s(hostId, "%llu", SteamUser()->GetSteamID().ConvertToUint64());
        mm->SetLobbyData(m_lobby, "hostid", hostId);  // (the search rows' VIEW PROFILE)
        char flag[16];
        sprintf_s(flag, "%ld", (long)InterlockedCompareExchange(&g_localFlag, 0, 0));
        mm->SetLobbyData(m_lobby, "flag", flag);  // (the search rows' flag: the host's license nationality, as the console's)
        WriteRacing();
        char test[8];
        sprintf_s(test, "%d", g_testPlayers);
        mm->SetLobbyData(m_lobby, "test", test);
        mm->SetLobbyData(m_lobby, "testracer", g_testSameRacer ? "same" : "");  // (debug: every test player takes the host's racer)
        std::vector<ModEntry> required;
        const char* had = mm->GetLobbyData(m_lobby, kModsKey);
        if (*had && DecodeMods(had, required)) {
            // A handover: the lobby keeps the mods its first host chose (this game joined with them; its own off ones stay off).
        } else if (!g_modsOk || !mm->SetLobbyData(m_lobby, kModsKey, EncodeMods(required = RequiredMods(g_mods, HostModsOff())).c_str())) {
            Log(true, L"could not publish this game's mod list; players will not be able to join");
        }
        UseLobbyMods(required);
    }

    // ENABLE MODIFICATIONS: this lobby uses a toggleable mod only if its required list names it (the joiner's check made sure
    // every game in it has that mod; a toggleable mod it leaves out stays off on every game).
    static void UseLobbyMods(const std::vector<ModEntry>& required) {
        RaceLobbyMods(ModListed(required, "egg-hangar"), ModListed(required, "metal-sonic"));
    }

    // ---- KICK (M7). Steam lobbies have no kick call: the host names the member in lobby data "kick" (its game leaves when it
    // reads it, with the console's "You have been kicked from the session.") and adds it to "banned", so a join of this lobby
    // refuses it (OnLobbyEntered). Both stay with the lobby through a host handover. The others see the usual "left" line.
    void Kick(uint64_t id) {
        if (!m_lobby.IsValid() || m_state != kLobbyInLobby || !m_isHost) {
            Log(false, L"kick: not hosting a lobby; ignored");
            return;
        }
        auto it = std::find_if(m_members.begin(), m_members.end(), [&](const LobbyMember& m) { return m.id == id; });
        if (it == m_members.end() || it->isMe || IsTestPlayer(id)) {
            Log(false, L"kick: %llu is not another player in this lobby; ignored", id);
            return;
        }
        ISteamMatchmaking* mm = SteamMatchmaking();
        char one[24];
        sprintf_s(one, "%llu", id);
        std::string banned = mm->GetLobbyData(m_lobby, "banned");
        if (!IdListHas(banned.c_str(), id)) {
            if (!banned.empty()) banned += ',';
            banned += one;
        }
        if (!mm->SetLobbyData(m_lobby, "banned", banned.c_str()) || !mm->SetLobbyData(m_lobby, "kick", one)) {
            Log(true, L"kick: Steam refused the lobby data; %s stays in the lobby", PersonaOf(CSteamID(id)).c_str());
            return;
        }
        Log(true, L"kicked %s out of the lobby; it can't rejoin this lobby", PersonaOf(CSteamID(id)).c_str());
    }

    // A guest's game reads its own id in the host's "kick" or "banned" (on every lobby data update; two kicks close together may
    // reach it as one update with the second "kick" value): leave, with the kicked notice.
    bool KickedFromLobby() {
        if (m_isHost || !m_lobby.IsValid()) return false;
        uint64_t me = SteamUser()->GetSteamID().ConvertToUint64();
        ISteamMatchmaking* mm = SteamMatchmaking();
        if (ParseSteamId(mm->GetLobbyData(m_lobby, "kick")) != me && !IdListHas(mm->GetLobbyData(m_lobby, "banned"), me)) return false;
        Log(true, L"the host (%s) kicked this player out of the lobby", PersonaOf(m_host).c_str());
        LeaveCurrent();
        SetState(kLobbyError, kLobbyErrKicked);
        return true;
    }

    void PollNewHost() {
        if (!m_hostGone.IsValid() || !m_lobby.IsValid()) return;
        if (AdoptNewHost()) { Publish(); return; }
        if (GetTickCount() - m_hostGoneAt < 5000) return;
        Log(true, L"the host left and Steam named no new host in 5 s; leaving the lobby");
        LeaveCurrent();
        SetState(kLobbyError, kLobbyErrGone);
        Publish();
    }

    // ---- member data and rules
    static constexpr LobbyRules kNoRules = {-1, -1, -1, -1, -1, -1};

    // Our racer and ready flag, as Steam member data of the current lobby.
    void WriteLocalData() {
        if (!m_lobby.IsValid()) return;
        char v[16];
        sprintf_s(v, "%d", m_localRacer);
        SteamMatchmaking()->SetLobbyMemberData(m_lobby, "racer", v);
        SteamMatchmaking()->SetLobbyMemberData(m_lobby, "ready", m_localReady ? "1" : "0");
        SteamMatchmaking()->SetLobbyMemberData(m_lobby, "net", m_netReady ? "1" : "0");
        int32_t input = (int32_t)InterlockedCompareExchange(&g_localInput, 0, 0);
        if (input != m_postedInput)
            Log(false, L"lobby: this player drives with the %s", input == kInputKeyboard ? L"keyboard" : input == kInputController ? L"controller" : L"(no device yet)");
        m_postedInput = input;
        SteamMatchmaking()->SetLobbyMemberData(m_lobby, "inp", InputData(m_postedInput));
        for (LobbyMember& m : m_members)
            if (m.isMe) m.input = m_postedInput;
    }
    int32_t m_postedInput = kInputUnknown;

    // Race connections need this game's Steam certificate, which can come minutes after the game
    // starts ("Cert request failed ... not logged into Steam" until then; seen 2026-10-20). The
    // host's Start waits until every member has it (member data "net").
    void NoteAuth(ESteamNetworkingAvailability a) {
        bool ready = a == k_ESteamNetworkingAvailability_Current;
        if (ready == m_netReady) return;
        m_netReady = ready;
        Log(false, ready ? L"race connections: ready (Steam certificate received)" : L"race connections: not ready (status %d)", (int)a);
        for (LobbyMember& m : m_members)
            if (m.isMe) m.net = ready;
        WriteLocalData();
    }
    STEAM_CALLBACK(Session, OnAuthStatus, SteamNetAuthenticationStatus_t) { NoteAuth(pParam->m_eAvail); }
    bool m_netReady = false;

    // Steam offline (M7; the console's "sign-in status changed" / "connection lost" family): Steam losing its connection to the
    // Steam servers ends the lobby, search or join this game is in (the lobby is gone for the others too), with its own notice;
    // while disconnected, Run refuses new ones. A race in progress ends through its own "connection lost" handling.
    STEAM_CALLBACK(Session, OnSteamDisconnected, SteamServersDisconnected_t) {
        if (m_stopping) return;
        m_online = false;
        bool inGame = m_lobby.IsValid() || Busy() || m_state == kLobbySearching || m_state == kLobbyJoining || m_state == kLobbyCreating;
        Log(true, L"Steam lost its connection to the Steam servers (EResult %d)%s", (int)pParam->m_eResult, inGame ? L": the online game ends" : L"");
        if (inGame) {
            m_hasDeferred = false;  // (a command waiting behind the abandoned create/join would run when its result comes in)
            Leave();
            SetState(kLobbyError, kLobbyErrSteamLost);
        }
        Publish();
    }
    STEAM_CALLBACK(Session, OnSteamConnected, SteamServersConnected_t) {
        if (m_stopping || m_online) return;
        m_online = true;
        Log(true, L"Steam is connected to the Steam servers again");
    }
    STEAM_CALLBACK(Session, OnSteamConnectFailure, SteamServerConnectFailure_t) {
        if (m_stopping) return;
        Log(false, L"Steam could not reconnect to the Steam servers (EResult %d%s)", (int)pParam->m_eResult, pParam->m_bStillRetrying ? L", still retrying" : L"");
    }
    bool m_online = true;
    DWORD m_authAsked = GetTickCount();  // Start asks once; Tick asks again every 30 s while not ready

    // This player's race progress, as member data "pos" = "place,lap,progress,laps,raceId,results" (M7: the lobby's race bar
    // for a player who joined while the race runs; results 1 = this player's race is over and it views the results screen, the
    // console's VIEWING RESULTS); place -1 = not racing (written once the results are left).
    void WriteProgress() {
        if (!m_lobby.IsValid()) return;
        char v[96];
        if (m_progress[0] < 0) v[0] = 0;
        else sprintf_s(v, "%d,%d,%d,%d,%u,%d", m_progress[0], m_progress[1], m_progress[2], m_progress[3], (uint32_t)m_progress[4], m_progress[5] ? 1 : 0);
        if (strcmp(SteamMatchmaking()->GetLobbyMemberData(m_lobby, SteamUser()->GetSteamID(), "pos"), v) != 0)
            SteamMatchmaking()->SetLobbyMemberData(m_lobby, "pos", v);
        if (m_isHost) WriteRacing();
        WritePresence();
    }

    // Host: lobby data "racing" = 1 while its race runs (the search list's STATUS column: GAME, else LOBBY, as the console's).
    void WriteRacing() {
        const char* v = m_progress[0] >= 0 ? "1" : "0";
        if (strcmp(SteamMatchmaking()->GetLobbyData(m_lobby, "racing"), v) != 0) SteamMatchmaking()->SetLobbyData(m_lobby, "racing", v);
    }

    // Host: its test players' race progress (they have no member data), as lobby data "testpos" =
    // "n:place,lap,progress,laps,raceId;..." for each test player n (1-based) racing.
    void WriteTestProgress() {
        if (!m_lobby.IsValid() || !m_isHost) return;
        std::string v;
        char one[96];
        for (int n = 0; n < kMaxMembers - 1; ++n) {
            const int32_t* p = m_testProgress[n];
            if (p[0] < 0) continue;
            sprintf_s(one, "%s%d:%d,%d,%d,%d,%u", v.empty() ? "" : ";", n + 1, p[0], p[1], p[2], p[3], (uint32_t)p[4]);
            v += one;
        }
        if (strcmp(SteamMatchmaking()->GetLobbyData(m_lobby, "testpos"), v.c_str()) != 0) SteamMatchmaking()->SetLobbyData(m_lobby, "testpos", v.c_str());
    }

    // Test player n's entry of "testpos" ("" when it isn't racing).
    static void TestProgressOf(const char* all, int n, LobbyMember& m) {
        char key[8], one[96] = {};
        sprintf_s(key, "%d:", n);
        for (const char* s = all; s && *s;) {
            const char* end = strchr(s, ';');
            size_t len = end ? (size_t)(end - s) : strlen(s);
            if (len > strlen(key) && !strncmp(s, key, strlen(key)) && len - strlen(key) < sizeof(one)) {
                memcpy(one, s + strlen(key), len - strlen(key));
                break;
            }
            s = end ? end + 1 : nullptr;
        }
        ReadProgress(one, m);
    }

    static void ReadProgress(const char* s, LobbyMember& m) {
        int place = -1, lap = 0, progress = 0, laps = 0, results = 0;
        unsigned race = 0;
        char tail = 0;
        int n = s && *s ? sscanf_s(s, "%d,%d,%d,%d,%u,%d%c", &place, &lap, &progress, &laps, &race, &results, &tail, 1) : 0;
        bool ok = (n == 5 || n == 6) && place >= 0 && place < 8 && lap >= 0 && lap <= 99 && progress >= 0 && progress <= 1000 && laps >= 1 &&
                  laps <= 9 && race != 0;  // (5 fields: a record without the results flag)
        m.place = ok ? place : -1;
        m.lap = ok ? lap : 0;
        m.progress = ok ? progress : 0;
        m.laps = ok ? laps : 0;
        m.raceId = ok ? race : 0;
        m.results = ok && n == 6 && results == 1 ? 1 : 0;
    }

    // The host's rules, as lobby data "rules" = "course,laps,privateSlots,ai,items,catchup".
    void WriteRules() {
        if (!m_lobby.IsValid() || !m_isHost) return;
        const LobbyRules& r = m_localRules;
        if (r.course < 0) return;  // the host's menu has not published its rules yet
        char v[96];
        sprintf_s(v, "%d,%d,%d,%d,%d,%d", r.course, r.laps, r.privateSlots, r.ai, r.items, r.catchup);
        if (strcmp(SteamMatchmaking()->GetLobbyData(m_lobby, "rules"), v) != 0) SteamMatchmaking()->SetLobbyData(m_lobby, "rules", v);
    }

    static int32_t DataInt(const char* s, int32_t none) {
        if (!s || !*s) return none;
        char* end = nullptr;
        long v = strtol(s, &end, 10);
        return (end && *end == 0 && v >= -1 && v < 1000) ? (int32_t)v : none;
    }

    // Re-reads every member's racer/ready and the host's rules (no Steam requests, no logging).
    void ReadLobbyData() {
        ISteamMatchmaking* mm = SteamMatchmaking();
        for (LobbyMember& m : m_members) {
            if (IsTestPlayer(m.id)) continue;  // (SyncTestPlayers below)
            m.racer = DataInt(mm->GetLobbyMemberData(m_lobby, CSteamID(m.id), "racer"), -1);
            m.ready = DataInt(mm->GetLobbyMemberData(m_lobby, CSteamID(m.id), "ready"), 0) == 1;
            m.net = m.isMe ? m_netReady : DataInt(mm->GetLobbyMemberData(m_lobby, CSteamID(m.id), "net"), 0) == 1;
            ReadProgress(mm->GetLobbyMemberData(m_lobby, CSteamID(m.id), "pos"), m);
            const char* inp = mm->GetLobbyMemberData(m_lobby, CSteamID(m.id), "inp");
            m.input = m.isMe ? m_postedInput : !strcmp(inp, "k") ? kInputKeyboard : !strcmp(inp, "c") ? kInputController : kInputUnknown;
        }
        int v[kRuleCount];
        char tail = 0;
        const char* s = mm->GetLobbyData(m_lobby, "rules");
        bool ok = sscanf_s(s, "%d,%d,%d,%d,%d,%d%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail, 1) == kRuleCount;
        for (int i = 0; ok && i < kRuleCount; ++i) ok = v[i] >= 0 && v[i] <= 255;
        m_rules = ok ? LobbyRules{v[0], v[1], v[2], v[3], v[4], v[5]} : kNoRules;
        SyncTestPlayers();
        OrderBySeats();
    }

    // Seats (badges 1P..8P) stay as they are while a player stays: the host first, then the host's lobby data
    // "seats" (the members' ids in seat order), then anyone not in it yet (a newcomer: the next seat; by id, so
    // every PC agrees). The host publishes the result. Without this a joiner with a lower Steam ID (or any real
    // joiner, ahead of the test players) took a seat in the middle and renumbered the players after it.
    void OrderBySeats() {
        std::vector<uint64_t> seats;
        for (const char* s = SteamMatchmaking()->GetLobbyData(m_lobby, "seats"); s && *s;) {
            char* end = nullptr;
            unsigned long long id = strtoull(s, &end, 10);
            if (end == s) break;
            if (id) seats.push_back(id);
            s = *end == ',' ? end + 1 : end;
        }
        auto key = [&](const LobbyMember& m) -> std::pair<int, uint64_t> {
            if (m.isHost) return {-1, 0};
            auto it = std::find(seats.begin(), seats.end(), m.id);
            return it != seats.end() ? std::make_pair((int)(it - seats.begin()), (uint64_t)0) : std::make_pair(INT_MAX, m.id);
        };
        std::stable_sort(m_members.begin(), m_members.end(), [&](const LobbyMember& a, const LobbyMember& b) { return key(a) < key(b); });
        if (!m_isHost) return;
        std::string v;
        char one[24];
        for (const LobbyMember& m : m_members) {
            sprintf_s(one, "%s%llu", v.empty() ? "" : ",", m.id);
            v += one;
        }
        if (strcmp(SteamMatchmaking()->GetLobbyData(m_lobby, "seats"), v.c_str()) != 0) SteamMatchmaking()->SetLobbyData(m_lobby, "seats", v.c_str());
    }

    // Test players (the host's config.ini [Debug] TestPlayers, lobby data "test"): pretend members after the real
    // ones, the same on every PC (ids, names, racers: the lowest AI racers nobody picked), always ready.
    void SyncTestPlayers() {
        m_members.erase(std::remove_if(m_members.begin(), m_members.end(), [](const LobbyMember& m) { return IsTestPlayer(m.id); }),
                        m_members.end());
        int n = DataInt(SteamMatchmaking()->GetLobbyData(m_lobby, "test"), 0);
        const char* progress = SteamMatchmaking()->GetLobbyData(m_lobby, "testpos");
        int taken[kMaxMembers], takenCount = 0, hostRacer = -1;
        for (const LobbyMember& m : m_members) {
            if (m.racer >= 0 && takenCount < kMaxMembers) taken[takenCount++] = m.racer;
            if (m.isHost) hostRacer = m.racer;
        }
        // Debug (the host's config.ini [Debug] TestPlayerRacer=same, lobby data "testracer"): every test player takes the host's
        // racer, for races with one racer several times (the consoles allowed each racer once; this checks what the PC does).
        bool same = strcmp(SteamMatchmaking()->GetLobbyData(m_lobby, "testracer"), "same") == 0;
        for (int k = 0; k < n && m_members.size() < (size_t)kMaxMembers; ++k) {
            LobbyMember m = {};
            m.id = kTestPlayerBase + k + 1;
            m.input = kInputUnknown;  // (the Drift+ rule and the majority count only real players)
            sprintf_s(m.name, "Test Player %d", k + 1);
            m.racer = same ? hostRacer : RaceTestPlayerRacer(k, taken, takenCount);
            m.ready = m.racer >= 0;
            m.net = 1;
            TestProgressOf(progress, k + 1, m);  // (place -1 when not racing)
            m_members.push_back(m);
        }
    }

    bool IsMember(uint64_t id) const {
        if (!m_lobby.IsValid() || !id || IsTestPlayer(id)) return false;
        for (const LobbyMember& m : m_members)
            if (m.id == id && !m.isMe) return true;
        return false;
    }

    // ---- P2P sessions (race messages)
    void NotePeer(uint64_t id) {
        if (std::find(m_peers.begin(), m_peers.end(), id) == m_peers.end()) m_peers.push_back(id);
    }

    // Before leaving a lobby or shutting Steam down: no session stays open behind us.
    void CloseRaceSessions() {
        for (uint64_t id : m_peers) {
            SteamNetworkingIdentity who;
            who.SetSteamID64(id);
            SteamNetworkingMessages()->CloseSessionWithUser(who);
        }
        if (!m_peers.empty()) Log(false, L"closed %u race connection(s)", (unsigned)m_peers.size());
        m_peers.clear();
        ClearRaceQueues();
    }

    // Another game wants to talk: only members of our lobby are accepted.
    STEAM_CALLBACK(Session, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t) {
        uint64_t from = pParam->m_identityRemote.GetSteamID64();
        if (!m_stopping && IsMember(from)) {
            SteamNetworkingMessages()->AcceptSessionWithUser(pParam->m_identityRemote);
            NotePeer(from);
            Log(false, L"race connection from %s accepted", PersonaOf(CSteamID(from)).c_str());
        } else {
            Log(false, L"race connection from %llu refused (not in our lobby)", from);
        }
    }

    STEAM_CALLBACK(Session, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t) {
        const SteamNetConnectionInfo_t& info = pParam->m_info;
        char addr[SteamNetworkingIPAddr::k_cchMaxString] = "";
        info.m_addrRemote.ToString(addr, sizeof(addr), true);
        Log(true, L"race connection to %llu failed: %d %hs", info.m_identityRemote.GetSteamID64(), (int)info.m_eEndReason, info.m_szEndDebug);
        Log(false, L"  (state %d, remote address %hs, relay POP %u, flags 0x%x, %hs)", (int)info.m_eState, addr, info.m_idPOPRelay,
            info.m_nFlags, info.m_szConnectionDescription);
    }

    // Valve's relay network (race connections between PCs that can't reach each other directly).
    STEAM_CALLBACK(Session, OnRelayStatus, SteamRelayNetworkStatus_t) {
        if (pParam->m_eAvail == m_relayAvail) return;
        m_relayAvail = pParam->m_eAvail;
        Log(false, L"relay network: %d (config %d, any relay %d) %hs", (int)pParam->m_eAvail, (int)pParam->m_eAvailNetworkConfig,
            (int)pParam->m_eAvailAnyRelay, pParam->m_debugMsg);
    }
    ESteamNetworkingAvailability m_relayAvail = k_ESteamNetworkingAvailability_Unknown;

    // Steam's overlay over the game: the lobby's ready check goes while it is open (natives.cpp kNatFrame).
    STEAM_CALLBACK(Session, OnOverlay, GameOverlayActivated_t) {
        InterlockedExchange(&g_overlayOpen, pParam->m_bActive ? 1 : 0);
        Log(false, L"Steam overlay %s", pParam->m_bActive ? L"opened" : L"closed");
    }

    void LeaveAbandoned(uint64_t lobby) {
        SteamMatchmaking()->LeaveLobby(CSteamID(lobby));
        Log(false, L"left lobby %llu (%s)", lobby, m_stopping ? L"stopping" : L"cancelled");
    }

    // Lobby members, rich presence, and (host) the member count searchers see.
    void RefreshLobby() {
        ISteamMatchmaking* mm = SteamMatchmaking();
        int n = mm->GetNumLobbyMembers(m_lobby);
        CSteamID me = SteamUser()->GetSteamID();
        m_members.clear();
        Log(false, L"lobby %llu: %d member(s), host %s", m_lobby.ConvertToUint64(), n, PersonaOf(m_host).c_str());
        for (int i = 0; i < n && i < kMaxMembers; ++i) {
            CSteamID id = mm->GetLobbyMemberByIndex(m_lobby, i);
            LobbyMember m = {};
            m.id = id.ConvertToUint64();
            m.input = kInputUnknown;
            CopyName(m.name, SteamFriends()->GetFriendPersonaName(id));
            m.isHost = id == m_host;
            m.isMe = id == me;
            // The host is always row 0.
            if (m.isHost) m_members.insert(m_members.begin(), m);
            else m_members.push_back(m);
            Log(false, L"  %s%s (%llu)", Widen(m.name).c_str(), m.isHost ? L" [host]" : L"", m.id);
        }
        // The others by Steam ID: Steam's member order may differ between PCs, and the lobby bars' badges
        // (and the host's race contract, which numbers the players in this order) must be the same everywhere.
        // ReadLobbyData then puts them in the host's seat order (OrderBySeats).
        size_t first = !m_members.empty() && m_members[0].isHost ? 1 : 0;
        std::sort(m_members.begin() + first, m_members.end(), [](const LobbyMember& a, const LobbyMember& b) { return a.id < b.id; });
        ReadLobbyData();
        char num[16];
        sprintf_s(num, "%d", (int)m_members.size());  // (with any test players: searchers see the seats in use)
        if (m_isHost && strcmp(mm->GetLobbyData(m_lobby, "members"), num) != 0) mm->SetLobbyData(m_lobby, "members", num);
        char id[32];
        sprintf_s(id, "%llu", m_lobby.ConvertToUint64());
        m_presenceMembers = n;
        WritePresence();
        SteamFriends()->SetRichPresence("connect", (std::string("+connect_lobby ") + id).c_str());
    }

    // Rich presence "status" (the friends list): the console's "In a network lobby" / "In a network game" (dialogs 4BA9303C /
    // 957C401A, re\m8-audit A5) with the lobby's seats; written when it changes (members, or this game's race starting or ending).
    void WritePresence() {
        if (!m_lobby.IsValid()) return;
        char status[128];
        sprintf_s(status, "%s (%d/%d)", m_progress[0] >= 0 ? "In a network game" : "In a network lobby", m_presenceMembers, kMaxMembers);
        if (strcmp(status, m_presence) == 0) return;
        strcpy_s(m_presence, status);
        SteamFriends()->SetRichPresence("status", status);
    }
    char m_presence[128] = "";
    int m_presenceMembers = 0;

    // Refuses a lobby whose host runs a different set of mods, naming each difference.
    bool CheckHostMods() {
        if (!ModsMatch(m_lobby, true)) return false;
        std::vector<ModEntry> needs;  // (ModsMatch decoded it already)
        DecodeMods(SteamMatchmaking()->GetLobbyData(m_lobby, kModsKey), needs);
        UseLobbyMods(needs);  // this game uses what the lobby lists, and only that
        return true;
    }

    // This game's mod set against the host's (lobby data kModsKey): at a join (verbose: the differences go to the log), and
    // for the search rows (quiet; the lobby's data is in from the search).
    bool ModsMatch(CSteamID lobby, bool verbose) {
        std::wstring host = verbose ? Widen(SteamMatchmaking()->GetLobbyData(lobby, "host")) : L"";
        if (verbose && host.empty()) host = PersonaOf(SteamMatchmaking()->GetLobbyOwner(lobby));
        std::vector<ModEntry> theirs;
        if (verbose) m_refusedMods = kRefusedOther;  // (what the refusal pop-up names; refined below once both lists are read)
        if (!DecodeMods(SteamMatchmaking()->GetLobbyData(lobby, kModsKey), theirs)) {
            if (verbose)
                Log(true, L"can't join %s's game: the host's mod list is missing or unreadable (the host may run an older Steam Multiplayer)", host.c_str());
            return false;
        }
        if (!g_modsOk) {
            if (verbose) Log(true, L"can't join %s's game: this game's own mod list could not be read", host.c_str());
            return false;
        }
        std::vector<std::string> diff = DescribeModDifferences(g_mods, theirs);
        if (diff.empty()) {
            if (verbose) Log(false, L"mod set matches the host (%u mods needed, %u installed here)", (unsigned)theirs.size(), (unsigned)g_mods.size());
            if (verbose) m_refusedMods = 0;
            return true;
        }
        if (verbose) {
            m_refusedMods = RefusedMods(g_mods, theirs);
            Log(true, L"can't join %s's game: your mods are different from the host's (%u difference(s)):", host.c_str(), (unsigned)diff.size());
            for (const std::string& d : diff) Log(true, L"  %s", Widen(d.c_str()).c_str());
        }
        return false;
    }

    void OnLobbyCreated(LobbyCreated_t* r, bool ioFailure) {
        m_pending = Op::None;
        if (ioFailure || r->m_eResult != k_EResultOK) {
            if (ioFailure) Log(true, L"lobby creation FAILED (I/O failure)");
            else Log(true, L"lobby creation FAILED (EResult %d)", (int)r->m_eResult);
            if (!m_stopping && !m_abandon) SetState(kLobbyError, kLobbyErrCreate);  // nobody waits for an abandoned one
        } else if (m_stopping || m_abandon) {
            LeaveAbandoned(r->m_ulSteamIDLobby);
        } else {
            m_lobby = CSteamID(r->m_ulSteamIDLobby);
            m_host = SteamUser()->GetSteamID();
            m_hostGone = CSteamID();
            m_hostChanges = 0;
            m_isHost = true;
            PublishHostData();
            Log(true, L"hosting %s lobby %llu", m_mode == kModeFriends ? L"friends-only" : L"public", m_lobby.ConvertToUint64());
            SetState(kLobbyInLobby);
            WriteLocalData();
            WriteRules();
            RefreshLobby();
        }
        RunDeferred();
        Publish();
    }

    void OnLobbyEntered(LobbyEnter_t* r, bool ioFailure) {
        m_pending = Op::None;
        const uint64_t target = m_pendingLobby;
        m_pendingLobby = 0;
        if (ioFailure || r->m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            if (ioFailure) Log(true, L"join of lobby %llu FAILED (I/O failure)", target);
            else Log(true, L"join of lobby %llu FAILED (response %u)", target, r->m_EChatRoomEnterResponse);
            // (nobody waits for an abandoned one; a lobby that no longer exists gets the console's "session is no longer available")
            if (!m_stopping && !m_abandon)
                SetState(kLobbyError, !ioFailure && r->m_EChatRoomEnterResponse == k_EChatRoomEnterResponseDoesntExist ? kLobbyErrJoinGone : kLobbyErrJoin);
        } else if (m_stopping || m_abandon) {
            LeaveAbandoned(target);
        } else {
            m_lobby = CSteamID(target);
            m_host = SteamMatchmaking()->GetLobbyOwner(m_lobby);
            m_hostGone = CSteamID();
            m_hostChanges = 0;
            m_isHost = m_host == SteamUser()->GetSteamID();  // (rare: joined just as the owner left: Steam made this game the owner)
            if (m_pendingJoin == target) m_pendingJoin = 0;  // (entered another way: nothing left to take)
            const char* version = SteamMatchmaking()->GetLobbyData(m_lobby, "asr_mp");
            Log(true, L"JOINED lobby %llu (host mod version \"%hs\")", target, version);
            if (strcmp(version, MOD_VERSION) != 0) {
                Log(true, L"can't join %s's game: the host runs Steam Multiplayer %hs, you run %hs", PersonaOf(m_host).c_str(),
                    version, MOD_VERSION);
                LeaveCurrent();
                SetState(kLobbyError, kLobbyErrVersion);
            } else if (!CheckHostMods()) {
                LeaveCurrent();
                SetState(kLobbyError, kLobbyErrMods);
            } else if (!m_isHost && IdListHas(SteamMatchmaking()->GetLobbyData(m_lobby, "banned"), SteamUser()->GetSteamID().ConvertToUint64())) {
                // KICK: the host kicked this player out of this lobby earlier; it stays out.
                Log(true, L"can't join %s's game: the host kicked this player out of it", PersonaOf(m_host).c_str());
                LeaveCurrent();
                SetState(kLobbyError, kLobbyErrKicked);
            } else if (m_joinPublic && SteamMatchmaking()->GetNumLobbyMembers(m_lobby) > PublicSeats(SteamMatchmaking()->GetLobbyData(m_lobby, "rules"))) {
                // The last public seat went while we joined: the rest are kept for the host's friends.
                Log(true, L"can't join %s's game: its public seats are taken (the rest are private)", PersonaOf(m_host).c_str());
                LeaveCurrent();
                SetState(kLobbyError, kLobbyErrJoin);
            } else {
                SetState(kLobbyInLobby);
                WriteLocalData();
                RefreshLobby();
                if (m_isHost) {  // the owner of a lobby whose host had just left: this game hosts it from here on
                    Log(true, L"joined lobby %llu as its owner (the host had just left): this game hosts it", target);
                    PublishHostData();
                    m_localRules = m_rules;
                    WriteRules();
                    RefreshLobby();
                }
            }
        }
        RunDeferred();
        Publish();
    }

    void OnLobbyList(LobbyMatchList_t* r, bool ioFailure) {
        m_pending = Op::None;
        if (ioFailure) {
            Log(true, L"lobby search FAILED (I/O failure)");
            SetState(kLobbyError, kLobbyErrSearch);
        } else if (m_state == kLobbySearching) {
            for (uint32 i = 0; i < r->m_nLobbiesMatching; ++i) AddRow(SteamMatchmaking()->GetLobbyByIndex(i), false);
            m_listDone = true;
            if (m_waiting.empty()) FinishSearch();  // else Tick() finishes it with the friends' answers (or at their time limit)
            else Log(false, L"public list in (%u lobby(s)); waiting for %u friend lobby(s)", r->m_nLobbiesMatching, (unsigned)m_waiting.size());
        }
        Publish();
    }

    STEAM_CALLBACK(Session, OnLobbyDataUpdate, LobbyDataUpdate_t) {
        if (m_stopping) return;
        if (m_lobby.IsValid() && pParam->m_ulSteamIDLobby == m_lobby.ConvertToUint64()) {  // racer, ready or rules changed
            if (!KickedFromLobby()) ReadLobbyData();  // (KICK: the host named this player; the lobby is left)
            Publish();
            return;
        }
        if (m_state == kLobbySearching && pParam->m_ulSteamIDLobby == pParam->m_ulSteamIDMember) {
            for (size_t i = 0; i < m_waiting.size(); ++i) {
                if (m_waiting[i] != pParam->m_ulSteamIDLobby) continue;
                m_waiting.erase(m_waiting.begin() + i);
                if (pParam->m_bSuccess) AddRow(CSteamID(pParam->m_ulSteamIDLobby), true);
                break;
            }
            Tick();
            Publish();
        }
    }

    // A Steam "Join Game" / accepted invite is only remembered (the newest wins): the menus take it when
    // the player can go (re\m6-join\FINDINGS.md): from the main menu once a license is picked, from the
    // online search, or from a lobby (leaving it first). One for the lobby we are in is dropped.
    void RememberJoin(uint64_t lobby) {
        if (!CSteamID(lobby).IsLobby()) { Log(false, L"the request does not name a lobby; ignored"); return; }
        if (m_lobby.IsValid() && m_lobby.ConvertToUint64() == lobby) { Log(false, L"already in that lobby"); return; }
        m_pendingJoin = lobby;
        Log(false, L"Steam join of lobby %llu waits for the menus", lobby);
    }

    // Steam friends list "Join Game" / accepted lobby invite while the game runs.
    STEAM_CALLBACK(Session, OnGameLobbyJoinRequested, GameLobbyJoinRequested_t) {
        if (m_stopping) return;
        Log(true, L"warm join request (GameLobbyJoinRequested) for lobby %llu via %s", pParam->m_steamIDLobby.ConvertToUint64(),
            PersonaOf(pParam->m_steamIDFriend).c_str());
        RememberJoin(pParam->m_steamIDLobby.ConvertToUint64());
        Publish();
    }

    // Same, when Steam routes the join through our rich presence "connect" string.
    STEAM_CALLBACK(Session, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t) {
        if (m_stopping) return;
        char connect[k_cchMaxRichPresenceValueLength + 1] = {};
        memcpy(connect, pParam->m_rgchConnect, k_cchMaxRichPresenceValueLength);
        Log(true, L"warm join request (GameRichPresenceJoinRequested) connect=\"%hs\" via %s", connect,
            PersonaOf(pParam->m_steamIDFriend).c_str());
        uint64_t lobby = LobbyFromConnectString(connect);
        if (lobby) RememberJoin(lobby);
        else Log(false, L"connect string is not \"+connect_lobby <lobby id>\"; ignored");
        Publish();
    }

    STEAM_CALLBACK(Session, OnLobbyChatUpdate, LobbyChatUpdate_t) {
        if (m_stopping || pParam->m_ulSteamIDLobby != m_lobby.ConvertToUint64()) return;
        CSteamID who(pParam->m_ulSteamIDUserChanged);
        uint32 s = pParam->m_rgfChatMemberStateChange;
        bool entered = (s & k_EChatMemberStateChangeEntered) != 0;
        Log(true, L"%s %s the lobby", PersonaOf(who).c_str(), entered ? L"entered" : L"left");
        if (!entered && !m_isHost && who == m_host) {
            // The lobby goes on with the member Steam makes its owner (host migration); until Steam names one
            // (usually at once), the list has no host row.
            m_hostGone = who;
            m_hostGoneAt = GetTickCount();
            if (!AdoptNewHost()) RefreshLobby();
        } else {
            RefreshLobby();
        }
        Publish();
    }

public:
    // Copies the session's view into the mailbox snapshot (no-op if unchanged).
    void Publish() {
        LobbySnapshot s = {};
        s.state = m_state;
        s.error = m_error;
        s.refusedMods = m_refusedMods;
        s.mode = m_mode;
        s.isHost = m_isHost;
        s.rowCount = m_rows.count;
        for (int i = 0; i < m_rows.count; ++i) s.rows[i] = m_rows.rows[i];
        s.memberCount = (int32_t)m_members.size();
        uint32_t now = GetTickCount() | 1;
        for (size_t i = 0; i < m_members.size() && i < kMaxMembers; ++i) {
            s.members[i] = m_members[i];
            // Voice chat: the bar's voice icon (this player's while its own voice goes out)
            s.members[i].talking = (m_members[i].isMe ? m_voice.Transmitting(now) : m_voice.Talking(m_members[i].id, now)) ? 1 : 0;
            s.members[i].muted = !m_members[i].isMe && m_voice.Muted(m_members[i].id) ? 1 : 0;
        }
        s.rules = m_rules;
        s.pendingJoin = m_pendingJoin;
        s.hostChanges = m_hostChanges;
        EnterCriticalSection(&g_boxLock);
        s.revision = g_snap.revision;
        // Queued commands have already set the state they lead to; publishing older state now
        // would undo that. The worker publishes again after applying them.
        if (g_cmdCount == 0 && memcmp(&s, &g_snap, sizeof(s)) != 0) {
            s.revision = g_snap.revision + 1;
            g_snap = s;
        }
        LeaveCriticalSection(&g_boxLock);
    }

private:
    uint64_t m_coldJoin;
    LobbyState m_state = kLobbyIdle;
    LobbyError m_error = kLobbyErrNone;
    int32_t m_refusedMods = 0;  // the snapshot's refusedMods (ModsMatch at a join)
    LobbyMode m_mode = kModeFriends;
    CSteamID m_lobby, m_host;
    bool m_isHost = false;
    CSteamID m_hostGone;         // the host who left, until Steam names the new owner (AdoptNewHost)
    DWORD m_hostGoneAt = 0;
    int32_t m_hostChanges = 0;   // handovers in this lobby (snapshot hostChanges)
    int32_t m_progress[kCommandArgs] = {-1, 0, 0, 0, 0, 0};  // this player's race progress (member data "pos"; place -1 = none)
    int32_t m_testProgress[kMaxMembers - 1][5] = {{-1}, {-1}, {-1}, {-1}, {-1}, {-1}, {-1}};  // host: its test players' (lobby data "testpos")
    bool m_testProgressDirty = false;
    SearchRows m_rows;
    bool m_listDone = true;  // the public list has answered (or none was asked for): the search ends with the friends' answers
    std::vector<LobbyMember> m_members;
    VoiceChat m_voice;  // voice chat (voice_chat.h): Configure at Start, Pump every loop, Shutdown at Stop
    std::vector<uint64_t> m_waiting;  // friends' lobbies whose data was requested
    DWORD m_searchStart = 0;
    Op m_pending = Op::None;
    uint64_t m_pendingLobby = 0;
    bool m_joinPublic = false;  // the pending join came from a public search's row: public seats only
    uint64_t m_pendingJoin = 0;  // a Steam join waiting for the menus (RememberJoin)
    bool m_abandon = false;
    bool m_stopping = false;
    Command m_deferred = {};   // the menu command waiting for a pending create/join
    bool m_hasDeferred = false;
    int32_t m_localRacer = -1, m_localReady = 0;  // what our menu last set (member data)
    LobbyRules m_localRules = kNoRules;           // what our menu last set as host
    LobbyRules m_rules = kNoRules;                // the current lobby's rules, as Steam has them
    std::vector<uint64_t> m_peers;                // games we exchanged race messages with
    bool m_strangerLogged = false;
    CCallResult<Session, LobbyCreated_t> m_created;
    CCallResult<Session, LobbyEnter_t> m_entered;
    CCallResult<Session, LobbyMatchList_t> m_listed;
};

// ---------------------------------------------------------------- worker
static HANDLE g_thread = nullptr;
static HANDLE g_stop = nullptr;
static uint64_t g_coldJoin = 0;
static bool g_ownAppIdEnv = false;  // this runtime set SteamAppId and must remove it on rollback

static void RestoreAppIdEnv() {
    if (g_ownAppIdEnv) SetEnvironmentVariableW(L"SteamAppId", nullptr);
    g_ownAppIdEnv = false;
}

// Steam's own networking messages (connection routes, failures): diagnostics for race connections.
// Called from Steam's threads (Log is locked); at most 400 lines a launch so it can't flood the log.
static volatile LONG g_netLines = 0;
static void NetDebug(ESteamNetworkingSocketsDebugOutputType type, const char* msg) {
    LONG n = InterlockedIncrement(&g_netLines);
    if (n <= 400) Log(false, L"steam net %d: %hs", (int)type, msg);
    else if (n == 401) Log(false, L"steam net: more messages not logged");
}

static DWORD WINAPI Worker(void*) {
    SteamErrMsg err = {};
    ESteamAPIInitResult r = SteamAPI_InitEx(&err);
    if (r != k_ESteamAPIInitResult_OK) {
        Log(true, L"SteamAPI_Init failed: %s (%hs). Online features are off for this launch.", InitResultName(r), err);
        return 0;
    }
    uint32 app = SteamUtils()->GetAppID();
    if (app != kAppId) {
        Log(true, L"Steam started for AppID %u, not %u; online features are off for this launch.", app, kAppId);
        SteamAPI_Shutdown();
        return 0;
    }
    if (WaitForSingleObject(g_stop, 0) == WAIT_OBJECT_0) {  // rollback arrived while Steam was starting
        SteamAPI_Shutdown();
        return 0;
    }
    CSteamID me = SteamUser()->GetSteamID();
    g_selfId = me.ConvertToUint64();
    Log(true, L"Steam ready: persona \"%s\", SteamID %llu, AppID %u, logged on %d", Widen(SteamFriends()->GetPersonaName()).c_str(),
        me.ConvertToUint64(), app, SteamUser()->BLoggedOn() ? 1 : 0);
    SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, NetDebug);
    SteamNetworkingUtils()->InitRelayNetworkAccess();  // so the first race connection doesn't wait for it
    {
        Session session(g_coldJoin);
        SetSteamUp(true);
        session.Start();
        HANDLE waits[2] = {g_stop, g_wake};
        // Every 30 ms in the menus; during a race every 2 ms, and at once when the game queues a message.
        for (;;) {
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, g_raceActive ? 2 : 30);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_FAILED) Sleep(30);  // never spin
            Command cmds[16];
            int n = TakeCommands(cmds, 16);
            for (int i = 0; i < n; ++i) session.Apply(cmds[i]);
            SteamAPI_RunCallbacks();
            session.PumpRace();
            session.PumpVoice();
            session.Tick();
            session.Publish();  // every state change reaches the menus, even one a path forgot to publish
        }
        SetSteamUp(false);
        session.Stop();
        // Let an outstanding create/join finish so its lobby is left rather than abandoned.
        for (int i = 0; i < 40 && session.Busy(); ++i) {
            SteamAPI_RunCallbacks();
            Sleep(50);
        }
        SteamAPI_RunCallbacks();
        if (session.Busy()) Log(false, L"stopping with a lobby request still unanswered");
    }
    SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
    SteamAPI_Shutdown();
    Log(false, L"Steam shut down");
    return 0;
}

// ---------------------------------------------------------------- ChaoGarage entry points
extern "C" HRESULT __cdecl SsasrModInitialize(const SsasrHostApiV1* api, const wchar_t* modDir) {
    if (!api || api->size < sizeof(SsasrHostApiV1) || api->version != SSASR_HOST_ABI_V1 || !modDir) return E_INVALIDARG;
    FingerprintKeep();
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_boxLock);  // before any hook: menu natives read the snapshot even with Steam off
    InitializeCriticalSection(&g_raceLock);
    g_snap.rules = LobbyRules{-1, -1, -1, -1, -1, -1};
    g_logReady = true;
    g_hostLog = api->log;  // ChaoGarage.dll stays loaded; the api struct itself does not outlive this call.
    std::wstring dir = modDir;
    g_logPath = dir + L"\\SteamMultiplayer.log";
    Log(false, L"---- " MOD_NAME L" " TEXT(MOD_VERSION) L" starting, pid %lu", GetCurrentProcessId());
    BackgroundInstall(api->game_image, dir);
    // Our menu scripts call the group-0x7A natives, so they are only substituted when those exist.
    if (NativesInstall(api->game_image)) {
        ScriptSubstInstall(api->game_image, dir);
        RaceConfigure((dir + L"\\config.ini").c_str());
        g_configIni = dir + L"\\config.ini";
        g_testPlayers = (int)GetPrivateProfileIntW(L"Debug", L"TestPlayers", 0, (dir + L"\\config.ini").c_str());
        if (g_testPlayers < 0 || g_testPlayers > kMaxMembers - 1) g_testPlayers = 0;
        if (g_testPlayers) Log(true, L"debugging: %d test player(s) join the lobbies this game hosts (config.ini [Debug] TestPlayers)", g_testPlayers);
        wchar_t sameRacer[16] = {};
        GetPrivateProfileStringW(L"Debug", L"TestPlayerRacer", L"", sameRacer, 16, (dir + L"\\config.ini").c_str());
        g_testSameRacer = g_testPlayers && _wcsicmp(sameRacer, L"same") == 0;
        if (g_testSameRacer) Log(true, L"debugging: the test players all take the host's racer (config.ini [Debug] TestPlayerRacer=same)");
        RaceInstall(api->game_image);  // online races (M3); only reachable from our menus
    } else {
        Log(true, L"multiplayer menus: off (menu natives not installed)");
    }

    // Inactive (S_OK) rather than failing: a failed runtime would disable every mod for this launch.
    std::wstring dll = dir + L"\\runtime\\steam_api.dll";
    std::string hex;
    if (!FileSha256(dll, hex) || hex != kSteamApiSha256) {
        Log(true, L"runtime\\steam_api.dll is missing or not the expected Steamworks 1.65 build (sha256 %hs); online features are off.",
            hex.empty() ? "unreadable" : hex.c_str());
        return S_OK;
    }
    wchar_t appId[16] = {};
    if (!GetEnvironmentVariableW(L"SteamAppId", appId, 16)) {
        if (!SetEnvironmentVariableW(L"SteamAppId", L"34190")) {
            Log(true, L"could not set SteamAppId (error %lu); online features are off.", GetLastError());
            return S_OK;
        }
        g_ownAppIdEnv = true;
        Log(false, L"SteamAppId was not set (game not started by Steam?); using %u", kAppId);
    }
    g_steamApi = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_steamApi) {
        Log(true, L"could not load steam_api.dll (error %lu); online features are off.", GetLastError());
        RestoreAppIdEnv();
        return S_OK;
    }
    if (!BindSteamApi()) {
        Log(true, L"steam_api.dll imports could not be bound; online features are off.");
        RestoreAppIdEnv();
        return S_OK;
    }
    std::wstring modsDir = dir.substr(0, dir.find_last_of(L"\\"));
    std::wstring badMod;
    g_modsOk = ScanMods(modsDir, g_mods, badMod);
    g_modsEncoded = EncodeMods(g_mods);
    if (g_modsOk) RaceUseEggHangar(std::any_of(g_mods.begin(), g_mods.end(), [](const ModEntry& m) { return m.id == "egg-hangar"; }));
    if (!g_modsOk) Log(true, L"could not read the mod list (%s); online games will refuse to start or join", badMod.empty() ? L"mods folder" : badMod.c_str());
    else if (g_modsEncoded.size() >= k_cubChatMetadataMax) { g_modsOk = false; Log(true, L"too many mods to publish in a Steam lobby (%u bytes)", (unsigned)g_modsEncoded.size()); }
    else Log(false, L"mod list: %u mods, %u bytes", (unsigned)g_mods.size(), (unsigned)g_modsEncoded.size());
    g_coldJoin = LobbyFromCommandLine();
    Log(false, L"command line: %s", GetCommandLineW());
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_thread = g_stop && g_wake ? CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr) : nullptr;
    if (!g_thread) {
        Log(true, L"could not start the Steam thread (error %lu); online features are off.", GetLastError());
        RestoreAppIdEnv();
    }
    return S_OK;
}

// The worker owns only Steam state (no game hooks). Once the stop event is set it starts no new
// lobby operation: it checks the event right after SteamAPI_InitEx and in every pump. If it does not
// finish within 10 s (Steam stalled), it finishes on its own later; rollback then returns without
// any file I/O, keeps the handle, and leaves SteamAppId for the still-running worker.
extern "C" HRESULT __cdecl SsasrModRollback() {
    // Game hooks first: they do not depend on the worker, and must not outlive this call even if
    // the worker is slow to stop.
    BackgroundRollback();
    RaceRollback();
    ScriptSubstRollback();
    NativesRollback();
    if (g_thread) {
        SetEvent(g_stop);
        if (WaitForSingleObject(g_thread, 10000) != WAIT_OBJECT_0) return S_OK;
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    RestoreAppIdEnv();
    Log(false, L"rolled back");
    return S_OK;
}

// Test entry points for tests/lobby_harness.cpp (the game never calls them): the same mailbox the
// menu natives use.
extern "C" int __cdecl SteamMpTestPost(int kind, int arg) { return LobbyPost((LobbyCommandKind)kind, arg) ? 1 : 0; }
extern "C" void __cdecl SteamMpTestRead(LobbySnapshot* out) { if (out) LobbyRead(*out); }
