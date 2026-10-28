#pragma once
#include <windows.h>
#include <stdint.h>

// Script natives for the multiplayer menus (group 0x7A; notes/NATIVE-BRIDGE.md, notes/STEAM-BRIDGE.md).
// Install swaps the script VM's native-dispatch vtable slot 0x6b467c (4 bytes, no code patch) in
// SsasrModInitialize. Our menu scripts call group 0x7A, so they must only be loaded when this
// succeeded: a 0x7Axx call without the bridge would fault.
bool NativesInstall(HMODULE gameImage);
void NativesRollback();

// Native numbers (low byte), shared with tools/netmenu/steam.py.
enum : unsigned {
    kNatGroup = 0x7A,
    kNatVersion = 0x00,      // () -> bridge version
    kNatState = 0x01,        // () -> LobbyState
    kNatError = 0x02,        // () -> LobbyError
    kNatHost = 0x03,         // (mode) -> 1 posted / 0
    kNatSearch = 0x04,       // (mode) -> 1 / 0
    kNatJoinRow = 0x05,      // (row) -> 1 / 0
    kNatLeave = 0x06,        // () -> 1 / 0
    kNatRowCount = 0x07,     // () -> rows (0 unless state is Results)
    kNatRowText = 0x08,      // (row, field, buffer) -> 1 written / 0; field 0 host name, 1 "players/max"
    kNatOtherCount = 0x09,   // () -> the other players in the lobby (0 unless in a lobby)
    kNatOtherText = 0x0A,    // (other, buffer) -> 1 / 0; other players, host first
    kNatIsHost = 0x0B,       // () -> 1 if this game hosts the lobby
    kNatRevision = 0x0C,     // () -> snapshot revision (changes whenever anything changes)
    // M3: lobby member data and the race start (notes/RACE-BRIDGE.md)
    kNatSetLocal = 0x0D,     // (racer index, ready) -> 1; posted to Steam only when it changed
    kNatSetRules = 0x0E,     // (course, laps, privateSlots, ai, items, catchup) -> 1; host only, when changed
    kNatCanStart = 0x0F,     // () -> RaceStartRefusal-style check without starting (0 = Start would go)
    kNatStart = 0x10,        // () -> RaceStartRefusal (0 = the contract was sent; the race starts)
    kNatRacePoll = 0x11,     // () -> RaceStatus (1 = start the race now)
    kNatRaceSlot = 0x12,     // (creation index, field) -> field 0: character hash (hash type), 1: pad (this player's racer) or -1
    kNatRaceCourse = 0x13,   // () -> the course name (string) of the armed race
    kNatRaceLaunched = 0x14, // (ok) -> 1; the script sent the start messages (1) or could not (0)
    kNatOtherReady = 0x15,   // (other) -> 1 if that player has picked a racer and is ready
    // One call per lobby frame (the lobby script is near its 32 KB jump limit, so the logic is here):
    // (racer index, racer popup mode, course, laps, privateSlots, ai, items, catchup, settings popup
    //  mode, Start key (Enter) pressed, primary pad, choosing[, select key (F2) pressed[, the lobby cursor]]) -> kFrame*
    //  action for the script (the Mac's keys: Enter = START, F2 = CHANGE RACER on your own bar / the player popup on
    //  another's). choosing = 1 until this player has picked a racer in this lobby (published as no racer: "?" on every screen).
    kNatFrame = 0x16,
    // M6: the lobby bars, every member in lobby order (host first = 1P), as the console drew them
    // (re\m6-lobby\FINDINGS.md).
    kNatMemberRacer = 0x17,  // (member) -> its racer as a character hash (hash type), or int 0 = none yet ("?")
    kNatMemberField = 0x18,  // (member, field) -> kMember*
    kNatMemberText = 0x19,   // (member, buffer) -> 1 written / 0: its Steam name
    // A guest's lobby panel shows the host's rules (lobby data "rules"), redrawn with the bars.
    kNatHostRule = 0x1A,     // (field: course, laps, privateSlots, ai, items, catchup) -> the host's value; -1: host, none or out of range
    kNatInvite = 0x1B,       // () -> 1 asked / 0: INVITE FRIENDS, the Steam overlay's invite dialog for this lobby
    // A Steam "Join Game" / invite the menus take when the player can go (re\m6-join\FINDINGS.md).
    kNatJoinPoll = 0x1C,     // () -> 1 if one is waiting (and Steam is up)
    kNatJoinTake = 0x1D,     // () -> 1: joining it (leaving any current lobby); the script then waits in its Joining state
    // M7: host migration. When the host leaves, Steam passes the lobby to another member and everyone stays.
    kNatHostChanges = 0x1E,  // () -> host handovers in this lobby so far (0 outside a lobby); the script notices a change
    // M7: the lobby's race bar for a player who joined while a race runs (re\m7-join-race\FINDINGS.md Q1): the bars'
    // racer icons, given by the lobby script with the bars (the frame native draws the bar from the members' progress).
    kNatMemberIcon = 0x1F,   // (member, icon hash or 0) -> 1
    // M7: VIEW PROFILE / GAMER CARD on the results (re\m7-profile\FINDINGS.md; our `results` module calls it every list frame):
    // (field, up pressed, down pressed, F1 pressed): field 0 steps the cursor over the rows with a player's profile and
    // opens the Steam profile on F1 -> the cursor's row (-1 none); 1 -> the row to un-highlight this frame (-1 none);
    // 2 -> the row to highlight (-1 none); 3 -> the footer: 1 CONTINUE + GAMER CARD, 2 CONTINUE only, 0 as it is.
    kNatProfile = 0x20,
    // The search rows' VIEW PROFILE (F2, the console's VIEW GAMER CARD): (row) -> 1 asked / 0 (no row, no host id: an older build's lobby).
    kNatRowProfile = 0x21,
    // The search rows' STATUS column (the console's LOBBY / GAME): (row) -> 1 while that lobby's race runs, else 0.
    kNatRowRacing = 0x22,
    // KICK (M7, host): (member) -> 1 asked / 0 (not the host, no popup asked, that player left, this player or a test player). The
    // lobby data "kick" names it and "banned" keeps it out (lobby.h); the player is the one kNatFrame's kFramePopup was about.
    kNatKick = 0x23,
    // The lobby player popup's VIEW STEAM PROFILE (the console's VIEW PROFILE / GAMER CARD): (member) -> 1 asked / 0 (no such
    // member, this player, a test player).
    kNatMemberProfile = 0x24,
    // The consoles' triangle / Y button on PC (INVITE FRIENDS in the lobby, VIEW PROFILE on the game list): () -> 1 when the key
    // named in config.ini [Menus] InviteKey (A) was pressed since the last call, read while this game's window is in front (as
    // the voice chat's push-to-talk key).
    kNatInviteKey = 0x25,
    // The lobby player popup's MUTE (an earlier console build's NOTHING / VIEW PROFILE / MUTE / KICK popup; re\m8-mute\FINDINGS.md): (member)
    // -> 1 muted now / 0 heard again / -1 nothing (no such member, this player, a test player). A toggle on this game only; the
    // player is the one kFramePopup / kFramePopupMuted was about (by Steam id, as KICK); the bar then turns yellow (or says
    // "(muted)" in a lobby scene without the yellow art) and its voice icon stays off.
    kNatMemberMute = 0x26,
    // M8: (row) -> the row host's license flag (an art hash of the search scene, re\m8-flag\FINDINGS.md), or int 0 when unknown.
    kNatRowFlag = 0x27,
    // 1.1 ENABLE MODIFICATIONS (notes\ENABLE-MODS.md): (lobby racer index) -> 1 pickable / 0 its mod is off in this lobby.
    kNatRacerOn = 0x28,
    // The search screen's CREATE question (the console's "enable your downloadable content?", TU2 EB933573): (field, value): field 0
    // -> 1 if Metal Sonic is installed, 1 -> 1 if Egg Hangar is; 2 -> 1: the next lobby uses Metal Sonic (value bit 0) and Egg
    // Hangar (bit 1), the others off. 3 -> the join refusal pop-up's body for the last refused join (RefusalText). -1: another field.
    kNatMods = 0x29,
    // (row) -> the row's mod icons sprite (a hash: Metal Sonic, Egg Hangar or both; tools\mod_icons.py), or int 0: the stock signal bars.
    kNatRowMods = 0x2a,
    // () -> 1 when back (Esc / the pad's back) was pressed this frame in the dialog input layer (a dialog is up; Enter not pressed
    // with it): the search screen closes the modifications question with it (the user, 2026-10-24; an addition: no PC or console
    // dialog closes on back; re\m9-esc\FINDINGS.md).
    kNatDialogBack = 0x2b,
};
// kNatMemberField fields (count / racing: member ignored; talking: voice chat; racing: a race runs here, the lobby's race bar is up).
enum : int32_t { kMemberReady = 0, kMemberIsMe = 1, kMemberCount = 2, kMemberTalking = 3, kMemberRacingHere = 4 };
// Join: a Steam join waits; Popup: the select key with the lobby cursor (kNatFrame arg 13) on another player's bar: the console's player popup;
// StartAlone / StartNotReady: the host's Start refused with the console's dialogs (fewer than 2 players / not everyone ready);
// PopupMuted (M8): the popup about a muted player, whose button says UNMUTE (only when the dialogs text reads it; else Popup).
enum : int32_t { kFrameNothing = 0, kFrameStartRace = 1, kFrameStartSent = 2, kFrameStartRefused = 3, kFrameJoin = 4, kFramePopup = 5, kFrameStartAlone = 6, kFrameStartNotReady = 7,
                 kFramePopupMuted = 8 };
constexpr int kBridgeVersion = 2;
