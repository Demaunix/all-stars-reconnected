#pragma once
#include <windows.h>
#include <stdint.h>
#include "race_wire.h"
#include "script_abi.h"

// The race bridge (notes/RACE-BRIDGE.md): one race on every PC from the host's contract, held
// until everyone has loaded, with the other players' cars played back from the network.
// Game thread only, except Install/Rollback (SsasrModInitialize / SsasrModRollback).

// Swaps the race hook slots (native table and phase vtables, 4 bytes each, no code bytes), all or
// none, after checking every slot holds the stock function. False: online races are off.
bool RaceInstall(HMODULE gameImage);
void RaceRollback();
// Reads config.ini [Race] (before any race): HostRunsAI, ItemSync.
void RaceConfigure(const wchar_t* configIni);

// Whether the Egg Hangar mod is installed, from a complete mod scan (not called when the scan failed): only then is its course,
// the lobby's 25th, raced; without it the course availability query says "no".
void RaceUseEggHangar(bool installed);
// ENABLE MODIFICATIONS (1.1, notes\ENABLE-MODS.md): whether this lobby uses Egg Hangar and Metal Sonic (installed here and named in
// the lobby's required mods). Off: Egg Hangar's course is neither offered nor raced; Metal Sonic can't be picked and is never an AI racer.
void RaceLobbyMods(bool eggOn, bool metalOn);

// The racer list the lobby menu uses (index -> character hash): the Roster Pack's 25, the stock 21, or the stock 21 + Metal Sonic
// (the standalone Metal Sonic 1.1.2). Set by the menu substitution once it knows which module it installed.
enum RacerListKind { kRacerListStock, kRacerListRoster, kRacerListMetal };
void RaceUseRacerList(RacerListKind kind);
// The character hash of lobby racer `index` (the list above), or 0 if there is no such racer.
uint32_t RaceRacerHash(int index);
// Whether lobby racer `index` may be picked in this lobby (RaceLobbyMods; the racer picker asks per row, Start checks it too).
bool RaceRacerOn(int index);
// Test player n's racer (0-based): the n-th lobby racer that can be an AI and that no index in `taken` names, or -1.
int RaceTestPlayerRacer(int n, const int* taken, int takenCount);
// The id of the last race this game took part in (0: none): the lobby's race bar shows only other races.
uint32_t RaceLastId();
// Every game native call passes here (natives.cpp's dispatcher) after the game's own handler: during
// an online race a few results are adjusted at known script call sites (the other players' name tags).
void RaceNativeAfter(void* vm, NativeCall* d);
// And before it: true when the call was handled here instead (the game's handler is skipped).
bool RaceNativeBefore(void* vm, NativeCall* d);
// 1.1.1: our fe_mainmenu's Steam-join check (native 7a1c, before its handler): the auto-update's notices are posted here
// (re\update-dialogs\FINDINGS.md C: the main menu is idle, no dialog is up, a license is picked).
void RaceMainMenuJoinCheck(void* vm);
// The results screen's profile cursor (native 7a20, natives.h kNatProfile; re\m7-profile\FINDINGS.md): field 0 steps it
// (the keys pressed this frame) and returns its row, fields 1-3 what this frame changed (the row to un-highlight, the row
// to highlight, the footer). Only during our online race's results; -1 / 0 otherwise.
int32_t RaceProfileNative(int32_t field, int32_t up, int32_t down, int32_t f1);

// A script call site race.cpp checks (ScriptSite): the `n` bytes (`code`) that identify it, starting at PC + `from` (the PC is just
// past the 8-byte native call), found exactly once in the bound module's code. The site's identity is those bytes, not an offset:
// another mod may rebuild a module in memory with a line inserted before the site (the bytes stay, the offset moves). `known` lists
// the PC's module offsets in the modules this build was checked against (0: unused), so the log can say when a site has moved;
// `module` is the module's name hash (documentation and the tests: the game gives a native hook no module name).
struct ScriptSiteDef {
    const char* name;
    uint32_t module;
    int from;
    const uint8_t* code;
    size_t n;
    uint32_t known[3];
};

#ifdef RACE_TEST
int RaceTestSites(const ScriptSiteDef** out, int cap);                   // every site race.cpp checks
uint32_t RaceTestSitePc(const uint8_t* module, const ScriptSiteDef& s);  // the site's PC offset in a module (0: off), as ScriptSite finds it
#endif

enum RaceStatus : int32_t {
    kRaceNone = 0,      // no race arranged
    kRaceStart = 1,     // a contract is armed: the lobby script must start the race now
    kRaceRunning = 2,   // the race has been set up from the contract
    kRaceFailed = 3,    // the start could not be arranged (see SteamMultiplayer.log)
};

// Menu natives (game thread): drain race messages while in the lobby, and read the status.
// kRaceStart is returned until the lobby script reports the launch (RaceLaunched).
RaceStatus RaceMenuPoll();
void RaceLaunched(bool ok);  // the lobby script sent the start messages (ok) or could not (cancels)
// Host: build the contract from the lobby (everyone has a racer and is ready), send it, arm it.
// Returns 0 on success or a reason code (RaceStartRefusal).
enum RaceStartRefusal : int32_t {
    kStartOk = 0, kStartNotHost = 1, kStartAlone = 2, kStartNotReady = 3, kStartNoRules = 4,
    kStartSameRacer = 5 /* no longer used: players may share a racer */, kStartBusy = 6, kStartNoList = 7, kStartSendFailed = 8, kStartRacerNotOnline = 9,
    kStartNetNotReady = 10,  // a player's game has no Steam certificate yet (member data "net"): the menu waits
    kStartPlayerInRace = 11, // a player is still in the last race (its results; member data "pos"): refused
    kStartRacerOff = 12,     // a player's racer belongs to a mod that is off in this lobby (RaceRacerOn): the menu says "not ready"
};
int32_t RaceHostStart();
// Drift+ off in the next race this lobby starts: a member drives with the keyboard, or the host's [Debug] ForceDriftPlusOff
// (the rule of src/steer.h; the Settings popup's DRIFT+ row shows it).
struct LobbySnapshot;
bool RaceDriftPlusOff(const LobbySnapshot& s);
// The armed contract, for the lobby script's start sequence (null when none).
const RaceContract* RaceContractArmed();
int RaceLocalIndex();  // this player's creation index in the armed contract (-1 when none)
void RaceCancel(AbortReason why);  // leave the arranged race (menus)
