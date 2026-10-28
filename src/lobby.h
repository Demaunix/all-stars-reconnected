#pragma once
// Game-thread interface to the Steam lobby session that runs on the Steam worker thread
// (steam_mp.cpp). The game thread posts commands and reads copies of the latest snapshot;
// it never calls Steam itself. Values are what the menu scripts see through our natives.
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum LobbyState : int32_t {
    kLobbyOff = 0,        // Steam is not running in this game (init failed / not started yet)
    kLobbyIdle = 1,
    kLobbyCreating = 2,
    kLobbySearching = 3,
    kLobbyResults = 4,    // search finished; rows (possibly none) are valid
    kLobbyJoining = 5,
    kLobbyInLobby = 6,
    kLobbyError = 7,      // see LobbyError; the session is idle again
};

enum LobbyError : int32_t {
    kLobbyErrNone = 0,
    kLobbyErrCreate = 1,      // Steam refused to create the lobby
    kLobbyErrJoin = 2,        // Steam refused the join (full, closed, no permission...)
    kLobbyErrMods = 3,        // the host runs a different set of mods (details in SteamMultiplayer.log)
    kLobbyErrGone = 4,        // the host left / the lobby closed
    kLobbyErrSearch = 5,      // the lobby search failed
    kLobbyErrVersion = 6,     // the host runs another Steam Multiplayer version
    kLobbyErrKicked = 7,      // the host kicked this player out of the lobby (or refuses it back after a kick)
    kLobbyErrSteamLost = 8,   // Steam lost its connection to the Steam servers while in a lobby, a search or a join: the game ended
    kLobbyErrSteamOff = 9,    // Steam is not connected to the Steam servers (offline mode, no network): nothing can be found or hosted
    kLobbyErrJoinGone = 10,   // the lobby was gone when the join arrived ("The game session is no longer available.")
};

enum LobbyMode : int32_t { kModeAnyone = 0, kModeFriends = 1 };

constexpr int kMaxRows = 8;       // the Game Search screen has 8 result rows
constexpr int kMaxMembers = 8;    // the console's lobbies: 8 seats (= kMaxHumans, race_wire.h)
constexpr int kNameLen = 64;      // UTF-8, NUL-terminated

struct LobbyRow {
    uint64_t lobby;
    char host[kNameLen];
    int32_t members, maxMembers;
    int32_t friendRow;     // found through a friend's game (a friend's lobby, friends-only or public): every seat counts
    int32_t otherVersion;  // the host runs another Steam Multiplayer build: shown, the join refused (kLobbyErrVersion)
    uint64_t hostId;       // the host's Steam id (lobby data "hostid"; 0 from an older build): the row's VIEW PROFILE (F2)
    int32_t racing;        // the host's race runs (lobby data "racing"): the console's STATUS column says GAME, else LOBBY
    int32_t otherMods;     // the host's mod set differs from ours: shown "(other mods)", the join refused (kLobbyErrMods)
    int32_t flag;          // the host's license nationality code (lobby data "flag"; -1 unknown): the row's flag, as the console's
    int32_t mods;          // 1.1: the row's mod icons in the SIGNAL cell (the console's DLC column): Egg Hangar's modset.h kMod* state in
                           // bits 0-1, Metal Sonic's in bits 2-3 (unused / the same build / another version / not installed here)
    int32_t otherModsRest; // otherMods because of a mod without an icon: the name still says "(other mods)"
};

// Test players (debugging; the host's config.ini [Debug] TestPlayers, lobby data "test"): pretend members every PC
// adds after the real ones, always ready; in a race the host's game drives their cars (as a departed player's).
constexpr uint64_t kTestPlayerBase = 0x7E57000000000000ull;  // no Steam ID has these top bits (universe 0x7e)
inline bool IsTestPlayer(uint64_t id) { return (id & 0xFFFF000000000000ull) == kTestPlayerBase; }

struct LobbyMember {
    uint64_t id;
    char name[kNameLen];
    int32_t isHost, isMe;
    int32_t racer;  // the racer list index this member picked, -1 = none yet (member data "racer")
    int32_t ready;  // member data "ready"
    int32_t net;    // member data "net": its game can make race connections (Steam gave it its certificate)
    // Race progress (member data "pos", M7): while this member races, its place (0-based; -1 = not racing), lap,
    // progress in thousandths of the whole race, the race's laps and id; the lobby's race bar shows them to a
    // player who joined while the race runs.
    int32_t place, lap, progress, laps;
    uint32_t raceId;
    int32_t results;  // its race is over and it views the results ("pos" 6th field): the bar says VIEWING RESULTS, as the console's
    int32_t talking;  // voice chat (M7): its voice arrived within the last kVoiceTalkingMs (this player: its own voice went out): the bar's voice icon
    int32_t muted;    // voice chat: this player doesn't hear it (the lobby popup's MUTE, or config.ini [Voice] Muted); the bar is yellow
    int32_t input;    // member data "inp": the device that drives its car (kInput*): the Drift+ rule and Majority Drift Parity
};

// The race rules the host chose (lobby data "rules"); -1 in every field until the host publishes.
struct LobbyRules {
    int32_t course, laps, privateSlots, ai, items, catchup;
};
constexpr int kRuleCount = 6;

// PRIVATE SLOTS: the console's lobbies had 8 seats, 0-7 of them kept for friends and invites. The public
// seats left for a public search, from the host's lobby data "rules" (no or bad rules: no limit).
constexpr int kConsoleSeats = 8;
inline int PublicSeats(const char* rules) {
    int course = 0, laps = 0, privateSlots = -1;
    if (!rules || sscanf_s(rules, "%d,%d,%d", &course, &laps, &privateSlots) != 3 || privateSlots < 0 || privateSlots >= kConsoleSeats)
        return kConsoleSeats;
    return kConsoleSeats - privateSlots;
}

// KICK (M7): Steam lobbies have no kick call, so the host names the member in its lobby data "kick" (that game leaves when
// it reads it) and lists everyone kicked so far in "banned" (comma-separated Steam ids; a join of this lobby refuses them).
// A Steam id as the lobby data holds it: decimal digits only, else 0.
inline uint64_t ParseSteamId(const char* s) {
    if (!s || !*s) return 0;
    uint64_t v = 0;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return 0;
        uint64_t d = (uint64_t)(*s - '0');
        if (v > (0xFFFFFFFFFFFFFFFFull - d) / 10) return 0;  // past 64 bits
        v = v * 10 + d;
    }
    return v;
}
inline bool IdListHas(const char* list, uint64_t id) {
    if (!list || !id) return false;
    while (*list) {
        const char* end = list;
        while (*end && *end != ',') ++end;
        char one[24];
        size_t n = (size_t)(end - list);
        if (n > 0 && n < sizeof(one)) {
            memcpy(one, list, n);
            one[n] = 0;
            if (ParseSteamId(one) == id) return true;
        }
        list = *end ? end + 1 : end;
    }
    return false;
}

struct LobbySnapshot {
    uint32_t revision;            // bumped on every change
    int32_t state, error, mode, isHost;
    int32_t rowCount;
    LobbyRow rows[kMaxRows];
    int32_t memberCount;
    LobbyMember members[kMaxMembers];
    LobbyRules rules;
    // A Steam "Join Game" / accepted invite (or the cold start's +connect_lobby) waiting for the menus to
    // take it (native JoinTake), 0 = none. Steam only remembers it: the menus act when the player can go.
    uint64_t pendingJoin;
    // Host handovers in this lobby: when the host leaves, Steam passes the lobby on to another member and
    // the lobby goes on with that host (the console's host migration). The lobby script shows a notice.
    int32_t hostChanges;
    // 1.1: the last join refused for mods (kLobbyErrMods): Egg Hangar's modset.h kMod* state in bits 0-1, Metal Sonic's in bits 2-3
    // (as LobbyRow::mods), bit 4: other mods differ too (or a mod list was unreadable). The refusal pop-up names them.
    int32_t refusedMods;
};
enum : int32_t { kRefusedOther = 16 };

enum LobbyCommandKind : int32_t {
    kCmdHost, kCmdSearch, kCmdJoinRow, kCmdLeave,
    kCmdSetLocal,  // args: racer index (-1 none), ready (0/1)
    kCmdSetRules,  // args: LobbyRules fields in order (host only; ignored otherwise)
    kCmdInvite,    // the Steam overlay's invite dialog for the current lobby (no args; ignored outside a lobby)
    kCmdJoinLobby, // args[0..1]: a Steam lobby id (low, high dword): join it, leaving the current one (a Steam Join Game)
    kCmdSetProgress,  // args: place (-1 = not racing), lap, progress (thousandths), laps, race id: member data "pos"
    kCmdSetTestProgress,  // host: args 0-4 as kCmdSetProgress for test player args[5] (1-7; 0 = clear all): lobby data "testpos"
    kCmdProfile,   // args[0..1]: a Steam id (low, high dword): that player's profile page in the Steam overlay (VIEW PROFILE / GAMER CARD)
    kCmdKick,      // args[0..1]: a member's Steam id (low, high dword): the host kicks it out of the lobby (lobby data "kick" / "banned")
    kCmdMute,      // args[0..1]: a member's Steam id (low, high dword): the lobby popup's MUTE toggles hearing its voice (this game only)
};
// Commands that change the session; the snapshot shows the state they lead to at once (Post).
inline bool IsSessionCommand(LobbyCommandKind kind) { return kind <= kCmdLeave || kind == kCmdJoinLobby; }
inline int32_t FirstState(LobbyCommandKind kind) {
    return kind == kCmdHost ? kLobbyCreating : kind == kCmdSearch ? kLobbySearching : kind == kCmdLeave ? kLobbyIdle : kLobbyJoining;
}
constexpr int kCommandArgs = kRuleCount;

// Game thread. False if the command queue is full or Steam is off.
bool LobbyPost(LobbyCommandKind kind, int32_t arg);
bool LobbyPostArgs(LobbyCommandKind kind, const int32_t* args, int count);
// Steam's overlay (INVITE FRIENDS, Shift+Tab) is open over the game (GameOverlayActivated_t).
bool LobbyOverlayOpen();
const wchar_t* LobbyConfigPath();  // the mod's config.ini (empty until the mod started)
// The lobby screen ran a frame (game thread; it calls the frame native every frame, ~50 a second). Without one for a
// moment (the race and its results, a loading screen, any other screen) the worker publishes this player as not ready;
// the next lobby frame publishes ready again.
void LobbyNoteLobbyFrame();
// This game's active license nationality code (game thread; profile +0x18), for the host's lobby data "flag" (the console's
// game-list flag, re\m8-flag\FINDINGS.md); -1 unknown.
void LobbyNoteFlag(int32_t code);
// ENABLE MODIFICATIONS (1.1, notes\ENABLE-MODS.md): whether this game has the toggleable mod `id` installed (complete scan only),
// and the host's answers for the lobby it creates next (game thread, before the Host command; the worker reads them at creation).
bool LobbyModInstalled(const char* id);
void LobbySetHostMods(bool metalOn, bool eggOn);
// The device that drives this game's car: the class of the primary pad (re\drift-plus\FINDINGS.md; game thread, every lobby
// frame), for the member data "inp".
enum : int32_t { kInputUnknown = 0, kInputKeyboard = 1, kInputController = 2 };  // (0: a zeroed member is unknown)
void LobbyNoteInput(int32_t kind);
constexpr uint32_t kLobbyAwayMs = 700;
// seen = the last lobby frame's GetTickCount | 1 (0: none yet), now = the worker's GetTickCount. Signed: in the same
// clock step as the frame, now can be seen - 1 (m7o compared unsigned: 0xffffffff ms "away" while at the lobby).
inline bool LobbyFrameStale(uint32_t seen, uint32_t now) { return seen && (int32_t)(now - seen) > (int32_t)kLobbyAwayMs; }
void LobbyRead(LobbySnapshot& out);

// ---------------------------------------------------------------- race messages
// A byte pipe between the game thread and the other games in the lobby, for src/race_wire.h
// messages. The worker sends and receives them with ISteamNetworkingMessages and only accepts
// them from members of the current lobby. Game thread only.
constexpr size_t kRaceMsgMax = 416;  // = kWireMax (race_wire.h)
struct RaceNetMsg {
    uint64_t peer;   // sender (inbound) or recipient (outbound; 0 = every other lobby member)
    uint32_t size;
    bool reliable;
    uint8_t data[kRaceMsgMax];
};
bool RaceNetSend(uint64_t to, const void* data, size_t size, bool reliable);
int RaceNetReceive(RaceNetMsg* out, int max);
void RaceNetActive(bool on);  // while on, the worker pumps every few ms instead of every 30 ms
uint64_t RaceNetSelf();       // this player's SteamID (0 if Steam is off)
