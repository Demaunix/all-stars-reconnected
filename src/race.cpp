// Race bridge, game-thread side: one race on every PC from the host's contract, held until
// everyone has loaded, the other players' cars played back from the network.
// Design, addresses and evidence: notes/RACE-BRIDGE.md. All addresses are in the game exe
// (base 0x400000; checked against the decrypted copy re\bin\SSASR-unpacked.exe).
//
// Every hook is a 4-byte swap of a data slot (script-native table or race-phase vtable). Each one
// calls the stock function unchanged, or (the holds) doesn't call it yet. No code bytes change.
// When no contract is armed, every hook just calls the stock function.
// Laps and results (M4, notes/LAPS.md) use the game's own functions the console's network code
// called: apply a nav path to the other player's car, add it to the finish list with the time its
// owner reports, and the stock race-end wait and 30-second timer.
// AI (M5, notes/NPC-SYNC.md): as on the console, the host's game drives the AI. Its Cars message
// carries its own car and the AI every tick; the guests switch the AI to network cars at hold A, show
// them from those poses, and take the host's final results for every car before their own race ends.
// Items (M5, notes/ITEMS-SYNC.md): as on the console, a car's owner decides its items. Every give and
// fire is an event the game posts to its event manager; the owner's game runs and sends its cars'
// events, the others drop the ones their own copy of that car makes and run the owner's instead, when
// that car is shown at the moment it fired. Each game decides the hits on its own cars.
#include "race.h"
#include <intrin.h>
#include <math.h>
#include <new>
#include <random>
#include <stdio.h>
#include "lobby.h"
#include "log.h"
#include "playout.h"
#include "steer.h"
#include "script_abi.h"
#include "race_tables.h"
#include "update.h"

namespace {

// ---------------------------------------------------------------- game addresses
#ifdef RACE_TEST
#define GAME_ADDRESS uintptr_t  // tests/test_race.cpp points these at fake memory and functions
#else
#define GAME_ADDRESS const uintptr_t
#endif
GAME_ADDRESS kScene = 0xc29fc0;            // [kScene] = scene; +0x1100 racer count (byte)
GAME_ADDRESS kCourseGraph = 0xd12560;      // nonzero once the course's navigation graph loaded
GAME_ADDRESS kSeedRng = 0x4be160;          // reseed the shared MT19937: stdcall(seed)
GAME_ADDRESS kSetControlState = 0x507f00;  // thiscall(controller, state id)
GAME_ADDRESS kSetPosition = 0x4f8440;      // racer vtable slot 9: thiscall(racer, float[4]); sets +0xc0 and the dirty flag
GAME_ADDRESS kSimTick = 0xc2a008;          // unpaused sim-step counter (50 per second)
GAME_ADDRESS kRacerTypeId = 0xd21dc0;      // the script type id 0905 returns its racer with (0x58cb21), registered at start-up
// Laps and results (notes/LAPS.md): the course's nav paths, the lap records and the finish list.
GAME_ADDRESS kApplyNavLine = 0x4e8880;     // thiscall(racer, nav): force the track cursor onto nav path `nav`, snap to +0xc0
GAME_ADDRESS kRecords = 0xcf7a68;          // [kRecords] + i * 0x90 = lap record of racer i (creation order)
GAME_ADDRESS kFinishCount = 0xcf7a57;      // byte: entries in the finish list
GAME_ADDRESS kFinishList = 0xcf7950;       // 8 bytes per entry: racer index (byte), reason (dword at +4)
GAME_ADDRESS kRaceOver = 0xcf7a6c;         // nonzero once the race has ended (set by 0x4cdbf0)
GAME_ADDRESS kAddFinish = 0x4ccbb0;        // cdecl(index, reason): append to the finish list (once per racer)
GAME_ADDRESS kStartEndTimer = 0x4ccf40;    // cdecl(seconds): start the race-end timer ("RACE ENDS IN")
GAME_ADDRESS kEndTimerElapsed = 0xcf7a8c;  // that timer's elapsed ticks (+1 per tick in 0x4cbbb0, called by 0x5b2250)
GAME_ADDRESS kEndTimerLimit = 0xcf7a90;    // and its length; the race ends once elapsed >= length (0x4cdd1f)
GAME_ADDRESS kTickRate = 0xc2a020;         // sim ticks per second
// The race clock on real time (re\race-clock\FINDINGS.md): the frame's time, the step accumulator, every step's count and
// this frame's (capped) step count, as the main loop's step timer 0x4a05f0 / 0x4a0710 keeps them.
GAME_ADDRESS kAppPtr = 0xc2a1b4;           // [kAppPtr] = the app object: +0xc the frame's timeGetTime (ms)
GAME_ADDRESS kStepAcc = 0xc2a000;          // float: ms left in the step accumulator (0..20), set once per frame
GAME_ADDRESS kStepCount = 0xc2a004;        // every sim step, paused or not: +1 at each step's end
GAME_ADDRESS kFrameSteps = 0x6ec868;       // the steps this frame runs (at most 4: the rest of a long frame is dropped)
const uint32_t kAppFrameTime = 0xc;
// The host's rules (notes/RULES.md): the profiles' race settings, 4 records of 0xac bytes, saved with the
// profile; the active one is [kActiveProfile]. +0xc items (0 none, 1 no All-Star, 2 all, read live at
// every item roll), +0x10 hazards (byte, read when the course loads).
GAME_ADDRESS kSettingsRecords = 0xc390e0;
GAME_ADDRESS kActiveProfile = 0xcdb530;
// Held items (re\m5-npc2\FINDINGS.md): the racer's own item clear, as its finish handler calls it.
GAME_ADDRESS kClearItems = 0x537880;       // thiscall(racer, 1), ret 4: drops held items (+0xc1c and the rest)
// Item events (notes/ITEMS-SYNC.md, re\m5-items2): the scene's event manager runs every posted event at once.
GAME_ADDRESS kEventAlloc = 0x516760;       // stdcall(kind) -> a new event (vtable +0, kind +4), null above kind 0x11
GAME_ADDRESS kEventFree = 0x516900;        // stdcall(event): deletes it
GAME_ADDRESS kProjectileType = 0x4c0fc0;   // cdecl(type) -> that projectile type's data, null for a type the game lacks
GAME_ADDRESS kPlayReaction = 0x502f60;     // thiscall(character animation, node hash), ret 4 -> al: started (sets bit 0x40)
// The intros (re\m6-intro\FINDINGS.md): the race flow's current record and its advance; the HUD text element lookup.
GAME_ADDRESS kFlowRecord = 0xcf7a70;       // [kFlowRecord] = the current race-flow record: +4 its state id
GAME_ADDRESS kFlowAdvance = 0x4cce60;      // cdecl(): on to the current record's next state (queued; its enter/exit run at the next flush)
GAME_ADDRESS kFindTextElement = 0x483860;  // thiscall(ECX = kElementTable, hash) -> the HUD text element, or 0 (missing, or not text)
// The course intro's announcer line and music (re\m7-intro-voice\FINDINGS.md): the announcer's speech queue (vtable, its
// update in slot 1), +0x16 its line count (word), +0x24 its run state (0 stopped); the scene's intro music, a sound object
// whose slot 0x6c is "is playing" (thiscall, al).
GAME_ADDRESS kVoiceQueue = 0xbdbf04;       // [kVoiceQueue] = the queue, or 0
GAME_ADDRESS kVoiceQueueVtable = 0x6a464c;
GAME_ADDRESS kVoiceQueueUpdate = 0x46e430;
GAME_ADDRESS kSoundVtable = 0x6a47e8;
GAME_ADDRESS kSoundIsPlaying = 0x471790;
const uint32_t kVoiceCount = 0x16, kVoiceRunState = 0x24, kSceneIntroMusic = 0x1334, kSoundIsPlayingSlot = 0x6c / 4;
const uint32_t kIntroCap = 1250;           // the course intro's longest wait (25 s; natural ends ~15-21 s)
const DWORD kHoldBMs = 45000;              // hold B's longest wait for the others (kIntroCap, the racer intro's 5 s, room for loading gaps)
// The announcer (re\m7-announcer\FINDINGS.md): commentary resolves a line's hash through the resolver 0x46d3e0, whose
// `call edx` returns to kCommentResolverRet: a group's Select picks the recorded take (CRT rand), a take's GetSource
// gives its channel data. CommentMan +0x14/+0x18 its entries (+4 hash, +0x24 wait ms, +0x30 elapsed, +0x34 deferred,
// +0x35 active); a group +4 its hash, +0x14 its takes (count at +0x14, item i from kGroupChild: +0 take hash).
GAME_ADDRESS kCommentResolverRet = 0x46d423;
GAME_ADDRESS kCommentMan = 0xbdbf0c;       // [kCommentMan] = CommentMan, or 0
GAME_ADDRESS kCommentPlay = 0x46d6b0;      // thiscall(CommentMan, entry), ret 4 -> al: queued
GAME_ADDRESS kSpeechFlush = 0x460000;      // cdecl(): drops every queued line but the one playing
GAME_ADDRESS kGroupChild = 0x55afb0;       // thiscall(group + 0x14, i), ret 4 -> the take's item
GAME_ADDRESS kCommentRequest = 0x4bbec0;   // cdecl(cue, wait ms, flush): a commentary line, through the game's gates
GAME_ADDRESS kWrongWayCall = 0x4bcba0;     // cdecl(record index): the wrong-way line for that car (its own gates)
const uint32_t kSceneCommentMute = 0x1350;  // scene byte: 1 = no commentary (the game's own, native 095f)
const uint32_t kCmEntries = 0x14, kCmCount = 0x18, kEntryHash = 4, kEntryCooldown = 0x20, kEntryWait = 0x24, kEntryElapsed = 0x30,
               kEntryDeferred = 0x34, kEntryActive = 0x35, kGroupHash = 4, kGroupTakes = 0x14;
const uint32_t kRacerWrongWay = 0xa28;     // racer byte: 1 while it drives the wrong way (Human state 0x5af150)
// The lap calls (0x4bc290: the local player's crossings, `+0x74`): LAP2, LAP3, LAP4 at those crossings, FLAP at the last.
const uint32_t kLapCues[4] = {0xd95d80cd, 0x0b102b5c, 0xd07c64a9, 0xef87189c};
// Fair call-outs (re\m7-announcer2\FINDINGS.md §1, §2): the overtake line and the 45 s it speaks from (read only by
// 0x4bc180), the homing warning's cooldown (0x4bc9d0 speaks above 46), the places' racers, the race clock 0x4c4840 waits
// for (above 8), the game's "no commentary" byte (every line request checks it), the race's mode, the call-outs' gate
// byte, the drivers' 2D win / lose lines (thiscall(the car's voice +0x9f4, 0), ret 4), the generic warning's cue.
GAME_ADDRESS kOvertakeCall = 0x4bc180;      // cdecl(record index, gained, place): GAIN (LEAD at 1st) or LOST (TRAIL at last)
GAME_ADDRESS kOvertakeCooldown = 0xce7bd4;  // float seconds
GAME_ADDRESS kAlertCooldown = 0xce7bb8;     // float seconds (shared with the attack lines)
GAME_ADDRESS kPlaceList = 0xcf79d0;         // 8 bytes per place: +0 the racer index there
GAME_ADDRESS kRaceClock = 0xcf7a80;
GAME_ADDRESS kCommentOff = 0xc29fdb;
GAME_ADDRESS kRaceMode = 0xcf7a58;
GAME_ADDRESS kCalloutsLive = 0xcf7a51;
GAME_ADDRESS kVoiceWin = 0x54d330;
GAME_ADDRESS kVoiceLose = 0x54d350;
GAME_ADDRESS kGenericAlert = 0x6f5104;
// Racer +0x9f4 its voice (cues at +0x72c, 4 bytes each: ALERT 9, RESULT1-3 0x10-0x12, RESULT4 0x13, FINFAIL 0x14), +0x1cfc
// the homing thing locked on it (its +0xc0 a position; set by 0x536d10, whose change calls the warning).
const uint32_t kRacerVoice = 0x9f4, kVoiceCues = 0x72c, kRacerLock = 0x1cfc;
const uint32_t kCueAlert = 9, kCueResult1 = 0x10, kCueResult4 = 0x13, kCueFinFail = 0x14;
// Driver voices (re\m7-vo-triggers\FINDINGS.md): the voice's 7 line groups at +0x4b4 (POS, NEG, RCE_WON, RCE_LST, ATTACK,
// ALLSTAR, INTRO; a group is the line's sounder in the speech queue) and their cooldowns at +0x4d0 (seconds, +dt every physics
// tick; every trigger speaks only from 20 s up and sets it to 0 first, also when its 3D line is out of range and nothing is
// queued); a group +0xb0 its take count, +0xc0 shuffle, +0xcc RANDOM.
const uint32_t kVoiceGroups = 0x4b4, kVoiceCooldowns = 0x4d0, kGroupTakeCount = 0xb0, kGroupShuffle = 0xc0, kGroupRandom = 0xcc;
const int kVoiceKinds = 7;
const float kVoiceFresh = 5.0f;  // a cooldown that fell below this was set to 0 by a trigger (the vehicle's setup writes 20 s)
const float kVoiceReady = 20.0f;  // a trigger speaks only from this cooldown up ([0x6a4ae4])
const uint32_t kGroupMode = 0x28;  // a group's byte: 1 = 3D (set by 0x474a00 just before the line is queued)
const uint32_t kIntroKind = 6;
const uint32_t kIntroHoldTicks = 2500;  // an intro line not said here within 50 s is dropped
const int kVoiceInCap = 16;
GAME_ADDRESS kVoiceLine = 0x54d0a0;  // thiscall(voice, kind, byte 3D, wait ms), ret 0xc: a driver line, through the game's gates
// A sound group played straight (as the speech queue starts a line, 0x46e6e7): its mode (0x474a00, thiscall(group, byte 3D),
// ret 4: +0x28), +0xac set, then its Play (vtable slot 14, no args).
GAME_ADDRESS kGroupSetMode = 0x474a00;
GAME_ADDRESS kGroupPlay = 0x470bf0;
const uint32_t kGroupStarting = 0xac;
const int kGroupPlaySlot = 0x38 / 4;
const float kOvertakeReady = 45.0f, kAlertReady = 46.0f, kAlertNear = 50.0f;
// Item lines (re\m7-item-lines\FINDINGS.md): the HIT line's 45 s (0x4bc390 speaks from 45 and starts over; read only there,
// ticked in 0x4bbf60 outside the racer updates), the race clock in seconds (each car's lines run from 7.0, 0x4bd3b0), the
// generic HIT cue (table 0x6f50e0 entry 6), the ALLSTM line (cdecl(record index): the All-Star user's, its own gates) and
// Opa-Opa's weapon line (cdecl(record index, cue)). Racer +0xba0: 1 while the car spins or flips (SpinOut 0x4f0730, FlipOut
// 0x4f1fe0, a failed stunt, a crash; cleared when the spin timer ends in 0x4f3c50). The racer's message listener is at
// racer+0x2c0; message 0x28 (MSG_HIT_BY_REVENGE: an All-Star touched the car) carries the All-Star user's racer at +8.
GAME_ADDRESS kHitCooldown = 0xce7bb4;
GAME_ADDRESS kRaceSeconds = 0xce7ba8;
GAME_ADDRESS kGenericHit = 0x6f50f8;
GAME_ADDRESS kAllStarHitCall = 0x4bcd90;
GAME_ADDRESS kOpaLineCall = 0x4bc970;
const uint32_t kRacerSpinning = 0xba0, kCueHit = 6, kListenerInRacer = 0x2c0;
const int32_t kMsgHitByAllStar = 0x28;
const float kHitReady = 45.0f, kHitFrom = 7.0f;
// Opa-Opa's All-Star (the same findings §1): the holder's class object at racer+0x1ff0+0x40 (vtable kOpaVtable; its racer
// at +0x10) and its weapon at +0x184: 0 beam, 1 missiles, 2 lasers (COM_ALL_T_41/40/42), 4 until chosen. A human picks it from
// a menu; any other car (a copy of the All-Star on another screen) draws it at random (slot 9, 0x5e9aed).
GAME_ADDRESS kOpaVtable = 0x6bb3d8;
GAME_ADDRESS kOpaRamStart = 0x5e9580;  // thiscall(obj), no args: what slot 11 runs for weapon 1 besides the line (the grow/ram)
const uint32_t kAllStarObject = 0x40, kOpaRacer = 0x10, kOpaWeapon = 0x184;
const uint32_t kOpaShotTimer = 0x160;        // float: time since the last shot; slot 11 sets it to 100000 ([0x6bb4b4]) so the first shot comes at once
const float kOpaShotTimerStart = 100000.0f;
const uint32_t kOpaCues[3] = {0x131930b8, 0xefc87eef, 0xf6a28420};
// Opa-Opa's shots (re\m7-allstar-shots\FINDINGS.md §1, §3): in phase 4 the class asks its input slot (slot 7, data 0x6bb3f4;
// thiscall(obj, out[12]): steer, vertical, button down, button pressed) and fires when `down` is set and the shot timer +0x160
// passed the weapon's interval (then 0x5e9f4a resets it to 0: the only write of 0); weapon 1 has no shots: `down` starts the
// ram's 2.5 s grow pulse (+0x154 = 2.5 when it was <= 0.001). A copy (a non-human car) gets the computer racer's random input.
const uint32_t kOpaRamPulse = 0x154, kOpaInputDown = 8, kOpaInputPressed = 9, kOpaInputSize = 12;
const float kOpaRamPulseValue = 2.5f;
const uint32_t kOpaShotWait = 90;  // ticks a sent shot waits for its copy to fire it at most
// The other All-Star classes whose fire is a timed pulse (re\m7-allstar-shots\FINDINGS.md §2): the computer racer's input slot
// (slot 7 of each class's vtable) presses at random; the press starts the pulse (a float set to its value when it was <= 0.001).
// The All-Star object's racer is at +0x10 for every class (0x5a0080 reads it there).
const uint32_t kAllStarRacer = 0x10;
struct PulseClass {
    uint32_t vtable, pulse;
    float value;
    const wchar_t* name;
};
const PulseClass kPulseClasses[] = {
    {0x6b7c50, 0x178, 2.5f, L"Super Sonic"},  // slot 7 data 0x6b7c6c = 0x5de860; the pulse 0x5ddb80
    {0x6b8380, 0x1b0, 1.5f, L"AiAi"},         // 0x6b839c = 0x590e30; 0x5906f0
    {0x6b8008, 0x150, 1.5f, L"B.D. Joe"},     // 0x6b8024 = 0x5e4810
};
const int kPulseClassCount = sizeof(kPulseClasses) / sizeof(kPulseClasses[0]);
// The "gate" All-Star classes (re\m7-allstar-shots2\FINDINGS.md): their fire has a gate the copy can open and a proof the owner reads
// right around the class's phase-4 tick (vtable slot 12 `thiscall(obj, float* dt)`, hooked), with a payload for the aimed ones.
enum GateKind { kGateEggman, kGateShadow, kGateAlexKidd, kGateZobio, kGateRyo, kGateKnuckles, kGateAmy, kGateUlala, kGateBeat, kGateBonanza, kGateKindCount };
struct GateClass {
    uint32_t vtable;
    const wchar_t* name;
};
const GateClass kGateClasses[kGateKindCount] = {
    {0x6bb510, L"Eggman"},     // slot 7 data 0x6bb52c = 0x5eacc0, slot 12 0x6bb540 = 0x5eb050
    {0x6b7bc8, L"Shadow"},     // 0x6b7be4 = 0x5dcf80, 0x6b7bf8 = 0x5dd7f0
    {0x6b7700, L"Alex Kidd"},  // 0x6b771c = 0x5d5af0, 0x6b7730 = 0x5d5e20
    {0x6b7de8, L"Zobio"},      // 0x6b7e04 = 0x5e1390, 0x6b7e18 = 0x5e14c0
    {0x6b7a30, L"Ryo"},        // 0x6b7a4c = 0x5da420, 0x6b7a60 = 0x5da570 (both of Ryo's classes share the vtable)
    {0x6b7b40, L"Knuckles"},   // 0x6b7b5c = 0x5dba40, 0x6b7b70 = 0x5dc160
    {0x6b7810, L"Amy"},        // 0x6b782c = 0x5d86c0, 0x6b7840 = 0x5d8870 (re\m7-allstar-shots3\FINDINGS.md)
    {0x6b7d60, L"Ulala"},      // 0x6b7d7c = 0x5e07c0, 0x6b7d90 = 0x5e09e0
    {0x6b7ab8, L"Beat"},       // 0x6b7ad4 = 0x5dade0, 0x6b7ae8 = 0x5db390
    {0x6b7920, L"Bonanza Bros"},  // 0x6b793c = 0x5bc4a0; the tick is SLOT 23 0x6b797c = 0x5bd470 (thiscall(obj, blimp), from the blimp's update)
};
// Bonanza Bros (FINDINGS §4): the blimp's aim angle +0x150 (turned by out+0 every call, +-20 degrees), the state +0x1a4 (an Attack
// press sets 3 from 0/1 while the anim object +0x1a0 exists), the thrown byte +0x186 (the bomb goes when the clip is at 14 and it is 0;
// it becomes 1; state 0 clears it). A copy's aim follows the owner's stream, its throw is held until the owner's.
const uint32_t kBonanzaAim = 0x150, kBonanzaState = 0x1a4, kBonanzaThrown = 0x186, kBonanzaAnim = 0x1a0, kBonanzaBlimp = 0x174;
const float kBonanzaAimStep = 0.005f;  // the aim is sent again once it turned this much (radians), at most every 3 calls
// Amy: the swing anim set by the target zone +0x140 (3/4/5; 0 = alternate), its time +0x144 (1.5 s); a swing from anim 2 or 7 to 3/4/5.
// Ulala: a shot from anim 2 (to 6/7/8, or with the anim kept when the target car is gone: then only the game's projectile serial
// [0xcdd048] tells). Beat: held (+0x130 = 0.9) or released (1.1), no shots.
const uint32_t kAmySwing = 0x140, kAmySwingTimer = 0x144, kBeatMode = 0x130;
const float kAmySwingTime = 1.5f, kBeatHeldValue = 0.9f;
uintptr_t kProjectileSerial = 0xcdd048;  // (a variable: the tests point it at their own)
inline uint32_t ProjectileSerial() { return kProjectileSerial ? *(volatile uint32_t*)kProjectileSerial : 0; }
// The racer's controller (slot 6 reads the human's steer/vertical floats there), its vehicle (Alex Kidd's shot aims by its steering)
// and its animation state (0x59fe90: bits 10-13 of the word at [[racer+0x2044]+0xcc]+0xe).
const uint32_t kCtrlSteer = 0x5c, kCtrlVertical = 0x58, kCtrlButton = 0x24, kRacerVehicle = 0x9f4, kVehicleSteer = 0x984, kVehicleSteerOn = 0x980;  // (kRacerController below; the button state 2 = pressed this tick)
const uint32_t kRacerAnimObject = 0x2044, kAnimData = 0xcc, kAnimWord = 0xe;
const float kAlexSteerRate = 0.9f;  // [0x6f6b50]: the tick integrates out+0 into the steering by 0.9 * dt (or shrinks it by 1 - 0.9 * dt)
// Eggman: the gate timer +0x134 (reset to 0 by a shot), its limit +0x138 (0.75), the rocket pattern index +0x130 (decremented before the spawn).
const uint32_t kEggmanTimer = 0x134, kEggmanLimit = 0x138, kEggmanPattern = 0x130;
// Shadow: the gate timer +0x174 (counts down; a shot sets 0.5). Alex Kidd: the cooldown +0x130 (a shot sets 0.5..1.5). Zobio: the
// cooldown +0x138 (a fire needs <= 0 and anim 2 -> 6). Ryo: anim != 3 -> 3. Knuckles: the level +0x130 (1..3), the cooldown
// +0x134 (a punch sets 0.5), the charge +0x138 (a release at >= 0.25 punches).
const uint32_t kShadowTimer = 0x174, kAlexCooldown = 0x130, kZobioCooldown = 0x138, kKnucklesLevel = 0x130, kKnucklesCooldown = 0x134, kKnucklesCharge = 0x138;
const float kShadowShotValue = 0.5f, kKnucklesPunchValue = 0.5f, kKnucklesReleaseCharge = 0.25f;
const int kZobioReadyAnim = 2, kRyoLiftAnim = 3, kEggmanHitAnim = 6;
const uint32_t kElementTable = 0xc28340;
const int32_t kStateTrackIntro = 0x39, kStateCharIntro = 0x3a;
// The lobby's race bar (M7, re\m7-join-race\FINDINGS.md Q1): our progress for a player who joined meanwhile.
GAME_ADDRESS kRecordDistance = 0x51a000;   // thiscall(lap record) -> float: the car's distance since the start (0x519f40 - rec+0x88)
const uint32_t kGraphLapLength = 100;      // course graph: the lap's length (float)
const uint32_t kProgressEvery = 50;        // sim ticks between progress updates (once a second)
// The racer object is the car body (notes/RACE-BRIDGE.md "Pose"): position +0xc0 (x y z, metres, Y up),
// rotation +0xd0 (quaternion x y z w), velocity +0x100 (m/s), angular velocity +0x120, matrix-dirty
// byte +0x1e0, life state +0xa1c (2 = active).
const uint32_t kRacerPos = 0xc0, kRacerRot = 0xd0, kRacerVel = 0x100, kRacerSpin = 0x120, kRacerDirty = 0x1e0, kRacerLife = 0xa1c;
const uint32_t kRacerInitialState = 0x9e4;   // read by the grid sort (0x49f6e0) and racer reset
const uint32_t kRacerController = 0x9f0;
const uint32_t kRacerRecord = 0x9d8;         // index of the racer's lap record (= creation index)
const uint32_t kRacerAiHold = 0x1fc8;        // byte: 0x4eaa10's latch, 0928 sets / 0929 clears (the online pause's AI hand-over)
const uint32_t kRacerAiThrottle = 0x20fc;    // byte: track cursor updated every 8th tick only (set at creation for AI)
const uint32_t kRacerHumanNumber = 0x9dc;    // humans 0.. in creation order on this PC, AI 8: the results badge "nP" (0x58d9fa)
const uint32_t kStateGridHuman = 0x17, kStateNetwork = 0x15, kStateAiRacing = 3, kStateGridAi = 0x18, kStatePodium = 0x1a;
const uint32_t kCtrlTopState = 0x228;        // controller: its current (top) state object; the state id is at +4 (0x49d720)
const uint32_t kRacerItem = 0xc1c;           // the item held (0 = none); while any car holds the All-Star nobody gets one (0x49d7b0)
const uint32_t kRacerAllStarState = 0x1ff0;  // All-Star holder state: 0 or 1 = not running one
const int32_t kInertDevice = 15;             // a pad index no PC has (0..15 keeps tables in range)
// Items. Event: +4 kind; kinds 0-2 +8 the racer (creation index), +0xc the gadget; kinds 1/2 +0x20 the
// launch frame (+0 projectile type), quantizer vtables at +0xa8/+0xb8/+0xc4/+0xd0 (never copied).
const uint32_t kSceneEvents = 0x1374;        // the event manager; its +0x12 byte counts this window's posts
const uint32_t kMgrPosts = 0x12;
const uint8_t kPostCap = 0x10;               // more posts in one 15-tick window are freed unrun (0x5c084f)
const uint8_t kInjectRoom = 12;              // inject only while the window has room for 4 more of this game's own
const uint32_t kEvKind = 4, kEvRacer = 8, kEvGadget = 0xc, kEvThrow = 0x14, kEvFrame = 0x20;
const uint32_t kEvVtables[] = {0xa8, 0xb8, 0xc4, 0xd0};
const uint32_t kBoxMessage = 0x06cd52a4;     // an item box's message to the racer driving through it (racer slot 8)
const uint32_t kGadgetAllStar = 0xb678e022, kGadgetRoulette = 0x85d1e53c;
const uint32_t kAllStarEnd = 0x4b;           // All-Star struct: byte, nonzero = the active tick ends it (0x58680e)
const uint32_t kAllStarLength = 0x30;        // and 4 floats, its length in seconds, set at its start unless nonzero (0x585ed0)
const float kAllStarCopyLength = 60.0f;      // a copy of another car's All-Star runs until its owner's ends (at most this)
const uint32_t kAllStarElapsed = 0x20;       // and 4 floats, the time it has run (the time-up check compares them with the length)
// Metal Sonic's All-Star (both Metal Sonic mods; re\m9-metal-allstar\FINDINGS.md): its class lives in a block Metal's runtime
// allocates, so it is recognised by the vtable its core publishes (MetalMaximumOverdrive.dll's `metal_runtime_config`: magic,
// version 1, 64 bytes, armed 1, the vtable at +0x10). Its plates and beam close only once elapsed >= length, which a copy's 60 s
// length never reaches: the owner sends its time-up (kAllStarTimeUp) and the copy's length becomes its elapsed at the owner's tick.
const uint32_t kMetalConfigMagic = 0x5452534d;
const uint32_t kMetalTimeUpWait = 250;       // ticks a copy may wait (still phase 2/3 here) for the time-up to apply
// Once applied, the copy closes its plates and ends itself (the game's own time-up; slot 4 holds phase 4 while a plate is open). The
// owner's end (phase 5, EndAllStars' S+0x4b) can arrive a tick before this copy's plates are shut, and S+0x4b ends the All-Star
// at once: it waits this long first (the independent check of mods.8), then ends it as before.
const uint32_t kMetalCloseGrace = 100;
const uint32_t kCtrlThrow = 0xb8;            // controller float some thrown items read at launch (0x5351b0): carried at event +0x14
const uint32_t kEventMaxWait = 25;           // run an event at most 0.5 s after it arrived, even if its car isn't shown that far yet
const int kEventQueueCap = 32;               // per sender
// What an item box or roll gives (table 0x6f0b70, 2nd column) and 0xc01ccf24 (fired at once when given):
// a received event naming anything else is refused. Never the roulette (given and fired without events).
const uint32_t kGadgets[] = {0x84418b5f, 0x5f724da9, 0x7711877d, 0x11a9bc48, 0x9a67cb42, 0xd045a8ca, 0x0053f395, 0xeec55d13,
                             0x3ebcdded, 0x805c3c3b, 0x0f6a6e94, 0x84917c21, 0x53eb2f26, 0x26e96da5, 0xba8e1dca, 0xd8ae59cb,
                             0x953d2512, 0xb678e022, 0x734481a5, 0xf005c29d, 0x94749a0f, 0xc01ccf24};
// Driver reactions (re\m6-reactions\FINDINGS.md). Racer +0x2040 the reaction controller (may be 0): +0xb0 its pop
// timer (4 equal floats; pops when all are 0). Racer +0x2044 the character animation (may be 0): byte +0 bit 0x40
// "a reaction plays" (set by 0x502f60, cleared at its end or a reset), +0xc4 its runner (may be 0): +0 base state
// (4 = can react, 0x502f90), +0x1c free instances, +0x20 the active list (newest first; +0x88 node, +0x8c next);
// node +0xc its hash.
const uint32_t kRacerReactions = 0x2040, kRacerCharAnim = 0x2044, kRcTimer = 0xb0;
const uint8_t kCaReacting = 0x40;
const uint32_t kCaRunner = 0xc4, kRunnerFree = 0x1c, kRunnerActive = 0x20, kInstNode = 0x88, kInstNext = 0x8c, kNodeHash = 0xc;
const int32_t kRunnerDriving = 4;
// The nodes the reaction pop plays (0x4c4410, 0x4c34e0): Lose, Win, Positive, Negative, Showboat, fired forwards,
// fired backwards, and the taunts NodeTauntSelectFwd/Back/Right/Left.
const uint32_t kReactionNodes[] = {0x46770a3a, 0x9db1d546, 0xa7a4b00f, 0x7cad4e8c, 0x1b13cf01, 0x68bd232b, 0x7e91a0e2,
                                   0xf27a8e21, 0x2066a85d, 0xffbd20ff, 0xf5c339db};
const uint32_t kReactionWait = 25;           // a received reaction waits at most 0.5 s for its car to be able to play it
// Track cursor = controller + 0x80: +0x30 current nav path, +0x80 last in-range path, +0xac forced
// path. Course graph: +0x1c nav path count, +0x48 nav path array (0x20 bytes each).
const uint32_t kCtrlTracker = 0x80, kTrackPath = 0x30, kTrackLastPath = 0x80, kTrackForced = 0xac;
const uint32_t kGraphPathCount = 0x1c, kGraphPaths = 0x48, kPathSize = 0x20;
// Lap record (0x90 bytes): +0x60 best lap, +0x68 time of the last counted line crossing (the finish
// time once finished; 1/4096 tick since GO), +0x74 counted crossings, +0x7c lap shown, +0x80
// position (0-based), +0x84 crossings that finish the race, +0x8c the racer.
const uint32_t kRecSize = 0x90, kRecBest = 0x60, kRecTime = 0x68, kRecCrossings = 0x74, kRecLap = 0x7c, kRecPosition = 0x80,
               kRecFinishAt = 0x84, kRecRacer = 0x8c;
const uint32_t kSceneHumans = 0x1104;        // human racers the race end waits for (dword)
const uint32_t kWffTimerState = 0x10;        // WaitingForFinishers phase object: -1 = race-end timer not started
const uint32_t kGraceTicks = 250;            // at the timer's end, wait up to 5 s more for the late player's own result
const uint32_t kResultsWaitTicks = 500;      // guest: hold the race end up to 10 s for the host's results
const uint32_t kAiPoseEvery = 2;             // host: the AI go out every 2nd sim tick (25 per second); its own car every tick
const float kSnapMetres = 10.0f;             // a car this far from its last sent pose jumped (respawn): don't blend across

static_assert(kWireMax <= kRaceMsgMax, "the race message pipe carries every wire message");

const char kNameGridHuman[] = "STATE_Control_StartingGridHuman";

typedef void(__thiscall* UpdateFn)(void* self);
typedef void(__stdcall* SeedFn)(uint32_t seed);
typedef void(__thiscall* SetStateFn)(void* controller, uint32_t id);
typedef void(__thiscall* SetPositionFn)(void* racer, const float* pos4);
typedef void(__thiscall* CommitFn)(void* racer, int views, void* viewList);
typedef void(__thiscall* ApplyNavFn)(void* racer, uint32_t nav);
typedef void(__cdecl* AddFinishFn)(uint32_t index, uint32_t reason);
typedef void(__cdecl* StartTimerFn)(uint32_t seconds);
typedef void(__thiscall* ClearItemsFn)(void* racer, int unused);
typedef void(__thiscall* PostFn)(void* manager, void* event);
typedef void*(__stdcall* EventAllocFn)(int32_t kind);
typedef bool(__thiscall* PlayReactionFn)(void* charAnim, uint32_t node);
typedef void(__stdcall* EventFreeFn)(void* event);
typedef void*(__cdecl* ProjectileTypeFn)(uint32_t type);
typedef int(__thiscall* RacerMessageFn)(void* racer, void* message);
typedef void(__cdecl* OpaLineCallFn)(int32_t recordIndex, uint32_t cue);  // 0x4bc970: Opa-Opa's weapon line (its own gates)

// ---------------------------------------------------------------- hooks
void __cdecl Hook0905(void* vm, NativeCall* d);
void __cdecl Hook0900(void* vm, NativeCall* d);
void __cdecl Hook090a(void* vm, NativeCall* d);
void __cdecl Hook0912(void* vm, NativeCall* d);
void __fastcall HookSetupRace(void* self, void*);
void __fastcall HookWaiting(void* self, void*);
void __fastcall HookWaitingNoIntro(void* self, void*);
void __fastcall HookTrackIntro(void* self, void*);
void __fastcall HookCharacterIntro(void* self, void*);
void __fastcall HookCountdown(void* self, void*);
void __fastcall HookRacing(void* self, void*);
void __fastcall HookRacerCommit(void* self, void*, int views, void* viewList);
void __fastcall HookWaitForFinishers(void* self, void*);
void __cdecl Hook1b01(void* vm, NativeCall* d);
void __cdecl Hook0916(void* vm, NativeCall* d);
void __cdecl Hook2003(void* vm, NativeCall* d);
void __cdecl Hook1304(void* vm, NativeCall* d);
void __cdecl Hook0913(void* vm, NativeCall* d);
void __fastcall HookEventPost(void* manager, void*, uint8_t* ev);
int __fastcall HookRacerMessage(void* racer, void*, uint8_t* message);
void __fastcall HookExhaust(uint8_t* exhaust, void*);
void __fastcall HookCatchup(uint8_t* state, void*);
uint32_t __fastcall HookTakeSelect(uint8_t* group, void*, uint32_t arg);
uint32_t __fastcall HookTakeSource(uint8_t* take, void*, uint32_t arg);
void __fastcall HookSpeechUpdate(void* queue, void*, uint32_t ms);
void __fastcall HookRacerTrack(uint8_t* racer, void*);
void __fastcall HookRaceEnd(void* self, void*);
void __fastcall HookRacerUpdate(uint8_t* racer, void*);
int __fastcall HookRacerListener(uint8_t* listener, void*, int32_t* message);
void __fastcall HookOpaStart(uint8_t* obj, void*);
void __fastcall HookOpaInput(uint8_t* obj, void*, uint8_t* out);
void __fastcall HookPulseInput(uint8_t* obj, void*, uint8_t* out);
void __fastcall HookGateInput(uint8_t* obj, void*, uint8_t* out);
void __fastcall HookGateTick(uint8_t* obj, void*, float* dt);
void __fastcall HookSteerHuman(uint8_t* state, void*);
void __fastcall HookSteerErratic(uint8_t* state, void*);
void __fastcall HookSteerGrid(uint8_t* state, void*);

struct Hook {
    uint32_t slot, original;
    void* fn;
    const wchar_t* what;
};
Hook g_hooks[] = {
    {0x6f6004, 0x58cb00, (void*)&Hook0905, L"native 0905 (create racer)"},
    {0x6f5ff0, 0x58d550, (void*)&Hook0900, L"native 0900 (race rules)"},
    {0x6f6018, 0x58d5c0, (void*)&Hook090a, L"native 090a (race teardown)"},
    {0x6b204c, 0x5b0cd0, (void*)&HookSetupRace, L"SetupRace update"},
    {0x6b20b8, 0x5b0f50, (void*)&HookWaiting, L"WaitingToStart update"},
    {0x6b20dc, 0x5b0f40, (void*)&HookWaitingNoIntro, L"WaitingToStartNoIntro update"},
    {0x6b2100, 0x5b1120, (void*)&HookTrackIntro, L"TrackIntro update"},
    {0x6b2124, 0x5b1640, (void*)&HookCharacterIntro, L"CharacterIntro update"},
    {0x6b2148, 0x5b1ac0, (void*)&HookCountdown, L"Countdown update"},
    {0x6b216c, 0x5b2160, (void*)&HookRacing, L"Racing update"},
    {0x6afbe4, 0x4f3c50, (void*)&HookRacerCommit, L"racer end-of-tick (vtable slot 13)"},
    {0x6b2190, 0x5b2380, (void*)&HookWaitForFinishers, L"WaitingForFinishers update"},
    {0x6f6038, 0x58de90, (void*)&Hook0912, L"native 0912 (results row)"},
    {0x6f5c2c, 0x602100, (void*)&Hook1b01, L"native 1b01 (blackboard read)"},
    {0x6f6048, 0x58d5a0, (void*)&Hook0916, L"native 0916 (game type)"},
    {0x6f6994, 0x5f9620, (void*)&Hook2003, L"native 2003 (settings read)"},
    {0x6f6558, 0x5fe3f0, (void*)&Hook1304, L"native 1304 (save)"},
    {0x6f603c, 0x58ded0, (void*)&Hook0913, L"native 0913 (results data)"},
    // Items: a group of their own (kItemHooks): if another mod holds these slots, online races still run, without item sync.
    {0x6b9c40, 0x5c0820, (void*)&HookEventPost, L"event manager post (vtable slot 4)"},
    {0x6afbd0, 0x4f4f20, (void*)&HookRacerMessage, L"racer message (vtable slot 8)"},
    // Exhaust effects: a group of their own (kFxHooks): without it, the other players' cars show no boost effects.
    {0x6af4fc, 0x580400, (void*)&HookExhaust, L"exhaust effect update (vtable slot 5)"},
    // Catch-up: a group of their own (kCatchupHooks), slot 11 of the 3 Human states; without it, CATCH-UP does nothing online.
    {0x6b1bac, 0x5ae2b0, (void*)&HookCatchup, L"Human catch-up (vtable slot 11)"},
    {0x6b1bdc, 0x5ae2b0, (void*)&HookCatchup, L"Human_Erratic catch-up (vtable slot 11)"},
    {0x6b1c0c, 0x5ae2b0, (void*)&HookCatchup, L"Human_MonkeyBall catch-up (vtable slot 11)"},
    // The announcer: a group of its own (kAnnouncerHooks); without it each screen keeps its own commentary.
    {0x6a4fdc, 0x476830, (void*)&HookTakeSelect, L"sound group take pick (vtable slot 7)"},
    {0x6a5530, 0x479a90, (void*)&HookTakeSource, L"sound take source (vtable slot 7)"},
    {0x6a4650, 0x46e430, (void*)&HookSpeechUpdate, L"speech queue update (vtable slot 1)"},
    // Fair call-outs: a group of their own (kFairHooks), with the announcer's; without it the host's commentary picks
    // its subjects as the stock does (the local player first).
    {0x6afbbc, 0x4f1eb0, (void*)&HookRacerTrack, L"racer tracker (vtable slot 3)"},
    {0x6b21b0, 0x5b23b0, (void*)&HookRaceEnd, L"race finished enter (vtable slot 1)"},
    // Fair item lines: a group of their own (kLineHooks), with the fair call-outs'; without it the host's HIT and ALLSTM lines
    // follow its own screen's view of the other players' cars (their real hits never reach it).
    {0x6afbc0, 0x4f3990, (void*)&HookRacerUpdate, L"racer update (vtable slot 4)"},
    {0x6afba8, 0x4ee020, (void*)&HookRacerListener, L"racer message listener (vtable slot 1)"},
    // Opa-Opa's All-Star: a group of its own (kOpaHooks), with the items'; without it a copy of the All-Star on another screen
    // fires a weapon drawn at random, at random moments.
    {0x6bb404, 0x5e9b10, (void*)&HookOpaStart, L"Opa-Opa All-Star start (vtable slot 11)"},
    {0x6bb3f4, 0x5ea2e0, (void*)&HookOpaInput, L"Opa-Opa All-Star input (vtable slot 7)"},
    // The pulse All-Stars' inputs (kPulseClasses, in that order): a group of their own (kPulseHooks), with the items'.
    {0x6b7c6c, 0x5de860, (void*)&HookPulseInput, L"Super Sonic All-Star input (vtable slot 7)"},
    {0x6b839c, 0x590e30, (void*)&HookPulseInput, L"AiAi All-Star input (vtable slot 7)"},
    {0x6b8024, 0x5e4810, (void*)&HookPulseInput, L"B.D. Joe All-Star input (vtable slot 7)"},
    // The gate All-Stars (kGateClasses, in that order; the input slot 7 then the phase-4 tick slot 12 of each): a group of their
    // own (kGateHooks), with the items'. re\m7-allstar-shots2\FINDINGS.md.
    {0x6bb52c, 0x5eacc0, (void*)&HookGateInput, L"Eggman All-Star input (vtable slot 7)"},
    {0x6bb540, 0x5eb050, (void*)&HookGateTick, L"Eggman All-Star tick (vtable slot 12)"},
    {0x6b7be4, 0x5dcf80, (void*)&HookGateInput, L"Shadow All-Star input (vtable slot 7)"},
    {0x6b7bf8, 0x5dd7f0, (void*)&HookGateTick, L"Shadow All-Star tick (vtable slot 12)"},
    {0x6b771c, 0x5d5af0, (void*)&HookGateInput, L"Alex Kidd All-Star input (vtable slot 7)"},
    {0x6b7730, 0x5d5e20, (void*)&HookGateTick, L"Alex Kidd All-Star tick (vtable slot 12)"},
    {0x6b7e04, 0x5e1390, (void*)&HookGateInput, L"Zobio All-Star input (vtable slot 7)"},
    {0x6b7e18, 0x5e14c0, (void*)&HookGateTick, L"Zobio All-Star tick (vtable slot 12)"},
    {0x6b7a4c, 0x5da420, (void*)&HookGateInput, L"Ryo All-Star input (vtable slot 7)"},
    {0x6b7a60, 0x5da570, (void*)&HookGateTick, L"Ryo All-Star tick (vtable slot 12)"},
    {0x6b7b5c, 0x5dba40, (void*)&HookGateInput, L"Knuckles All-Star input (vtable slot 7)"},
    {0x6b7b70, 0x5dc160, (void*)&HookGateTick, L"Knuckles All-Star tick (vtable slot 12)"},
    {0x6b782c, 0x5d86c0, (void*)&HookGateInput, L"Amy All-Star input (vtable slot 7)"},
    {0x6b7840, 0x5d8870, (void*)&HookGateTick, L"Amy All-Star tick (vtable slot 12)"},
    {0x6b7d7c, 0x5e07c0, (void*)&HookGateInput, L"Ulala All-Star input (vtable slot 7)"},
    {0x6b7d90, 0x5e09e0, (void*)&HookGateTick, L"Ulala All-Star tick (vtable slot 12)"},
    {0x6b7ad4, 0x5dade0, (void*)&HookGateInput, L"Beat All-Star input (vtable slot 7)"},
    {0x6b7ae8, 0x5db390, (void*)&HookGateTick, L"Beat All-Star tick (vtable slot 12)"},
    {0x6b793c, 0x5bc4a0, (void*)&HookGateInput, L"Bonanza Bros All-Star input (vtable slot 7)"},
    {0x6b797c, 0x5bd470, (void*)&HookGateTick, L"Bonanza Bros All-Star blimp tick (vtable slot 23)"},
    // Drift+ off (kContractNoDriftPlus): the human steering states' updates (vtable slot 3), a group of their own (kSteerHooks);
    // without it a controller keeps its stick + d-pad sum online. re\drift-plus\FINDINGS.md.
    {0x6b1b8c, 0x5af150, (void*)&HookSteerHuman, L"Human update (vtable slot 3)"},
    {0x6b1bbc, 0x5af6a0, (void*)&HookSteerErratic, L"Human_Erratic update (vtable slot 3)"},
    {0x6b1ca0, 0x5a4f60, (void*)&HookSteerGrid, L"StartingGridHuman update (vtable slot 3)"},
};
const int kHookCount = sizeof(g_hooks) / sizeof(g_hooks[0]);
const int kSteerHooks = 3;      // the last entries
const int kGateHooks = 20;      // the entries before them (2 per gate class)
const int kPulseHooks = 3;      // the entries before them
const int kOpaHooks = 2;        // the entries before them
const int kLineHooks = 2;       // the entries before it
const int kFairHooks = 2;       // the entries before them
const int kAnnouncerHooks = 3;  // the entries before them
const int kCatchupHooks = 3;  // the entries before them
const int kFxHooks = 1;       // the entry before them
const int kItemHooks = 2;     // the entries before it
const int kCoreHooks = kHookCount - kSteerHooks - kItemHooks - kFxHooks - kCatchupHooks - kAnnouncerHooks - kFairHooks - kLineHooks - kOpaHooks - kPulseHooks - kGateHooks;
bool g_installed = false;
bool g_gateHooks = false;       // the gate All-Stars' group is installed
bool g_steerHooks = false;      // the Drift+ group is installed
bool g_forceDriftPlusOff = false;  // config.ini [Debug] ForceDriftPlusOff: as host, Drift+ off without a keyboard player
uint32_t g_debugStallMs = 0;       // config.ini [Debug] StallMs: freeze this game once, 10 s into each online race (a stutter to test)
bool g_announcerHooks = false;  // the announcer group is installed
bool g_fairHooks = false;       // the fair call-outs group is installed
bool g_lineHooks = false;       // the fair item lines group is installed
bool g_opaHooks = false;        // the All-Star weapon group is installed
bool g_pulseHooks = false;      // the pulse All-Stars' input group is installed
bool g_progressClearPending = false;  // the race's "not racing" member data still to post (the command queue was full)
bool g_itemHooks = false;     // the item group is installed
bool g_fxHooks = false;       // the exhaust group is installed
bool g_catchupHooks = false;  // the catch-up group is installed

template <typename T> T Original(int i) { return (T)(uintptr_t)g_hooks[i].original; }
enum { kH0905, kH0900, kH090a, kHSetup, kHWaiting, kHWaitingNoIntro, kHTrackIntro, kHCharIntro, kHCountdown, kHRacing, kHRacerCommit, kHWff, kH0912,
       kH1b01, kH0916, kH2003, kH1304, kH0913, kHEventPost, kHRacerMessage, kHExhaust, kHCatchup,
       kHTakeSelect = kHCatchup + 3, kHTakeSource, kHSpeechUpdate, kHRacerTrack, kHRaceEnd, kHRacerUpdate, kHRacerListener, kHOpaStart, kHOpaInput,
       kHPulseSonic, kHPulseAiAi, kHPulseJoe, kHGateFirst };  // gate class c: input kHGateFirst + 2c, tick + 2c + 1
const int kHSteerHuman = kHGateFirst + kGateHooks, kHSteerErratic = kHSteerHuman + 1, kHSteerGrid = kHSteerHuman + 2;
static_assert(kHEventPost == kCoreHooks && kHExhaust == kCoreHooks + kItemHooks && kHCatchup == kHExhaust + kFxHooks &&
              kHTakeSelect == kHCatchup + kCatchupHooks && kHRacerTrack == kHTakeSelect + kAnnouncerHooks &&
              kHRacerUpdate == kHRacerTrack + kFairHooks && kHOpaStart == kHRacerUpdate + kLineHooks &&
              kHPulseSonic == kHOpaStart + kOpaHooks && kHGateFirst == kHPulseSonic + kPulseHooks && kPulseHooks == kPulseClassCount &&
              kHGateFirst + kGateHooks + kSteerHooks == kHookCount && kGateHooks == 2 * kGateKindCount,
              "the hook groups match the table");

// The host's items and hazards written into this profile's active settings record for the race (the
// game reads them there, not from the script). The profile's own values are put back before any save
// (1304), when the race is closed (090a) and when the mod unloads. Kept outside `g` (EndRace clears it).
struct RulesOverride {
    uint8_t* record = nullptr;  // non-null while the host's rules are in it
    uint32_t items = 0;
    uint8_t hazards = 0;
} g_rules;

// A race our lobby started is in progress or just ended: its results and pause menus offer RETURN TO
// LOBBY, and the menus come back to the lobby (notes/RETURN-TO-LOBBY.md). Kept outside `g`, which
// EndRace clears when the race is closed (090a), before the menus start.
bool g_returnToLobby = false;
// The open pause menu is the online one (set at its open site, Hook0916; cleared when the race is closed).
bool g_netPause = false;
// The online pause menu is open right now: set at its open site, cleared at its unpause (CONTINUE) and race-end
// close sites and when the race is closed. (g_netPause stays set for the race: it only tells the pause sites apart.)
bool g_pauseOpen = false;
// The sim tick of the last Countdown, Racing or WaitingForFinishers update (any race, ours or not).
uint32_t g_raceUpdateTick = 0;
// A license (profile) was picked this run (2004(20, i) with a used slot): a Steam join may leave a race.
bool g_licensePicked = false;
// The Steam join this game left a race for (once per request).
uint64_t g_joinQuitFor = 0;

// config.ini [Race] HostRunsAI (default 1): this game, as host, drives the AI for everyone (contract flag).
bool g_hostAiConfig = true;
// config.ini [Race] ItemSync (default 1): this game, as host, has every car's items decided by its owner (contract flag).
bool g_itemsConfig = true;

// ---------------------------------------------------------------- race state (game thread)
enum Phase {
    kIdle,       // nothing arranged
    kArmed,      // contract armed, the lobby script is starting the race
    kCreating,   // racers being created (0905 calls)
    kCreated,    // all 8 created; waiting for SetupRace
    kSetUp,      // grid placed; waiting for hold A
    kHoldA,      // loaded, held at WaitingToStart
    kIntros,     // released; intros playing
    kHoldB,      // held before the first countdown step
    kRunning,    // countdown and race (stock code)
    kFailed,     // the contract could not be applied: the race (if any) runs as a normal offline race
};

struct Peer {
    uint64_t id;
    int index;        // creation index (= roster entry)
    uint8_t reached;  // highest hold stage this peer reported
    bool gone;
    bool finished;    // its Finished arrived (fin); applied once to its lap record (applied)
    bool applied;
    RaceFinish fin;
    bool itemsClosed;  // gone: its queued item events dropped, its All-Star copies ended
    bool carHandled;   // gone: its car handed to an AI (CarLeft)
    uint8_t abortReason;  // gone through an Abort message: its reason (0: gone from the lobby, no message)
    bool test;        // a test player (lobby.h): no game behind it; the host's game drives its car and casts its votes
};

// A car this game shows from the network: another player's, or (guest) one of the host's AI.
struct Car {
    Playout play;
    int navChanges;   // nav path changes applied to it
};

struct Race {
    Phase phase = kIdle;
    RaceContract c = {};
    bool host = false;
    int local = -1;
    int calls = 0;
    void* racers[kGridSlots] = {};
    Peer peers[kMaxHumans];
    int peerCount = 0;
    uint8_t reached = 0;   // highest hold stage this game reached
    uint8_t released = 0;  // highest hold stage released
    DWORD holdSince = 0;
    bool courseLogged = false;
    bool launched = false;  // the lobby script has sent the start messages
    DWORD launchedAt = 0;
    DWORD lastLoadedSent = 0;  // holds resend Loaded every 2 s (idempotent; covers a lost message)
    bool finishSent = false;   // our Finished went out
    bool resultsStarted = false;  // this game's results started (their RESULTS_START: results 0x1ae5)
    bool dnfMarked = false;    // the race end's unfinished players marked DNF (MarkPlayersDnf)
    bool dnfOrdered = false;   // and the DNF rows moved last (DnfLast)
    // The announcer (re\m7-announcer\FINDINGS.md): a guest's own commentary muted while it says the host's lines; the
    // host's group being resolved (its Select comes before its take's GetSource); the line a guest is replaying (its
    // take forced); the host's lines waiting to be said here; the lap-call byte last seen (it changes at a flush).
    bool announceMuted = false, introTakeLogged = false;
    uint32_t pendingGroup = 0, pendingTake = 0, forceCue = 0, forceTake = 0;
    // Host: the lap calls follow the leader (user, 2026-10-22): its own player's are held (their entries' cooldowns,
    // kept here); the most crossings any car has made; the next line follows a flush. Guest: its car's wrong way.
    bool lapsHeld = false, flushNext = false, wrongWay = false;
    uint32_t lapCooldown[4] = {};
    uint32_t leaderCrossings = 0;
    RaceAnnounce lines[16] = {};
    uint32_t lineArrived[16] = {};  // the sim tick each waiting line arrived (a full speech queue keeps it waiting a while)
    int lineCount = 0, announceSent = 0, announceSaid = 0, announceMissed = 0, announceWaited = 0;
    // Host, fair call-outs (user, 2026-10-22): each car's mentions this race and the sim tick of its last (| 1; 0: none), the
    // homing lock each car had at its last commit, the recordings said this race; log line counts.
    int mentions[kGridSlots] = {};
    uint32_t mentionTick[kGridSlots] = {};
    uintptr_t lockSeen[kGridSlots] = {};
    uint32_t takesUsed[256] = {};
    bool placeCalled[kGridSlots] = {};  // the player's place line was said (at their finish, or at the race end)
    void* placeSounders[4] = {};        // the last place line's sounders (one per part) while the announcer says it
    int placeParts = 0, placeTalker = -1;
    int takeCount = 0, passLogged = 0, alertLogged = 0, freshLogged = 0;
    // Fair item lines (re\m7-item-lines\FINDINGS.md). Host: each car spinning now (the cars this game drives: their flag; the
    // other players' cars: their games' reports) and whether this spin has had its line decision; guest: its own car's spin
    // flag last reported. Log line counts.
    bool spinNow[kGridSlots] = {};
    bool spinDecided[kGridSlots] = {};
    bool spinning = false;
    int hitLogged = 0, hitSaid = 0, hitReports = 0, allStarHitLogged = 0, allStarHitSaid = 0, allStarHitMuted = 0;
    // Opa-Opa's All-Star weapon (re\m7-opa\FINDINGS.md): the owner sends it with the phase once the All-Star is active (the
    // one last sent per car); the others write it into their copy as it becomes active, or, when the copy got there first
    // (it fires nothing meanwhile), as soon as it arrives.
    int opaWeaponSent[kGridSlots] = {kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon};
    int opaWeapon[kGridSlots] = {kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon};
    bool opaPending[kGridSlots] = {};  // the copy became active before the weapon arrived: apply it when it does
    int opaLogged = 0;
    // Metal Sonic's All-Star time-up (owner: sent for this All-Star; copy: due here, since that tick).
    bool metalTimeUpSent[kGridSlots] = {};
    bool metalTimeUp[kGridSlots] = {};
    uint32_t metalSince[kGridSlots] = {};
    bool metalClosing[kGridSlots] = {};       // copy: the time-up was applied (its plates close by themselves)
    uint32_t metalClosingSince[kGridSlots] = {};
    int metalLogged = 0;
    // Opa-Opa's shots (re\m7-allstar-shots\FINDINGS.md §3). Owner: the shots sent per car (a running count), the ram pulse last
    // seen. Copy: a sent shot due here (its weapon, the tick it arrived); counts.
    uint8_t opaShotCount[kGridSlots] = {};
    float opaRamSeen[kGridSlots] = {};
    bool opaShotArmed[kGridSlots] = {true, true, true, true, true, true, true, true};  // owner: the shot timer has run (> 0) since the last shot sent (a 0 then is a new shot)
    bool opaShotDue[kGridSlots] = {};
    bool opaShotForced[kGridSlots] = {};  // copy: the gate was opened last tick (the proof is read at the next race update)
    int opaShotWeapon[kGridSlots] = {kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon, kNoWeapon};
    uint32_t opaShotSince[kGridSlots] = {};
    int opaShotsSent = 0, opaShotsIn = 0, opaShotsFired = 0, opaShotsDropped = 0, opaShotLogged = 0;
    // The pulse All-Stars (kPulseClasses): the owner's pulse last seen and shots sent per car; a copy's due pulse (its class),
    // whether its gate was opened last tick, when it arrived; counts.
    float pulseSeen[kGridSlots] = {};
    uint8_t pulseCount[kGridSlots] = {};
    bool pulseDue[kGridSlots] = {}, pulseForced[kGridSlots] = {};
    int pulseClass[kGridSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t pulseSince[kGridSlots] = {};
    // The gate All-Stars (kGateClasses): per car: a copy's due shot (its class, payload, when it arrived; armed = forced for this
    // tick, the input hook says "fire"), its mirrored hold (Knuckles); the owner's input as our hook saw it this tick (the host's
    // computer racers: slot 7; a human's comes from its controller), its hold start sent; counts for the log.
    int gateClass[kGridSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};
    bool gateDue[kGridSlots] = {}, gateArmed[kGridSlots] = {}, gateHold[kGridSlots] = {}, gateHoldSent[kGridSlots] = {}, gateInputSeen[kGridSlots] = {};
    bool gateInputPressed[kGridSlots] = {};  // the host's computer racer pressed this tick (our input hook saw it)
    uint32_t gatePayload[kGridSlots][2] = {}, gateInput[kGridSlots][2] = {};
    uint32_t gateSince[kGridSlots] = {};
    // Bonanza Bros: a copy's due Attack (and when it arrived), the owner's aim stream as the copy has it; the owner's last aim sent
    // and the calls since.
    bool gateAttackDue[kGridSlots] = {}, gateAimSeen[kGridSlots] = {};
    uint32_t gateAttackSince[kGridSlots] = {};
    float gateAim[kGridSlots] = {}, gateAimSent[kGridSlots] = {};
    uint8_t gateAimCalls[kGridSlots] = {};
    uint8_t gateCount[kGridSlots] = {};
    float gateTickDt = 0.0f;
    int gateSent = 0, gateIn = 0, gateFired = 0, gateDropped = 0, gateLogged = 0;
    int pulseSent = 0, pulseIn = 0, pulseFired = 0, pulseDropped = 0, pulseLogged = 0;
    // Driver voices (log only, the mirror's first step): each car's cooldowns last seen, its lines counted and those that reached
    // this game's speech queue, per kind; log line counts (the players' cars have their own).
    float voiceCool[kGridSlots][kVoiceKinds] = {};
    bool voiceSeen[kGridSlots] = {};
    uint16_t voiceLines[kGridSlots][kVoiceKinds] = {}, voiceQueued[kGridSlots][kVoiceKinds] = {};
    int voiceLogged = 0, voiceLoggedHuman = 0;
    // The players' racers' lines on every screen (re\m7-vo-triggers\FINDINGS.md): the other players' lines to say here (an intro
    // line waits for this game's racer intro: its enter flushes the speech queue) and the sim tick each arrived; the racer
    // intro's update has run here; counts and log lines.
    RaceVoice voiceIn[kVoiceInCap] = {};
    uint32_t voiceInTick[kVoiceInCap] = {};
    // Intro lines waiting to be said straight (StartIntros: a free stream; in game only about 3 play at once): since when, the tries.
    bool introRetry[kGridSlots] = {};
    uint32_t introRetrySince[kGridSlots] = {};
    int introTries[kGridSlots] = {};
    int voiceInCount = 0, voiceSent = 0, voicePlayed = 0, voiceUnheard = 0, voiceDropped = 0, voiceMirrorLogged = 0;
    bool charIntroRan = false, voiceMuteLogged = false;
    bool finishSendFailed = false;
    bool graceOn = false, graceLogged = false;
    uint32_t graceSince = 0;   // sim tick the grace at the race-end timer's end began
    bool overLogged = false, resultsLogged = false;
    int badgeRows = 0;         // results rows drawn with the lobby's player numbers
    int rowHuman = -1;         // the human whose results row 0912 built last (-1: an AI's, or another call)
    bool rowNameLogged = false;
    // VIEW PROFILE / GAMER CARD (re\m7-profile\FINDINGS.md): the human at each results row (a place; -1: a computer racer's),
    // the rows built, and the profile cursor: its row, what this frame changed (the row to un-highlight / highlight, the
    // footer), frames stepped, the last F1.
    int placeHuman[kGridSlots] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int placeRows = 0;
    int profileRow = -1, profileOff = -1, profileOn = -1, profileFooter = 0, profileFrames = 0;
    DWORD profileF1At = 0;
    int titleRacer = -1;       // the pause title's car, numbered by its lobby slot during that one 0f0f call
    int32_t titleSaved = 0;    // its own human number, put back right after
    bool titleLogged = false;
    bool quitBoxLogged = false;
    int crossingLogs = 0;  // catch-up: line crossings measured as of the last race update (CountedDistance), logged up to 20
    bool tipsLogged = false;
    bool difficultySet = false;  // the bridge's difficulty read gave the host's (Hook2003)
    bool racersSet = false;      // the bridge's racer count read gave Cars() (Hook2003)
    int32_t laps[kGridSlots] = {};  // last lap shown (record +0x7c) per human, for the log
    Car cars[kGridSlots];
    bool remote[kGridSlots] = {};   // this game writes car k's pose from the network
    bool racing = false;            // the race clock runs (a Racing or WaitingForFinishers update ran)
    // The race clock on real time (ClockObserve / ClockCatchUp): the frame last seen (its time, the step count then) and the
    // first one's (the bases), the dropped time D (ms) at the last frame start seen and at the countdown's release, the
    // ticks added to the race clock since; for the log: the catch-ups, the largest, lines written.
    bool clockSeen = false, clockValid = false, clockBase = false, clockBroken = false, clockStalled = false;
    uint32_t clockT = 0, clockStep = 0, clockT0 = 0, clockStep0 = 0;
    double clockD = 0, clockD0 = 0;
    uint32_t clockAdded = 0, clockBiggest = 0;
    int clockFixes = 0, clockLogged = 0;
    char names[kMaxHumans][32] = {};  // the humans' Steam names (lobby at Arm), for the HUD's tags
    bool tagLogged[5] = {};           // the tags' log lines, once per race: gate, car, name, unknown label, a computer racer's name
    // Exhaust effects: the owner's flame serial per car it sends; the mode each received car shows and the
    // local tick its flame last fired again (HookExhaust).
    uint8_t fxSerial[kGridSlots] = {};
    uint32_t fxFired[kGridSlots] = {};
    bool fxFiredSet[kGridSlots] = {};
    uint8_t fxShown[kGridSlots] = {kFxNone, kFxNone, kFxNone, kFxNone, kFxNone, kFxNone, kFxNone, kFxNone};
    uint32_t fxStamp[kGridSlots] = {};
    bool fxLogged = false;
    int catchupLogged = 0;  // the catch-up last logged for this game's car: 0 off, 1 slowed, 2 boosted
    // The progress bar: another player's car shows its human marker in the rival entry the bar draws it with
    // (RaceNativeBefore / After). Each entry's fields as they were, to put back when that player leaves.
    struct BarCopy {
        uint32_t human;     // the human marker now in the rival entry (0 = not applied)
        uint32_t fields[5]; // the rival entry's +0, +4, +8, +0x10, +0x18
        uint8_t flags[2];   // and +0x14, +0x15
    } bar[kMaxHumans] = {};
    // A player who left: their car goes on as an AI (the console's message 8). With the host's AI and the
    // host still here, the host's game drives it for everyone, like its other AI (adopted on every game).
    bool adopted[kGridSlots] = {};
    // Guest, host's AI (notes/NPC-SYNC.md): the AI follow the host's game from hold A.
    bool aiPuppets = false;
    uint32_t initial[kGridSlots] = {};    // the AI's and the other players' cars' initial state (+0x9e4) before, for a hand-back
    int carsIn = 0;                       // Cars messages taken from the host
    bool resultsIn = false, resultsApplied = false;
    RaceResults results = {};
    bool endWaitOn = false, endGaveUp = false;
    uint32_t endWaitSince = 0;            // sim tick the hold for the host's results began
    // Host: the cars this game drives, one Cars message per tick, and the final results.
    RaceCars batch = {};
    int carsSent = 0;
    bool resultsSent = false, resultsRetryLogged = false;
    // Every sender: each car's last sent position, to flag a jump (respawn) as a snap.
    float lastPos[kGridSlots][3] = {};
    bool lastPosSet[kGridSlots] = {};
    bool stateLogged[kGridSlots] = {};  // a network car was found in another state (logged once)
    int itemsCleared = 0;               // items dropped from network cars (they never use them)
    bool sorted = false;                // this game's race end has sorted the results (a WFF update saw it over)
    bool orderChecked = false;
    // Items (contract flag kContractItems, notes/ITEMS-SYNC.md). In: each other game's events for its
    // cars, in arrival order, until that game's car is shown at the moment they happened. Out: events
    // the message queue couldn't take yet, in order.
    struct Queued {
        RaceEvent e;
        uint32_t arrived;  // our sim tick
    };
    Queued queue[kMaxHumans][kEventQueueCap];  // per peer (g.peers index)
    int queueHead[kMaxHumans] = {}, queueCount[kMaxHumans] = {};
    RaceEvent outbox[8];
    int outCount = 0;
    void* injecting = nullptr;          // the event being run for another game's car: our post hook passes it unchanged
    int32_t allStarSeen[kGridSlots] = {};   // owner: each of its cars' All-Star phase last sent
    bool allStarEnd[kGridSlots] = {};       // the owner's All-Star is over: end this game's copy once it runs
    int evSent = 0, evRun = 0, evLate = 0, evDropped = 0, evRefused = 0, evUnsent = 0, boxesIgnored = 0, evLogged = 0;
    // Driver reactions: the owner's "reaction playing" flag per car last seen; a received one waiting to play.
    uint8_t reacting[kGridSlots] = {};
    uint32_t reactNode[kGridSlots] = {}, reactSince[kGridSlots] = {};
    int reactSent = 0, reactShown = 0, reactDropped = 0, reactLogged = 0, reactLoggedHuman = 0, reactUnknown = 0;
    bool reactDropLogged = false;
    // The intros: the course intro has ended here (a press, or its end by IntroUpdate), the racer intro's end logged.
    bool introEnded = false, racerIntroLogged = false;
    // HUD toasts (M7, re\m7-join-race\FINDINGS.md Q2): the lobby's members since Arm (to see joins and leaves), and the
    // toasts waiting for the HUD's ticker, shown one at a time.
    uint64_t members[kMaxMembers] = {};
    char memberNames[kMaxMembers][32] = {};
    int memberCount = 0;
    struct Toast { uint8_t kind; char name[32]; } toasts[8] = {};
    int toastCount = 0;
    bool toastLogged[5] = {}, toastRefused = false;
    // Our race progress last given to the lobby (member data "pos": place, lap, progress, laps, race id) and when.
    int32_t progressSent[5] = {-1, 0, 0, 0, 0};
    int32_t testProgressSent[kMaxHumans][5] = {};  // host: its test players' (by creation index)
    bool testProgressPosted = false;
    uint32_t progressTick = 0;
    // Drift+ off (kContractNoDriftPlus): which of this game's controller's steering inputs steers (SteerOwner), and the
    // race's log lines (the rule acting, a steer value it left alone).
    uint8_t steerOwner = 0;
    bool steerLogged = false, steerSkipLogged = false;
} g;
uint32_t g_lastRaceId = 0;  // the last race this game took part in (RaceLastId; kept across EndRace)

// Back to a fresh race state in place (the state holds 8 playback buffers: no temporary copy).
void ClearRace() {
    g.~Race();
    new (&g) Race();
}

bool HostAi() { return (g.c.flags & kContractHostAi) != 0; }

// The cars in this race: 8, or only the players' with the lobby's AI RACERS OFF (re\m6-rules\FINDINGS.md A).
// Creation indexes 0..Cars()-1; the slots after them have no racer.
int Cars() { return g.c.ai == 0 && g.c.humanCount >= 2 ? g.c.humanCount : kGridSlots; }

// The host's game drives car k: its own player (creation index 0) and, with the flag, the AI and the
// cars of players who left.
bool HostCar(int k) { return k == 0 || (HostAi() && k >= 0 && k < kGridSlots && (k >= g.c.humanCount || g.adopted[k])); }

// Items are decided by each car's owner in this race (the host's contract flag; this game has the item hooks).
bool ItemsOn() { return g_itemHooks && (g.c.flags & kContractItems) && g.phase == kRunning; }

// This game decides car k's items and sends them: our player's car, and the host's AI on the host.
bool SendsItems(int k) { return k == g.local || (g.host && k != 0 && HostCar(k)); }

const RacerEntry* g_racerList = nullptr;
// Egg Hangar's course (the console's 25th lobby course, kCourses' last; tools/netmenu/settings.py): raced only where the Egg Hangar
// mod is installed (RaceUseEggHangar; the mod check makes every lobby member's set the host's).
const uint32_t kEggCourseHash = 0x2c0dae1f;  // name_hash("doomeggzone_dlc")
bool g_eggHangar = false;
bool g_eggScanned = false;  // RaceUseEggHangar ran (only after a complete mod scan)
// ENABLE MODIFICATIONS (RaceLobbyMods): this lobby's use of Egg Hangar and Metal Sonic (on outside a lobby, as 1.0.0).
bool g_eggLobby = true, g_metalLobby = true;
bool CourseUsable(int course) {
    return course >= 0 && course < (int)(sizeof(kCourses) / sizeof(kCourses[0])) &&
           (kCourses[course].hash != kEggCourseHash || (g_eggHangar && g_eggLobby));
}
int g_racerCount = 0;
bool RacerOn(int i) { return g_racerList && i >= 0 && i < g_racerCount && (g_metalLobby || strcmp(g_racerList[i].name, "mechasonic") != 0); }

const wchar_t* PhaseName(Phase p) {
    static const wchar_t* names[] = {L"idle", L"armed", L"creating", L"created", L"set up", L"hold A", L"intros", L"hold B", L"running", L"failed"};
    return names[p];
}

uint32_t SimTick() { return *(volatile uint32_t*)kSimTick; }

uint64_t LobbyHost(const LobbySnapshot& s) {
    for (int i = 0; i < s.memberCount; ++i)
        if (s.members[i].isHost) return s.members[i].id;
    return 0;
}

void SendToAll(const void* data, size_t n, bool reliable) {
    if (n && !RaceNetSend(0, data, n, reliable) && reliable) Log(false, L"race: message queue full, a message was not sent");
}

void SendAbort(AbortReason why) {
    uint8_t b[kWireMax];
    SendToAll(b, EncodeAbort(g.c.race, why, b, sizeof(b)), true);
}

// The lobby's items ON = all items (2, the default), OFF = none; hazards always the default (1, the
// lobby has no setting for them), so every PC loads the same course layers.
const uint32_t kItemsAll = 2, kItemsNone = 0;
const uint8_t kHazardsOn = 1;
const uint32_t kSetItems = 0xc, kSetHazards = 0x10, kSettingsSize = 0xac;

void ApplyRules(const RaceContract& c) {
    uint32_t active = *(volatile uint32_t*)(uintptr_t)kActiveProfile;
    if (g_rules.record || active > 3) {
        if (active > 3) Log(true, L"race: no active profile (%u); this game keeps its own items setting", active);
        return;
    }
    uint8_t* rec = (uint8_t*)(uintptr_t)kSettingsRecords + active * kSettingsSize;
    g_rules.items = *(uint32_t*)(rec + kSetItems);
    g_rules.hazards = rec[kSetHazards];
    g_rules.record = rec;
    *(uint32_t*)(rec + kSetItems) = c.items ? kItemsAll : kItemsNone;
    rec[kSetHazards] = kHazardsOn;
    Log(false, L"race: host's rules: items %u, hazards %u (this profile's: %u, %u; put back before any save)", *(uint32_t*)(rec + kSetItems),
        rec[kSetHazards], g_rules.items, g_rules.hazards);
}

void RestoreRules(const wchar_t* when) {
    if (!g_rules.record) return;
    *(uint32_t*)(g_rules.record + kSetItems) = g_rules.items;
    g_rules.record[kSetHazards] = g_rules.hazards;
    g_rules.record = nullptr;
    Log(false, L"race: this profile's items and hazards put back (%s)", when);
}

bool RaceOver();
double StepMs();

// Guest: the AI that followed the host's game drive themselves again (the host left, or the race goes
// on offline): the AI state for the race (or the grid before GO), as the console's message 6 did.
bool AnyAdopted() {
    for (int k = 1; k < g.c.humanCount && k < kGridSlots; ++k)
        if (g.adopted[k]) return true;
    return false;
}

void HandBackAi(const wchar_t* why) {
    if (!g.aiPuppets && !AnyAdopted()) return;  // (with AI off: only the cars of players who left)
    g.aiPuppets = false;
    uint32_t state = g.racing ? kStateAiRacing : kStateGridAi;
    bool over = RaceOver();  // finished or on the podium: they stay as they are, only no more poses
    for (int k = 1; k < kGridSlots; ++k) {
        if (k < g.c.humanCount && !g.adopted[k]) continue;  // (the AI, and the cars of players who left)
        g.adopted[k] = false;
        g.remote[k] = false;
        uint8_t* racer = (uint8_t*)g.racers[k];
        if (!racer || over) continue;
        void* controller = *(void**)(racer + kRacerController);
        if (controller) ((SetStateFn)(uintptr_t)kSetControlState)(controller, state);
        *(uint32_t*)(racer + kRacerInitialState) = g.initial[k];
    }
    Log(true, L"race: %s; %s", why, over ? L"the AI stay as they finished" : L"this game drives the AI again");
}

// The contract can't be applied here: tell the others, and let the game run as a normal race.
void Fail(const wchar_t* why) {
    Log(true, L"online race %08x: %s; this game continues as an offline race", g.c.race, why);
    if (g.phase != kIdle && g.phase != kFailed) SendAbort(kAbortMismatch);
    HandBackAi(L"offline race");
    g.phase = kFailed;
    g.released = 2;
    RaceNetActive(false);
    RestoreRules(L"offline race");
}

void CloseNetDialog();  // the console's session message (below the Steam-join code)
void AnnounceStart();   // the shared announcer (below the intro code)
void AnnounceStop(const wchar_t* why);
void LeaderLapCalls();
void PlayerFinishLines();
void ReportWrongWay();
void CallWrongWay(int k);
void ItemLines();       // the fair item lines (below the fair call-outs)
void CallAllStarHit(int k, int other);
void LogItemLines();
bool FairCallsOn();     // the fair call-outs (below the announcer)
void FairAlert(int k, const uint8_t* racer);
uint32_t FreshTake(uint8_t* group, uint32_t stock);
void NoteMention(int k);
bool AnnouncedPlayer(int k);
uint32_t PlayerCue(int k, uint32_t idx);

void EndRace(const wchar_t* why) {
    if (g.phase != kIdle) Log(false, L"online race %08x over (%s)", g.c.race, why);
    RestoreRules(L"race over");
    CloseNetDialog();
    AnnounceStop(L"the race is over");
    if (g.announceSent) Log(false, L"race: announcer: %d lines went to the other players", g.announceSent);
    if (g.progressSent[0] >= 0) {  // the lobby's race bar: this player races no more (retried from the menus if the queue is full:
        int32_t none[5] = {-1, 0, 0, 0, 0};  // the host's Start waits for it)
        g_progressClearPending = !LobbyPostArgs(kCmdSetProgress, none, 5);
    }
    if (g.testProgressPosted) {  // and its test players'
        int32_t none[6] = {-1, 0, 0, 0, 0, 0};
        LobbyPostArgs(kCmdSetTestProgress, none, 6);
    }
    // Left before this game's results (the pause menu's EXIT / RETURN TO LOBBY, a Steam join): the others may race on, and this
    // player waits in the lobby watching them, as on the console (re\m8-audit A2): that race is no longer "this game's" for the
    // lobby's race bar (natives.cpp: members racing a race other than RaceLastId).
    if (g.phase >= kHoldA && g.phase <= kRunning && !g.resultsStarted && g_lastRaceId == g.c.race) {
        g_lastRaceId = 0;
        Log(false, L"race: left before the finish; the lobby's race bar shows the others' race");
    }
    ClearRace();
    g_netPause = false;
    g_pauseOpen = false;
    RaceNetActive(false);
}

Peer* PeerById(uint64_t id) {
    for (int i = 0; i < g.peerCount; ++i)
        if (g.peers[i].id == id) return &g.peers[i];
    return nullptr;
}

// A name as the HUD can draw it: whole UTF-8 characters of 1-3 bytes (its decoder, 0x482c90, handles no
// more and checks nothing), anything else '?', cut to fit `n` with the NUL.
void TagText(const char* in, char* out, size_t n) {
    size_t o = 0;
    const uint8_t* s = (const uint8_t*)in;
    while (*s && o + 1 < n) {
        uint8_t c = *s;
        int len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 0;
        bool ok = len > 0 && c >= 0x20 && !(len == 2 && c < 0xc2);
        for (int k = 1; ok && k < len; ++k) ok = (s[k] & 0xc0) == 0x80;
        if (ok && len == 3 && ((c == 0xe0 && s[1] < 0xa0) || (c == 0xed && s[1] >= 0xa0))) ok = false;
        if (!ok) {
            out[o++] = '?';
            ++s;
            while ((*s & 0xc0) == 0x80) ++s;
            continue;
        }
        if (o + len + 1 > n) break;
        memcpy(out + o, s, len);
        o += len;
        s += len;
    }
    out[o] = 0;
}

void Arm(const RaceContract& c, bool host) {
    RestoreRules(L"a new race");  // (after a failed start, before the menus' EndRace)
    ClearRace();
    g.c = c;
    g.host = host;
    uint64_t me = RaceNetSelf();
    for (int i = 0; i < c.humanCount; ++i) {
        if (c.humans[i] == me) {
            g.local = i;
            continue;
        }
        Peer& p = g.peers[g.peerCount++];
        p.id = c.humans[i];
        p.index = i;
        p.test = IsTestPlayer(p.id);
        g.remote[i] = true;
    }
    LobbySnapshot s;
    LobbyRead(s);
    for (int i = 0; i < c.humanCount; ++i)
        for (int j = 0; j < s.memberCount; ++j)
            if (s.members[j].id == c.humans[i]) TagText(s.members[j].name, g.names[i], sizeof(g.names[i]));
    g.memberCount = 0;  // the lobby as it is now: later joins and leaves become HUD toasts
    for (int j = 0; s.state == kLobbyInLobby && j < s.memberCount && j < kMaxMembers; ++j) {
        g.members[g.memberCount] = s.members[j].id;
        TagText(s.members[j].name, g.memberNames[g.memberCount], sizeof(g.memberNames[0]));
        ++g.memberCount;
    }
    g.phase = kArmed;
    RaceNetActive(true);
    const char* course = c.course < sizeof(kCourses) / sizeof(kCourses[0]) ? kCourses[c.course].name : "?";
    Log(true, L"online race %08x: %hs, %u laps, %u players; this game creates racer %d (%s)%s%s%s", c.race, course, c.laps, c.humanCount,
        g.local, host ? L"host" : L"guest", HostAi() ? L"; the host's game drives the AI" : L"",
        !(c.flags & kContractItems) ? L"" : g_itemHooks ? L"; each car's game decides its items" : L"; items are shared, but this game's item hooks are off",
        !(c.flags & kContractNoDriftPlus) ? L"" : g_steerHooks ? L"; Drift+ off (a keyboard player races, or the host's [Debug] ForceDriftPlusOff)" : L"; Drift+ off, but this game's steering hooks are off");
}

// Every racer is one this game's lobby list has: humans not the per-player-appearance racers, AI only from the AI pool.
bool RosterKnown(const RaceContract& c) {
    if (!g_racerList) return false;
    for (int k = 0; k < kGridSlots; ++k) {
        const RacerEntry* e = nullptr;
        for (int i = 0; i < g_racerCount; ++i)
            if (g_racerList[i].hash == c.roster[k]) e = &g_racerList[i];
        if (!e) return false;
        bool human = k < c.humanCount;
        if (human && (!strcmp(e->name, "mii") || !strcmp(e->name, "avatar"))) return false;
        if (!human && !e->ai) return false;
    }
    return true;
}

// ---------------------------------------------------------------- messages
void PushPose(const RacePose& pose) {
    PlayoutPose pp;
    memcpy(pp.pos, pose.pos, sizeof(pp.pos));
    memcpy(pp.rot, pose.rot, sizeof(pp.rot));
    memcpy(pp.vel, pose.vel, sizeof(pp.vel));
    memcpy(pp.spin, pose.spin, sizeof(pp.spin));
    pp.nav = pose.nav;
    pp.fx = pose.fx;
    g.cars[pose.slot].play.Push(pose.tick, SimTick(), pp, (pose.flags & kPoseSnap) != 0);
}

Peer* PeerByIndex(int index) {
    for (int i = 0; i < g.peerCount; ++i)
        if (g.peers[i].index == index) return &g.peers[i];
    return nullptr;
}

bool HostGone();
const char* RacerName(int k);
int RecordIndex(const void* racer);
uint32_t* FinishReason(int index);

// Car k drives itself here from now on, as an AI: the state the console's message 8 gave a departed
// player's car (0x5afd50: AI racing, or the AI grid state before GO).
void DriveHere(int k) {
    g.remote[k] = false;
    uint8_t* racer = (uint8_t*)g.racers[k];
    if (!racer) return;
    void* controller = *(void**)(racer + kRacerController);
    if (controller) ((SetStateFn)(uintptr_t)kSetControlState)(controller, g.racing ? kStateAiRacing : kStateGridAi);
    *(uint32_t*)(racer + kRacerInitialState) = g.initial[k];
}

// Player k left the race: their car goes on as an AI instead of standing still. With the host's AI the
// host's game drives it and sends it with its other cars; the guests show it from those. Otherwise (or
// once the host is gone) every game drives its own copy, as with the AI after the host left. A test
// player's car goes the same way from hold A on (`test`).
void CarLeft(int k, bool test) {
    if (k < 0 || k >= kGridSlots || !g.racers[k] || k == g.local) return;
    wchar_t who[96];  // "player 1 left" / "Test Player 1 (car 2)"
    if (test) swprintf_s(who, L"%hs (car %d)", g.names[k], k);
    else swprintf_s(who, L"player %d left", k);
    // Over here, or that car already finished (its time stays; as an AI it would race on and fire items): left as it is.
    if (RaceOver() || FinishReason(RecordIndex(g.racers[k]))) { Log(false, L"race: %s; their car stays where it finished", who); return; }
    if (HostAi() && !HostGone()) {
        g.adopted[k] = true;
        if (g.host) {
            DriveHere(k);
            g.lastPosSet[k] = false;
            Log(true, L"race: %s; this game drives their car (%hs) as an AI for everyone", who, RacerName(k));
        } else {
            g.cars[k].play.Reset();  // the host's poses run on its own clock
            Log(true, L"race: %s; their car (%hs) follows the host's game as an AI", who, RacerName(k));
        }
        return;
    }
    DriveHere(k);
    Log(true, L"race: %s; this game drives their car (%hs) as an AI", who, RacerName(k));
}

// The game that decides car k has left: the other player's own car, or (guest) the host's AI.
bool OwnerGone(int k) {
    if (g.adopted[k]) return HostGone();
    if (Peer* p = PeerByIndex(k)) return p->gone;
    return k >= g.c.humanCount && HostGone();
}

// Guest: the host (creation index 0) has left the race or the lobby.
bool HostGone() {
    Peer* h = g.host ? nullptr : PeerByIndex(0);
    return h && h->gone;
}

// Intro vote-to-skip (below the phase hooks).
// The console's session messages (below the Steam-join code): the last other player gone, the host gone.
void CheckSessionEnd(const LobbySnapshot& s);
void CloseNetDialog();
void RetryDialogClose();

// HUD toasts (re\m7-join-race\FINDINGS.md Q2): the console's bottom-right ticker, "{0} HAS JOINED THE LOBBY." and the
// others, for lobby changes while this game races. Kinds as the console's event kinds.
enum ToastKind : uint8_t { kToastJoined = 0, kToastLeftGame = 1, kToastLeftLobby = 2, kToastReturned = 4 };

void QueueToast(uint8_t kind, const char* name) {
    if (g.toastCount >= (int)(sizeof(g.toasts) / sizeof(g.toasts[0]))) return;
    Race::Toast& t = g.toasts[g.toastCount++];
    t.kind = kind;
    TagText(name && *name ? name : "?", t.name, sizeof(t.name));
}

// The lobby's members against the list kept since Arm: a new member joined the lobby (while we race), a missing one
// left it. A racer who quits the race but stays in the lobby is "left the game" (its Abort, HandleMessage).
void NoteLobbyChanges(const LobbySnapshot& s) {
    if (s.state != kLobbyInLobby) return;
    for (int j = 0; j < s.memberCount && j < kMaxMembers; ++j) {
        bool known = false;
        for (int i = 0; i < g.memberCount; ++i) known = known || g.members[i] == s.members[j].id;
        if (!known && s.members[j].id != RaceNetSelf()) QueueToast(kToastJoined, s.members[j].name);
    }
    for (int i = 0; i < g.memberCount; ++i) {
        bool present = false;
        for (int j = 0; j < s.memberCount; ++j) present = present || s.members[j].id == g.members[i];
        if (!present && g.members[i] != RaceNetSelf()) QueueToast(kToastLeftLobby, g.memberNames[i]);
    }
    g.memberCount = 0;
    for (int j = 0; j < s.memberCount && j < kMaxMembers; ++j) {
        g.members[g.memberCount] = s.members[j].id;
        TagText(s.members[j].name, g.memberNames[g.memberCount], sizeof(g.memberNames[0]));
        ++g.memberCount;
    }
}
// Items (below the cars).
void QueueEvent(int i, const RaceEvent& e);
void ItemsPeerGone(int i);
void FlushEvents();
void RunQueuedEvents();
void EndAllStars();
void SendAllStarPhases();
void SendAllStarShots();
void RunAllStarShot(const RaceEvent& e);
void CheckOpaShots();
void ForceOpaShots();
void SendPulseShots();
void CheckPulseShots();
void ForcePulseShots();
void ForceMetalTimeUp();
void DropGateShots();
void RunAllStarGate(const RaceEvent& e);
void SendReactions();
void QueueReaction(const RaceEvent& e);
void PlayReactions();
void LogItems();
void LogVoices();

void HandleMessage(const RaceNetMsg& m, uint64_t lobbyHost) {
    WireType type;
    uint32_t race;
    if (!WirePeek(m.data, m.size, type, race)) return;
    if (type == kMsgContract) {
        RaceContract c;
        if (m.peer != lobbyHost) { Log(false, L"race contract from %llu ignored (not the lobby host)", m.peer); return; }
        if (!DecodeContract(m.data, m.size, c)) { Log(true, L"race contract from the host could not be read"); return; }
        if (g.phase != kIdle && g.phase != kFailed) { Log(false, L"race contract %08x ignored: race %08x is in progress", c.race, g.c.race); return; }
        bool mine = false;
        for (int i = 0; i < c.humanCount; ++i) mine |= c.humans[i] == RaceNetSelf();
        if (!mine) { Log(false, L"race contract %08x does not include this player; ignored", c.race); return; }
        uint8_t b[kWireMax];
        if (!CourseUsable(c.course) || kCourses[c.course].hash != c.courseHash) {
            Log(true, L"race contract %08x names a course this game doesn't know", c.race);
            RaceNetSend(m.peer, b, EncodeAbort(c.race, kAbortMismatch, b, sizeof(b)), true);  // the host needn't wait
            return;
        }
        if (!RosterKnown(c)) {  // an unknown character would be created with no character data (crash later)
            Log(true, L"race contract %08x names a racer this game can't use here", c.race);
            RaceNetSend(m.peer, b, EncodeAbort(c.race, kAbortMismatch, b, sizeof(b)), true);
            return;
        }
        Arm(c, false);
        return;
    }
    if (g.phase == kIdle || race != g.c.race) return;  // not for the race in hand
    Peer* p = PeerById(m.peer);
    if (!p) return;
    switch (type) {
    case kMsgLoaded: {
        uint32_t r;
        uint8_t stage, slot;
        if (!DecodeLoaded(m.data, m.size, r, stage, slot) || slot != p->index) break;
        if (stage > p->reached) {
            p->reached = stage;
            Log(false, L"race: %llu reached hold %u", m.peer, stage);
        }
        if (g.host && stage <= g.released) {  // released before this player got there (timeout, lost Go)
            uint8_t b[kWireMax];
            size_t n = EncodeGo(g.c.race, stage, b, sizeof(b));
            if (n) RaceNetSend(m.peer, b, n, true);
        }
        break;
    }
    case kMsgGo: {
        uint32_t r;
        uint8_t stage;
        if (m.peer == lobbyHost && DecodeGo(m.data, m.size, r, stage) && stage > g.released) {
            g.released = stage;
            Log(false, L"race: the host released hold %u", stage);
        }
        break;
    }
    case kMsgPose: {
        RacePose pose;
        if (!p->gone && DecodePose(m.data, m.size, pose) && pose.slot == p->index) PushPose(pose);
        break;
    }
    case kMsgCars: {  // the host's cars: its player, and the AI when the host drives them
        RaceCars cars;
        if (m.peer != lobbyHost || g.host || !DecodeCars(m.data, m.size, cars)) break;
        ++g.carsIn;
        for (int i = 0; i < cars.count; ++i)
            if (HostCar(cars.cars[i].slot) && cars.cars[i].slot != g.local) PushPose(cars.cars[i]);
        break;
    }
    case kMsgResults: {
        if (m.peer != lobbyHost || g.host || !HostAi() || g.resultsIn) break;
        if (DecodeResults(m.data, m.size, g.results)) {
            g.resultsIn = true;
            Log(false, L"race: the host's results arrived");
        }
        break;
    }
    case kMsgEvent: {  // an item event of a car the sender decides (checked in QueueEvent)
        RaceEvent e;
        bool armed = g_itemHooks && (g.c.flags & kContractItems) && (g.phase == kHoldB || g.phase == kRunning);
        if (armed && DecodeEvent(m.data, m.size, e)) QueueEvent((int)(p - g.peers), e);
        break;
    }
    case kMsgAnnounce: {  // a line of the host's announcer, said here (re\m7-announcer)
        RaceAnnounce a;
        if (p->index == 0 && g.announceMuted && DecodeAnnounce(m.data, m.size, a) && a.race == g.c.race) {
            if (g.lineCount == (int)(sizeof(g.lines) / sizeof(g.lines[0]))) {  // (the queue holds 8: the oldest goes)
                memmove(g.lines, g.lines + 1, sizeof(g.lines) - sizeof(g.lines[0]));
                memmove(g.lineArrived, g.lineArrived + 1, sizeof(g.lineArrived) - sizeof(g.lineArrived[0]));
                --g.lineCount;
            }
            g.lineArrived[g.lineCount] = SimTick();
            g.lines[g.lineCount++] = a;
        }
        break;
    }
    case kMsgWrongWay: {  // the sender's car turned the wrong way: the host's announcer calls it
        uint32_t r;
        uint8_t slot;
        if (g.host && DecodeWrongWay(m.data, m.size, r, slot) && r == g.c.race && slot == p->index && !p->gone) CallWrongWay(slot);
        break;
    }
    case kMsgItemLine: {  // the sender's car took a hit in its own game: the host's announcer calls it (ItemLines, CallAllStarHit)
        RaceItemLine l;
        if (!g.host || !DecodeItemLine(m.data, m.size, l) || l.race != g.c.race || l.slot != p->index || p->gone) break;
        if (l.kind != kLineHit || l.other) ++g.hitReports;  // (a spin's end isn't a hit)
        if (l.kind == kLineHit) g.spinNow[l.slot] = l.other != 0;
        else CallAllStarHit(l.slot, l.other);
        break;
    }
    case kMsgVoice: {  // the sender's racer said a driver line: said here too (PlayVoiceLines, before the speech queue's update)
        RaceVoice v;
        if (!DecodeVoice(m.data, m.size, v) || v.race != g.c.race || p->gone || p->test || v.slot >= g.c.humanCount) break;
        const Peer* subject = PeerByIndex(v.slot);
        bool testIntro = p->index == 0 && subject && subject->test && v.kind == kIntroKind;  // (the host says its test players')
        if (v.slot != p->index && !testIntro) break;
        if (g.voiceInCount == kVoiceInCap) {  // (full: the oldest goes)
            memmove(g.voiceIn, g.voiceIn + 1, (kVoiceInCap - 1) * sizeof(g.voiceIn[0]));
            memmove(g.voiceInTick, g.voiceInTick + 1, (kVoiceInCap - 1) * sizeof(g.voiceInTick[0]));
            --g.voiceInCount;
            ++g.voiceDropped;
        }
        g.voiceIn[g.voiceInCount] = v;
        g.voiceInTick[g.voiceInCount++] = SimTick();
        break;
    }
    case kMsgFinished: {
        RaceFinish f;
        if (DecodeFinished(m.data, m.size, f) && f.slot == p->index && !p->finished) {
            p->finished = true;  // applied to its lap record by the next race update (ApplyFinishes)
            p->fin = f;
        }
        break;
    }
    case kMsgAbort: {
        uint32_t r;
        uint8_t reason;
        if (DecodeAbort(m.data, m.size, r, reason) && !p->gone) {
            p->gone = true;
            p->abortReason = reason;
            Log(true, L"race: a player left the race (reason %u)", reason);
            // "left the game" while the race runs; after a finish (theirs or the race's) a quit is a return to the lobby.
            if (reason == kAbortQuit && p->index >= 0 && p->index < kMaxHumans)
                QueueToast(RaceOver() || p->finished ? kToastReturned : kToastLeftGame, g.names[p->index]);
        }
        break;
    }
    default: break;
    }
}

void Pump() {
    RaceNetMsg in[32];
    LobbySnapshot s;
    LobbyRead(s);
    uint64_t host = LobbyHost(s);
    int n;
    while ((n = RaceNetReceive(in, 32)) > 0)
        for (int i = 0; i < n; ++i) HandleMessage(in[i], host);
    if (g.phase == kIdle) return;
    if (g.phase >= kHoldA && g.phase <= kRunning) NoteLobbyChanges(s);
    for (int i = 0; i < g.peerCount; ++i) {
        Peer& p = g.peers[i];
        bool present = false;
        for (int j = 0; s.state == kLobbyInLobby && j < s.memberCount; ++j) present |= s.members[j].id == p.id;
        if (!present && !p.gone) {
            p.gone = true;
            Log(true, L"race: a player left the lobby (%llu)", p.id);
        }
        if (p.gone && !g.host && p.index == 0 && g.released < kStageCountdown) {  // the host (creation index 0) is gone
            g.released = kStageCountdown;
            Log(true, L"race: the host is gone; racing on without waiting");
        }
        if ((p.gone || p.test) && !p.carHandled && g.phase >= kHoldA && g.phase <= kRunning) {  // (from hold A its car is a network car)
            p.carHandled = true;
            CarLeft(p.index, p.test);
        }
        if (p.gone && !p.itemsClosed && !p.test) {  // (a test player's car is the host's own: its items stay as they are)
            p.itemsClosed = true;
            ItemsPeerGone(i);
        }
    }
    if ((g.aiPuppets || AnyAdopted()) && HostGone()) HandBackAi(L"the host left");
    if (HostGone()) AnnounceStop(L"the host left");
    CheckSessionEnd(s);
}

// Host: release a hold once every player (still connected) has reached it.
void HostReleases() {
    if (!g.host) return;
    for (uint8_t stage = kStageLoaded; stage <= kStageCountdown; ++stage) {
        if (g.released >= stage) continue;
        if (g.reached < stage) return;
        for (int i = 0; i < g.peerCount; ++i)
            if (!g.peers[i].gone && !g.peers[i].test && g.peers[i].reached < stage) return;
        uint8_t b[kWireMax];
        SendToAll(b, EncodeGo(g.c.race, stage, b, sizeof(b)), true);
        g.released = stage;
        Log(false, L"race: released hold %u for everyone", stage);
    }
}

void FlushCars();

// One sim tick of race bookkeeping (called from the phase update, before the cars move): messages in,
// holds released, the host's cars of the last tick out. Poses go out and come in at each car's
// end-of-tick (HookRacerCommit).
void Tick() {
    Pump();
    HostReleases();
    FlushCars();
    FlushEvents();
}

bool Active() { return g.phase >= kArmed && g.phase <= kRunning; }

void* SceneRacerCount(uint8_t& count) {
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    count = scene ? scene[0x1100] : 0;
    return scene;
}

// ---------------------------------------------------------------- laps and results (notes/LAPS.md)
uint8_t* Graph() { return *(uint8_t**)(uintptr_t)kCourseGraph; }

uint8_t* Tracker(const uint8_t* racer) {
    uint8_t* controller = *(uint8_t**)(racer + kRacerController);
    return controller ? controller + kCtrlTracker : nullptr;
}

// The nav path a car's track cursor is on, as an index into the course's path array (built from the
// course file, so the same index on every PC), or kNavNone.
uint16_t NavOf(const uint8_t* racer) {
    uint8_t* graph = Graph();
    uint8_t* t = Tracker(racer);
    if (!graph || !t) return kNavNone;
    uintptr_t path = *(uintptr_t*)(t + kTrackPath);
    if (!path) path = *(uintptr_t*)(t + kTrackLastPath);  // the game's own fallback (0x4fcab0)
    uintptr_t base = *(uintptr_t*)(graph + kGraphPaths);
    uint32_t count = *(uint32_t*)(graph + kGraphPathCount);
    if (!path || path < base || (path - base) % kPathSize) return kNavNone;
    uintptr_t i = (path - base) / kPathSize;
    return i < count && i < kNavNone ? (uint16_t)i : kNavNone;
}

// A car shown from the network follows the nav path its owner's cursor is on, through the console's
// own handler for a nav-line change (0x4e8880): it forces the path and snaps the cursor to the
// position just written. Without it the cursor can stay on a wrong path all race: wrong laps and position.
void ApplyNav(uint8_t* racer, uint16_t nav, int k) {
    uint8_t* graph = Graph();
    uint8_t* t = Tracker(racer);
    if (nav == kNavNone || !graph || !t || nav >= *(uint32_t*)(graph + kGraphPathCount)) return;
    uintptr_t want = *(uintptr_t*)(graph + kGraphPaths) + (uintptr_t)nav * kPathSize;
    if (*(uintptr_t*)(t + kTrackForced) == want) return;
    ((ApplyNavFn)(uintptr_t)kApplyNavLine)(racer, nav);
    if (++g.cars[k].navChanges <= 3 && k < g.c.humanCount) Log(false, L"race: player %d's car follows nav path %u", k, nav);
}

uint8_t* Record(int index) {
    uint8_t* base = *(uint8_t**)(uintptr_t)kRecords;
    return base && index >= 0 && index < kGridSlots ? base + index * kRecSize : nullptr;
}

int RecordIndex(const void* racer) { return racer ? *(const int32_t*)((const uint8_t*)racer + kRacerRecord) : -1; }

// The finish-list reason of racer `index`, or null while it isn't in the list.
uint32_t* FinishReason(int index) {
    uint8_t n = *(volatile uint8_t*)(uintptr_t)kFinishCount;
    for (int k = 0; k < n && k < 16; ++k) {  // the list has room for 16
        uint8_t* e = (uint8_t*)(uintptr_t)kFinishList + k * 8;
        if (e[0] == index) return (uint32_t*)(e + 4);
    }
    return nullptr;
}

bool RaceOver() { return *(volatile uint32_t*)(uintptr_t)kRaceOver != 0; }

// Our car has finished: it is in the finish list, or its lap record says so (+0x74 >= +0x84, what the
// race end counts, 0x51a010), whichever the game shows first.
bool CarFinished(int k, uint32_t* reason = nullptr) {
    if (k < 0 || k >= kGridSlots || !g.racers[k]) return false;
    int index = RecordIndex(g.racers[k]);
    if (uint32_t* r = FinishReason(index)) {
        if (reason) *reason = *r;
        return true;
    }
    uint8_t* rec = Record(index);
    uint32_t at = rec ? *(uint32_t*)(rec + kRecFinishAt) : 0;
    if (!at || *(uint32_t*)(rec + kRecCrossings) < at) return false;
    if (reason) *reason = 1;
    return true;
}

bool LocalFinished(uint32_t* reason = nullptr) { return CarFinished(g.local, reason); }

// A test player's car finishing counts as that player's finish (they have no game to send a Finished): the
// first finish starts the "RACE ENDS IN" timer as a real player's would.
void NoteTestFinishes() {
    if (RaceOver()) return;  // (the race end's estimated times aren't finishes)
    for (int i = 0; i < g.peerCount; ++i) {
        Peer& p = g.peers[i];
        if (!p.test || p.gone || p.finished || !CarFinished(p.index)) continue;
        p.finished = p.applied = true;  // (its lap record has it already)
        Log(false, L"race: %hs (car %d) finished", p.index >= 0 && p.index < kMaxHumans ? g.names[p.index] : "a test player", p.index);
    }
}

// The race end: the game gives every unfinished car an estimated time (0x4cd290): reason 6 (the results row's
// "DNF"), or for a car on its last lap (+0x74 == the laps, 0x4cd6d1-0x4cd71e) reason 1 with a projected time (seen
// 2026-10-22: a player on lap 3 of 3 showed a time). The console showed DNF for every player who hadn't finished (PS3 footage,
// notes\PS3-ONLINE.md): this game's player and every player still in the race (test players too) who hadn't finished
// get reason 6, once, before any result goes out. Computer racers (and departed players' cars) keep the estimates.
void MarkPlayersDnf() {
    if (g.dnfMarked) return;
    g.dnfMarked = true;
    for (int k = 0; k < g.c.humanCount && k < kGridSlots; ++k) {
        Peer* p = k == g.local ? nullptr : PeerByIndex(k);
        if (k != g.local && (!p || p->gone)) continue;
        // Another real player finished if their game said so (Finished); this game's car and a test player's (driven
        // here) if the lap record counts the last crossing (the race end's estimate doesn't count one).
        uint8_t* rec = g.racers[k] ? Record(RecordIndex(g.racers[k])) : nullptr;
        uint32_t at = rec ? *(uint32_t*)(rec + kRecFinishAt) : 0;
        bool crossed = rec && at && *(uint32_t*)(rec + kRecCrossings) >= at;
        if (!rec || (p && !p->test ? p->finished : crossed || (k == g.local && g.finishSent))) continue;
        uint32_t* reason = FinishReason(RecordIndex(g.racers[k]));
        if (!reason || *reason == 6) continue;
        Log(false, L"race: player %d (%hs) hadn't finished when the race ended: DNF (the game's reason %u)", k, RacerName(k), *reason);
        *reason = 6;
    }
}

// The console listed the racers who didn't finish last (user, 2026-10-22). The results rows follow the finish
// list, sorted by time (0x4cc790, rec+0x80 the places): its DNF entries (reason 6) move to its end in their order,
// and the places follow. Once, after MarkPlayersDnf; the game's own sort isn't run again (it may award again).
void DnfLast() {
    if (g.dnfOrdered) return;
    g.dnfOrdered = true;
    uint8_t n = *(volatile uint8_t*)(uintptr_t)kFinishCount;
    if (n > 16) return;
    uint8_t* list = (uint8_t*)(uintptr_t)kFinishList;
    uint8_t entries[16][8];
    int m = 0, moved = 0;
    for (int dnf = 0; dnf < 2; ++dnf)
        for (int i = 0; i < n; ++i)
            if ((*(uint32_t*)(list + i * 8 + 4) == 6) == (dnf == 1)) {
                moved += i != m;
                memcpy(entries[m++], list + i * 8, 8);
            }
    if (!moved) return;
    memcpy(list, entries, (size_t)n * 8);
    for (int r = 0; r < n; ++r)
        if (uint8_t* rec = Record(list[r * 8])) *(int32_t*)(rec + kRecPosition) = r;
    Log(false, L"race: the results list the racers who didn't finish last (%d rows moved)", moved);
}

bool OthersStillRacing() {
    for (int i = 0; i < g.peerCount; ++i)
        if (!g.peers[i].gone && !g.peers[i].test && !g.peers[i].finished) return true;  // (a test player's car is the host's AI)
    return false;
}

bool OthersFinished() {
    for (int i = 0; i < g.peerCount; ++i)
        if (g.peers[i].applied && g.peers[i].finished) return true;
    return false;
}

// A lap-record time (1/4096 sim tick since GO) as m'ss"mmm.
void TimeText(uint32_t t, wchar_t* out, size_t n) {
    uint32_t rate = *(volatile uint32_t*)(uintptr_t)kTickRate;
    uint64_t ms = (uint64_t)t * 1000 / (4096ull * (rate ? rate : 50));
    swprintf_s(out, n, L"%u'%02u\"%03u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms % 1000));
}

const char* RacerName(int k) {
    for (int i = 0; g_racerList && i < g_racerCount; ++i)
        if (g_racerList[i].hash == g.c.roster[k]) return g_racerList[i].name;
    return "?";
}

// The racer's menu name ("AMIGO", "BEAT"), for the computer racers' HUD tags.
const char* RacerDisplayName(int k) {
    for (int i = 0; g_racerList && i < g_racerCount; ++i)
        if (g_racerList[i].hash == g.c.roster[k]) return g_racerList[i].display;
    return nullptr;
}

const wchar_t* Who(int k) { return k == g.local ? L", you" : k < g.c.humanCount ? L", other player" : L""; }

// Our car has finished: tell the others its time (once; any reason, so the estimate this game makes
// for our player when its race-end timer runs out reaches the game that is waiting for it).
void SendOwnFinish() {
    if (g.finishSent || g.local < 0) return;
    uint32_t reason;
    uint8_t* rec = Record(RecordIndex(g.racers[g.local]));
    if (!rec || !LocalFinished(&reason)) return;
    RaceFinish f = {g.c.race, (uint8_t)g.local, (uint8_t)reason, *(uint32_t*)(rec + kRecTime), *(uint32_t*)(rec + kRecBest)};
    uint8_t b[kWireMax];
    size_t n = EncodeFinished(f, b, sizeof(b));
    wchar_t t[24];
    TimeText(f.time, t, 24);
    if (!n) {
        g.finishSent = true;
        Log(true, L"race: you finished (%s, reason %u), but that can't be sent", t, reason);
        return;
    }
    if (!RaceNetSend(0, b, n, true)) {  // queue full: tried again next tick
        if (!g.finishSendFailed) Log(false, L"race: you finished, but the message queue is full; retrying");
        g.finishSendFailed = true;
        return;
    }
    g.finishSent = true;
    Log(false, L"race: you finished: %s (reason %u); told the others (the race clock caught up %.0f ms here)", t, reason, g.clockAdded * StepMs());
}

// Car k gets a result measured elsewhere, as the console's network handler for a finish did
// (0x5c0670): its lap record's time and best lap, finished (a later crossing here can't change the
// time), and its finish-list reason set, or added. `seen`: it was already in this game's list.
bool SetResult(int k, uint8_t reasonIn, uint32_t time, uint32_t best, bool* seen) {
    uint8_t* racer = (uint8_t*)g.racers[k];
    int index = RecordIndex(racer);
    uint8_t* rec = Record(index);
    if (!rec || *(void**)(rec + kRecRacer) != racer) return false;
    *(uint32_t*)(rec + kRecTime) = time;
    *(uint32_t*)(rec + kRecBest) = best;
    *(uint32_t*)(rec + kRecCrossings) = *(uint32_t*)(rec + kRecFinishAt);
    uint32_t* reason = FinishReason(index);
    if (seen) *seen = reason != nullptr;
    if (reason) *reason = reasonIn;  // what 0x4cc090 does
    else ((AddFinishFn)(uintptr_t)kAddFinish)((uint32_t)index, reasonIn);
    return true;
}

// Another player's Finished: their car gets the time and reason its own game measured, and counts as
// finished here.
void ApplyFinishes() {
    for (int i = 0; i < g.peerCount; ++i) {
        Peer& p = g.peers[i];
        if (!p.finished || p.applied) continue;
        p.applied = true;
        wchar_t t[24];
        TimeText(p.fin.time, t, 24);
        if (RaceOver()) { Log(false, L"race: player %d finished in %s after this game's race had ended (not used)", p.index, t); continue; }
        bool seen = false;
        if (!SetResult(p.index, p.fin.reason, p.fin.time, p.fin.best, &seen)) { Log(true, L"race: player %d's lap record was not found; their finish isn't shown", p.index); continue; }
        Log(false, L"race: player %d finished: %s (reason %u)%s", p.index, t, p.fin.reason, seen ? L"; this game had already seen the car finish" : L"");
    }
}

// ---------------------------------------------------------------- the host's results (host's AI)
// Host: once its race is over and sorted, every car's final time, best lap, reason and place go to the
// guests (once). Its race end estimated every unfinished car (0x4cd290) with its own distances, and the
// final order is a sort by time alone (0x4cc790), so a guest with the same times shows the same results;
// the places settle equal times (the sort isn't stable).
bool Usable(uint32_t reason) { return reason == 1 || reason == 2 || reason == 3 || reason == 6; }  // not 4/5 (re\m5-npc2\FINDINGS.md Q2)

void TrySendResults() {
    if (!g.host || !HostAi() || g.resultsSent || g.phase != kRunning || !RaceOver() || !g.sorted) return;
    RaceResults res = {};
    res.race = g.c.race;
    int with = 0;
    bool placeUsed[kGridSlots] = {}, placesOk = true;
    for (RaceResultEntry& e : res.cars) e.rank = kRankNone;  // (slots past Cars(): no car, no result)
    for (int k = 0; k < Cars(); ++k) {
        int index = RecordIndex(g.racers[k]);
        uint8_t* rec = Record(index);
        uint32_t* reason = FinishReason(index);
        if (!rec) { placesOk = false; continue; }
        int32_t place = *(int32_t*)(rec + kRecPosition);
        if (place < 0 || place >= Cars() || placeUsed[place]) placesOk = false;
        else placeUsed[place] = true;
        res.cars[k].rank = (uint8_t)place;
        if (!reason || !Usable(*reason)) continue;
        res.cars[k].reason = (uint8_t)*reason;
        res.cars[k].time = *(uint32_t*)(rec + kRecTime);
        res.cars[k].best = *(uint32_t*)(rec + kRecBest);
        ++with;
    }
    if (!placesOk)
        for (RaceResultEntry& e : res.cars) e.rank = kRankNone;
    uint8_t b[kWireMax];
    size_t n = EncodeResults(res, b, sizeof(b));
    if (n && !RaceNetSend(0, b, n, true)) {  // queue full: tried again next tick
        if (!g.resultsRetryLogged) Log(false, L"race: the results can't be queued yet; retrying");
        g.resultsRetryLogged = true;
        return;
    }
    g.resultsSent = true;
    Log(false, L"race: this game's results went to the other players (%d cars)", with);
}

// Guest: the host's results replace this game's for every car the host has one for, before this
// game's race end sorts them. After that end they would need a second sort (which would add points
// and a profile stat twice), so they are then left out.
void ApplyHostResults() {
    if (!g.resultsIn || g.resultsApplied) return;
    g.resultsApplied = true;
    if (RaceOver()) { Log(true, L"race: the host's results came after this game's race had ended (not used)"); return; }
    int n = 0;
    for (int k = 0; k < kGridSlots; ++k) {
        const RaceResultEntry& e = g.results.cars[k];
        if (!Usable(e.reason) || !SetResult(k, e.reason, e.time, e.best, nullptr)) continue;
        ++n;
        if (k == g.local) g.finishSent = true;  // the host has our result: nothing to send
        if (Peer* p = PeerByIndex(k)) {
            p->finished = p->applied = true;
            p->fin = {g.c.race, (uint8_t)k, e.reason, e.time, e.best};
        }
    }
    Log(false, L"race: the host's results applied to %d cars", n);
}

// Guest: this game's race would end now (our player and the others are done, or the race-end timer is
// at its end) but the host's results aren't here: hold it, the timer two ticks short of its end, for
// up to kResultsWaitTicks. False once that time is up: this game then ends with its own results.
bool HoldForHostResults(bool mine, int32_t* timer) {
    uint32_t* elapsed = (uint32_t*)(uintptr_t)kEndTimerElapsed;
    uint32_t limit = *(volatile uint32_t*)(uintptr_t)kEndTimerLimit;
    bool timerEnd = timer && *timer != -1 && limit >= 2 && *elapsed + 2 >= limit;
    if (!(mine && !OthersStillRacing()) && !timerEnd) return true;
    if (!g.endWaitOn) {
        g.endWaitOn = true;
        g.endWaitSince = SimTick();
        Log(false, L"race: waiting for the host's results");
    }
    if (SimTick() - g.endWaitSince >= kResultsWaitTicks) {
        g.endGaveUp = true;
        Log(true, L"race: the host's results did not come within %u s; this game shows its own", kResultsWaitTicks / 50);
        return false;
    }
    if (timerEnd) *elapsed = limit - 2;
    return true;
}

bool WaitingForHostResults() { return !g.host && HostAi() && !g.resultsApplied && !g.endGaveUp && !HostGone(); }

// Guest: after this game's sort, cars with equal times may stand in another order than on the host (the
// sort isn't stable). The rows (the sorted finish list) and places then go into the host's order, when
// that order agrees with the times here (so only equal times move).
void MatchHostOrder() {
    if (g.host || !g.resultsApplied || g.orderChecked || !g.sorted) return;
    g.orderChecked = true;
    const int cars = Cars();
    int slotAt[kGridSlots];  // the car at each of the host's places
    bool placeSeen[kGridSlots] = {};
    for (int k = 0; k < cars; ++k) {
        uint8_t rank = g.results.cars[k].rank;
        if (rank == kRankNone || rank >= cars || placeSeen[rank]) return;  // the host sent no places
        placeSeen[rank] = true;
        slotAt[rank] = k;
    }
    uint8_t* list = (uint8_t*)(uintptr_t)kFinishList;
    if (*(volatile uint8_t*)(uintptr_t)kFinishCount != cars) {
        Log(true, L"race: the results list has %u cars, not %d; its order is this game's", *(volatile uint8_t*)(uintptr_t)kFinishCount, cars);
        return;
    }
    uint8_t entries[kGridSlots][8];
    int moved = 0;
    for (int r = 0; r < cars; ++r) {
        int index = RecordIndex(g.racers[slotAt[r]]);
        uint8_t* rec = Record(index);
        if (!rec) return;
        uint32_t* why = FinishReason(index);
        uint32_t* whyPrev = r ? FinishReason(RecordIndex(g.racers[slotAt[r - 1]])) : nullptr;
        bool dnf = why && *why == 6, prevDnf = whyPrev && *whyPrev == 6;
        bool sameBlock = r && dnf == prevDnf;  // (DNF rows come last: DnfLast)
        if ((r && prevDnf && !dnf) ||
            (sameBlock && *(uint32_t*)(rec + kRecTime) < *(uint32_t*)(Record(RecordIndex(g.racers[slotAt[r - 1]])) + kRecTime))) {
            Log(true, L"race: this game's results differ from the host's in more than equal times; shown as this game sorted them");
            return;
        }
        int at = -1;
        for (int i = 0; i < cars; ++i)
            if (list[i * 8] == index) at = i;
        if (at < 0) return;
        memcpy(entries[r], list + at * 8, 8);
        moved += at != r;
    }
    if (!moved) return;
    memcpy(list, entries, (size_t)cars * 8);
    for (int r = 0; r < cars; ++r) *(int32_t*)(Record(RecordIndex(g.racers[slotAt[r]])) + kRecPosition) = r;
    Log(false, L"race: %d results rows put in the host's order (equal times)", moved);
}

// Network cars never use items, but still pick up item boxes here (no category test, 0x539c10): one
// holding the All-Star would keep it, and while anyone holds it no one else gets one (0x49d7b0). Their
// items are dropped with the racer's own clear, unless an All-Star is running.
void DropNetworkCarItems() {
    for (int k = 0; k < kGridSlots; ++k) {
        uint8_t* racer = (uint8_t*)g.racers[k];
        if (!g.remote[k] || !racer || !*(uint32_t*)(racer + kRacerItem)) continue;
        // With items mirrored they are their owner's, run here for it (notes/ITEMS-SYNC.md); a car whose
        // owner has left drops them like any network car.
        if (ItemsOn() && !OwnerGone(k)) continue;
        int32_t allStar = *(int32_t*)(racer + kRacerAllStarState);
        if (allStar != 0 && allStar != 1) continue;
        ((ClearItemsFn)(uintptr_t)kClearItems)(racer, 1);
        if (g.itemsCleared++ < 3) Log(false, L"race: car %d (%hs) picked up an item here; dropped (network cars don't use items)", k, RacerName(k));
    }
}

// The humans' laps, for the log (compare both games' logs: each car's laps as its owner and the
// other game counted them).
int PlaceOf(int k) {
    uint8_t* r = Record(RecordIndex(g.racers[k]));
    return r ? *(int32_t*)(r + kRecPosition) : 99;
}

// Every car's place on this screen, in order (compare both games' lines at the same moment).
void LogPlaces() {
    int order[kGridSlots];
    const int cars = Cars();
    for (int k = 0; k < cars; ++k) order[k] = k;
    std::sort(order, order + cars, [](int a, int b) { return PlaceOf(a) < PlaceOf(b); });
    wchar_t line[320];
    size_t n = 0;
    line[0] = 0;
    for (int i = 0; i < cars; ++i) {
        int k = order[i];
        int w = swprintf_s(line + n, sizeof(line) / sizeof(line[0]) - n, L" %d %hs%s", PlaceOf(k) + 1, RacerName(k), k == g.local ? L"*" : L"");
        if (w < 0) break;
        n += (size_t)w;
    }
    Log(false, L"race: places here:%s", line);
}

// The lobby's race bar (natives.cpp DrawRaceBar) for a player who joined while we race: our place, lap and progress
// (thousandths of the whole race, from the record's distance over laps x lap length) once a second as member data
// (steam_mp.cpp "pos"); cleared when the race is closed.
typedef float(__fastcall* RecordDistanceFn)(void* record, void*);
bool ProgressOf(int k, int32_t v[5]) {  // car k's place, lap, progress (thousandths), laps, race id
    uint8_t* rec = k >= 0 && k < kGridSlots && g.racers[k] ? Record(RecordIndex(g.racers[k])) : nullptr;
    if (!rec) return false;
    uint8_t* graph = Graph();
    float lapLength = graph ? *(float*)(graph + kGraphLapLength) : 0.0f;
    float total = lapLength * (float)g.c.laps;
    float dist = ((RecordDistanceFn)(uintptr_t)kRecordDistance)(rec, nullptr);
    float frac = total > 0 && wire::Finite(dist) && wire::Finite(total) ? dist / total : 0.0f;
    v[0] = *(int32_t*)(rec + kRecPosition);
    v[1] = *(int32_t*)(rec + kRecLap);
    v[2] = frac <= 0 ? 0 : frac >= 1 ? 1000 : (int32_t)(frac * 1000.0f);
    v[3] = (int32_t)g.c.laps;
    v[4] = (int32_t)g.c.race;
    if (v[0] < 0 || v[0] >= kGridSlots) v[0] = 0;
    if (v[1] < 0) v[1] = 0;
    return true;
}

void PublishProgress() {
    if (g.progressTick && SimTick() - g.progressTick < kProgressEvery) return;
    g.progressTick = SimTick() ? SimTick() : 1;
    int32_t v[6];
    if (ProgressOf(g.local, v) && memcmp(v, g.progressSent, sizeof(g.progressSent)) != 0 && LobbyPostArgs(kCmdSetProgress, v, 5))
        memcpy(g.progressSent, v, sizeof(g.progressSent));
    for (int i = 0; g.host && i < g.peerCount; ++i) {  // the host's test players, who have no game to send theirs (lobby data "testpos")
        const Peer& p = g.peers[i];
        if (!p.test || p.gone || p.index < 0 || p.index >= kMaxHumans || !ProgressOf(p.index, v)) continue;
        v[5] = (int32_t)(p.id - kTestPlayerBase);
        if (memcmp(v, g.testProgressSent[p.index], sizeof(g.testProgressSent[0])) == 0) continue;
        if (LobbyPostArgs(kCmdSetTestProgress, v, 6)) memcpy(g.testProgressSent[p.index], v, sizeof(g.testProgressSent[0]));
        g.testProgressPosted = true;
    }
}

void LogLaps() {
    bool changed = false;
    for (int k = 0; k < g.c.humanCount; ++k) {
        uint8_t* rec = Record(RecordIndex(g.racers[k]));
        if (!rec) continue;
        int32_t lap = *(int32_t*)(rec + kRecLap);
        if (lap == g.laps[k]) continue;
        g.laps[k] = lap;
        changed = true;
        Log(false, L"race: racer %d (%hs%s) on lap %d, position %d", k, RacerName(k), Who(k), lap, *(int32_t*)(rec + kRecPosition) + 1);
    }
    if (changed) LogPlaces();
}

void LogResults(const wchar_t* when) {
    if (g.resultsLogged || !*(uint8_t**)(uintptr_t)kRecords) return;
    g.resultsLogged = true;
    int order[kGridSlots];
    const int cars = Cars();
    for (int k = 0; k < cars; ++k) order[k] = k;
    auto pos = [](int k) { return PlaceOf(k); };
    std::sort(order, order + cars, [&](int a, int b) { return pos(a) < pos(b); });
    Log(false, L"online race %08x results (%s):", g.c.race, when);
    for (int i = 0; i < cars; ++i) {
        int k = order[i];
        uint8_t* rec = Record(RecordIndex(g.racers[k]));
        if (!rec) continue;
        uint32_t* reason = FinishReason(RecordIndex(g.racers[k]));
        wchar_t t[24];
        TimeText(*(uint32_t*)(rec + kRecTime), t, 24);
        const wchar_t* how = !reason ? L"not finished" : *reason == 1 ? L"finished" : *reason == 2 ? L"time up" : *reason == 6 ? L"estimated" : L"other";
        Log(false, L"  %d. racer %d (%hs%s)  %s  %s", pos(k) + 1, k, RacerName(k), Who(k), t, how);
    }
}

// ---------------------------------------------------------------- native hooks
// The bridge pushes numbers through its unary ops as hashes (type 3); the natives read raw values.
bool IntOrHash(const NativeCall* d, uint32_t i) { return i < d->argc && (d->types[i] == kTypeInt || d->types[i] == kTypeHash); }

const char* StringArg(const NativeCall* d, uint32_t i) {
    if (i >= d->argc || (d->types[i] != kTypeHeapString && d->types[i] != kTypeLiteralString)) return nullptr;
    return (const char*)(uintptr_t)d->values[i];
}

// 0905(character, stateName, device) -> racer. The script creates 8 racers in slot order; our lobby
// sent it one local human (at our creation index) and AI everywhere else. Call k becomes roster
// entry k: our human as the script made it, the other humans as network racers, the AI as given.
void __cdecl Hook0905(void* vm, NativeCall* d) {
    LobbyNoteRaceStart();  // (any race: the Steam worker gathers the friend figures once for it)
    NativeFn original = Original<NativeFn>(kH0905);
    if (g.phase == kArmed && g.launched && GetTickCount() - g.launchedAt > 20000) {
        Log(true, L"online race %08x: the race did not start within 20 s of the lobby's start; dropped", g.c.race);
        SendAbort(kAbortMismatch);
        EndRace(L"start timed out");
    }
    if (g.phase == kCreated && Cars() < kGridSlots) {  // AI off: a bridge that makes more cars than the players'
        Fail(L"the race setup made more racers than the race has players (AI off)");
        original(vm, d);
        return;
    }
    if (!(g.phase == kArmed && g.launched) && g.phase != kCreating) { original(vm, d); return; }
    g_returnToLobby = true;  // this race came from our lobby, even if the contract fails below
    int k = g.calls++;
    const char* state = StringArg(d, 1);
    bool scriptHuman = state && strcmp(state, kNameGridHuman) == 0;
    if (k >= Cars() || d->argc < 3 || !IntOrHash(d, 0) || !IntOrHash(d, 2)) {
        Fail(L"the race setup made an unexpected racer call");
        original(vm, d);
        return;
    }
    if (scriptHuman != (k == g.local)) {
        wchar_t why[96];
        swprintf_s(why, L"racer %d was set up as %s", k, scriptHuman ? L"a player" : L"an AI");
        Fail(why);
        original(vm, d);
        return;
    }
    // The other players' cars are made as AI racers (a network racer gets no engine sound) with a pad
    // no PC has, and become network racers at hold A, before the countdown.
    bool remote = k != g.local && k < g.c.humanCount;
    d->values[0] = g.c.roster[k];
    if (remote) d->values[2] = (uint32_t)kInertDevice;
    original(vm, d);
    // 0905 returns {racer, [0xd21dc0]}: a registered object type, not the plain type 7 (seen in game).
    void* racer = d->retType && d->retType == *(volatile uint32_t*)kRacerTypeId ? (void*)(uintptr_t)d->retValue : nullptr;
    if (!racer) { Fail(L"the game did not create a racer"); return; }
    g.racers[k] = racer;
    // The online pause's "the AI holds this car" latch (re\m8-pause\FINDINGS.md): 0928 sets racer+0x1fc8 and hands the car to the
    // AI only when it was clear; 0929 (CONTINUE) clears it. The 8 racers live for the whole run (built once at start-up), and a race
    // left from the open pause (RETURN TO LOBBY / EXIT) never runs 0929: the next race's first pause then did nothing and the car
    // stood still. Cleared at each creation (the new controller's own flag +0x24c starts at 0 too).
    uint8_t* hold = (uint8_t*)racer + kRacerAiHold;
    if (*hold) {
        Log(false, L"race: car %d: the online pause's AI flag was left set by an earlier race; cleared", k);
        *hold = 0;
    }
    g.phase = k == Cars() - 1 ? kCreated : kCreating;
    if (g.phase == kCreated) Log(false, L"race: %d racers created from the contract%s", Cars(), Cars() < kGridSlots ? L" (AI off)" : L"");
}

// 0900(type, ?, laps, ?): the host's lap count instead of this profile's.
void __cdecl Hook0900(void* vm, NativeCall* d) {
    if ((g.phase == kCreated || g.phase == kCreating) && IntOrHash(d, 2)) d->values[2] = g.c.laps;  // value only; type kept
    Original<NativeFn>(kH0900)(vm, d);
}

// ---------------------------------------------------------------- script call sites
// Every script site this file checks (ScriptSiteDef, race.h) is found by its bytes, not by a fixed offset (1.1.1-dp13: another
// mod may rebuild a module in memory with a row or a line inserted before a site; the bytes stay, the offset moves; until then
// such a mod kept its whole feature off while this DLL was loaded). The bound module (VM+8, a relocated "smosbinL": +8 size, +0xc
// code offset, +0x10 code length, notes/SCRIPT-LOADER.md) is searched once per site and the answer kept per module: the PC
// offset where the bytes occur exactly once in its code, or 0 (absent: another module, or the bytes changed; or more than once:
// ambiguous, the site is off there, logged once). Each pattern was checked to occur once in its module and in no other module
// of the stock, Roster Pack and Metal Sonic 1.1.2 packages (tests/test_race.cpp runs that check on the real packages): the
// bytes identify the module as well as the site, as the offset-and-bytes check did. Script thread only.
const uint32_t kScriptResults = 0x8a972456, kScriptPauseMenu = 0x791427eb, kScriptFrontMain = 0x01d0bc8b, kScriptBridge = 0x44c833f0,
               kScriptHud = 0x96930381, kScriptGame = 0xbed107e7, kScriptDialogs = 0x694fe697, kScriptMainMenu = 0xee8a2362;
const int kMaxSites = 48, kMaxSiteModules = 320;
const uint32_t kSiteModuleMax = 0x1000000, kSiteHeader = 0x50;

struct ModuleSites {
    const uint8_t* module;
    uint32_t size, codeStart, codeLen, ident;  // the header words at the first look (another module at the same address: looked at again)
    uint64_t resolved;                          // bit i: pc[i] is known
    uint32_t pc[kMaxSites];                     // site i's PC offset in this module, or 0
};
const ScriptSiteDef* g_sites[kMaxSites];  // every site seen, by address (its index is its bit above)
int g_siteCount = 0;
ModuleSites g_siteModules[kMaxSiteModules];
int g_siteModuleCount = 0, g_siteLastModule = 0;
unsigned g_siteModuleNext = 0;  // the next record to reuse once the table is full (round robin)
bool g_siteListFullLogged = false, g_siteTableFullLogged = false;

uint32_t SiteU32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

bool KnownSiteOffset(const ScriptSiteDef& s, uint32_t pc) {
    for (uint32_t k : s.known)
        if (k && k == pc) return true;
    return false;
}

// The site's PC offset in the module's code (the header words given), or 0; logs what a reader of the mod log should know.
uint32_t SearchSite(const uint8_t* mod, uint32_t codeStart, uint32_t codeLen, const ScriptSiteDef& s) {
    if (!s.code || s.n == 0 || s.n > codeLen) return 0;
    const uint8_t* p = mod + codeStart;
    const uint8_t* last = mod + codeStart + codeLen - s.n;  // the last place the bytes could start
    uint32_t found = 0;
    int count = 0;
    while (p <= last) {
        p = (const uint8_t*)memchr(p, s.code[0], (size_t)(last - p) + 1);
        if (!p) break;
        if (memcmp(p, s.code, s.n) == 0) {
            if (++count > 1) break;
            found = (uint32_t)(p - mod);
        }
        ++p;
    }
    if (count > 1) {
        Log(true, L"script site %hs: its bytes occur more than once in the bound module (%u bytes): the site is off there", s.name,
            codeStart + codeLen);
        return 0;
    }
    if (count == 0 || (int64_t)found - s.from <= 0) return 0;
    uint32_t pc = (uint32_t)((int64_t)found - s.from);
    if (!KnownSiteOffset(s, pc))
        Log(true, L"script site %hs: found at +0x%x (this build knew +0x%x; another mod rebuilt the module)", s.name, pc, s.known[0]);
    return pc;
}

// The site's index in g_sites (its bit in every module's record), or -1 when the list is full (resolved without the cache).
int SiteIndex(const ScriptSiteDef& s) {
    for (int i = 0; i < g_siteCount; ++i)
        if (g_sites[i] == &s) return i;
    if (g_siteCount >= kMaxSites) {
        if (!g_siteListFullLogged) Log(true, L"script sites: more than %d sites; the rest are searched on every call", kMaxSites);
        g_siteListFullLogged = true;
        return -1;
    }
    g_sites[g_siteCount] = &s;
    return g_siteCount++;
}

// The site's PC offset in `mod`, or 0 (not a module, or the site isn't there once).
uint32_t SitePc(const uint8_t* mod, const ScriptSiteDef& s) {
    if (memcmp(mod, "smosbinL", 8) != 0) return 0;
    uint32_t size = SiteU32(mod + 8), codeStart = SiteU32(mod + 0xc), codeLen = SiteU32(mod + 0x10), ident = SiteU32(mod + 0x24);
    if (size < kSiteHeader || size > kSiteModuleMax || codeStart < kSiteHeader || codeStart > size || codeLen > size - codeStart) return 0;
    int index = SiteIndex(s);
    if (index < 0) return SearchSite(mod, codeStart, codeLen, s);
    ModuleSites* m = nullptr;
    if (g_siteLastModule < g_siteModuleCount && g_siteModules[g_siteLastModule].module == mod) m = &g_siteModules[g_siteLastModule];
    for (int i = 0; i < g_siteModuleCount && !m; ++i)
        if (g_siteModules[i].module == mod) m = &g_siteModules[g_siteLastModule = i];
    if (m && (m->size != size || m->codeStart != codeStart || m->codeLen != codeLen || m->ident != ident)) {  // another module here now
        m->size = size;
        m->codeStart = codeStart;
        m->codeLen = codeLen;
        m->ident = ident;
        m->resolved = 0;
    }
    if (!m) {
        if (g_siteModuleCount >= kMaxSiteModules) {  // full: the oldest record is reused (its module is searched again if seen again)
            if (!g_siteTableFullLogged) Log(true, L"script sites: more than %d script modules seen; the oldest answers are dropped", kMaxSiteModules);
            g_siteTableFullLogged = true;
            m = &g_siteModules[g_siteLastModule = g_siteModuleNext++ % kMaxSiteModules];
        } else {
            m = &g_siteModules[g_siteLastModule = g_siteModuleCount++];
        }
        m->module = mod;
        m->size = size;
        m->codeStart = codeStart;
        m->codeLen = codeLen;
        m->ident = ident;
        m->resolved = 0;
    }
    uint64_t bit = 1ull << index;
    if (!(m->resolved & bit)) {
        m->pc[index] = SearchSite(mod, codeStart, codeLen, s);
        m->resolved |= bit;
    }
    return m->pc[index];
}

// The native was called from site `s` of its script (VM+4 PC, just past the call; VM+8 module): the site's bytes are at PC + from,
// and that is the one place they occur in the bound module's code.
bool ScriptSite(void* vm, const ScriptSiteDef& s) {
    const uint8_t* pc = *(const uint8_t* const*)((uint8_t*)vm + 4);
    const uint8_t* mod = *(const uint8_t* const*)((uint8_t*)vm + 8);
    if (!pc || !mod || pc <= mod) return false;
    uint32_t at = SitePc(mod, s);
    return at && (uintptr_t)(pc - mod) == at && !memcmp(pc + s.from, s.code, s.n);
}

// 090a tears the race down (back to the menus).
// The results screen's row loop (module `results`, 0912(position) at offset 0x422d) reads each car's
// human number once (0x58d9fa) for its "nP" badge. Each game numbers only its own player (0, "1P");
// the other player's car is an AI there (8, no badge). For that one call the humans get their lobby
// slot instead (host 1P, guest 2P), the same on every screen. Only at that call site: the same native
// also decides the profile's race-win stat (`results` 0x53ce), which must see the real numbers.
// Our `results` module (script_subst.cpp; tools\build_menus.py results_with_profile) has 12 bytes inserted at 0x25e1: every site
// of that module after 0x25e1 lies kResultsInsert bytes further than in the stock module; the stock offsets are known too (the
// same bytes), though the stock module isn't loaded with our menus, and without them no online race runs these sites.
// tests/test_race.cpp checks every site against the built module.
const uint32_t kResultsInsert = 12;
const uint8_t kResultsRowCode[] = {0x32, 0x01, 0x12, 0x09, 0x00, 0x00, 0x96, 0x09, 0x07, 0x01, 0x31, 0x4b, 0x14};
const ScriptSiteDef kResultsRow = {"results row (0912)", kScriptResults, -8, kResultsRowCode, sizeof(kResultsRowCode),
                                   {0x422d + kResultsInsert + 8, 0x422d + 8}};  // the interpreter has moved the PC past the 8-byte call

bool ResultsRowCall(void* vm) { return ScriptSite(vm, kResultsRow); }

void __cdecl Hook0912(void* vm, NativeCall* d) {
    NativeFn original = Original<NativeFn>(kH0912);
    g.rowHuman = -1;
    if (g.phase != kRunning || !vm || !RaceOver() || !ResultsRowCall(vm)) { original(vm, d); return; }
    // The row's car: the finish list's entry at that place (0x4cc2d0), by its record index.
    if (d && d->argc == 1 && IntOrHash(d, 0) && d->values[0] < kGridSlots && d->values[0] < *(volatile uint8_t*)(uintptr_t)kFinishCount) {
        int index = ((const uint8_t*)(uintptr_t)kFinishList)[d->values[0] * 8];
        for (int k = 0; k < g.c.humanCount && k < kMaxHumans; ++k)
            if (g.racers[k] && RecordIndex(g.racers[k]) == index) g.rowHuman = k;
        g.placeHuman[d->values[0]] = g.rowHuman;  // (the profile cursor's rows)
        if ((int)d->values[0] + 1 > g.placeRows) g.placeRows = (int)d->values[0] + 1;
    }
    int32_t saved[kMaxHumans];
    for (int k = 0; k < g.c.humanCount; ++k) {
        int32_t* number = (int32_t*)((uint8_t*)g.racers[k] + kRacerHumanNumber);
        saved[k] = *number;
        *number = k;
    }
    original(vm, d);
    for (int k = 0; k < g.c.humanCount; ++k) *(int32_t*)((uint8_t*)g.racers[k] + kRacerHumanNumber) = saved[k];
    if (!g.badgeRows++) Log(false, L"race: results rows show the lobby's player numbers (host 1P)");
}

// The 1-player finish panel (module `results` 0x379c, shown when this game's player crosses the line)
// asks 0913 for the results row of creation index 0 (`INT8 0; NAT 0913` at 0x37ec) for its golden place,
// portrait and name: our player on the host, but the host's car on a guest (re\m5-finishbanner). There
// it gets our own index. 0913 (0x58ded0) maps its argument by creation index and bounds-checks it.
const uint8_t kFinishPanelCode[] = {0x51, 0x00, 0x32, 0x01, 0x13, 0x09, 0x00, 0x00, 0xd8, 0x07, 0x07, 0x01, 0x51, 0x00, 0x23};
const ScriptSiteDef kFinishPanel = {"finish panel (0913)", kScriptResults, -10, kFinishPanelCode, sizeof(kFinishPanelCode),
                                    {0x37ee + kResultsInsert + 8, 0x37ee + 8}};

void __cdecl Hook0913(void* vm, NativeCall* d) {
    if (g.phase == kRunning && g.local > 0 && vm && d->argc == 1 && d->types[0] == kTypeInt && d->values[0] == 0 &&
        ScriptSite(vm, kFinishPanel)) {
        d->values[0] = (uint32_t)g.local;  // value only; type kept
        static int logged = 0;
        if (logged++ < 2) Log(false, L"race: the finish panel shows this game's car (racer %d)", g.local);
    }
    Original<NativeFn>(kH0913)(vm, d);
}

void __cdecl Hook090a(void* vm, NativeCall* d) {
    if (g.phase != kIdle) {
        if (g.phase == kRunning) {
            if (RaceOver()) LogResults(L"when the race was closed");
            if (RaceOver() && !g.badgeRows) Log(false, L"race: the results rows were not recognised; player badges are each game's own");
            for (int i = 0; i < g.peerCount; ++i)
                Log(false, L"race: player %d's car changed nav path %d times", g.peers[i].index, g.cars[g.peers[i].index].navChanges);
            if (HostAi() && g.host) Log(false, L"race: %d car messages sent to the other players", g.carsSent);
            LogItems();
            LogItemLines();
            LogVoices();
            if (HostAi() && !g.host) {
                int ai = 0;
                for (int k = g.c.humanCount; k < kGridSlots; ++k) ai += g.cars[k].navChanges;
                Log(false, L"race: %d car messages from the host; the AI changed nav path %d times", g.carsIn, ai);
            }
        }
        if (Active()) SendAbort(kAbortQuit);
        EndRace(L"the race was closed");
    }
    Original<NativeFn>(kH090a)(vm, d);
}

// ---------------------------------------------------------------- back to the lobby (notes/RETURN-TO-LOBBY.md)
// The console's way back is still in the PC scripts. The results and pause menus pick their items by
// game type (0916): an online race (5) has RETURN TO LOBBY and no PLAY AGAIN / RESTART, which would
// restart this game alone. Our races are Single Races (1), so those two calls see 5. Every other
// 0916 call (finish display, autosave) sees the real type.
const uint8_t kResultsMenuCode[] = {0x32, 0x00, 0x16, 0x09, 0x00, 0x00, 0x2a, 0x04, 0x32, 0x00, 0x38, 0x09, 0x00, 0x00, 0x2b, 0x04};
const ScriptSiteDef kResultsMenu = {"results retry/quit menu (0916)", kScriptResults, -8, kResultsMenuCode, sizeof(kResultsMenuCode),
                                    {0x22fd + 8}};  // results GetRetryExitMenu (before the profile cursor's insertion: the same in ours)
const uint8_t kPauseMenuCode[] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x2f, 0x02, 0x51, 0x00, 0x02, 0x3b, 0x66, 0xef, 0x33};
const ScriptSiteDef kPauseMenu = {"pause menu item list (0916)", kScriptPauseMenu, -8, kPauseMenuCode, sizeof(kPauseMenuCode), {0x0cdd + 8}};
// The results list's time limit (our `results` module, re\m7-profile\FINDINGS.md: the stock's 0x25e9 moved 12 bytes down by the
// profile cursor's frame call): with type 5 the list stays up to 60 s (g9 = 3000 ticks, the console's online limit) instead of 15.
const uint8_t kResultsListCode[] = {0x32, 0x00, 0x16, 0x09, 0x00, 0x00, 0x9d, 0x04, 0x51, 0x05, 0x4a, 0x14, 0x08, 0x00, 0x57, 0x09};
const ScriptSiteDef kResultsList = {"results list limit (0916)", kScriptResults, -8, kResultsListCode, sizeof(kResultsListCode),
                                    {0x25e9 + kResultsInsert + 8, 0x25e9 + 8}};
const uint32_t kGameTypeOnline = 5;

// Online pause (M6, re\m6-pause\FINDINGS.md): the PC pause menu still has the console's. With game type 5
// it calls 0928 instead of 0926: no world pause; this player's car is driven by the AI
// (AIRacing_DisconnectedController) while the menu is open and the HUD stays; CONTINUE calls 0929 instead
// of 0927 (the car back); the menu closes itself when the results start. So the menu sees 5 at its open
// site, and then (while that pause is open) at its other type-5 sites: the race-end close, the HUD_UNPAUSE
// skips and the unpause. The latch keeps the unpause the same kind as the pause.
const uint8_t kPauseOpenCode[16] = {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xb8, 0x00, 0x51, 0x05, 0x4b, 0x14, 0x21, 0x00, 0x32, 0x00};  // from PC - 8
const ScriptSiteDef kPauseOpen = {"pause open (0916)", kScriptPauseMenu, -8, kPauseOpenCode, 16, {0x04c4}};
const uint8_t kPauseCodes[6][16] = {
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xf4, 0x00, 0x51, 0x05, 0x4a, 0x02, 0x0e, 0x42, 0xd2, 0x86},  // race end: close
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x61, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x15, 0x00, 0x02, 0x81},  // HUD_UNPAUSE skips
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x6e, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x15, 0x00, 0x02, 0x81},
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x7d, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x15, 0x00, 0x02, 0x81},
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0x94, 0x01, 0x51, 0x05, 0x4b, 0x14, 0x0c, 0x00, 0x32, 0x00},  // unpause: 0929
    {0x32, 0x00, 0x16, 0x09, 0x01, 0x00, 0xcb, 0x01, 0x51, 0x05, 0x4a, 0x02, 0x0e, 0x42, 0xd2, 0x86},  // controls page: race end
};
enum { kPauseRaceEndClose, kPauseHudSkipA, kPauseHudSkipB, kPauseHudSkipC, kPauseUnpause, kPauseControlsRaceEnd };
const ScriptSiteDef kPauseSites[6] = {
    {"pause race-end close (0916)", kScriptPauseMenu, -8, kPauseCodes[0], 16, {0x067e}},
    {"pause HUD_UNPAUSE skip a (0916)", kScriptPauseMenu, -8, kPauseCodes[1], 16, {0x0842}},
    {"pause HUD_UNPAUSE skip b (0916)", kScriptPauseMenu, -8, kPauseCodes[2], 16, {0x089a}},
    {"pause HUD_UNPAUSE skip c (0916)", kScriptPauseMenu, -8, kPauseCodes[3], 16, {0x08f6}},
    {"pause unpause (0916)", kScriptPauseMenu, -8, kPauseCodes[4], 16, {0x098d}},
    {"pause controls page race-end close (0916)", kScriptPauseMenu, -8, kPauseCodes[5], 16, {0x0aa1}},
};

// The results set blackboard key 0x86d2420e ("the results started") and nothing on PC clears it, so after any
// race it reads 1: the online pause menu's race-end checks (pause_menu 0x0686, 0x0aa9) would close the next
// pause at once. There it reads 0 until this game's car has finished (then 1: the console's close at the paused
// player's finish), its results started (results 0x1ae5, their RESULTS_START 1b03(86d2420e, 1)) or the race is
// over. Closed later, the pause menu destroyed the TOPBAR, BUTTONS and TXTBOX scenes it shares with the results,
// which had just found them and made none: no RESULTS title or CONTINUE (re\m7-results-title\FINDINGS.md).
const uint32_t kResultsStartedKey = 0x86d2420e;
const uint8_t kResultsStartCode[] = {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x51, 0x01, 0x32, 0x02, 0x03, 0x1b, 0x00, 0x00, 0xc6, 0x02};
const ScriptSiteDef kResultsStart = {"RESULTS_START (1b03)", kScriptResults, -15, kResultsStartCode, sizeof(kResultsStartCode),
                                     {0x1ade + 15}};  // results: HASH 86d2420e; INT8 1; NAT 1b03/2 (from PC - 15; both .sbn)
const uint8_t kPauseEndReadCodes[2][14] = {
    {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0xf4, 0x00, 0x4f},  // from PC - 13
    {0x02, 0x0e, 0x42, 0xd2, 0x86, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0xcb, 0x01, 0x4f},
};
const ScriptSiteDef kPauseEndReads[2] = {
    {"pause race-end key read (1b01)", kScriptPauseMenu, -13, kPauseEndReadCodes[0], 14, {0x068e}},
    {"pause controls page key read (1b01)", kScriptPauseMenu, -13, kPauseEndReadCodes[1], 14, {0x0ab1}},
};

void __cdecl Hook0916(void* vm, NativeCall* d) {
    Original<NativeFn>(kH0916)(vm, d);
    // The stock 0916 returns the type through 0x48c640, which always stores type 3 (hash); the script
    // compares it with an int (`INT8 5; EQ`). Only the value changes.
    if (!vm || (d->retType != kTypeHash && d->retType != kTypeInt)) return;
    if (ScriptSite(vm, kPauseOpen)) {
        g_netPause = g_returnToLobby && g.phase == kRunning && !RaceOver();
        g_pauseOpen = g_netPause;
        if (!g_netPause) return;
        d->retValue = kGameTypeOnline;
        Log(false, L"race: online pause: the race keeps running; the AI drives this car until CONTINUE");
        return;
    }
    for (int i = 0; i < 6; ++i)
        if (g_netPause && ScriptSite(vm, kPauseSites[i])) {
            // CONTINUE (0x098d, right before the unpause), or the race end's close: the race-end polls (0x067e, 0x0aa1) run
            // every frame while the menu is up and close it only once the race is over.
            bool endPoll = i == kPauseRaceEndClose || i == kPauseControlsRaceEnd;
            if (i == kPauseUnpause || (endPoll && (RaceOver() || g.resultsStarted || LocalFinished()))) g_pauseOpen = false;
            d->retValue = kGameTypeOnline;
            return;
        }
    if (!g_returnToLobby) return;
    if (ScriptSite(vm, kResultsList)) {
        d->retValue = kGameTypeOnline;
        static int listLogged = 0;
        if (listLogged++ < 2) Log(false, L"race: results: the list stays up to 60 s (the console's online limit) for the profile cursor");
        return;
    }
    bool results = ScriptSite(vm, kResultsMenu);
    if (!results && !ScriptSite(vm, kPauseMenu)) return;
    d->retValue = kGameTypeOnline;
    static int logged = 0;
    if (logged++ < 4) Log(false, L"race: the %s menu offers RETURN TO LOBBY", results ? L"results" : L"pause");
}

// front_main picks the menu screen to open after a race from blackboard key 0x72d1c72b (written by the
// results and pause menus). After our race: 0xb0a0d459, the console's "back to the network lobby"
// (fe_network_game export 0x45b79703, which our lobby reads as "re-enter"), or 0x7ab1c9a7 (the
// network menu's Search) when this player is no longer in the Steam lobby. Only at front_main's one
// read of that key (+0x438), once per race.
const uint32_t kReturnKey = 0x72d1c72b, kReturnLobby = 0xb0a0d459, kReturnSearch = 0x7ab1c9a7;
const uint8_t kFrontMainReadCode[] = {0x02, 0x2b, 0xc7, 0xd1, 0x72, 0x32, 0x01, 0x01, 0x1b, 0x01, 0x00, 0x92, 0x00};
const ScriptSiteDef kFrontMainRead = {"front_main return key read (1b01)", kScriptFrontMain, -13, kFrontMainReadCode,
                                      sizeof(kFrontMainReadCode), {0x438 + 8}};

void __cdecl Hook1b01(void* vm, NativeCall* d) {
    Original<NativeFn>(kH1b01)(vm, d);
    if (g_netPause && vm && d->argc >= 1 && d->types[0] == kTypeHash && d->values[0] == kResultsStartedKey) {
        for (const ScriptSiteDef& s : kPauseEndReads)
            if (ScriptSite(vm, s) && !RaceOver()) {
                if (LocalFinished()) {  // at this game's own finish (before its results make their scenes; a first race's key is 0 yet)
                    d->retValue = 1;
                    if (d->retType == kTypeNone) d->retType = kTypeInt;
                } else if (!g.resultsStarted) {
                    d->retValue = 0;  // value only; type kept
                }
            }
        return;
    }
    if (!vm || d->argc < 1 || d->types[0] != kTypeHash || d->values[0] != kReturnKey) return;
    if (!ScriptSite(vm, kFrontMainRead)) return;
    LobbySnapshot js;
    LobbyRead(js);
    if (js.pendingJoin && js.state != kLobbyOff && g_licensePicked) {  // a Steam join waits: the online search takes it
        g_returnToLobby = false;
        g_joinQuitFor = 0;
        Log(false, L"race: to the online game search for a Steam join (the game chose %08x)", d->retValue);
        d->retValue = kReturnSearch;
        d->retType = kTypeHash;
        return;
    }
    if (!g_returnToLobby) return;
    g_returnToLobby = false;
    if (d->retType != kTypeNone && d->retType != kTypeHash) {  // every writer stores a hash: not the key we know
        Log(true, L"race: the menus' return key has type %u; the game picks its own menu", d->retType);
        return;
    }
    LobbySnapshot s;
    LobbyRead(s);
    bool lobby = s.state == kLobbyInLobby;
    Log(false, L"race: back to the %s (the game chose %08x)", lobby ? L"lobby" : L"online game search", d->retValue);
    d->retValue = lobby ? kReturnLobby : kReturnSearch;
    d->retType = kTypeHash;
}

// The bridge's race routine reads the AI difficulty with 2003(3) (stock fe_game_bridge 0x2080, Roster
// Pack 0x2122, the standalone Metal Sonic 1.1.2 0x20a7: re\m9-metal112\FINDINGS.md) and writes it to blackboard GameSetup.Difficulty (0 Easy, 1 Medium, 2 Hard; SetupRace
// copies it to the AI). For our race it reads the host's: lobby AI 1..3 = difficulty 0..2. The profile
// isn't touched. Lobby AI 0 ("off"): no AI racers (Easy here; nothing uses it).
const uint8_t kBridgeDifficultyCode[] = {0x51, 0x03, 0x32, 0x01, 0x03, 0x20, 0x01, 0x00, 0xbe, 0x03};
const ScriptSiteDef kBridgeDifficulty = {"bridge difficulty (2003)", kScriptBridge, -10, kBridgeDifficultyCode, sizeof(kBridgeDifficultyCode),
                                         {0x2080 + 8, 0x2122 + 8, 0x20a7 + 8}};  // stock, Roster Pack, Metal 1.1.2
// Right after, 2003(1) reads the racer count (stock 0x20f9, Roster Pack 0x219b, Metal 1.1.2 0x2120): the bridge makes that many
// cars, its own player and then AI while any are left (re\m6-rules\FINDINGS.md A1). Our race: Cars().
const uint8_t kBridgeRacersCode[] = {0x51, 0x01, 0x32, 0x01, 0x03, 0x20, 0x01, 0x00, 0xcb, 0x03};
const ScriptSiteDef kBridgeRacers = {"bridge racer count (2003)", kScriptBridge, -10, kBridgeRacersCode, sizeof(kBridgeRacersCode),
                                     {0x20f9 + 8, 0x219b + 8, 0x2120 + 8}};

// The course availability query 2003(73, course hash) (our lobby's refresh_courses, tools/netmenu/settings.py; also the stock
// course select and leaderboards) for Egg Hangar's course: the stock answer (0x547aa0: the profile's unlocks, which the Egg Hangar
// mod's runtime extends to its course, or "yes" for every course with the unlock-all flags) stays, except "no" when the mod scan
// read every mod and Egg Hangar isn't among them (the unlock-all flags would offer a course that isn't there). The stock call runs
// first, so the answer keeps its type.
const uint32_t kQueryCourseAvailable = 73;

void __cdecl Hook2003(void* vm, NativeCall* d) {
    Original<NativeFn>(kH2003)(vm, d);
    if (d && d->argc == 2 && d->values[0] == kQueryCourseAvailable && d->values[1] == kEggCourseHash) {
        if ((g_eggScanned && !g_eggHangar) || !g_eggLobby) d->retValue = 0;
        return;
    }
    if (!(g.phase == kArmed && g.launched) && g.phase != kCreating) return;
    if (!vm || d->argc != 1 || d->retType != kTypeInt) return;
    if (ScriptSite(vm, kBridgeRacers)) {
        int32_t own = (int32_t)d->retValue;
        d->retValue = (uint32_t)Cars();  // (also a profile whose racer count isn't 8)
        g.racersSet = true;
        if (own != Cars()) Log(false, L"race: %d racers from the host's rules (this profile's: %d)", Cars(), own);
        return;
    }
    if (!ScriptSite(vm, kBridgeDifficulty)) return;
    int32_t own = (int32_t)d->retValue, host = g.c.ai >= 1 ? g.c.ai - 1 : 0;
    d->retValue = (uint32_t)host;
    g.difficultySet = true;
    Log(false, L"race: AI difficulty %d from the host's rules (lobby AI %u; this profile's: %d)%s", host, g.c.ai, own,
        g.c.ai ? L"" : L"; AI racers off");
}

// 1304 starts a save (the profile manager, settings records included, to ssr_save.bin): the profile's
// own items and hazards go back first.
void __cdecl Hook1304(void* vm, NativeCall* d) {
    RestoreRules(L"before a save");
    Original<NativeFn>(kH1304)(vm, d);
}

// ---------------------------------------------------------------- phase hooks
void __fastcall HookSetupRace(void* self, void*) {
    if (g.phase == kCreated) {
        Tick();
        // The grid sort (inside this update) shuffles the AI and puts racers whose initial state is
        // StartingGridHuman at the back in creation order. The other players' cars are AI racers, so
        // they are marked as humans for this one call: every PC then builds the same grid.
        uint32_t saved[kGridSlots] = {};
        for (int i = 0; i < g.peerCount; ++i) {
            uint32_t* initial = (uint32_t*)((uint8_t*)g.racers[g.peers[i].index] + kRacerInitialState);
            saved[i] = *initial;
            *initial = kStateGridHuman;
        }
        ((SeedFn)(uintptr_t)kSeedRng)(g.c.seed);  // same grid shuffle on every PC
        ApplyRules(g.c);  // before the update: it loads the item and hazard layers (0x4c9dc0)
        if (!g.difficultySet) Log(true, L"race: the race setup's AI difficulty read was not recognised; this game uses its own profile's AI difficulty");
        if (!g.racersSet) Log(true, L"race: the race setup's racer count read was not recognised; this game uses its own profile's");
        Original<UpdateFn>(kHSetup)(self);
        for (int i = 0; i < g.peerCount; ++i) *(uint32_t*)((uint8_t*)g.racers[g.peers[i].index] + kRacerInitialState) = saved[i];
        g.phase = kSetUp;
        g.holdSince = GetTickCount();
        return;
    }
    if (g.phase == kArmed || g.phase == kCreating) Fail(L"the race started before all its racers were created");
    // A race that isn't ours (offline): its menus are the stock ones, even if front_main never read
    // the return key after our last race (another branch, or another mod's front_main).
    if (g.phase == kIdle) g_returnToLobby = false;
    Original<UpdateFn>(kHSetup)(self);
}

void ResendLoaded(uint8_t stage) {
    if (g.released >= stage || GetTickCount() - g.lastLoadedSent < 2000) return;
    uint8_t b[kWireMax];
    SendToAll(b, EncodeLoaded(g.c.race, stage, (uint8_t)g.local, b, sizeof(b)), true);
    g.lastLoadedSent = GetTickCount();
}

// Hold A: loaded, racers placed, loading screen still up. Returns true while held.
bool HoldA() {
    if (g.phase == kSetUp) {
        uint8_t count = 0;
        SceneRacerCount(count);
        bool course = *(void**)(uintptr_t)kCourseGraph != nullptr;
        if (!course || count != Cars()) {
            if (!g.courseLogged) Log(true, L"online race %08x: the course did not load (course %d, racers %u); waiting", g.c.race, course, count);
            g.courseLogged = true;
            if (GetTickCount() - g.holdSince < 30000) return true;
            if (!course) {
                // Released without a course graph, the race HUD crashes (Codex's Kit39 crash), so this stays.
                static DWORD lastLog = 0;
                if (!lastLog) SendAbort(kAbortMismatch);  // once: the others race on instead of waiting
                if (!lastLog || GetTickCount() - lastLog > 15000) {
                    Log(true, L"online race %08x: the course never loaded; close the game (Alt+F4)", g.c.race);
                    lastLog = GetTickCount() | 1;
                }
                return true;
            }
            wchar_t why[64];
            swprintf_s(why, L"the race has %u racers, not %d", count, Cars());
            Fail(why);
            return false;
        }
        for (int i = 0; i < g.peerCount; ++i) {  // the other players' cars become network cars
            uint8_t* racer = (uint8_t*)g.racers[g.peers[i].index];
            void* controller = *(void**)(racer + kRacerController);
            g.initial[g.peers[i].index] = *(uint32_t*)(racer + kRacerInitialState);
            if (controller) ((SetStateFn)(uintptr_t)kSetControlState)(controller, kStateNetwork);
            *(uint32_t*)(racer + kRacerInitialState) = kStateNetwork;
            if (racer[kRacerAiThrottle]) Log(false, L"race: player %d's car updates its track cursor every 8th tick (AI setting)", g.peers[i].index);
        }
        if (!g.host && HostAi() && !HostGone() && Cars() > g.c.humanCount) {  // the AI follow the host's game, as network cars too
            int throttled = 0;
            for (int k = g.c.humanCount; k < Cars(); ++k) {
                uint8_t* racer = (uint8_t*)g.racers[k];
                void* controller = *(void**)(racer + kRacerController);
                g.initial[k] = *(uint32_t*)(racer + kRacerInitialState);
                if (controller) ((SetStateFn)(uintptr_t)kSetControlState)(controller, kStateNetwork);
                *(uint32_t*)(racer + kRacerInitialState) = kStateNetwork;
                g.remote[k] = true;
                throttled += racer[kRacerAiThrottle] ? 1 : 0;
            }
            g.aiPuppets = true;
            Log(false, L"race: the %d AI cars follow the host's game (%d update their track cursor every 8th tick)", Cars() - g.c.humanCount,
                throttled);
        }
        for (int k = 0; k < Cars(); ++k)  // lap records are in creation order (0x49fa2e)
            if (RecordIndex(g.racers[k]) != k) Log(true, L"race: racer %d has lap record %d; laps and results may be off", k, RecordIndex(g.racers[k]));
        uint8_t* graph = Graph();
        Log(false, L"race: this course has %u nav paths", graph ? *(uint32_t*)(graph + kGraphPathCount) : 0);
        g.phase = kHoldA;
        g_lastRaceId = g.c.race;  // (from here on: a start that failed leaves this player in the lobby, where the others' race shows)
        g.reached = kStageLoaded;
        g.holdSince = GetTickCount();
        g.lastLoadedSent = GetTickCount();
        uint8_t b[kWireMax];
        SendToAll(b, EncodeLoaded(g.c.race, kStageLoaded, (uint8_t)g.local, b, sizeof(b)), true);
        Log(false, L"race: loaded, waiting for the others");
        HostReleases();
    }
    if (g.phase != kHoldA) return false;
    ResendLoaded(kStageLoaded);
    if (g.released >= kStageLoaded) {
        g.phase = kIntros;
        Log(false, L"race: hold A released after %lu ms", GetTickCount() - g.holdSince);
        return false;
    }
    if (GetTickCount() - g.holdSince > 90000) {
        Log(true, L"online race %08x: the other players did not load within 90 s; starting anyway", g.c.race);
        g.released = kStageCountdown;
        g.phase = kIntros;
        return false;
    }
    return true;
}

void Waiting(void* self, int which) {
    if (Active()) Tick();
    if (Active() && HoldA()) return;
    Original<UpdateFn>(which)(self);
}
void __fastcall HookWaiting(void* self, void*) { Waiting(self, kHWaiting); }
void __fastcall HookWaitingNoIntro(void* self, void*) {
    if (g.phase > kHoldA && Active()) Fail(L"the race was restarted");  // a restart isn't shared
    Waiting(self, kHWaitingNoIntro);
}

void IntroUpdate(void* self, int which);  // the intros: each screen's own skip, the course intro's end (below the HUD helpers)
void __fastcall HookTrackIntro(void* self, void*) { IntroUpdate(self, kHTrackIntro); }
void __fastcall HookCharacterIntro(void* self, void*) { IntroUpdate(self, kHCharIntro); }

// ---------------------------------------------------------------- the race clock on real time
// The game steps its simulation 50 times a second, at most 4 steps a frame (0x4a05f0): a frame longer than about 80 ms
// drops the rest, and the race clock [kRaceClock] (one tick per step) falls behind real time. A PC with long frames then
// reports a better time than the race really took (player logs: 0-2.7 s a race, up to 8 s; re\race-clock\FINDINGS.md).
// D = T - acc - step ms * (steps + n), taken at a frame's first step, stays the same while the simulation keeps up and
// grows by exactly the time dropped. From the countdown's release, each Racing / WaitingForFinishers tick adds the whole
// ticks dropped since to the race clock, so the HUD time, the lap and finish times and the race end's estimates (scaled
// from a finisher's time, 0x4cd290) are real time on every PC. Nothing in a Single Race tests the clock for one value.
double StepMs() {
    uint32_t rate = *(volatile uint32_t*)(uintptr_t)kTickRate;
    return 1000.0 / (rate ? rate : 50);
}

// Every step from the intros to the race end (the intro, countdown, Racing and WaitingForFinishers hooks), before the stock
// update. D at a frame's first step already holds that frame's own dropped steps ([kFrameSteps] is the capped count).
void ClockObserve() {
    uint8_t* app = *(uint8_t**)(uintptr_t)kAppPtr;
    if (!app) return;
    uint32_t t = *(volatile uint32_t*)(app + kAppFrameTime);
    uint32_t step = *(volatile uint32_t*)(uintptr_t)kStepCount;
    if (!g.clockSeen) {
        g.clockSeen = true;
        g.clockT0 = t;
        g.clockStep0 = step;
    } else if (t != g.clockT && step == g.clockStep + 1) {  // a new frame, and no step missed since: its first step
        float acc = *(volatile float*)(uintptr_t)kStepAcc;
        int32_t n = *(volatile int32_t*)(uintptr_t)kFrameSteps;
        g.clockD = (double)(uint32_t)(t - g.clockT0) - acc - StepMs() * ((double)(uint32_t)(step - g.clockStep0) + n);
        g.clockValid = true;
        if (g.phase == kRunning && !g.clockBase) {  // released before a frame start was seen: from here
            g.clockBase = true;
            g.clockD0 = g.clockD;
        }
    }
    g.clockT = t;
    g.clockStep = step;
}

// At the countdown's release (every screen lined up): the dropped time from here on counts.
void ClockStart() {
    if (!g.clockValid) return;  // (ClockObserve takes the next frame start)
    g.clockBase = true;
    g.clockD0 = g.clockD;
}

// A Racing / WaitingForFinishers tick, before the stock update: the race clock catches up the whole ticks dropped.
void ClockCatchUp() {
    if (!g.clockBase || g.clockBroken || RaceOver()) return;
    double lost = g.clockD - g.clockD0;
    if (lost < 0) return;
    if (lost > 3600000.0) {  // over an hour: not a long frame
        g.clockBroken = true;
        Log(true, L"race: clock: the frame timer read %.0f ms dropped; the race clock is left as it is", lost);
        return;
    }
    uint32_t due = (uint32_t)(lost / StepMs());
    if (due <= g.clockAdded) return;
    uint32_t add = due - g.clockAdded;
    *(volatile uint32_t*)(uintptr_t)kRaceClock += add;
    g.clockAdded = due;
    ++g.clockFixes;
    if (add > g.clockBiggest) g.clockBiggest = add;
    if (add >= 5 && g.clockLogged < 20) {  // 100 ms or more at once
        ++g.clockLogged;
        Log(false, L"race: clock: a long frame here dropped %.0f ms; the race clock caught up (%.0f ms in all)", add * StepMs(),
            g.clockAdded * StepMs());
    }
}

void ClockSummary() {
    if (g.clockBase) Log(false, L"race: clock: this game's race clock caught up %.0f ms dropped by long frames (%d catch-ups, the largest %.0f ms)",
                         g.clockAdded * StepMs(), g.clockFixes, g.clockBiggest * StepMs());
    else Log(true, L"race: clock: the frame timer was never read; this game's times are its simulation's");
}

// Hold B: before the first countdown step, until everyone has reached it.
const uint32_t kCountdownSalt = 0x474f474f;  // "GOGO": not the grid's or the tips' seed
void __fastcall HookCountdown(void* self, void*) {
    g_raceUpdateTick = SimTick();
    if (Active()) {
        Tick();
        if (g.phase == kIntros || g.phase == kHoldA) {
            g.phase = kHoldB;
            g.reached = kStageCountdown;
            g.holdSince = GetTickCount();
            g.lastLoadedSent = GetTickCount();
            uint8_t b[kWireMax];
            SendToAll(b, EncodeLoaded(g.c.race, kStageCountdown, (uint8_t)g.local, b, sizeof(b)), true);
            HostReleases();
        }
        ClockObserve();
        if (g.phase == kHoldB) {
            ResendLoaded(kStageCountdown);
            // (a player who skipped the intros at once waits here for the others' whole intros: up to kIntroCap + the racer intro's 5 s)
            if (g.released < kStageCountdown && GetTickCount() - g.holdSince <= kHoldBMs) return;
            if (g.released < kStageCountdown) Log(true, L"online race %08x: the others did not reach the countdown within %lu s; starting", g.c.race, kHoldBMs / 1000);
            else Log(false, L"race: countdown released after %lu ms", GetTickCount() - g.holdSince);
            g.phase = kRunning;
            ClockStart();
            // The shared RNG again from the race's seed, here where every screen is lined up: the intros may have
            // drawn a different number of times per PC (the course-music picker draws once only on a PC that owns 2+
            // of the zone's tracks; re\m7-music\FINDINGS.md 4).
            ((SeedFn)(uintptr_t)kSeedRng)(g.c.seed ^ kCountdownSalt);
            AnnounceStart();
        }
    }
    Original<UpdateFn>(kHCountdown)(self);
}

// Racing and WaitingForFinishers. The race end (0x4cdbf0) waits until the finished humans equal
// scene+0x1104, which counts only this game's player. While another player is still racing, or has
// finished while ours hasn't, that count is raised by one for the length of the update only, as in a
// 2-player split-screen race: the race waits for the other player's Finished, and the stock 30 s
// "RACE ENDS IN" timer ends it for a player far behind. Outside the update the game sees the real
// count (with 2 there, the results screen would go on to the Mission/League flow).
void RaceUpdate(void* self, int which, bool wff) {
    g_raceUpdateTick = SimTick();
    if (!Active()) { Original<UpdateFn>(which)(self); return; }
    if (g.phase == kRunning) g.racing = true;  // (before Tick: a car handed to the AI now gets the racing state)
    Tick();
    bool wait = false;
    if (g.phase == kRunning) {
        ClockObserve();
        ClockCatchUp();  // (before the stock update: this tick's lap and finish stamps include it)
        if (g_debugStallMs && !g.clockStalled && *(volatile uint32_t*)(uintptr_t)kRaceClock >= 500 && !RaceOver()) {
            g.clockStalled = true;  // (10 s in: a stutter of [Debug] StallMs, to test ClockCatchUp in game)
            Log(false, L"race: debugging: this game freezes for %u ms now (config.ini [Debug] StallMs)", g_debugStallMs);
            Sleep(g_debugStallMs);
        }
        DropNetworkCarItems();
        ApplyFinishes();
        NoteTestFinishes();
        ApplyHostResults();
        CheckOpaShots();  // (the cars' All-Star ticks run in the scene update, after this race update: last tick's proof)
        CheckPulseShots();
        DropGateShots();
        RunQueuedEvents();
        if (ItemsOn()) PlayReactions();  // every tick (also after the race end: the puppets' own reactions stay quiet)
        EndAllStars();
        ForceOpaShots();
        ForcePulseShots();
        ForceMetalTimeUp();
        bool mine = LocalFinished();
        // Guest with the host's AI: the race also waits for the host's results (its end is the host's).
        bool table = WaitingForHostResults();
        wait = !RaceOver() && (OthersStillRacing() || (!mine && OthersFinished()) || table);
        // Another player finished first: the timer our own finish would have started (0x5b2250).
        int32_t* timer = wff ? (int32_t*)((uint8_t*)self + kWffTimerState) : nullptr;
        if (wff && wait && !mine && OthersFinished() && *timer == -1) {
            ((StartTimerFn)(uintptr_t)kStartEndTimer)(30);
            *timer = 0;
            Log(false, L"race: another player finished first; this race ends in 30 s");
        }
        if (table && !RaceOver() && !HoldForHostResults(mine, timer)) wait = OthersStillRacing() || (!mine && OthersFinished());
        // Without the host's AI: ours finished first and the timer is about to run out with another
        // player still racing. That player's game ends its race at about the same moment (its timer
        // started when our Finished arrived) and estimates their time itself; wait up to 5 s more for
        // that result, so both screens show the same one, by holding the timer two ticks short of its
        // end. (With the host's AI the host estimates everyone at its timer's end and sends that.)
        if (!HostAi() && wff && wait && mine && OthersStillRacing() && *timer != -1) {
            uint32_t* elapsed = (uint32_t*)(uintptr_t)kEndTimerElapsed;
            uint32_t limit = *(volatile uint32_t*)(uintptr_t)kEndTimerLimit;
            if (limit >= 2 && *elapsed + 2 >= limit) {
                if (!g.graceOn) {
                    g.graceOn = true;
                    g.graceSince = SimTick();
                    Log(false, L"race: the race-end timer ran out; waiting up to 5 s for the other player's own result");
                }
                if (SimTick() - g.graceSince < kGraceTicks) {
                    *elapsed = limit - 2;
                } else if (!g.graceLogged) {
                    g.graceLogged = true;
                    Log(true, L"race: the other player's result did not come; this game estimates their time");
                }
            }
        }
    }
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    uint32_t* humans = wait && scene ? (uint32_t*)(scene + kSceneHumans) : nullptr;
    if (humans) ++*humans;
    Original<UpdateFn>(which)(self);
    if (humans) --*humans;
    if (g.phase != kRunning) return;
    SendAllStarPhases();
    SendAllStarShots();  // (after the phases: a phase-4 message with the weapon always comes before the shots)
    SendPulseShots();
    SendReactions();
    if (wff && RaceOver()) g.sorted = true;  // the update sorts in the tick it ends (or the first WFF tick after)
    if (g.sorted) {
        MarkPlayersDnf();
        DnfLast();
    }
    SendOwnFinish();
    TrySendResults();
    MatchHostOrder();
    LogLaps();
    if (g.racing) PublishProgress();
    LeaderLapCalls();
    PlayerFinishLines();
    ReportWrongWay();
    ItemLines();
    if (RaceOver() && !g.overLogged) {
        g.overLogged = true;
        Log(false, L"race: this game's race is over");
        ClockSummary();
    }
    if (wff && RaceOver()) LogResults(L"as sorted by the game");  // the update sorts in the tick it ends
}

void __fastcall HookRacing(void* self, void*) { RaceUpdate(self, kHRacing, false); }
void __fastcall HookWaitForFinishers(void* self, void*) { RaceUpdate(self, kHWff, true); }

// ---------------------------------------------------------------- the cars, once per car per tick
// Racer vtable slot 13 (0x4f3c50) runs for every car at the end of each sim step, after physics: it
// commits the car to its scene nodes, which keep the previous and current transform per tick and are
// blended by the renderer. So a pose written just before it shows smoothly, and our own car's pose
// read just after it is final for this tick.
void WritePose(uint8_t* racer, const PlayoutPose& p) {
    float pos4[4] = {p.pos[0], p.pos[1], p.pos[2], *(float*)(racer + kRacerPos + 12)};  // 0x4f8440 copies all 4
    ((SetPositionFn)kSetPosition)(racer, pos4);
    memcpy(racer + kRacerRot, p.rot, sizeof(p.rot));
    float vel4[4] = {p.vel[0], p.vel[1], p.vel[2], 0.0f};
    memcpy(racer + kRacerVel, vel4, sizeof(vel4));
    memcpy(racer + kRacerSpin, p.spin, sizeof(p.spin));
    racer[kRacerDirty] = 1;  // rebuild the matrix from the new rotation
}

// ---------------------------------------------------------------- exhaust effects (re\m6-race-hud\EXHAUST-FINDINGS.md)
// A network car's input block stays empty, so its boost state never builds and its exhaust ("SpeedBoost"
// component, racer+0x1e64) shows nothing: no grid rev, no drift or start boost flames. The owner sends its
// exhaust's mode with each pose; the receiver's exhaust update (vtable slot 5, data slot 0x6af4fc) is
// shown that mode by giving it, for that call only, the boost fields it reads. Physics, boost timers and
// items never see them (the update only reads them; the game step is single-threaded).
const uint32_t kRacerExhaust = 0x1e64, kExhaustVtable = 0x6af4e8, kExFired = 0x70, kExMode = 0x78, kExRacer = 4;
const uint32_t kRacerBoostHolder = 0x9f4, kHolderBoost = 0x3c4, kRacerStatus = 0x1d10;
const uint32_t kBoostFlags[5] = {0x18a, 0x190, 0x192, 0x193, 0x195};  // any set: the flame (mode 4)
const uint32_t kBoostCharge = 0x2f0, kBoostLevel = 0x334, kBoostStamp = 0x184;  // 4 charge lanes, level 0..3, fired tick
static_assert(kGridSlots == 8, "Race::fxShown starts with 8 kFxNone");
int SlotOf(const void* racer);
bool StillNetworkCar(const uint8_t* racer, int k);

// The owner's exhaust mode for car k, with a serial that changes when its flame fires again (+0x70).
uint8_t FxOf(const uint8_t* racer, int k) {
    const uint8_t* e = *(const uint8_t* const*)(racer + kRacerExhaust);
    if (!e || *(const uint32_t*)e != kExhaustVtable) return kFxNone;
    uint32_t mode = *(const uint32_t*)(e + kExMode), fired = *(const uint32_t*)(e + kExFired);
    if (mode > 7 || mode == kFxNone || mode == 6) return kFxNone;
    if (g.fxFiredSet[k] && fired != g.fxFired[k]) g.fxSerial[k] = (uint8_t)((g.fxSerial[k] + 1) & 15);
    g.fxFired[k] = fired;
    g.fxFiredSet[k] = true;
    return (uint8_t)(mode | (g.fxSerial[k] << 3));
}

// The receiver: the mode its owner's screen shows, from the pose just written (one tick behind).
void NoteFx(int k, uint8_t fx) {
    if (FxState(fx) == kFxNone) { g.fxShown[k] = kFxNone; return; }
    if (FxState(g.fxShown[k]) == kFxNone || FxSerial(fx) != FxSerial(g.fxShown[k])) g.fxStamp[k] = SimTick();
    g.fxShown[k] = fx;
    if (!g.fxLogged && g_fxHooks) Log(false, L"race: car %d (%hs) shows its owner's exhaust effects", k, RacerName(k));
    g.fxLogged = true;
}

typedef void(__thiscall* ExhaustFn)(void* exhaust);
void __fastcall HookExhaust(uint8_t* e, void*) {
    ExhaustFn original = Original<ExhaustFn>(kHExhaust);
    uint8_t* racer = e && (g.phase == kHoldB || g.phase == kRunning) ? *(uint8_t**)(e + kExRacer) : nullptr;
    int k = racer ? SlotOf(racer) : -1;
    uint8_t s = k >= 0 ? FxState(g.fxShown[k]) : kFxNone;
    uint8_t* holder = s != kFxNone && g.remote[k] ? *(uint8_t**)(racer + kRacerBoostHolder) : nullptr;
    uint8_t* b = holder ? *(uint8_t**)(holder + kHolderBoost) : nullptr;
    if (!b || !StillNetworkCar(racer, k)) { original(e); return; }
    uint8_t flags[5], charge[16];
    uint32_t level = *(uint32_t*)(b + kBoostLevel), stamp = *(uint32_t*)(b + kBoostStamp), status = *(uint32_t*)(racer + kRacerStatus);
    for (int i = 0; i < 5; ++i) { flags[i] = b[kBoostFlags[i]]; b[kBoostFlags[i]] = 0; }
    memcpy(charge, b + kBoostCharge, sizeof(charge));
    *(uint32_t*)(racer + kRacerStatus) = status & ~8u;
    float lanes = s <= 3 ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) memcpy(b + kBoostCharge + 4 * i, &lanes, 4);
    if (s <= 3) *(uint32_t*)(b + kBoostLevel) = s;
    if (s == 4) {
        b[kBoostFlags[0]] = 1;
        *(uint32_t*)(b + kBoostStamp) = g.fxStamp[k];
    }
    original(e);
    for (int i = 0; i < 5; ++i) b[kBoostFlags[i]] = flags[i];
    memcpy(b + kBoostCharge, charge, sizeof(charge));
    *(uint32_t*)(b + kBoostLevel) = level;
    *(uint32_t*)(b + kBoostStamp) = stamp;
    *(uint32_t*)(racer + kRacerStatus) = status;
}

// ---------------------------------------------------------------- catch-up (re\m6-rules\FINDINGS.md B)
// The lobby's CATCH-UP: the console's human catch-up, slot 11 of the Human states (0x5ae2b0): the leading
// human is slowed (racer+0xb00, 4 speed scales) and the last one boosted (input block +0x4c), by their gap
// to the next human. Stock, it counts only this PC's humans and the bridge turns it on only for 2+ of them
// (state +0x59), so online it stayed off. Here each game runs it for its own car against every player's
// car still racing (each car's catch-up runs where its physics do: on its owner's game).
GAME_ADDRESS kRaceDistance = 0x4e8740;  // thiscall(racer) -> float: the lap record's race distance (0 before the first crossing)
GAME_ADDRESS kNearCheck = 0x4fb650;     // thiscall(tracker, float metres) -> bool: the stock function keeps the defaults then
GAME_ADDRESS kArenaCourse = 0xd127f2;   // byte: the course is an arena (no catch-up)
GAME_ADDRESS kNearMetres = 0x6b9038;    // float 350
GAME_ADDRESS kScaleDefaults = 0xc28570; // 4 floats {1, 1, 1, 1}
const uint32_t kStateRacer = 0x10, kStateController = 0x14, kStateInputs = 0x18, kStateTracker = 0x1c, kStateCatchupOn = 0x59;
const uint32_t kRacerSpeedScale = 0xb00, kInputsCatchup = 0x4c, kCtrlCatchupParams = 0x250;
const uint32_t kParamCatchupMin = 0x94, kParamCatchupMax = 0x98, kParamCatchup = 0x9c;     // HumanCatchupRange Min/Max, HumanCatchupMax
const uint32_t kParamSlowMin = 0xa0, kParamSlowMax = 0xa4, kParamSlowDown = 0xa8;          // HumanSlowDownRange Min/Max, HumanSlowDownMin
typedef float(__thiscall* DistanceFn)(void* racer);
typedef bool(__thiscall* NearFn)(void* tracker, float metres);
// The flip at lap crossings (re\m8-flag\FINDINGS.md Q2): 0x4e8740 = (lap - 1) * L + wrap(live cursor - start line), but the lap
// (record +0x7c) is counted only in the race update, after the cars' controller updates moved their cursors: in the tick a car
// passes the line it reads one lap short (the leader ranked last for a tick). Every car is measured instead as of the last race
// update: its cursor then is kept at racer+0xb84 (0x51a500 -> 0x4e8250).
GAME_ADDRESS kNavPosition = 0x4e80f0;  // thiscall(racer) -> float: the live track cursor (0x519f40 reads it)
const uint32_t kRacerCountedNav = 0xb84, kSceneSectors = 0x10e4, kSectorStartLine = 0x2f8;  // (and kGraphLapLength, graph +0x64)
typedef float(__thiscall* NavPositionFn)(void* racer);

float CountedDistance(uint8_t* r, const uint8_t* rec) {
    float d = ((DistanceFn)(uintptr_t)kRaceDistance)(r);
    if (!rec || !*(const uint32_t*)(rec + kRecCrossings)) return d;  // (0 before the first crossing)
    const uint8_t* graph = Graph();
    const uint8_t* scene = *(const uint8_t* const*)(uintptr_t)kScene;
    const uint8_t* sectors = scene ? *(const uint8_t* const*)(scene + kSceneSectors) : nullptr;
    if (!graph || !sectors) return d;
    float lap = *(const float*)(graph + kGraphLapLength), line = *(const float*)(sectors + kSectorStartLine);
    if (!(lap > 0)) return d;
    auto wrap = [lap, line](float p) { p -= line; for (int i = 0; p < 0 && i < 8; ++i) p += lap; return p; };  // as 0x519f40
    float live = wrap(((NavPositionFn)(uintptr_t)kNavPosition)(r)), counted = wrap(*(const float*)(r + kRacerCountedNav));
    if (fabsf(live - counted) > lap / 2 && g.crossingLogs < 20) {
        ++g.crossingLogs;
        Log(false, L"race: catch-up: a car passed the line before its lap record counted it (read %.0f, counted %.0f)", d, d - live + counted);
    }
    return d - live + counted;
}
typedef void(__thiscall* CatchupFn)(void* state);

// ---------------------------------------------------------------- Drift+ off (steer.h, re\drift-plus\FINDINGS.md)
// After a human steering state's update (the controller's update runs before the car's physics in the same tick): this game's
// own car, driven by a controller, steers with its stick OR its d-pad. The stock value is rebuilt from the device first; when it
// doesn't match (a state that changes the steering on its own), it is left alone.
typedef void(__thiscall* SteerStateFn)(uint8_t* state);
const uint32_t kInputMgrPtr = 0xc2a1b4, kKeyboardVtable = 0x67e444, kActionRows = 0x6b3390, kRacerSteerFlags = 0x1d0c;

float DeviceValue(const uint8_t* dev, uint32_t id) {
    if (id >= 0x2f || !(dev[0xd4 + id] & 1)) return 0;  // (ids past 0x2e would read the bound flags as channel numbers)
    const float* channels = *(const float* const*)(dev + 0x1bc);
    int32_t c = *(const int32_t*)(dev + 0x18 + 4 * id);
    return channels && c >= 0 && c < 256 ? channels[c] : 0;
}

void SteerAfter(uint8_t* state, const wchar_t* what) {
    if (!(g.c.flags & kContractNoDriftPlus) || g.phase == kIdle || g.phase == kFailed || g.local < 0 || !state) return;  // (failed: an offline race)
    uint8_t* racer = *(uint8_t**)(state + kStateRacer);
    uint8_t* controller = *(uint8_t**)(state + kStateController);
    uint8_t* inputs = *(uint8_t**)(state + kStateInputs);
    if (!racer || racer != g.racers[g.local] || !controller || !inputs) return;
    uint8_t* in = *(uint8_t**)(uintptr_t)kInputMgrPtr;
    uint8_t* mgr = in ? *(uint8_t**)(in + 0x14) : nullptr;
    int32_t device = *(int32_t*)inputs;  // the racer's device index (CS[0])
    if (!mgr || device < 0 || device >= 16 || device >= *(int32_t*)(mgr + 4)) return;
    uint8_t* dev = *(uint8_t**)(mgr + 0xc + 4 * device);
    uint32_t row = *(uint32_t*)(controller + 0x20);
    if (!dev || *(uint32_t*)dev == kKeyboardVtable || row > 3) return;  // (a keyboard can't steer twice)
    const uint32_t* ids = (const uint32_t*)(uintptr_t)(kActionRows + 0x4c * row);
    float stick = DeviceValue(dev, ids[4]), left = DeviceValue(dev, ids[15]), right = DeviceValue(dev, ids[16]);
    bool flip = (*(uint32_t*)(racer + kRacerSteerFlags) >> 3) & 1;  // (0x5af150's last step: the steering reversed)
    float stock = right + (stick - left);
    float* steer = (float*)(inputs + 0x40);
    if (fabsf(*steer - (flip ? -stock : stock)) > 1e-4f) {
        if (!g.steerSkipLogged) Log(false, L"race: Drift+ off: the %s steering (%.3f) isn't the stick + d-pad sum (%.3f); left as it is", what, *steer, stock);
        g.steerSkipLogged = true;
        return;
    }
    float one = SteerOneInput(stick, left, right, g.steerOwner);
    if (flip) one = -one;
    if (one == *steer) return;
    if (!g.steerLogged)
        Log(false, L"race: Drift+ off: the stick (%.2f) and the d-pad (%.0f) were used together; only the %s steers (%.2f, not %.2f)", stick,
            right - left, g.steerOwner == kSteerStick ? L"stick" : L"d-pad", one, *steer);
    g.steerLogged = true;
    *steer = one;
}

void __fastcall HookSteerHuman(uint8_t* state, void*) {
    Original<SteerStateFn>(kHSteerHuman)(state);
    SteerAfter(state, L"Human");
}
void __fastcall HookSteerErratic(uint8_t* state, void*) {
    Original<SteerStateFn>(kHSteerErratic)(state);
    SteerAfter(state, L"Human_Erratic");
}
void __fastcall HookSteerGrid(uint8_t* state, void*) {
    Original<SteerStateFn>(kHSteerGrid)(state);
    SteerAfter(state, L"StartingGridHuman");
}

void __fastcall HookCatchup(uint8_t* state, void*) {
    Original<CatchupFn>(kHCatchup)(state);  // inert online (+0x59 = 0)
    if (!state || state[kStateCatchupOn] || !g.c.catchup || g.phase != kRunning || !g.racing || g.local < 0) return;
    uint8_t* racer = *(uint8_t**)(state + kStateRacer);
    if (RaceOver()) {  // the race is over: the defaults again (once)
        if (g.catchupLogged && racer == g.racers[g.local] && *(uint8_t**)(state + kStateInputs)) {
            float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            memcpy(racer + kRacerSpeedScale, one, sizeof(one));
            *(float*)(*(uint8_t**)(state + kStateInputs) + kInputsCatchup) = 0;
            g.catchupLogged = 0;
        }
        return;
    }
    uint8_t* controller = *(uint8_t**)(state + kStateController);
    uint8_t* inputs = *(uint8_t**)(state + kStateInputs);
    void* tracker = *(void**)(state + kStateTracker);
    if (!racer || racer != g.racers[g.local] || !controller || !inputs || *(volatile uint8_t*)(uintptr_t)kArenaCourse) return;
    float scale[4];
    memcpy(scale, (const void*)(uintptr_t)kScaleDefaults, sizeof(scale));
    if (scale[0] == 0) scale[0] = scale[1] = scale[2] = scale[3] = 1.0f;
    memcpy(racer + kRacerSpeedScale, scale, sizeof(scale));
    float catchup = 0;
    // The players' cars still racing, by race distance (farthest first).
    struct Entry { int k; float d; } cars[kMaxHumans];
    int n = 0, me = -1;
    for (int k = 0; k < g.c.humanCount && k < kMaxHumans; ++k) {
        uint8_t* r = (uint8_t*)g.racers[k];
        if (!r || g.adopted[k] || !Record(RecordIndex(r)) || FinishReason(RecordIndex(r))) continue;
        if (k != g.local && OwnerGone(k)) continue;
        cars[n++] = {k, CountedDistance(r, Record(RecordIndex(r)))};
    }
    std::sort(cars, cars + n, [](const Entry& a, const Entry& b) { return a.d > b.d; });
    for (int i = 0; i < n; ++i)
        if (cars[i].k == g.local) me = i;
    const uint8_t* p = *(const uint8_t* const*)(controller + kCtrlCatchupParams);
    int rank = 0;
    if (n >= 2 && me >= 0 && p && !((NearFn)(uintptr_t)kNearCheck)(tracker, *(volatile float*)(uintptr_t)kNearMetres)) {
        auto f = [p](uint32_t at) { return *(const float*)(p + at); };
        if (me == 0) {  // leading: slowed by its lead over the 2nd
            float gap = cars[1].d - cars[0].d, sd = 1.0f;
            if (f(kParamSlowMax) > gap) sd = f(kParamSlowDown);
            else if (f(kParamSlowMin) > gap) {
                float t = (gap - f(kParamSlowMin)) / (f(kParamSlowMax) - f(kParamSlowMin));
                sd = f(kParamSlowDown) * t + 1.0f - t;
            }
            if (sd != 1.0f) {
                for (float& s : scale) s = sd;
                memcpy(racer + kRacerSpeedScale, scale, sizeof(scale));
                rank = 1;
            }
        } else if (me == n - 1) {  // last: boosted by its gap to the one ahead
            float gap = cars[me - 1].d - cars[me].d;
            if (gap > f(kParamCatchupMax)) catchup = f(kParamCatchup);
            else if (gap > f(kParamCatchupMin)) catchup = (gap - f(kParamCatchupMin)) / (f(kParamCatchupMax) - f(kParamCatchupMin)) * f(kParamCatchup);
            if (catchup != 0) rank = 2;
        }
    }
    *(float*)(inputs + kInputsCatchup) = catchup;
    if (rank != g.catchupLogged) {
        g.catchupLogged = rank;
        static const wchar_t* what[] = {L"off", L"slowed (leading)", L"boosted (last)"};
        Log(false, L"race: catch-up for this game's car: %s (%d players racing; scale %.2f, boost %.2f)", what[rank], n, scale[0], catchup);
    }
}

// Car k's pose this tick, as it goes out. A car far from its last sent pose has jumped (a respawn, or
// it was not active for a while): flagged as a snap, so the receiver doesn't blend across the jump.
bool ReadPose(const uint8_t* racer, int k, RacePose& p) {
    p = RacePose{};
    p.race = g.c.race;
    p.slot = (uint8_t)k;
    p.tick = SimTick();
    memcpy(p.pos, racer + kRacerPos, sizeof(p.pos));
    memcpy(p.rot, racer + kRacerRot, sizeof(p.rot));
    memcpy(p.vel, racer + kRacerVel, sizeof(p.vel));
    memcpy(p.spin, racer + kRacerSpin, sizeof(p.spin));
    p.nav = NavOf(racer);
    p.fx = FxOf(racer, k);
    float d2 = 0;
    for (int i = 0; i < 3; ++i) d2 += (p.pos[i] - g.lastPos[k][i]) * (p.pos[i] - g.lastPos[k][i]);
    if (g.lastPosSet[k] && !(d2 <= kSnapMetres * kSnapMetres)) p.flags |= kPoseSnap;
    memcpy(g.lastPos[k], p.pos, sizeof(p.pos));
    g.lastPosSet[k] = true;
    if (PoseValid(p)) return true;
    static bool refusedLogged = false;  // once: explains "the other car never moves" on the other PC
    if (!refusedLogged) {
        Log(true, L"race: car %d's pose is out of range and was not sent (pos %.1f %.1f %.1f)", k, p.pos[0], p.pos[1], p.pos[2]);
        refusedLogged = true;
    }
    return false;
}

void SendPose(const uint8_t* racer) {
    RacePose p;
    uint8_t b[kWireMax];
    size_t n = ReadPose(racer, g.local, p) ? EncodePose(p, b, sizeof(b)) : 0;
    if (n) RaceNetSend(0, b, n, false);
}

// Host with the host's AI: its own car and the AI go out together, one Cars message per sim tick,
// sent as soon as the tick is over (at the next tick's first race call).
void FlushCars() {
    if (!g.batch.count || g.batch.tick == SimTick()) return;
    uint8_t b[kWireMax];
    size_t n = EncodeCars(g.batch, b, sizeof(b));
    if (n && RaceNetSend(0, b, n, false)) ++g.carsSent;
    static bool refusedLogged = false;
    if (!n && !refusedLogged) {
        refusedLogged = true;
        Log(true, L"race: a car message (%u cars) could not be encoded; the other players don't see this game's cars", g.batch.count);
    }
    g.batch.count = 0;
}

void AddToBatch(const uint8_t* racer, int k) {
    FlushCars();  // a batch of an earlier tick goes first
    for (int i = 0; i < g.batch.count; ++i)
        if (g.batch.cars[i].slot == k) return;  // (once per car per tick; a second commit would spoil the batch)
    if (g.batch.count >= kMaxCars || !ReadPose(racer, k, g.batch.cars[g.batch.count])) return;
    g.batch.race = g.c.race;
    g.batch.tick = SimTick();
    ++g.batch.count;
}

int SlotOf(const void* racer) {
    for (int k = 0; k < kGridSlots; ++k)
        if (g.racers[k] == racer) return k;
    return -1;
}

// The id of the state a car's controller is in now (its top state, 0x49d720), or 0 if none yet.
uint32_t TopState(const uint8_t* racer) {
    const uint8_t* controller = *(const uint8_t* const*)(racer + kRacerController);
    const uint8_t* state = controller ? *(const uint8_t* const*)(controller + kCtrlTopState) : nullptr;
    return state ? *(const uint32_t*)(state + 4) : 0;
}

// A car shown from the network gets its owner's pose only while it is a network car: the podium
// (MSG_RACE_PODIUM, 0x534f00) switches every car to state 0x1a, which our poses must not drag around.
// State 0: the switch to Network21 is queued and enters at the controller's next update (0x507f00 pops
// every state first): still ours.
bool StillNetworkCar(const uint8_t* racer, int k) {
    uint32_t state = TopState(racer);
    if (state == kStateNetwork || state == 0) return true;
    if (!g.stateLogged[k] && state != kStatePodium && !RaceOver()) {
        g.stateLogged[k] = true;
        Log(true, L"race: car %d (%hs) is in control state 0x%x, not a network car; its owner's moves aren't shown", k, RacerName(k), state);
    }
    return false;
}

void __fastcall HookRacerCommit(void* self, void*, int views, void* viewList) {
    bool racePhase = g.phase == kHoldB || g.phase == kRunning;
    if (racePhase && g.host) {
        FlushCars();
        TrySendResults();  // (retry: the race updates stop soon after the race is over)
    }
    bool live = racePhase && *(int32_t*)((uint8_t*)self + kRacerLife) == 2;
    int k = live ? SlotOf(self) : -1;
    if (k >= 0 && g.remote[k] && StillNetworkCar((const uint8_t*)self, k)) {
        PlayoutPose pose;
        // The host's AI are shown at the same moment of its race as its own car.
        const Playout* clock = !g.host && k != 0 && HostCar(k) ? &g.cars[0].play : nullptr;
        if (g.cars[k].play.Sample(SimTick(), pose, clock)) {
            WritePose((uint8_t*)self, pose);
            ApplyNav((uint8_t*)self, pose.nav, k);  // snaps the cursor to the position just written
            NoteFx(k, pose.fx);
        } else {
            NoteFx(k, kFxNone);  // its owner went quiet: the game's own (still) exhaust
        }
    }
    Original<CommitFn>(kHRacerCommit)(self, views, viewList);
    if (k >= 0) FairAlert(k, (const uint8_t*)self);  // (a homing lock set this tick)
    if (k < 0 || g.remote[k]) return;
    bool hostCars = g.host && HostAi();
    if (k == g.local) {
        if (hostCars) AddToBatch((const uint8_t*)self, k);
        else SendPose((const uint8_t*)self);
    } else if (hostCars && HostCar(k) && SimTick() % kAiPoseEvery == 0) {
        AddToBatch((const uint8_t*)self, k);
    }
}

// ---------------------------------------------------------------- items (notes/ITEMS-SYNC.md)
// A car's owner decides its items, as on the console (where 0x4e8910 "this car is simulated here"
// gated every give, fire and hit; on PC it always says yes). The owner's game runs its cars' events and
// sends their bytes; the others drop the events their own copy of that car makes and run the owner's,
// once that car is shown at the moment it fired. Hits stay with each game: every game decides the hits
// on its own cars (racer vs projectile: the victim decides, 0x51cb30), which is the console's rule.
uint8_t* EventManager() {
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    return scene ? *(uint8_t**)(scene + kSceneEvents) : nullptr;
}

bool KnownGadget(uint32_t gadget) {
    for (uint32_t x : kGadgets)
        if (x == gadget) return true;
    return false;
}

uint32_t GadgetOf(const RaceEvent& e) { return *(const uint32_t*)(e.body + kEvGadget - 8); }

// The first events of a race, for the test logs (compare both games' lines).
void LogEvent(const wchar_t* what, const RaceEvent& e) {
    if (g.evLogged++ >= 60) return;
    if (e.kind == kEventAllStarPhase) {
        uint32_t body = *(const uint32_t*)e.body;
        Log(false, L"race: items: %s: car %d (%hs) All-Star phase %u%s", what, e.slot, RacerName(e.slot), AllStarPhaseOf(body),
            AllStarWeaponOf(body) != kNoWeapon ? L" (weapon known)" : L"");
        return;
    }
    uint32_t gadget = GadgetOf(e);
    Log(false, L"race: items: %s: car %d (%hs) %s %08x%s (owner's tick %u)", what, e.slot, RacerName(e.slot), e.kind == 0 ? L"gets" : L"fires",
        gadget, gadget == kGadgetAllStar ? L" All-Star" : L"", e.tick);
}

// Out, in order: what the message queue can't take now waits in the outbox (tried again every tick).
void FlushEvents() {
    int done = 0;
    while (done < g.outCount) {
        uint8_t b[kWireMax];
        size_t n = EncodeEvent(g.outbox[done], b, sizeof(b));
        if (n && !RaceNetSend(0, b, n, true)) break;
        if (n) ++g.evSent;
        ++done;
    }
    if (!done) return;
    memmove(g.outbox, g.outbox + done, (g.outCount - done) * sizeof(RaceEvent));
    g.outCount -= done;
}

void SendEvent(const RaceEvent& e) {
    const int cap = sizeof(g.outbox) / sizeof(g.outbox[0]);
    if (g.outCount == cap) FlushEvents();
    if (g.outCount == cap) {
        if (g.evUnsent++ < 3) Log(true, L"race: items: the message queue is full; an event (item or reaction) of car %d was not sent", e.slot);
        return;
    }
    g.outbox[g.outCount++] = e;
    FlushEvents();
}

// An event as it goes out: its bytes after the head, taken before it runs (a fire unpacks fields in
// place), the quantizer vtables cleared, and at +0x14 (unused) the firing car's controller value that
// some thrown items read at launch.
bool Capture(const uint8_t* ev, int32_t kind, int k, RaceEvent& e) {
    e = RaceEvent{};
    e.race = g.c.race;
    e.tick = SimTick();
    e.kind = (uint8_t)kind;
    e.slot = (uint8_t)k;
    e.size = EventBodySize(e.kind);
    memcpy(e.body, ev + 8, e.size);
    if (kind == 1 || kind == 2) {
        for (uint32_t v : kEvVtables) memset(e.body + v - 8, 0, 4);
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        const uint8_t* ctrl = racer ? *(const uint8_t* const*)(racer + kRacerController) : nullptr;
        float t = ctrl ? *(const float*)(ctrl + kCtrlThrow) : 0.0f;
        memcpy(e.body + kEvThrow - 8, &t, 4);
    }
    return EventValid(e);
}

// Every event the game posts. Kinds 0-2 (give, fire, aimed fire) of a car another game owns are that
// game's to decide; the ones we run for it pass unchanged.
void __fastcall HookEventPost(void* manager, void*, uint8_t* ev) {
    PostFn original = Original<PostFn>(kHEventPost);
    int32_t kind = ev ? *(int32_t*)(ev + kEvKind) : -1;
    if (!ev || ev == g.injecting || !ItemsOn() || kind < 0 || kind > 2 || ev[kEvRacer] >= kGridSlots) {
        original(manager, ev);
        return;
    }
    int k = ev[kEvRacer];
    if (g.remote[k]) {  // this game's copy of another game's car (a pickup, or a side effect of its fire run here)
        ((EventFreeFn)(uintptr_t)kEventFree)(ev);
        if (g.evDropped++ < 5)
            Log(false, L"race: items: car %d (%hs) %s here; left to its owner's game", k, RacerName(k), kind == 0 ? L"got an item" : L"fired");
        return;
    }
    // A full window frees the event unrun (stock): then there is nothing to send. Sent before it runs,
    // so what it posts while running (an item given at once, a fire) goes out after it.
    RaceEvent e;
    if (SendsItems(k) && ((uint8_t*)manager)[kMgrPosts] < kPostCap && Capture(ev, kind, k, e)) {
        SendEvent(e);
        LogEvent(L"sent", e);
    }
    original(manager, ev);
}

// Racer vtable slot 8 (0x4f4f20): every message to a car. An item box gives nothing to a car another
// game owns (that game decides what it got); 1 is what the stock early-outs return.
int __fastcall HookRacerMessage(void* racer, void*, uint8_t* message) {
    if (ItemsOn() && message) {
        int k = SlotOf(racer);
        const uint8_t* m = *(const uint8_t* const*)(message + 0xc);  // the message hash at +4 (as 0x4f4f34 reads it)
        if (k >= 0 && g.remote[k] && m && *(const uint32_t*)(m + 4) == kBoxMessage) {
            ++g.boxesIgnored;
            return 1;
        }
    }
    return Original<RacerMessageFn>(kHRacerMessage)(racer, message);
}

// The sender may decide car `slot`: its own car, and the AI when it is the host and drives them.
bool SenderOwns(const Peer& p, int slot) { return slot == p.index || (p.index == 0 && HostCar(slot)); }

void QueueEvent(int i, const RaceEvent& e) {
    const Peer& p = g.peers[i];
    if (p.gone) return;  // nothing more of theirs runs here
    if (e.kind > 2 && !PseudoEvent(e.kind)) {  // hits, homing, sub-projectiles: each game decides its own
        if (g.evRefused++ < 5) Log(true, L"race: items: an event of kind %u from player %d refused (not shared)", e.kind, p.index);
        return;
    }
    if (!SenderOwns(p, e.slot) || (!PseudoEvent(e.kind) && e.body[0] != e.slot)) {
        if (g.evRefused++ < 5) Log(true, L"race: items: an event from player %d for car %d refused (not that player's car)", p.index, e.slot);
        return;
    }
    if (g.queueCount[i] == kEventQueueCap) {
        if (g.evRefused++ < 5) Log(true, L"race: items: too many events from player %d waiting; one dropped", p.index);
        return;
    }
    Race::Queued& q = g.queue[i][(g.queueHead[i] + g.queueCount[i]) % kEventQueueCap];
    q.e = e;
    q.arrived = SimTick();
    ++g.queueCount[i];
}

// A received event can run here: a car this game shows from the network, an item the game has, and for
// a fire a projectile type it has (several fire branches use the new projectile without a null check).
bool Runnable(const RaceEvent& e, const uint8_t* racer) {
    uint8_t count = 0;
    SceneRacerCount(count);
    if (e.slot >= count || !racer || !g.remote[e.slot] || !KnownGadget(GadgetOf(e))) return false;
    uint32_t type = *(const uint32_t*)(e.body + kEvFrame - 8);
    return e.kind == 0 || !type || ((ProjectileTypeFn)(uintptr_t)kProjectileType)(type) != nullptr;
}

// Runs another game's event for its car, as the game runs its own: a new event of that kind with the
// owner's bytes (never the head or the quantizer vtables), through the stock post.
void RunEvent(uint8_t* manager, const RaceEvent& e) {
    uint8_t* racer = (uint8_t*)g.racers[e.slot];
    uint32_t gadget = GadgetOf(e);
    if (!Runnable(e, racer)) {
        if (g.evRefused++ < 5) Log(true, L"race: items: an event for car %d (kind %u, item %08x) can't run here; skipped", e.slot, e.kind, gadget);
        return;
    }
    // A car holding another item refuses a new one (0x539680); its owner has used or lost that one.
    uint32_t held = *(uint32_t*)(racer + kRacerItem);
    if (e.kind == 0 && held && held != gadget) ((ClearItemsFn)(uintptr_t)kClearItems)(racer, 1);
    uint8_t* ev = (uint8_t*)((EventAllocFn)(uintptr_t)kEventAlloc)(e.kind);
    if (!ev || *(int32_t*)(ev + kEvKind) != e.kind) {
        if (ev) ((EventFreeFn)(uintptr_t)kEventFree)(ev);
        if (g.evRefused++ < 5) Log(true, L"race: items: the game made no event of kind %u", e.kind);
        return;
    }
    if (e.kind == 0) {
        memcpy(ev + 8, e.body, e.size);
    } else {
        memcpy(ev + 8, e.body, kEvThrow - 8);  // racer .. +0x13
        uint32_t vt[4];
        for (int i = 0; i < 4; ++i) memcpy(&vt[i], ev + kEvVtables[i], 4);
        memcpy(ev + kEvFrame, e.body + kEvFrame - 8, 0xe0 - kEvFrame);
        for (int i = 0; i < 4; ++i) memcpy(ev + kEvVtables[i], &vt[i], 4);
    }
    // Some thrown items read the firing car's controller value at launch: the owner's, for this call only.
    uint8_t* ctrl = *(uint8_t**)(racer + kRacerController);
    float own = 0, owner;
    memcpy(&owner, e.body + kEvThrow - 8, 4);
    bool lend = e.kind != 0 && ctrl && wire::Finite(owner);
    if (lend) {
        own = *(float*)(ctrl + kCtrlThrow);
        *(float*)(ctrl + kCtrlThrow) = owner;
    }
    g.injecting = ev;
    Original<PostFn>(kHEventPost)(manager, ev);  // runs it (or frees it if the window is full); not ours afterwards
    g.injecting = nullptr;
    if (lend && *(uint8_t**)(racer + kRacerController) == ctrl) *(float*)(ctrl + kCtrlThrow) = own;
    ++g.evRun;
    // This copy of an All-Star runs until its owner's ends: its own length would depend on this screen's
    // places (0x585de0), so it gets a long one now, before its first tick computes one.
    if (e.kind != 0 && gadget == kGadgetAllStar) {
        int32_t phase = *(int32_t*)(racer + kRacerAllStarState);
        if (phase == 2) {
            float* length = (float*)(racer + kRacerAllStarState + kAllStarLength);
            for (int i = 0; i < 4; ++i) length[i] = kAllStarCopyLength;
            g.allStarEnd[e.slot] = false;
            g.metalClosing[e.slot] = false;
        }
        Log(false, L"race: items: car %d (%hs) All-Star started here%s", e.slot, RacerName(e.slot),
            phase == 2 ? L"; it ends with its owner's" : L" (the game refused it)");
    }
    LogEvent(L"shown", e);
}

// Metal Sonic's All-Star class vtable, from its core's published config (0 = no Metal Sonic, or the config isn't the expected one).
static uint32_t MetalVtableFromCore() {
    HMODULE core = GetModuleHandleW(L"MetalMaximumOverdrive.dll");
    const volatile uint32_t* c = core ? (const volatile uint32_t*)GetProcAddress(core, "metal_runtime_config") : nullptr;
    if (!c || c[0] != kMetalConfigMagic || c[1] != 1 || c[2] != 64 || c[3] != 1) return 0;
    return c[4];
}
static uint32_t (*g_metalVtable)() = MetalVtableFromCore;

// Metal Sonic's All-Star class object of car k's holder state, if it is running one, else null.
uint8_t* MetalObject(const uint8_t* racer) {
    uint8_t* obj = racer ? *(uint8_t**)(racer + kRacerAllStarState + kAllStarObject) : nullptr;
    uint32_t vtable = obj ? g_metalVtable() : 0;
    return vtable && *(uint32_t*)obj == vtable ? obj : nullptr;
}

// Owner: its car's Metal Sonic All-Star has run its time out (the game's own check: elapsed >= length).
bool MetalTimeUp(const uint8_t* racer) {
    if (!MetalObject(racer)) return false;
    const float* elapsed = (const float*)(racer + kRacerAllStarState + kAllStarElapsed);
    const float* length = (const float*)(racer + kRacerAllStarState + kAllStarLength);
    return length[0] > 0.0f && elapsed[0] >= length[0];
}

// Copy: the owner's time-up, at the owner's tick: the copy's length becomes its elapsed, so this tick's All-Star update closes the
// plates and the beam as the owner's did (its 60 s copy length would keep them open). Still transforming here (phase 2/3): it
// waits; over here, or not Metal's class here, or too late: dropped (the 60 s length and the owner's end stay the fallback).
void ForceMetalTimeUp() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.metalTimeUp[k]) continue;
        uint8_t* racer = (uint8_t*)g.racers[k];
        int32_t phase = racer ? *(int32_t*)(racer + kRacerAllStarState) : 0;
        uint8_t* obj = phase == 4 ? MetalObject(racer) : nullptr;
        if (racer && g.remote[k] && (phase == 2 || phase == 3) && SimTick() - g.metalSince[k] <= kMetalTimeUpWait) continue;
        g.metalTimeUp[k] = false;
        if (!racer || !g.remote[k] || !obj) {
            if (g.metalLogged++ < 10) Log(false, L"race: items: car %d (%hs)'s Metal Sonic All-Star time-up not applied here (%s)", k, RacerName(k),
                                          phase == 4 ? L"not Metal Sonic's class here" : L"its All-Star isn't active here");
            continue;
        }
        float* elapsed = (float*)(racer + kRacerAllStarState + kAllStarElapsed);
        float* length = (float*)(racer + kRacerAllStarState + kAllStarLength);
        for (int i = 0; i < 4; ++i)
            if (elapsed[i] < length[i]) length[i] = elapsed[i];
        g.metalClosing[k] = true;
        g.metalClosingSince[k] = SimTick();
        if (g.metalLogged++ < 10)
            Log(false, L"race: items: car %d (%hs)'s Metal Sonic All-Star closes here with its owner's (after %.1f s)", k, RacerName(k), elapsed[0]);
    }
}

// Opa-Opa's All-Star class object of car k's holder state, if it is running one (its vtable), else null.
uint8_t* OpaObject(const uint8_t* racer) {
    uint8_t* obj = *(uint8_t**)(racer + kRacerAllStarState + kAllStarObject);
    return obj && *(uint32_t*)obj == (uint32_t)kOpaVtable ? obj : nullptr;
}

// The owner's All-Star phase: once it is over there (not 2-4), this game's copy ends at its next tick in phase 3 or 4
// (EndAllStars). Opa-Opa's weapon comes with the owner's phase 4: a copy not yet active takes it as it becomes active
// (HookOpaStart); one that became active first (firing nothing, weapon 4) gets it now: the weapon, the ram's start for
// weapon 1 (what slot 11 runs for it), and the line about it (the commentary's own gates: said where the announcer speaks).
typedef void(__thiscall* OpaRamStartFn)(void* obj);
void RunAllStarPhase(const RaceEvent& e) {
    uint32_t body;
    memcpy(&body, e.body, 4);
    uint32_t phase = AllStarPhaseOf(body);
    int weapon = AllStarWeaponOf(body);
    g.allStarEnd[e.slot] = !(phase >= 2 && phase <= 4);
    uint8_t* racer = (uint8_t*)g.racers[e.slot];
    if (phase == 4 && (body & kAllStarTimeUp)) {  // Metal Sonic's All-Star ran its time out on its owner's screen
        g.metalTimeUp[e.slot] = true;
        g.metalSince[e.slot] = SimTick();
    }
    if (!(phase >= 2 && phase <= 4)) g.metalTimeUp[e.slot] = false;
    if (!(phase >= 2 && phase <= 5)) {
        g.opaWeapon[e.slot] = kNoWeapon;
        g.opaPending[e.slot] = false;
        if (g.opaShotDue[e.slot]) ++g.opaShotsDropped;
        g.opaShotDue[e.slot] = false;
        if (g.pulseDue[e.slot]) ++g.pulseDropped;
        g.pulseDue[e.slot] = g.pulseForced[e.slot] = false;
        if (g.gateDue[e.slot]) ++g.gateDropped;
        g.gateDue[e.slot] = g.gateArmed[e.slot] = g.gateHold[e.slot] = false;  // (the All-Star is over: a hold or a due shot with it)
        g.gateAttackDue[e.slot] = g.gateAimSeen[e.slot] = false;
    } else if (weapon != kNoWeapon && g.opaWeapon[e.slot] != weapon) {
        g.opaWeapon[e.slot] = weapon;
        if (g.opaLogged++ < 10) Log(false, L"race: items: car %d (%hs)'s All-Star weapon is %d (its owner's choice)", e.slot, RacerName(e.slot), weapon);
        if (g.opaPending[e.slot]) {
            g.opaPending[e.slot] = false;
            uint8_t* obj = racer && *(int32_t*)(racer + kRacerAllStarState) == 4 ? OpaObject(racer) : nullptr;  // (still active here)
            if (obj && g.remote[e.slot]) {
                *(int32_t*)(obj + kOpaWeapon) = weapon;
                *(float*)(obj + kOpaShotTimer) = kOpaShotTimerStart;  // as slot 11 left it (the blank copy's fire flags may have reset it)
                if (weapon == 1) ((OpaRamStartFn)(uintptr_t)kOpaRamStart)(obj);
                if (weapon != kWeaponNone) ((OpaLineCallFn)(uintptr_t)kOpaLineCall)(RecordIndex(racer), kOpaCues[weapon]);
                if (g.opaLogged++ < 10) Log(false, L"race: items: car %d (%hs)'s All-Star here fires its owner's weapon %d from now", e.slot, RacerName(e.slot), weapon);
            }
        }
    }
    LogEvent(L"shown", e);
}

// Opa-Opa's All-Star class, vtable slot 11 (0x5e9b10): the transform becomes active and fires its weapon [obj+0x184] (and says
// the line about it; 4 = none: fires and says nothing). A copy of another game's All-Star gets its owner's weapon; one whose
// owner's choice hasn't arrived gets 4 instead of its own random draw, and the choice when it comes (RunAllStarPhase).
void __fastcall HookOpaStart(uint8_t* obj, void*) {
    UpdateFn original = Original<UpdateFn>(kHOpaStart);
    uint8_t* racer = obj ? *(uint8_t**)(obj + kOpaRacer) : nullptr;
    int k = racer && ItemsOn() ? SlotOf(racer) : -1;
    if (k < 0 || !g.remote[k]) {
        original(obj);
        return;
    }
    int weapon = g.opaWeapon[k];
    int32_t drawn = *(int32_t*)(obj + kOpaWeapon);
    bool known = WeaponValid(weapon);
    *(int32_t*)(obj + kOpaWeapon) = known ? weapon : kWeaponNone;
    g.opaPending[k] = !known;
    original(obj);
    if (g.opaLogged++ < 10) {
        if (known)
            Log(false, L"race: items: car %d (%hs)'s All-Star fires its owner's weapon %d here%s", k, RacerName(k), weapon,
                drawn == weapon ? L"" : drawn == kWeaponNone ? L" (not drawn here)" : L" (this screen had drawn another)");
        else
            Log(false, L"race: items: car %d (%hs)'s All-Star became active here before its owner's weapon arrived (this screen's draw %d dropped): it "
                L"fires nothing until then", k, RacerName(k), drawn);
    }
}

// Runs the other games' events once their car is shown at the moment they happened, or kEventMaxWait
// after arrival (a sender gone quiet), in order per sender. From the race update before the stock one:
// after the frame's messages, before any car or projectile moves (the frame order, 0x4967c0).
void RunQueuedEvents() {
    uint8_t* manager = EventManager();
    if (!ItemsOn() || RaceOver() || !manager) return;
    for (int i = 0; i < g.peerCount; ++i) {
        const Peer& p = g.peers[i];
        while (g.queueCount[i]) {
            Race::Queued& q = g.queue[i][g.queueHead[i]];
            uint32_t shown;
            bool due = !g.cars[p.index].play.ShownTick(shown) || (int32_t)(shown - q.e.tick) >= 0;
            if (!due && SimTick() - q.arrived < kEventMaxWait) break;
            // Our own player's fire must still fit this window (a full one frees it unrun): next tick.
            if (!PseudoEvent(q.e.kind) && manager[kMgrPosts] >= kInjectRoom) return;
            if (!due) ++g.evLate;
            if (q.e.kind == kEventAllStarPhase) RunAllStarPhase(q.e);
            else if (q.e.kind == kEventReaction) QueueReaction(q.e);
            else if (q.e.kind == kEventAllStarShot) RunAllStarShot(q.e);
            else if (q.e.kind == kEventAllStarGate) RunAllStarGate(q.e);
            else RunEvent(manager, q.e);
            g.queueHead[i] = (g.queueHead[i] + 1) % kEventQueueCap;
            --g.queueCount[i];
        }
    }
}

// ---------------------------------------------------------------- driver reactions (re\m6-reactions\FINDINGS.md)
// A driver's reactions (taunts, showboating, celebrations, firing poses) come from its own game's events and
// inputs, so another game's car never made its owner's. The owner sends the animation node each reaction
// starts; the others play that node on their copy of the car (the game's 0x502f60, as its reaction pop does)
// once it can react, and that copy's own reactions no longer pop. Commentary stays per screen, as on the console.
uint32_t ReactionNode(const uint8_t* ca) {  // the newest active node of the reaction set (the list's head is the newest)
    const uint8_t* runner = *(const uint8_t* const*)(ca + kCaRunner);
    const uint8_t* inst = runner ? *(const uint8_t* const*)(runner + kRunnerActive) : nullptr;
    for (int n = 0; inst && n < 16; ++n, inst = *(const uint8_t* const*)(inst + kInstNext)) {
        const uint8_t* node = *(const uint8_t* const*)(inst + kInstNode);
        if (!node) continue;
        uint32_t h = *(const uint32_t*)(node + kNodeHash);
        for (uint32_t x : kReactionNodes)
            if (x == h) return h;
    }
    return 0;
}

void LogReaction(const wchar_t* what, int k, uint32_t node) {  // the players' cars have their own 30 lines (the AI's fill theirs early)
    int& n = k < g.c.humanCount && !g.adopted[k] ? g.reactLoggedHuman : g.reactLogged;
    if (n++ < 30) Log(false, L"race: reactions: %s: car %d (%hs) node %08x", what, k, RacerName(k), node);
}

// Owner: each reaction one of its cars' drivers started (its "reaction playing" flag rose).
void SendReactions() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        if (!racer) continue;
        const uint8_t* ca = *(const uint8_t* const*)(racer + kRacerCharAnim);
        uint8_t now = ca ? (uint8_t)(ca[0] & kCaReacting) : 0, was = g.reacting[k];
        g.reacting[k] = now;  // (every car, so a car adopted later doesn't re-send a reaction already playing)
        uint32_t node = now && !was && SendsItems(k) ? ReactionNode(ca) : 0;
        if (!node && now && !was && k == g.local && g.reactUnknown++ < 10) {  // this game's driver plays a node outside the set
            const uint8_t* runner = *(const uint8_t* const*)(ca + kCaRunner);
            const uint8_t* inst = runner ? *(const uint8_t* const*)(runner + kRunnerActive) : nullptr;
            const uint8_t* head = inst ? *(const uint8_t* const*)(inst + kInstNode) : nullptr;
            Log(false, L"race: reactions: this game's car started a reaction the others don't get (newest node %08x)",
                head ? *(const uint32_t*)(head + kNodeHash) : 0);
        }
        if (!node) continue;
        RaceEvent e = {};
        e.race = g.c.race;
        e.tick = SimTick();
        e.kind = kEventReaction;
        e.slot = (uint8_t)k;
        e.size = EventBodySize(kEventReaction);
        memcpy(e.body, &node, 4);
        SendEvent(e);
        ++g.reactSent;
        LogReaction(L"sent", k, node);
    }
}

void QueueReaction(const RaceEvent& e) {
    uint32_t node;
    memcpy(&node, e.body, 4);
    bool known = false;  // only the reaction nodes: any other node of the character's table would play too
    for (uint32_t x : kReactionNodes) known |= x == node;
    if (!known) { ++g.reactDropped; return; }
    g.reactNode[e.slot] = node;
    g.reactSince[e.slot] = SimTick();
}

// Before the frame's cars move: the other games' cars play their owners' reactions (a newer one replaces
// one still waiting) once they can (driving, no reaction playing, a free animation instance: with none
// the game would set the playing flag without playing), else dropped after kReactionWait.
void PlayReactions() {
    for (int k = 0; k < kGridSlots; ++k) {
        uint8_t* racer = (uint8_t*)g.racers[k];
        if (!racer || !g.remote[k]) continue;
        uint8_t* rc = *(uint8_t**)(racer + kRacerReactions);
        if (rc)
            for (int i = 0; i < 4; ++i) ((float*)(rc + kRcTimer))[i] = 1.0f;  // its own never pop (0x4c5310 pops at 0)
        uint32_t node = g.reactNode[k];
        if (!node) continue;
        uint8_t* ca = *(uint8_t**)(racer + kRacerCharAnim);
        uint8_t* runner = ca ? *(uint8_t**)(ca + kCaRunner) : nullptr;
        if (!runner || (ca[0] & kCaReacting) || *(int32_t*)runner != kRunnerDriving || !*(uint32_t*)(runner + kRunnerFree)) {
            if (SimTick() - g.reactSince[k] > kReactionWait) {
                if (!g.reactDropLogged)  // the first drop says which gate held it (base state 4 on a puppet is unverified)
                    Log(false, L"race: reactions: car %d's reaction %08x dropped: runner %d, state %d, reacting %d, free %u", k, node, runner ? 1 : 0,
                        runner ? *(int32_t*)runner : -1, ca ? (ca[0] & kCaReacting) ? 1 : 0 : -1, runner ? *(uint32_t*)(runner + kRunnerFree) : 0);
                g.reactDropLogged = true;
                g.reactNode[k] = 0;
                ++g.reactDropped;
            }
            continue;
        }
        g.reactNode[k] = 0;
        if (((PlayReactionFn)(uintptr_t)kPlayReaction)(ca, node)) {
            ++g.reactShown;
            LogReaction(L"shown", k, node);
        }
    }
}

// A copy of another game's All-Star whose owner's is over ends here through the game's own end flag:
// its active tick (or transform end) then ends it the stock way (0x586240), in phase 3 or 4 only.
void EndAllStars() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        uint8_t* racer = (uint8_t*)g.racers[k];
        if (!g.allStarEnd[k] || !racer) continue;
        uint8_t* s = racer + kRacerAllStarState;
        int32_t phase = *(int32_t*)s;
        if (phase == 2) continue;  // not yet transforming (its first tick clears the flag)
        // Metal Sonic's copy closing after its owner's time-up: it ends itself once its plates are shut (kMetalCloseGrace).
        if (phase == 4 && g.metalClosing[k] && SimTick() - g.metalClosingSince[k] < kMetalCloseGrace) continue;
        g.allStarEnd[k] = false;
        if (phase != 3 && phase != 4) continue;  // not running here
        s[kAllStarEnd] = 1;
        Log(false, L"race: items: car %d (%hs) All-Star ends here with its owner's", k, RacerName(k));
    }
}

// ---- Opa-Opa's shots (re\m7-allstar-shots\FINDINGS.md §3). The owner's screen decides the shots (its player's input, or the
// computer racer's random input on the host); every other screen's copy fires only when the owner did: the owner sends each shot
// (a pseudo-event in the item queue, after the phase message that carries the weapon), the copy's input slot (hooked) outputs
// nothing of its own and "fire" while a sent shot is due, with the class's gate opened for that tick (the shot timer set past
// the interval; the ram's pulse set to 0 so the fire can start it). The cars' All-Star ticks run in the scene update, AFTER
// this race update (frame 0x4967c0: the race phases at 0x4968a5, the scene at 0x4968b5), so the gate's proof (the timer reset
// to 0; the pulse at 2.5) shows at the next race update: it is read first there, before the gate would be opened again. A sent
// shot the copy couldn't use within kOpaShotWait ticks (its All-Star not active yet, the weapon still unknown) is dropped.
typedef void(__thiscall* OpaInputFn)(void* obj, uint8_t* out);

// Owner, after the race update: the shot the class fired in the last scene update, if any (the timer reset to 0 after having
// run, or the ram's pulse risen to 2.5), for its cars with Opa-Opa's All-Star active.
void SendAllStarShots() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        if (!racer || !SendsItems(k)) continue;
        const uint8_t* obj = *(const int32_t*)(racer + kRacerAllStarState) == 4 ? OpaObject(racer) : nullptr;
        if (!obj) {
            g.opaRamSeen[k] = 0.0f;
            g.opaShotArmed[k] = true;  // (slot 11 leaves the timer at 100000; only a fire writes 0: the first 0 seen is a shot)
            continue;
        }
        int32_t w = *(const int32_t*)(obj + kOpaWeapon);
        float pulse = *(const float*)(obj + kOpaRamPulse), timer = *(const float*)(obj + kOpaShotTimer);
        bool shot = false;
        if (w == 0 || w == 2) {
            shot = timer == 0.0f && g.opaShotArmed[k];  // (a tick without time passing keeps the 0: no second shot)
            g.opaShotArmed[k] = timer > 0.0f;
        } else if (w == 1) {
            shot = pulse == kOpaRamPulseValue && g.opaRamSeen[k] != kOpaRamPulseValue;
        }
        g.opaRamSeen[k] = pulse;
        if (!shot) continue;
        RaceEvent e = {};
        e.race = g.c.race;
        e.tick = SimTick();
        e.kind = kEventAllStarShot;
        e.slot = (uint8_t)k;
        e.size = EventBodySize(kEventAllStarShot);
        uint32_t body = AllStarShotBody((int)w, ++g.opaShotCount[k]);
        memcpy(e.body, &body, 4);
        SendEvent(e);
        ++g.opaShotsSent;
        if (g.opaShotLogged++ < 20) Log(false, L"race: items: car %d (%hs)'s All-Star shot %u sent (weapon %d)", k, RacerName(k), g.opaShotCount[k], w);
    }
}

// Copy: a shot of the owner's is due here (a newer one replaces one still waiting).
void RunAllStarShot(const RaceEvent& e) {
    uint32_t body;
    memcpy(&body, e.body, 4);
    int w = AllStarShotWeapon(body), cls = AllStarPulseClass(body);
    if (cls >= 0) {  // a pulse All-Star's shot
        if (!g_pulseHooks || cls >= kPulseClassCount || !g.remote[e.slot]) return;
        if (g.pulseDue[e.slot]) ++g.pulseDropped;
        g.pulseDue[e.slot] = true;
        g.pulseForced[e.slot] = false;
        g.pulseClass[e.slot] = cls;
        g.pulseSince[e.slot] = SimTick();
        ++g.pulseIn;
        return;
    }
    if (!g_opaHooks || w == kNoWeapon || !g.remote[e.slot]) return;  // (without the hooks the copy fires on its own, as before)
    if (g.opaShotDue[e.slot]) ++g.opaShotsDropped;
    g.opaShotDue[e.slot] = true;
    g.opaShotForced[e.slot] = false;
    g.opaShotWeapon[e.slot] = w;
    g.opaShotSince[e.slot] = SimTick();
    ++g.opaShotsIn;
}

// Copy, first thing in the race update: a shot whose gate was opened last tick was fired in the scene update between (the
// gate's own proof: the timer reset to 0; the pulse at 2.5): done.
void CheckOpaShots() {
    if (!g_opaHooks || !ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.opaShotDue[k] || !g.opaShotForced[k]) continue;
        g.opaShotForced[k] = false;
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        const uint8_t* obj = racer && g.remote[k] && *(const int32_t*)(racer + kRacerAllStarState) == 4 ? OpaObject(racer) : nullptr;
        if (!obj || *(const int32_t*)(obj + kOpaWeapon) != g.opaShotWeapon[k]) continue;
        bool fired = g.opaShotWeapon[k] == 1 ? *(const float*)(obj + kOpaRamPulse) == kOpaRamPulseValue : *(const float*)(obj + kOpaShotTimer) == 0.0f;
        if (!fired) continue;
        g.opaShotDue[k] = false;
        ++g.opaShotsFired;
        if (g.opaShotLogged++ < 20)
            Log(false, L"race: items: car %d (%hs)'s All-Star fired its owner's shot here (weapon %d, %u ticks after it arrived)", k, RacerName(k),
                g.opaShotWeapon[k], SimTick() - g.opaShotSince[k]);
    }
}

// Copy, before the stock update (after the queue): a due shot gets the class's gate opened for this tick's scene update, once
// its copy is active with the owner's weapon in.
void ForceOpaShots() {
    if (!g_opaHooks || !ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.opaShotDue[k]) continue;
        uint8_t* racer = (uint8_t*)g.racers[k];
        uint8_t* obj = racer && g.remote[k] && *(int32_t*)(racer + kRacerAllStarState) == 4 ? OpaObject(racer) : nullptr;
        bool ready = obj && *(int32_t*)(obj + kOpaWeapon) == g.opaShotWeapon[k];
        if (SimTick() - g.opaShotSince[k] > kOpaShotWait) {
            g.opaShotDue[k] = false;
            g.opaShotForced[k] = false;
            ++g.opaShotsDropped;
            if (g.opaShotLogged++ < 20)
                Log(false, L"race: items: car %d (%hs)'s All-Star shot wasn't fired here in time (%s); dropped", k, RacerName(k),
                    !obj ? L"its All-Star isn't active here" : !ready ? L"its weapon isn't in yet" : L"the gate didn't fire");
            continue;
        }
        if (!ready) continue;  // (not yet: the weapon arrives with the phase)
        if (g.opaShotWeapon[k] == 1) {
            if (*(float*)(obj + kOpaRamPulse) > 0.001f) *(float*)(obj + kOpaRamPulse) = 0.0f;
        } else {
            *(float*)(obj + kOpaShotTimer) = kOpaShotTimerStart;
        }
        g.opaShotForced[k] = true;
    }
}

// ---- The pulse All-Stars (kPulseClasses): the same story as Opa-Opa's shots, with the pulse float as the gate: the owner's
// pulse rising to its value is a shot; the copy's input slot (hooked) outputs nothing of its own and "fire" while a sent shot is
// due, its pulse zeroed for that tick so the fire can start it; the proof (the pulse at its value) is read at the next race update.
int PulseClassOf(const uint8_t* obj) {
    if (!obj) return -1;
    for (int i = 0; i < kPulseClassCount; ++i)
        if (*(const uint32_t*)obj == kPulseClasses[i].vtable) return i;
    return -1;
}

const uint8_t* ActivePulseObject(int k, int cls) {  // car k's All-Star object while active (phase 4) and of class cls
    const uint8_t* racer = (const uint8_t*)g.racers[k];
    if (!racer || *(const int32_t*)(racer + kRacerAllStarState) != 4) return nullptr;
    const uint8_t* obj = *(const uint8_t* const*)(racer + kRacerAllStarState + kAllStarObject);
    return obj && (cls < 0 ? PulseClassOf(obj) >= 0 : PulseClassOf(obj) == cls) ? obj : nullptr;
}

void SendPulseShots() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.racers[k] || !SendsItems(k)) continue;
        const uint8_t* obj = ActivePulseObject(k, -1);
        if (!obj) {
            g.pulseSeen[k] = 0.0f;
            continue;
        }
        const PulseClass& c = kPulseClasses[PulseClassOf(obj)];
        float pulse = *(const float*)(obj + c.pulse);
        bool shot = pulse == c.value && g.pulseSeen[k] != c.value;
        g.pulseSeen[k] = pulse;
        if (!shot) continue;
        RaceEvent e = {};
        e.race = g.c.race;
        e.tick = SimTick();
        e.kind = kEventAllStarShot;
        e.slot = (uint8_t)k;
        e.size = EventBodySize(kEventAllStarShot);
        uint32_t body = AllStarPulseBody(PulseClassOf(obj), ++g.pulseCount[k]);
        memcpy(e.body, &body, 4);
        SendEvent(e);
        ++g.pulseSent;
        if (g.pulseLogged++ < 20) Log(false, L"race: items: car %d (%hs)'s %s All-Star shot %u sent", k, RacerName(k), c.name, g.pulseCount[k]);
    }
}

void CheckPulseShots() {
    if (!g_pulseHooks || !ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.pulseDue[k] || !g.pulseForced[k]) continue;
        g.pulseForced[k] = false;
        const uint8_t* obj = g.remote[k] ? ActivePulseObject(k, g.pulseClass[k]) : nullptr;
        if (!obj || *(const float*)(obj + kPulseClasses[g.pulseClass[k]].pulse) != kPulseClasses[g.pulseClass[k]].value) continue;
        g.pulseDue[k] = false;
        ++g.pulseFired;
        if (g.pulseLogged++ < 20)
            Log(false, L"race: items: car %d (%hs)'s %s All-Star fired its owner's shot here (%u ticks after it arrived)", k, RacerName(k),
                kPulseClasses[g.pulseClass[k]].name, SimTick() - g.pulseSince[k]);
    }
}

void ForcePulseShots() {
    if (!g_pulseHooks || !ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.pulseDue[k]) continue;
        uint8_t* obj = g.remote[k] ? (uint8_t*)ActivePulseObject(k, g.pulseClass[k]) : nullptr;
        if (SimTick() - g.pulseSince[k] > kOpaShotWait) {
            g.pulseDue[k] = g.pulseForced[k] = false;
            ++g.pulseDropped;
            if (g.pulseLogged++ < 20)
                Log(false, L"race: items: car %d (%hs)'s %s All-Star shot wasn't fired here in time (%s); dropped", k, RacerName(k),
                    kPulseClasses[g.pulseClass[k]].name, !obj ? L"its All-Star isn't active here" : L"the gate didn't fire");
            continue;
        }
        if (!obj) continue;
        float* pulse = (float*)(obj + kPulseClasses[g.pulseClass[k]].pulse);
        if (*pulse > 0.001f) *pulse = 0.0f;
        g.pulseForced[k] = true;
    }
}

// The pulse All-Star classes' vtable slot 7: the computer racer's input (a random press). A copy of another game's All-Star
// outputs nothing of its own, and "fire" while its owner's shot is due (ForcePulseShots zeroed the pulse).
void __fastcall HookPulseInput(uint8_t* obj, void*, uint8_t* out) {
    int cls = PulseClassOf(obj);
    int hook = cls >= 0 ? kHPulseSonic + cls : -1;
    if (hook < 0) {  // (not one of ours: the slot's stock function by the object's own vtable; never ourselves again)
        uint32_t fn = obj ? (*(uint32_t**)obj)[7] : 0;
        if (fn && fn != (uint32_t)(uintptr_t)&HookPulseInput && out) ((OpaInputFn)(uintptr_t)fn)(obj, out);
        else if (out) memset(out, 0, kOpaInputSize);
        return;
    }
    Original<OpaInputFn>(hook)(obj, out);
    uint8_t* racer = *(uint8_t**)(obj + kAllStarRacer);
    int k = racer && out && ItemsOn() ? SlotOf(racer) : -1;
    if (k < 0 || !g.remote[k]) return;
    memset(out, 0, kOpaInputSize);
    if (g.pulseDue[k] && g.pulseClass[k] == cls) out[kOpaInputDown] = out[kOpaInputPressed] = 1;
}

// ---- The gate All-Stars (kGateClasses; re\m7-allstar-shots2\FINDINGS.md). Each class's phase-4 tick (vtable slot 12) is wrapped:
// right after the stock tick the OWNER reads the class's proof of a shot (Eggman's timer reset to 0, Shadow's 0.5, Alex Kidd's
// cooldown risen, Zobio's anim 2 -> 6, Ryo's anim -> 3, Knuckles' punch cooldown 0.5) and sends it with its payload (Eggman's rocket
// pattern, Shadow's steer/vertical, Alex Kidd's steering, Knuckles' level); right before the stock tick a COPY with a shot due opens
// the class's gate (and the input slot, hooked, says "fire" for that tick), and reads the same proof after it. Knuckles' hold is
// mirrored too: the owner's hold start (level 0 -> 1) makes the copy hold until the punch arrives; a copy not holding has its charge
// held at 0, so the All-Star's end can't release a phantom punch. A copy's input outputs nothing of its own.
typedef void(__fastcall* GateTickFn)(uint8_t* obj, void*, float* dt);
int GateClassOf(const uint8_t* obj) {
    if (!obj) return -1;
    for (int i = 0; i < kGateKindCount; ++i)
        if (*(const uint32_t*)obj == kGateClasses[i].vtable) return i;
    return -1;
}

int AnimOf(const uint8_t* racer) {  // 0x59fe90; -1 without the animation objects
    const uint8_t* a = racer ? *(const uint8_t* const*)(racer + kRacerAnimObject) : nullptr;
    const uint8_t* d = a ? *(const uint8_t* const*)(a + kAnimData) : nullptr;
    return d ? (*(const uint16_t*)(d + kAnimWord) >> 10) & 0xf : -1;
}

void SendGateEvent(int k, int cls, uint32_t flags, uint32_t a, uint32_t b) {
    RaceEvent e = {};
    e.race = g.c.race;
    e.tick = SimTick();
    e.kind = kEventAllStarGate;
    e.slot = (uint8_t)k;
    e.size = EventBodySize(kEventAllStarGate);
    uint32_t head = AllStarGateHead(cls, flags & kGateFlagHold ? g.gateCount[k] : ++g.gateCount[k], flags);
    memcpy(e.body, &head, 4);
    memcpy(e.body + 4, &a, 4);
    memcpy(e.body + 8, &b, 4);
    SendEvent(e);
    ++g.gateSent;
    if (g.gateLogged++ < 24) {
        if (flags & kGateFlagHold) Log(false, L"race: items: car %d (%hs)'s %s All-Star hold started; sent", k, RacerName(k), kGateClasses[cls].name);
        else Log(false, L"race: items: car %d (%hs)'s %s All-Star shot %u sent (payload %08x %08x)", k, RacerName(k), kGateClasses[cls].name, g.gateCount[k], a, b);
    }
}

// Copy: a shot (or Knuckles' hold start) of another game's All-Star arrived; the class's tick takes it when its copy is active.
void RunAllStarGate(const RaceEvent& e) {
    uint32_t head;
    memcpy(&head, e.body, 4);
    int cls = AllStarGateClass(head);
    uint32_t flags = AllStarGateFlags(head);
    if (!g_gateHooks || cls < 0 || cls >= kGateKindCount || !g.remote[e.slot]) return;  // (without the hooks the copy fires on its own, as before)
    if (flags & kGateFlagAim) {  // Bonanza Bros: the blimp's aim as it turns
        float aim;
        memcpy(&aim, e.body + 4, 4);
        if (!wire::Finite(aim)) return;
        g.gateAim[e.slot] = aim;
        g.gateAimSeen[e.slot] = true;
        g.gateClass[e.slot] = cls;
        return;
    }
    if (flags & kGateFlagAttack) {  // Bonanza Bros: the Attack started: the copy's blimp attacks at its next call
        if (g.gateDue[e.slot] && g.gateClass[e.slot] == kGateBonanza) {  // (the last attack's throw never went here: it belongs to that clip)
            g.gateDue[e.slot] = false;
            ++g.gateDropped;
        }
        g.gateAttackDue[e.slot] = true;
        g.gateAttackSince[e.slot] = SimTick();
        g.gateClass[e.slot] = cls;
        if (g.gateLogged++ < 24) Log(false, L"race: items: car %d (%hs)'s %s All-Star attacks here, as its owner's", e.slot, RacerName(e.slot), kGateClasses[cls].name);
        return;
    }
    if (flags & (kGateFlagHold | kGateFlagRelease)) {  // Knuckles' hold start; Beat's spray held (with the steer) or released
        g.gateHold[e.slot] = (flags & kGateFlagHold) != 0;
        g.gateClass[e.slot] = cls;
        if (g.gateHold[e.slot] && cls == kGateBeat) {  // (Beat's steer; Knuckles' hold carries nothing, and its due punch keeps its level)
            float steer;
            memcpy(&steer, e.body + 4, 4);
            if (!wire::Finite(steer)) steer = 0.0f;
            memcpy(&g.gatePayload[e.slot][0], &steer, 4);
        }
        if (g.gateLogged++ < 24)
            Log(false, L"race: items: car %d (%hs)'s %s All-Star %s here, as its owner's", e.slot, RacerName(e.slot), kGateClasses[cls].name, g.gateHold[e.slot] ? L"holds" : L"lets go");
        return;
    }
    float aim0, aim1;
    memcpy(&aim0, e.body + 4, 4);
    memcpy(&aim1, e.body + 8, 4);
    if ((cls == kGateShadow && (!wire::Finite(aim0) || !wire::Finite(aim1))) || ((cls == kGateAlexKidd || cls == kGateBonanza) && !wire::Finite(aim0))) {
        ++g.gateDropped;  // (an aim that isn't a number: never from this build)
        return;
    }
    if (g.gateDue[e.slot]) ++g.gateDropped;
    g.gateDue[e.slot] = true;
    g.gateArmed[e.slot] = false;
    g.gateClass[e.slot] = cls;
    memcpy(g.gatePayload[e.slot], e.body + 4, 8);
    if (cls == kGateBonanza) {  // the throw's aim is the stream's newest value too (the owner resets its stream to it)
        g.gateAim[e.slot] = aim0;
        g.gateAimSeen[e.slot] = true;
    }
    g.gateSince[e.slot] = SimTick();
    ++g.gateIn;
}

float GateFloat(uint32_t bits) { float f; memcpy(&f, &bits, 4); return f; }
uint32_t GateBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Copy, every race update: a shot not fired within kOpaShotWait ticks (its All-Star isn't active here yet, or the class refused
// every tick: Eggman in a hit reaction, Zobio not in its ready pose) is dropped.
void DropGateShots() {
    if (!g_gateHooks || !ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        if (g.gateAttackDue[k] && SimTick() - g.gateAttackSince[k] > kOpaShotWait) {
            g.gateAttackDue[k] = false;
            ++g.gateDropped;
            if (g.gateLogged++ < 24) Log(false, L"race: items: car %d (%hs)'s Bonanza Bros All-Star attack wasn't started here in time; dropped", k, RacerName(k));
        }
        if (!g.gateDue[k] || SimTick() - g.gateSince[k] <= kOpaShotWait) continue;
        g.gateDue[k] = g.gateArmed[k] = false;
        ++g.gateDropped;
        if (g.gateLogged++ < 24)
            Log(false, L"race: items: car %d (%hs)'s %s All-Star shot wasn't fired here in time; dropped", k, RacerName(k), kGateClasses[g.gateClass[k]].name);
    }
}

// Bonanza Bros, the blimp's call (slot 23) of a COPY, before the stock call: the aim as the owner's stream has it; a due throw
// opens the thrown byte (and sets the owner's aim at the throw), otherwise the throw is held while the attack runs (states 2-4), so
// the clip's end can't throw a bomb the owner never threw; a due Attack arms the input's press.
void BonanzaBefore(int k, uint8_t* obj) {
    if (g.gateAimSeen[k]) *(float*)(obj + kBonanzaAim) = g.gateAim[k];
    int32_t state = *(const int32_t*)(obj + kBonanzaState);
    if (g.gateDue[k] && g.gateClass[k] == kGateBonanza && !g.gateAttackDue[k]) {  // (a throw waits for its own attack to start here)
        *(uint8_t*)(obj + kBonanzaThrown) = 0;
        *(float*)(obj + kBonanzaAim) = GateFloat(g.gatePayload[k][0]);
        g.gateArmed[k] = true;  // (the proof is read after the call)
    } else if (state >= 2 && state <= 4) {
        *(uint8_t*)(obj + kBonanzaThrown) = 1;
    }
    // The press only from the ready states (0/1): in 2-4 the thrown byte is held, and a press in the very call that ends the clip
    // would set state 3 without the Attack's anim (the stock plays it only while the byte is 0) and leave the copy stuck.
    if (g.gateAttackDue[k] && (state == 0 || state == 1)) g.gateArmed[k] = true;  // (the input hook presses)
}

// The owner, after the stock call: the Attack's start (state 0/1 -> 3 with nothing thrown), a throw (the thrown byte 0 -> 1, with the
// aim now and the aim of the call before: the bomb's direction was built from that one), and the aim as it turns.
void BonanzaAfter(int k, const uint8_t* obj, int32_t stateBefore, uint8_t thrownBefore, float aimBefore) {
    float aim = *(const float*)(obj + kBonanzaAim);
    if ((stateBefore == 0 || stateBefore == 1) && *(const int32_t*)(obj + kBonanzaState) == 3 && *(const uint8_t*)(obj + kBonanzaThrown) == 0)
        SendGateEvent(k, kGateBonanza, kGateFlagAttack, GateBits(aim), 0);
    if (thrownBefore == 0 && *(const uint8_t*)(obj + kBonanzaThrown) == 1) {
        SendGateEvent(k, kGateBonanza, 0, GateBits(aim), GateBits(aimBefore));
        g.gateAimSent[k] = aim;
        g.gateAimCalls[k] = 0;
        return;
    }
    int32_t state = *(const int32_t*)(obj + kBonanzaState);
    bool moved = aim - g.gateAimSent[k] > kBonanzaAimStep || g.gateAimSent[k] - aim > kBonanzaAimStep;
    if (moved && (++g.gateAimCalls[k] >= 3 || (state >= 2 && state <= 4))) {  // every call while the attack runs (the bomb's direction)
        g.gateAimSent[k] = aim;
        g.gateAimCalls[k] = 0;
        SendGateEvent(k, kGateBonanza, kGateFlagAim, GateBits(aim), 0);
    }
}


// The owner's proof after the stock tick, with the payload; the copy's proof is the same reading.
bool GateFired(int cls, const uint8_t* obj, const uint8_t* racer, int animBefore, float before, float beforeGate, uint32_t serialBefore, bool pressed, float dt,
               uint32_t* payload) {
    switch (cls) {
    case kGateAmy: {  // a swing: from the ready poses to one of the three swing anims (the target zone's, or the alternating pair)
        int after = AnimOf(racer);
        if (!((animBefore == 2 || animBefore == 7) && after >= 3 && after <= 5)) return false;
        payload[0] = (uint32_t)after;
        return true;
    }
    case kGateUlala: {  // a shot from the ready pose: the aim anim (6/7/8), or none when the target car is gone (only a new object, with a press, tells)
        int after = AnimOf(racer);
        if (animBefore != 2 || !((after >= 6 && after <= 8) || (after == 2 && pressed && ProjectileSerial() != serialBefore))) return false;
        payload[0] = (uint32_t)after;
        return true;
    }
    case kGateBeat: return false;  // (no shots: the held state is mirrored by HookGateTick)
    case kGateBonanza: return false;  // (BonanzaBefore / BonanzaAfter)
    case kGateEggman:  // the timer had dt added, then a shot reset it to exactly 0
        if (dt <= 0.0f || *(const float*)(obj + kEggmanTimer) != 0.0f) return false;
        payload[0] = (uint32_t)*(const int32_t*)(obj + kEggmanPattern);
        return true;
    case kGateShadow:  // (a tick without time passing keeps the 0.5: no second shot)
        if (*(const float*)(obj + kShadowTimer) != kShadowShotValue || (dt <= 0.0f && beforeGate == kShadowShotValue)) return false;
        return true;  // (the aim is read by the caller: the input of this tick)
    case kGateAlexKidd:
        if (!(*(const float*)(obj + kAlexCooldown) > before)) return false;
        {
            const uint8_t* veh = *(const uint8_t* const*)(racer + kRacerVehicle);
            payload[0] = veh ? *(const uint32_t*)(veh + kVehicleSteer) : 0;
        }
        return true;
    case kGateZobio: return animBefore == kZobioReadyAnim && AnimOf(racer) == 6;
    case kGateRyo: return animBefore != kRyoLiftAnim && AnimOf(racer) == kRyoLiftAnim;
    case kGateKnuckles:
        if (*(const float*)(obj + kKnucklesCooldown) != kKnucklesPunchValue || (dt <= 0.0f && beforeGate == kKnucklesPunchValue)) return false;
        payload[0] = (uint32_t)(int32_t)before;  // the level before the tick (the punch zeroes it)
        return true;
    }
    return false;
}

// The copy's gate, opened right before the stock tick for a due shot; false when the class would refuse this tick (retried).
bool GateForce(int k, int cls, uint8_t* obj, const uint8_t* racer) {
    int anim = AnimOf(racer);
    switch (cls) {
    case kGateEggman:
        if (anim >= kEggmanHitAnim) return false;  // (in a hit reaction: the stock refuses the shot)
        *(float*)(obj + kEggmanTimer) = *(float*)(obj + kEggmanLimit);
        {
            int32_t pattern = (int32_t)g.gatePayload[k][0];
            if (pattern < 0 || pattern > 7) pattern = 0;  // (the rocket tables have 8 entries)
            *(int32_t*)(obj + kEggmanPattern) = pattern + 1;  // the tick decrements before the spawn
        }
        return true;
    case kGateShadow: *(float*)(obj + kShadowTimer) = 0.0f; return true;
    case kGateAlexKidd: *(float*)(obj + kAlexCooldown) = 0.0f; return true;  // (the steering is written by the input hook, inside the tick)
    case kGateZobio:
        if (anim >= 0 && anim != kZobioReadyAnim) return false;
        *(float*)(obj + kZobioCooldown) = 0.0f;
        return true;
    case kGateRyo: return anim != kRyoLiftAnim;
    case kGateAmy: {
        if (anim >= 0 && anim != 2 && anim != 7) return false;  // (not in a ready pose: the stock refuses the swing)
        int32_t swing = (int32_t)g.gatePayload[k][0];
        *(int32_t*)(obj + kAmySwing) = swing >= 3 && swing <= 5 ? swing : 0;  // (0: the copy alternates its own pair, as a target-less swing)
        *(float*)(obj + kAmySwingTimer) = kAmySwingTime;
        return true;
    }
    case kGateUlala: return anim < 0 || anim == 2;  // (the copy aims at its own race order's car ahead: the orders are synced)
    case kGateBeat: case kGateBonanza: return false;
    case kGateKnuckles: {
        int32_t level = (int32_t)g.gatePayload[k][0];
        if (level < 1 || level > 3) level = 1;
        *(float*)(obj + kKnucklesCooldown) = 0.0f;
        *(float*)(obj + kKnucklesCharge) = kKnucklesReleaseCharge;
        *(int32_t*)(obj + kKnucklesLevel) = level;
        return true;
    }
    }
    return false;
}

void __fastcall HookGateTick(uint8_t* obj, void*, float* dt) {
    int cls = GateClassOf(obj);
    int hook = cls >= 0 ? kHGateFirst + 2 * cls + 1 : -1;
    if (hook < 0) {  // (not one of ours: the slot's stock function by the object's own vtable; never ourselves again)
        uint32_t fn = obj ? (*(uint32_t**)obj)[12] : 0;
        if (fn && fn != (uint32_t)(uintptr_t)&HookGateTick) ((GateTickFn)(uintptr_t)fn)(obj, nullptr, dt);
        return;
    }
    uint8_t* racer = *(uint8_t**)(obj + kAllStarRacer);
    int k = racer && ItemsOn() ? SlotOf(racer) : -1;
    bool owner = k >= 0 && !g.remote[k] && SendsItems(k), copy = k >= 0 && g.remote[k];
    if (cls == kGateBonanza) {  // slot 23: the argument is the blimp, not dt; its own before/after (the aim stream, the held throw)
        bool ours = k >= 0 && (owner || copy) && dt && (uint8_t*)dt == *(uint8_t**)(obj + kBonanzaBlimp);
        if (ours) {
            g.gateArmed[k] = false;
            g.gateInputSeen[k] = false;
            if (copy) BonanzaBefore(k, obj);
        }
        int32_t stateBefore = *(const int32_t*)(obj + kBonanzaState);
        uint8_t thrownBefore = *(const uint8_t*)(obj + kBonanzaThrown);
        float aimBefore = *(const float*)(obj + kBonanzaAim);
        Original<GateTickFn>(hook)(obj, nullptr, dt);
        if (!ours) return;
        if (owner) { BonanzaAfter(k, obj, stateBefore, thrownBefore, aimBefore); return; }
        if (g.gateAttackDue[k] && stateBefore != 3 && *(const int32_t*)(obj + kBonanzaState) == 3) {
            g.gateAttackDue[k] = false;
            if (g.gateLogged++ < 24) Log(false, L"race: items: car %d (%hs)'s Bonanza Bros All-Star attack started here (%u ticks after it arrived)", k, RacerName(k), SimTick() - g.gateAttackSince[k]);
        }
        if (g.gateDue[k] && g.gateClass[k] == cls && thrownBefore == 0 && *(const uint8_t*)(obj + kBonanzaThrown) == 1) {
            g.gateDue[k] = false;
            ++g.gateFired;
            if (g.gateLogged++ < 24) Log(false, L"race: items: car %d (%hs)'s Bonanza Bros All-Star threw its owner's bomb here (%u ticks after it arrived)", k, RacerName(k), SimTick() - g.gateSince[k]);
        }
        g.gateArmed[k] = false;
        return;
    }
    float tick = dt ? *dt : 0.0f;
    g.gateTickDt = tick;
    if (k >= 0) {
        g.gateArmed[k] = false;
        g.gateInputSeen[k] = false;
        if (copy && g.gateDue[k] && g.gateClass[k] == cls) g.gateArmed[k] = GateForce(k, cls, obj, racer);
        else if (copy && cls == kGateKnuckles && !g.gateHold[k]) *(float*)(obj + kKnucklesCharge) = 0.0f;  // (no phantom punch at the end)
    }
    int animBefore = AnimOf(racer);
    float before = cls == kGateAlexKidd ? *(float*)(obj + kAlexCooldown) : cls == kGateKnuckles ? (float)*(int32_t*)(obj + kKnucklesLevel) : 0.0f;
    float beforeGate = cls == kGateShadow ? *(float*)(obj + kShadowTimer) : cls == kGateKnuckles ? *(float*)(obj + kKnucklesCooldown) : 0.0f;
    uint32_t serialBefore = cls == kGateUlala ? ProjectileSerial() : 0;
    Original<GateTickFn>(hook)(obj, nullptr, dt);
    if (k < 0 || (!owner && !copy)) return;
    uint32_t payload[2] = {0, 0};
    // The owner's press this tick: a computer racer's as our input hook saw it, a human's from its controller (slot 6's out+9); a copy's
    // is the armed force.
    const uint8_t* ctrl = *(const uint8_t* const*)(racer + kRacerController);
    bool pressed = copy ? g.gateArmed[k] : g.gateInputSeen[k] ? g.gateInputPressed[k] : ctrl && *(const int32_t*)(ctrl + kCtrlButton) == 2;
    bool fired = GateFired(cls, obj, racer, animBefore, before, beforeGate, serialBefore, pressed, tick, payload);
    if (owner && cls == kGateBeat) {  // the spray held or released: its edges go out, with the steer (the spray's aim) at the edge
        bool held = *(const float*)(obj + kBeatMode) == kBeatHeldValue;
        if (held != g.gateHoldSent[k]) {
            g.gateHoldSent[k] = held;
            uint32_t steer = 0;
            const uint8_t* ctrl = *(const uint8_t* const*)(racer + kRacerController);
            if (g.gateInputSeen[k]) steer = g.gateInput[k][0];
            else if (ctrl) steer = *(const uint32_t*)(ctrl + kCtrlSteer);
            SendGateEvent(k, cls, held ? kGateFlagHold : kGateFlagRelease, steer, 0);
        }
        return;
    }
    if (owner) {
        if (cls == kGateShadow && fired) {  // the aim: this tick's input (a human's from its controller, a computer racer's as our input hook saw it)
            const uint8_t* ctrl = *(const uint8_t* const*)(racer + kRacerController);
            if (g.gateInputSeen[k]) { payload[0] = g.gateInput[k][0]; payload[1] = g.gateInput[k][1]; }
            else if (ctrl) { payload[0] = *(const uint32_t*)(ctrl + kCtrlSteer); payload[1] = *(const uint32_t*)(ctrl + kCtrlVertical); }
        }
        if (cls == kGateKnuckles) {
            int level = *(const int32_t*)(obj + kKnucklesLevel);
            if (fired) g.gateHoldSent[k] = false;
            else if (level >= 1 && !g.gateHoldSent[k]) {  // the hold started (anim 3): the copies hold too
                g.gateHoldSent[k] = true;
                SendGateEvent(k, cls, kGateFlagHold, 0, 0);
            } else if (level == 0 && *(const float*)(obj + kKnucklesCharge) == 0.0f) {
                g.gateHoldSent[k] = false;  // (a fresh All-Star after one that ended mid-charge: its hold start goes out again)
            }
        }
        if (fired) SendGateEvent(k, cls, 0, payload[0], payload[1]);
        return;
    }
    if (!g.gateArmed[k]) return;
    g.gateArmed[k] = false;
    if (!fired) return;  // (the class refused this tick: forced again next tick, until the drop)
    g.gateDue[k] = false;
    if (cls == kGateKnuckles) g.gateHold[k] = false;
    ++g.gateFired;
    if (g.gateLogged++ < 24)
        Log(false, L"race: items: car %d (%hs)'s %s All-Star fired its owner's shot here (%u ticks after it arrived)", k, RacerName(k), kGateClasses[cls].name,
            SimTick() - g.gateSince[k]);
}

// The gate All-Star classes' vtable slot 7: the computer racer's input. A copy of another game's All-Star outputs nothing of its
// own, "fire" while its owner's shot is armed for this tick (with the owner's aim for Shadow, the owner's steering for Alex Kidd),
// and "held" while its owner's Knuckles holds. The host's computer racers' input is remembered as the owner's aim.
void __fastcall HookGateInput(uint8_t* obj, void*, uint8_t* out) {
    int cls = GateClassOf(obj);
    int hook = cls >= 0 ? kHGateFirst + 2 * cls : -1;
    if (hook < 0) {
        uint32_t fn = obj ? (*(uint32_t**)obj)[7] : 0;
        if (fn && fn != (uint32_t)(uintptr_t)&HookGateInput && out) ((OpaInputFn)(uintptr_t)fn)(obj, out);
        else if (out) memset(out, 0, kOpaInputSize);
        return;
    }
    Original<OpaInputFn>(hook)(obj, out);
    uint8_t* racer = *(uint8_t**)(obj + kAllStarRacer);
    int k = racer && out && ItemsOn() ? SlotOf(racer) : -1;
    if (k < 0) return;
    if (!g.remote[k]) {
        memcpy(&g.gateInput[k][0], out, 4);
        memcpy(&g.gateInput[k][1], out + 4, 4);
        g.gateInputPressed[k] = out[kOpaInputPressed] != 0;
        g.gateInputSeen[k] = true;
        return;
    }
    memset(out, 0, kOpaInputSize);
    if (cls == kGateKnuckles) {
        out[kOpaInputDown] = g.gateHold[k] && !g.gateArmed[k] ? 1 : 0;  // held until the punch's tick
        return;
    }
    if (cls == kGateBeat) {  // the spray as its owner holds it, aimed by the owner's steer at the edge
        out[kOpaInputDown] = g.gateHold[k] ? 1 : 0;
        if (g.gateHold[k]) memcpy(out, &g.gatePayload[k][0], 4);
        return;
    }
    if (cls == kGateBonanza) {  // the blimp: no turning of its own (the aim comes from the owner's stream), Attack when the owner's is due
        out[kOpaInputPressed] = g.gateAttackDue[k] && g.gateArmed[k] ? 1 : 0;
        return;
    }
    if (!g.gateArmed[k] || g.gateClass[k] != cls) return;
    out[kOpaInputPressed] = 1;
    if (cls == kGateShadow) {
        memcpy(out, &g.gatePayload[k][0], 4);
        memcpy(out + 4, &g.gatePayload[k][1], 4);
    } else if (cls == kGateAlexKidd) {
        // The tick integrates this input into the vehicle's steering before the shot reads it: steer 0 shrinks the steering by
        // (1 - 0.9 dt), so the owner's value is planted divided by that (as it was before the owner's own tick shrank it).
        uint8_t* veh = *(uint8_t**)(racer + kRacerVehicle);
        if (veh) {
            float v = GateFloat(g.gatePayload[k][0]), f = 1.0f - kAlexSteerRate * g.gateTickDt;
            *(float*)(veh + kVehicleSteer) = *(int32_t*)(veh + kVehicleSteerOn) != 0 && f > 0.01f ? v / f : v;
        }
    }
}

// Opa-Opa's All-Star class, vtable slot 7 (0x5ea2e0): the computer racer's input for the All-Star (random fire). A copy of
// another game's All-Star outputs nothing of its own, and "fire" while its owner's shot is due (ForceOpaShots opened the gate).
void __fastcall HookOpaInput(uint8_t* obj, void*, uint8_t* out) {
    Original<OpaInputFn>(kHOpaInput)(obj, out);
    uint8_t* racer = obj ? *(uint8_t**)(obj + kOpaRacer) : nullptr;
    int k = racer && out && ItemsOn() ? SlotOf(racer) : -1;
    if (k < 0 || !g.remote[k]) return;
    memset(out, 0, kOpaInputSize);
    if (g.opaShotDue[k] && *(int32_t*)(obj + kOpaWeapon) == g.opaShotWeapon[k]) out[kOpaInputDown] = out[kOpaInputPressed] = 1;
}

// Owner: Opa-Opa's weapon for its car, once final, else kNoWeapon. The menu runs in phase 3 (+0x184: the last All-Star's
// value, 4 at the menu's start, then the cursor); the value is final at the change to phase 4 (re\m7-opa\FINDINGS.md §3): 0-2,
// or 4 when nothing was chosen (the owner fires nothing).
int OpaWeaponChosen(const uint8_t* racer, int32_t phase) {
    const uint8_t* obj = OpaObject(racer);
    if (!obj || phase != 4) return kNoWeapon;
    int32_t w = *(const int32_t*)(obj + kOpaWeapon);
    return WeaponValid(w) ? (int)w : kNoWeapon;
}

// Owner: its cars' All-Star phases, whenever one changes from or to a running All-Star (2-5); the phase-4 message carries
// Opa-Opa's weapon (sent again if it were seen only later), so the other screens' copies fire the same one.
void SendAllStarPhases() {
    if (!ItemsOn()) return;
    for (int k = 0; k < kGridSlots; ++k) {
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        if (!racer || !SendsItems(k)) continue;
        int32_t phase = *(const int32_t*)(racer + kRacerAllStarState), was = g.allStarSeen[k];
        bool running = phase >= 2 && phase <= 5, wasRunning = was >= 2 && was <= 5;
        if (running && !wasRunning) {
            g.opaWeaponSent[k] = kNoWeapon;
            g.gateHoldSent[k] = false;  // a fresh All-Star (Knuckles / Beat: its first hold goes out whatever the last one left)
            g.gateAimSent[k] = 0.0f;    // (Bonanza Bros: the blimp's aim stream starts over)
            g.gateAimCalls[k] = 3;
            g.metalTimeUpSent[k] = false;  // (Metal Sonic: one time-up per All-Star)
        }
        int weapon = running ? OpaWeaponChosen(racer, phase) : kNoWeapon;
        bool timeUp = phase == 4 && !g.metalTimeUpSent[k] && MetalTimeUp(racer);
        if (phase == was && !timeUp && (weapon == kNoWeapon || weapon == g.opaWeaponSent[k])) continue;
        g.allStarSeen[k] = phase;
        if (!running && !wasRunning) continue;
        if (weapon != kNoWeapon) g.opaWeaponSent[k] = weapon;
        RaceEvent e = {};
        e.race = g.c.race;
        e.tick = SimTick();
        e.kind = kEventAllStarPhase;
        e.slot = (uint8_t)k;
        e.size = EventBodySize(kEventAllStarPhase);
        uint32_t body = AllStarPhaseBody((uint32_t)phase, weapon, timeUp);
        memcpy(e.body, &body, 4);
        SendEvent(e);
        LogEvent(L"sent", e);
        if (timeUp) {
            g.metalTimeUpSent[k] = true;
            if (g.metalLogged++ < 10) Log(false, L"race: items: car %d (%hs)'s Metal Sonic All-Star ran its time out; sent", k, RacerName(k));
        }
        if (weapon != kNoWeapon && g.opaLogged++ < 10) Log(false, L"race: items: car %d (%hs)'s All-Star weapon %d sent with its phase", k, RacerName(k), weapon);
    }
}

// A player left: nothing more of theirs runs here, and their cars' All-Star copies end.
void ItemsPeerGone(int i) {
    if (!g_itemHooks || !(g.c.flags & kContractItems)) return;
    g.queueCount[i] = 0;
    for (int k = 0; k < kGridSlots; ++k)
        if (SenderOwns(g.peers[i], k)) {
            g.allStarEnd[k] = true;
            g.reactNode[k] = 0;  // a reaction of theirs still waiting to play
            g.opaWeapon[k] = kNoWeapon;
            g.opaPending[k] = false;
            g.opaShotDue[k] = false;
            g.pulseDue[k] = g.pulseForced[k] = false;
            g.metalTimeUp[k] = false;
            g.spinNow[k] = false;  // (their last report may have been "spinning")
        }
}

void LogItems() {
    if (!(g.c.flags & kContractItems)) return;
    Log(false, L"race: items: %d events sent, %d of the others' shown here (%d late), %d of this game's copies dropped, %d item boxes left to "
        L"their owner, %d refused, %d not sent", g.evSent, g.evRun, g.evLate, g.evDropped, g.boxesIgnored, g.evRefused, g.evUnsent);
    Log(false, L"race: reactions: %d sent, %d of the others' shown here, %d dropped (their car couldn't react in time)", g.reactSent,
        g.reactShown, g.reactDropped);
    if (g.opaShotsSent || g.opaShotsIn)
        Log(false, L"race: items: Opa-Opa's All-Star shots: %d sent, %d of the others' arrived, %d fired here, %d dropped", g.opaShotsSent, g.opaShotsIn,
            g.opaShotsFired, g.opaShotsDropped);
    if (g.pulseSent || g.pulseIn)
        Log(false, L"race: items: the pulse All-Stars' shots (Super Sonic, AiAi, B.D. Joe): %d sent, %d of the others' arrived, %d fired here, %d dropped",
            g.pulseSent, g.pulseIn, g.pulseFired, g.pulseDropped);
    if (g.gateSent || g.gateIn)
        Log(false, L"race: items: the gate All-Stars' messages (Eggman, Shadow, Alex Kidd, Zobio, Ryo, Knuckles, Amy, Ulala, Beat, Bonanza Bros): %d sent, %d shots of the others' arrived, %d fired here, %d dropped",
            g.gateSent, g.gateIn, g.gateFired, g.gateDropped);
}

#ifdef RACE_TEST
}  // namespace
// tests/test_race.cpp: fake game functions and addresses, and the hooks themselves.
void RaceTestSetOriginal(int hook, void* fn) { g_hooks[hook].original = (uint32_t)(uintptr_t)fn; }
void* RaceTestHook(int hook) { return g_hooks[hook].fn; }
void RaceTestSetAddresses(uintptr_t scene, uintptr_t courseGraph, uintptr_t seed, uintptr_t setState, uintptr_t setPosition, uintptr_t simTick,
                          uintptr_t racerTypeId) {
    kScene = scene; kCourseGraph = courseGraph; kSeedRng = seed; kSetControlState = setState; kSetPosition = setPosition; kSimTick = simTick;
    kRacerTypeId = racerTypeId;
}
void RaceTestSetLapAddresses(uintptr_t applyNav, uintptr_t records, uintptr_t finishCount, uintptr_t finishList, uintptr_t raceOver,
                             uintptr_t addFinish, uintptr_t startEndTimer, uintptr_t timerElapsed, uintptr_t timerLimit, uintptr_t tickRate) {
    kApplyNavLine = applyNav; kRecords = records; kFinishCount = finishCount; kFinishList = finishList; kRaceOver = raceOver;
    kAddFinish = addFinish; kStartEndTimer = startEndTimer; kEndTimerElapsed = timerElapsed; kEndTimerLimit = timerLimit; kTickRate = tickRate;
}
void RaceTestSetClockAddresses(uintptr_t app, uintptr_t stepAcc, uintptr_t stepCount, uintptr_t frameSteps, uintptr_t raceClock) {
    kAppPtr = app; kStepAcc = stepAcc; kStepCount = stepCount; kFrameSteps = frameSteps; kRaceClock = raceClock;
}
int RaceTestPhase() { return g.phase; }
void* RaceTestRacer(int k) { return g.racers[k]; }
bool RaceTestReturnFlag() { return g_returnToLobby; }
void RaceTestSetReturnFlag(bool on) { g_returnToLobby = on; }
void RaceTestSetRulesAddresses(uintptr_t records, uintptr_t active) { kSettingsRecords = records; kActiveProfile = active; }
void RaceTestSetHostAi(bool on) { g_hostAiConfig = on; }
void RaceTestSetMetalVtable(uint32_t (*fn)()) { g_metalVtable = fn; }
void RaceTestSetClearItems(uintptr_t fn) { kClearItems = fn; }
bool RaceTestAiPuppets() { return g.aiPuppets; }
void RaceTestSetItems(bool on) { g_itemsConfig = g_itemHooks = on; }
void RaceTestSetItemConfig(bool config, bool hooks) { g_itemsConfig = config; g_itemHooks = hooks; }
void RaceTestSetItemAddresses(uintptr_t alloc, uintptr_t free, uintptr_t projectileType) {
    kEventAlloc = alloc; kEventFree = free; kProjectileType = projectileType;
}
void RaceTestSetReactionAddress(uintptr_t play) { kPlayReaction = play; }
namespace {
#endif

// ---------------------------------------------------------------- install
bool WriteSlot(uint32_t slot, uint32_t expected, uint32_t value) {
    DWORD old, ignored;
    if (!VirtualProtect((void*)(uintptr_t)slot, 4, PAGE_READWRITE, &old)) return false;
    LONG prev = InterlockedCompareExchange((volatile LONG*)(uintptr_t)slot, (LONG)value, (LONG)expected);
    VirtualProtect((void*)(uintptr_t)slot, 4, old, &ignored);
    return (uint32_t)prev == expected;
}

// Hooks [from, to) all or none, after checking every slot holds the stock function. False: none changed.
bool InstallGroup(int from, int to, const wchar_t* what) {
    for (int i = from; i < to; ++i) {
        const Hook& h = g_hooks[i];
        uint32_t now = *(volatile uint32_t*)(uintptr_t)h.slot;
        if (now != h.original) {
            Log(true, L"%s: off (%s slot 0x%06x holds 0x%08x, expected 0x%06x; another mod may hook it)", what, h.what, h.slot, now, h.original);
            return false;
        }
    }
    for (int i = from; i < to; ++i) {
        if (!WriteSlot(g_hooks[i].slot, g_hooks[i].original, (uint32_t)(uintptr_t)g_hooks[i].fn)) {
            for (int j = from; j < i; ++j) WriteSlot(g_hooks[j].slot, (uint32_t)(uintptr_t)g_hooks[j].fn, g_hooks[j].original);
            Log(true, L"%s: off (could not hook %s, error %lu)", what, g_hooks[i].what, GetLastError());
            return false;
        }
    }
    return true;
}

void RollbackGroup(int from, int to) {
    for (int i = from; i < to; ++i) {
        const Hook& h = g_hooks[i];
        if (!WriteSlot(h.slot, (uint32_t)(uintptr_t)h.fn, h.original)) Log(false, L"online races: %s slot changed by someone else; left as is", h.what);
    }
}

}  // namespace

bool RaceInstall(HMODULE gameImage) {
    if ((uintptr_t)gameImage != 0x400000) return false;
    if (!InstallGroup(0, kCoreHooks, L"online races")) return false;
    g_installed = true;
    g_itemHooks = InstallGroup(kHEventPost, kHExhaust, L"online item sync");
    g_fxHooks = InstallGroup(kHExhaust, kHCatchup, L"the other cars' exhaust effects");
    g_catchupHooks = InstallGroup(kHCatchup, kHTakeSelect, L"online catch-up");
    g_announcerHooks = InstallGroup(kHTakeSelect, kHRacerTrack, L"the shared announcer");
    g_fairHooks = g_announcerHooks && InstallGroup(kHRacerTrack, kHRacerUpdate, L"fair call-outs");
    g_lineHooks = g_fairHooks && InstallGroup(kHRacerUpdate, kHOpaStart, L"fair item lines");
    g_opaHooks = g_itemHooks && InstallGroup(kHOpaStart, kHPulseSonic, L"the All-Star weapon");
    g_pulseHooks = g_itemHooks && InstallGroup(kHPulseSonic, kHGateFirst, L"the pulse All-Stars' shots");
    g_gateHooks = g_itemHooks && InstallGroup(kHGateFirst, kHSteerHuman, L"the gate All-Stars' shots");
    g_steerHooks = InstallGroup(kHSteerHuman, kHookCount, L"Drift+ off");
    Log(false, L"online races: %d race hooks installed%s%s%s%s%s%s%s%s%s%s",
        kCoreHooks + (g_itemHooks ? kItemHooks : 0) + (g_fxHooks ? kFxHooks : 0) + (g_catchupHooks ? kCatchupHooks : 0) +
            (g_announcerHooks ? kAnnouncerHooks : 0) + (g_fairHooks ? kFairHooks : 0) + (g_lineHooks ? kLineHooks : 0) + (g_opaHooks ? kOpaHooks : 0) +
            (g_pulseHooks ? kPulseHooks : 0) + (g_gateHooks ? kGateHooks : 0) + (g_steerHooks ? kSteerHooks : 0),
        g_itemHooks ? L" (items included)" : L"; items show only on their own player's screen",
        g_fxHooks ? L"" : L"; the other players' cars show no boost effects", g_catchupHooks ? L"" : L"; CATCH-UP is off online",
        g_announcerHooks ? L"" : L"; each screen keeps its own announcer",
        !g_announcerHooks || g_fairHooks ? L"" : L"; the host's announcer favours its own player as offline",
        !g_fairHooks || g_lineHooks ? L"" : L"; the host's hit lines follow its own screen",
        !g_itemHooks || g_opaHooks ? L"" : L"; Opa-Opa's All-Star weapon may differ between screens",
        !g_itemHooks || g_pulseHooks ? L"" : L"; Super Sonic's, AiAi's and B.D. Joe's All-Stars fire at random on the other screens",
        !g_itemHooks || g_gateHooks ? L"" : L"; Eggman's, Shadow's, Alex Kidd's, Zobio's, Ryo's, Knuckles', Amy's, Ulala's, Beat's and Bonanza Bros' All-Stars fire at random on the other screens",
        g_steerHooks ? L"" : L"; controllers keep Drift+ online");
    return true;
}

void RaceRollback() {
    if (!g_installed) return;
    RestoreRules(L"the mod is unloading");
    RollbackGroup(0, kCoreHooks);
    if (g_itemHooks) RollbackGroup(kHEventPost, kHExhaust);
    if (g_fxHooks) RollbackGroup(kHExhaust, kHCatchup);
    if (g_catchupHooks) RollbackGroup(kHCatchup, kHTakeSelect);
    if (g_announcerHooks) RollbackGroup(kHTakeSelect, kHRacerTrack);
    if (g_fairHooks) RollbackGroup(kHRacerTrack, kHRacerUpdate);
    if (g_lineHooks) RollbackGroup(kHRacerUpdate, kHOpaStart);
    if (g_opaHooks) RollbackGroup(kHOpaStart, kHPulseSonic);
    if (g_pulseHooks) RollbackGroup(kHPulseSonic, kHGateFirst);
    if (g_gateHooks) RollbackGroup(kHGateFirst, kHSteerHuman);
    if (g_steerHooks) RollbackGroup(kHSteerHuman, kHookCount);
    g_installed = g_itemHooks = g_fxHooks = g_catchupHooks = g_announcerHooks = g_fairHooks = g_lineHooks = g_opaHooks = g_pulseHooks = g_gateHooks = g_steerHooks = false;
}

RaceStatus RaceMenuPoll() {
    // A race that started and ended was closed by 090a; one that failed to start ends here.
    if (g.phase == kFailed) EndRace(L"back in the menus after a failed start");
    if (g_progressClearPending && g.phase == kIdle) {
        int32_t none[5] = {-1, 0, 0, 0, 0};
        g_progressClearPending = !LobbyPostArgs(kCmdSetProgress, none, 5);
    }
    RetryDialogClose();
    Pump();
    LobbySnapshot s;
    LobbyRead(s);
    if (g.phase == kArmed && !g.launched && s.state != kLobbyInLobby) RaceCancel(kAbortQuit);
    if (g.phase == kArmed && !g.launched) return kRaceStart;
    return kRaceNone;
}

void RaceLaunched(bool ok) {
    if (g.phase != kArmed) return;
    if (ok) {
        g.launched = true;
        g.launchedAt = GetTickCount();
        Log(false, L"race: the lobby sent the start to the game");
    } else {
        Log(true, L"online race %08x: the menus could not start the race", g.c.race);
        SendAbort(kAbortMismatch);
        EndRace(L"start failed in the menus");
    }
}

const RaceContract* RaceContractArmed() { return g.phase == kArmed ? &g.c : nullptr; }
int RaceLocalIndex() { return g.phase == kArmed ? g.local : -1; }

void RaceCancel(AbortReason why) {
    if (g.phase == kIdle) return;
    SendAbort(why);
    EndRace(L"cancelled in the menus");
}

int32_t RaceHostStart() {
    LobbySnapshot s;
    LobbyRead(s);
    if (s.state != kLobbyInLobby || !s.isHost) return kStartNotHost;
    if (s.memberCount < 2) return kStartAlone;
    if (g.phase != kIdle) return kStartBusy;
    if (!g_racerList) return kStartNoList;
    const LobbyRules& r = s.rules;
    if (!CourseUsable(r.course) || r.laps < 1 || r.laps > 9) return kStartNoRules;
    RaceContract c = {};
    c.course = (uint8_t)r.course;
    c.courseHash = kCourses[r.course].hash;
    c.laps = (uint8_t)r.laps;
    c.items = (uint8_t)(r.items ? 1 : 0);
    c.catchup = (uint8_t)(r.catchup ? 1 : 0);
    c.ai = (uint8_t)(r.ai >= 0 && r.ai <= 3 ? r.ai : 0);
    c.flags = g_hostAiConfig ? (uint8_t)(kContractHostAi | (g_itemsConfig && g_itemHooks ? kContractItems : 0)) : 0;
    if (g_announcerHooks) c.flags |= kContractAnnouncer;
    if (RaceDriftPlusOff(s)) c.flags |= kContractNoDriftPlus;
    c.humanCount = (uint8_t)s.memberCount;
    bool used[64] = {};
    for (int i = 0; i < s.memberCount; ++i) {  // members are host first (steam_mp.cpp)
        const LobbyMember& m = s.members[i];
        if (m.racer < 0 || m.racer >= g_racerCount || !m.ready) return kStartNotReady;
        // Players may share a racer (the user's choice, 2026-10-23, after an 8-Sonic race worked): cars are made per grid slot.
        if (!strcmp(g_racerList[m.racer].name, "mii") || !strcmp(g_racerList[m.racer].name, "avatar")) return kStartRacerNotOnline;
        if (!RacerOn(m.racer)) return kStartRacerOff;  // (picked before the lobby turned its mod off: the picker hides it now)
        used[m.racer] = true;
        c.humans[i] = m.id;
        c.roster[i] = g_racerList[m.racer].hash;
    }
    for (int i = 0; i < s.memberCount; ++i)  // every game can make race connections, or the contract can't reach it
        if (!s.members[i].net) return kStartNetNotReady;
    // Every player is back from the last race (its results screen; their "pos" goes once their race is closed): a race
    // sent to a game still showing results held the others at the loading screen until it came back (user, 2026-10-22).
    for (int i = 0; i < s.memberCount; ++i)
        if (!s.members[i].isMe && !IsTestPlayer(s.members[i].id) && s.members[i].place >= 0) return kStartPlayerInRace;
    std::random_device rd;  // (only now: the menu retries this every frame while a connection isn't ready)
    std::mt19937 rng(rd() ^ GetTickCount());
    do c.race = rng(); while (!c.race);
    c.seed = rng();
    int pool[64], poolCount = 0;  // AI: every other racer that can be an AI, shuffled
    for (int i = 0; i < g_racerCount; ++i)
        if (!used[i] && g_racerList[i].ai && RacerOn(i)) pool[poolCount++] = i;
    std::shuffle(pool, pool + poolCount, rng);
    if (poolCount < kGridSlots - c.humanCount) return kStartNoList;
    for (int k = c.humanCount, j = 0; k < kGridSlots; ++k, ++j) c.roster[k] = g_racerList[pool[j]].hash;
    uint8_t b[kWireMax];
    size_t n = EncodeContract(c, b, sizeof(b));
    if (!n || !RaceNetSend(0, b, n, true)) return kStartSendFailed;
    Arm(c, true);
    return kStartOk;
}

bool RaceDriftPlusOff(const LobbySnapshot& s) {
    if (g_forceDriftPlusOff) return true;  // (tests on one PC: a controller player and test players)
    for (int i = 0; i < s.memberCount && i < kMaxMembers; ++i)  // the user's rule; test players don't count (input unknown)
        if (s.members[i].input == kInputKeyboard) return true;
    return false;
}

void RaceConfigure(const wchar_t* configIni) {
    g_hostAiConfig = GetPrivateProfileIntW(L"Race", L"HostRunsAI", 1, configIni) != 0;
    if (!g_hostAiConfig) Log(false, L"online races: as host, each game drives its own AI (config.ini [Race] HostRunsAI=0)");
    g_itemsConfig = GetPrivateProfileIntW(L"Race", L"ItemSync", 1, configIni) != 0;
    if (g_hostAiConfig && !g_itemsConfig) Log(false, L"online races: as host, each game shows only its own player's items (config.ini [Race] ItemSync=0)");
    g_forceDriftPlusOff = GetPrivateProfileIntW(L"Debug", L"ForceDriftPlusOff", 0, configIni) != 0;
    if (g_forceDriftPlusOff) Log(false, L"online races: as host, Drift+ is off in every race (config.ini [Debug] ForceDriftPlusOff=1)");
    g_debugStallMs = GetPrivateProfileIntW(L"Debug", L"StallMs", 0, configIni);
    if (g_debugStallMs > 30000) g_debugStallMs = 30000;
    if (g_debugStallMs) Log(true, L"debugging: this game freezes for %u ms 10 s into each online race (config.ini [Debug] StallMs)", g_debugStallMs);
}

void RaceUseEggHangar(bool installed) {
    g_eggHangar = installed;
    g_eggScanned = true;
    Log(false, L"online races: Egg Hangar's course is %s", installed ? L"offered (the Egg Hangar mod is installed)" : L"not offered (no Egg Hangar mod)");
}

void RaceLobbyMods(bool eggOn, bool metalOn) {
    if (eggOn == g_eggLobby && metalOn == g_metalLobby) return;
    g_eggLobby = eggOn;
    g_metalLobby = metalOn;
    Log(false, L"this lobby's modifications: Egg Hangar %s, Metal Sonic %s (off = not used here, or not installed)", eggOn ? L"on" : L"off",
        metalOn ? L"on" : L"off");
}

// The lobby's racer list: chosen by which menu module was installed (Roster Pack 25, stock 21, or stock 21 + Metal Sonic 1.1.2).
void RaceUseRacerList(RacerListKind kind) {
    g_racerList = kind == kRacerListRoster ? kRacersRoster : kind == kRacerListMetal ? kRacersMetal : kRacersStock;
    g_racerCount = kind == kRacerListRoster ? (int)(sizeof(kRacersRoster) / sizeof(kRacersRoster[0]))
                   : kind == kRacerListMetal ? (int)(sizeof(kRacersMetal) / sizeof(kRacersMetal[0]))
                                             : (int)(sizeof(kRacersStock) / sizeof(kRacersStock[0]));
}

// The other players' name tags (M6, re\m6-race-hud\FINDINGS.md (b)). The console's online tags are gone
// from the PC HUD (its game-type-5 calls are empty), but the split-screen "nP" tags are still there: hud
// 0x36b6 draws them when 0938 (the humans in the race, scene+0x1104) is above 1, over every racer 0911
// calls human, except the screen's own and only within 100 m. During our race those two calls see the
// other players: 2 humans at that gate, and the connected players' cars as humans. Nothing else changes
// (0911's other callers and the race logic still see network cars).
// Egg Hangar (1.2.0) substitutes its own `hud` for the stock one (the console's Death Egg music picker, ea4224a3...): the same
// code, later (gate 0x36d4, human 0x5b7f, text 0x5cc4; each pattern found once there). Without these the tags and the race's
// notice ticker were silently off on every PC with Egg Hangar (the user's 1.1.0 check B); since 1.1.1-dp13 the sites are found by
// their bytes wherever they lie (the known offsets below are the stock hud's and Egg Hangar's).
namespace {
const uint8_t kHudTagGateCode[] = {0x32, 0x00, 0x38, 0x09, 0x00, 0x00, 0x18, 0x04, 0x51, 0x01, 0x48, 0x14, 0x0c, 0x00};  // from PC - 8
const ScriptSiteDef kHudTagGate = {"HUD tags gate (0938)", kScriptHud, -8, kHudTagGateCode, sizeof(kHudTagGateCode), {0x36b6 + 8, 0x36d4 + 8}};
const uint8_t kHudTagHumanCode[] = {0x07, 0x01, 0x39, 0x32, 0x01, 0x11, 0x09, 0x00, 0x00, 0x8b, 0x09, 0x14, 0x17, 0x00};  // from PC - 11
const ScriptSiteDef kHudTagHuman = {"HUD tags human (0911)", kScriptHud, -11, kHudTagHumanCode, sizeof(kHudTagHumanCode), {0x5b1d + 8, 0x5b7f + 8}};

// The tags' gate (0938 at hud 0x36b6, or Egg Hangar's hud 0x36d4): also the race's notice ticker's frame.
bool HudTagGate(void* vm) { return ScriptSite(vm, kHudTagGate); }

Peer* TaggedPeer(int k) {
    Peer* p = PeerByIndex(k);
    return p && !p->gone ? p : nullptr;
}

// 1.1.1 (the user's request; an addition: the consoles tagged only the players): a computer racer's car gets a tag with its
// racer's name too: the grid's computer racers and a departed player's car once it drives as one. Never this screen's car.
bool CpuTagged(int k) {
    return k >= 0 && k < kGridSlots && k != g.local && g.racers[k] && (k >= g.c.humanCount || g.adopted[k]) && !TaggedPeer(k) &&
           RacerDisplayName(k);
}

bool TagsOn() {
    if (g.phase != kRunning) return false;
    for (int i = 0; i < g.peerCount; ++i)
        if (!g.peers[i].gone) return true;
    return false;
}

bool Number(const NativeCall* d) { return d->retType == kTypeInt || d->retType == kTypeHash || d->retType == kTypeNone; }

// The tag's text: TagFunc (hud 0x5b4f) gives its text element the localized label "kP" (021f(TXT, NODE(label))
// at hud 0x5c62). For a connected player the element gets their Steam name instead, through the element text
// setter native 0248 uses (0x47f3a0: stdcall(element hash, text object), ret 8; the text is copied at once by
// 0x485410 through the object's vtbl[0] = 0x485200, so a text-buffer object of our own is enough).
GAME_ADDRESS kSetElementText = 0x47f3a0;
const uint32_t kTextBufferVtable = 0x6a71f8;  // native 1610 objects: +8 capacity, +0xc char*
const uint8_t kHudTagTextCode[] = {0x07, 0x0c, 0x3c, 0x0b, 0x02, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x00, 0x00, 0xd3, 0x09, 0x18};  // from PC - 14
const ScriptSiteDef kHudTagText = {"HUD tag text (021f)", kScriptHud, -14, kHudTagTextCode, sizeof(kHudTagTextCode), {0x5c62 + 8, 0x5cc4 + 8}};
const uint32_t kTagLabels[kGridSlots] = {0x1c49c7c3, 0x8c88e2d8, 0xf4de4cbd, 0x68ec5037, 0x16695b6b, 0x8b2f5c98, 0xd4db35e5, 0x3a3c603e};  // "1P".."8P"
typedef bool(__stdcall* SetElementTextFn)(uint32_t element, void* text);

struct TextBuffer {
    uint32_t vtable, zero;
    int32_t capacity;
    char* text;
};

// The top progress bar (M6, re\m6-race-hud\FINDINGS.md (a) and its follow-up). Its HUD object H =
// [scene+0x1174] has marker entries of 0x1c bytes: E(k+2) = rival entry of list index k, E(k+10) = human
// entry of creation index k (list index = creation index for our racers). Init (native 1f16 -> 0x5017f0)
// fills both sets for all 8 slots, then gives each racer the human entry if its controller is category 1
// (0x4e8f90), else the rival one, and the per-frame update (1f15 -> 0x501550) positions only that entry. The
// humans' markers sit below the bar with their number, the rivals' above (each node's own layout animation:
// no code difference). Another player's car is never category 1 here, so it is drawn with its rival entry:
// that entry gets the human entry's sprites and node instead (data only; nothing else reads E(2..17), and the
// category stays: it decides the race logic). Entry: +0 marker, +4 head, +8 text, +0xc racer, +0x10 node
// (marker+0xe4), +0x14 boost-flash byte, +0x15 All-Star byte, +0x18 default image.
GAME_ADDRESS kLocalPlayers = 0xc2a044;  // local players (split screen): 1 = the single-screen sprites
GAME_ADDRESS kIconHash = 0x4ffea0;      // cdecl(racer) -> its progress-bar head icon (0: not in its table)
GAME_ADDRESS kSetImage = 0x47fb00;      // thiscall(texture sets 0xc282a8; element, image, set 0x3a6dfc06), ret 0xc
const uint32_t kTextureSets = 0xc282a8, kIconSet = 0x3a6dfc06;
const uint32_t kSceneBar = 0x1174, kBarReady = 0x379, kBarEntry = 0x1c, kMarkerNode = 0xe4;
const int kElemShow = 0x48 / 4, kElemHide = 0x50 / 4;  // UI element vtable: thiscall(element), no args
typedef void(__thiscall* ElementFn)(void* element);
typedef uint32_t(__cdecl* IconHashFn)(void* racer);
typedef bool(__thiscall* SetImageFn)(uintptr_t sets, uint32_t element, uint32_t image, uint32_t set);

uint32_t& Field(uint8_t* entry, uint32_t at) { return *(uint32_t*)(entry + at); }
void CallElement(uint32_t element, int slot) {
    if (element) (*(ElementFn**)(uintptr_t)element)[slot]((void*)(uintptr_t)element);
}
const uint32_t kCopied[5] = {0, 4, 8, 0x10, 0x18};

uint8_t* ProgressBar() {
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    uint8_t* bar = scene ? *(uint8_t**)(scene + kSceneBar) : nullptr;
    return bar && bar[kBarReady] == 1 && *(volatile uint32_t*)(uintptr_t)kLocalPlayers == 1 ? bar : nullptr;
}

// Every connected other player's car: its human marker; a player who left: the rival one again.
void UpdateProgressBar() {
    if (g.phase < kHoldA || g.phase > kRunning) return;
    uint8_t* bar = ProgressBar();
    if (!bar) return;
    for (int k = 0; k < g.c.humanCount && k < kMaxHumans; ++k) {
        if (k == g.local || !g.racers[k]) continue;
        uint8_t* rival = bar + (k + 2) * kBarEntry;
        uint8_t* human = bar + (k + 10) * kBarEntry;
        Race::BarCopy& b = g.bar[k];
        bool applied = b.human && Field(rival, 0) == b.human;
        if (!TaggedPeer(k)) {
            if (!applied) continue;
            CallElement(Field(rival, 0x10), kElemHide);
            for (int i = 0; i < 5; ++i) Field(rival, kCopied[i]) = b.fields[i];
            rival[0x14] = b.flags[0];
            rival[0x15] = b.flags[1];
            if (!FinishReason(RecordIndex(g.racers[k]))) CallElement(Field(rival, 0x10), kElemShow);
            b.human = 0;
            Log(false, L"race: progress bar: player %d's car is a computer racer's again", k);
            continue;
        }
        if (applied || Field(rival, 0xc) != (uint32_t)(uintptr_t)g.racers[k]) continue;
        uint32_t marker = Field(human, 0);
        if (!marker || !Field(human, 4) || !Field(human, 0x10) || Field(human, 0x10) != *(uint32_t*)(uintptr_t)(marker + kMarkerNode)) {
            if (!b.human) Log(true, L"race: progress bar: no player marker %dP in this HUD; player %d's car keeps its own", k + 1, k);
            b.human = 1;  // (logged once; 1 never matches a marker)
            continue;
        }
        for (int i = 0; i < 5; ++i) b.fields[i] = Field(rival, kCopied[i]);
        b.flags[0] = rival[0x14];
        b.flags[1] = rival[0x15];
        CallElement(Field(rival, 0x10), kElemHide);
        for (int i = 0; i < 5; ++i) Field(rival, kCopied[i]) = Field(human, kCopied[i]);
        rival[0x14] = rival[0x15] = 0;
        CallElement(Field(rival, 0x10), kElemShow);
        uint32_t icon = ((IconHashFn)(uintptr_t)kIconHash)(g.racers[k]);
        ((SetImageFn)(uintptr_t)kSetImage)(kTextureSets, Field(rival, 4), icon, kIconSet);
        b.human = marker;
        Log(false, L"race: progress bar: player %d's car shows as %dP", k, k + 1);
    }
}

// ---------------------------------------------------------------- HUD toasts (re\m7-join-race\FINDINGS.md Q2)
// The console's bottom-right ticker ("{0} HAS JOINED THE LOBBY." and the others) is in the PC HUD scene, but the code
// that fed it is an empty stub on PC. Ours runs once per HUD frame at the same place (after native 0938 at hud 0x36b6,
// RaceNativeAfter): when the ticker's text element is idle, the next queued toast is formatted into its bound text
// buffer (the hud's own 64-byte buffer) and the group's scroll is played. Priority as the console's: left the game,
// left the lobby, returned, joined. The HUD's font is ASCII only: other characters become '?'.
GAME_ADDRESS kElementIdle = 0x47f130;   // stdcall(element hash) ret 4 -> al: its animation is idle (0 for a missing element)
GAME_ADDRESS kPlayElement = 0x47f8c0;   // stdcall(element hash, key from, key to, c, loop) ret 0x14: plays an animation (groups recurse)
GAME_ADDRESS kSceneText = 0x47fb50;     // thiscall(ECX = kTextureSets, scene hash, text key) ret 8 -> the localized text, or 0
const uint32_t kToastText = 0xca4107ae, kToastGroup = 0xc49502fc, kToastScene = 0x91c494ea;  // the HUD scene's ticker
const uint32_t kToastFrom = 0x3e3460a8, kToastTo = 0xf5fda6b0;  // the group's scroll keyframes (t1 -> t480)
const uint32_t kElemBoundText = 0x108;  // text element: its bound text object (021e), a kTextBufferVtable object
struct ToastKey { uint32_t key; const char* english; };
const ToastKey kToastKeys[5] = {{0x8da10d88, "{0} HAS JOINED THE LOBBY."}, {0x993d0c07, "{0} HAS LEFT THE GAME."},
                                {0x967e55cf, "{0} HAS LEFT THE LOBBY."}, {0, nullptr}, {0x265035ad, "{0} HAS RETURNED TO THE LOBBY."}};
const uint8_t kToastOrder[4] = {kToastLeftGame, kToastLeftLobby, kToastReturned, kToastJoined};
typedef uint8_t*(__thiscall* FindTextFn)(uintptr_t table, uint32_t hash);
typedef bool(__stdcall* ElementIdleFn)(uint32_t element);
typedef void(__stdcall* PlayElementFn)(uint32_t element, uint32_t from, uint32_t to, uint32_t c, uint32_t loop);
typedef const char*(__fastcall* SceneTextFn)(uintptr_t sets, void*, uint32_t scene, uint32_t key);

// "{0}" in the format replaced by the name, ASCII only ('?' for anything else), cut to fit.
void FormatToast(const char* fmt, const char* name, char* out, size_t cap) {
    size_t o = 0;
    for (const char* f = fmt; *f && o + 1 < cap; ) {
        if (f[0] == '{' && f[1] == '0' && f[2] == '}') {
            for (const uint8_t* s = (const uint8_t*)name; *s && o + 1 < cap; ) {
                if (*s >= 0x20 && *s < 0x80) { out[o++] = (char)*s++; continue; }
                out[o++] = '?';
                ++s;
                while ((*s & 0xc0) == 0x80) ++s;
            }
            f += 3;
            continue;
        }
        uint8_t ch = (uint8_t)*f++;
        if (ch >= 0x20 && ch < 0x80) { out[o++] = (char)ch; continue; }
        out[o++] = '?';  // one per code point
        while ((*(const uint8_t*)f & 0xc0) == 0x80) ++f;
    }
    out[o] = 0;
}

void ToastFrame() {
    if (!g.toastCount || g.phase != kRunning) return;  // (the HUD is up from the countdown; during the intros it isn't)
    if (!((ElementIdleFn)(uintptr_t)kElementIdle)(kToastText)) return;  // (0: the HUD has no ticker; the toasts wait, then go with the race)
    uint8_t* e = ((FindTextFn)(uintptr_t)kFindTextElement)(kElementTable, kToastText);
    TextBuffer* obj = e ? *(TextBuffer**)(e + kElemBoundText) : nullptr;
    if (!obj || obj->vtable != kTextBufferVtable || obj->capacity < 32 || obj->capacity > 4096 || !obj->text) {
        if (!g.toastRefused) Log(true, L"race: the HUD's ticker text was not recognised; no lobby notices are shown");
        g.toastRefused = true;
        g.toastCount = 0;
        return;
    }
    int pick = -1;
    for (uint8_t kind : kToastOrder) {
        for (int i = 0; pick < 0 && i < g.toastCount; ++i)
            if (g.toasts[i].kind == kind) pick = i;
        if (pick >= 0) break;
    }
    if (pick < 0) { g.toastCount = 0; return; }
    Race::Toast t = g.toasts[pick];
    for (int i = pick + 1; i < g.toastCount; ++i) g.toasts[i - 1] = g.toasts[i];
    --g.toastCount;
    const ToastKey& k = kToastKeys[t.kind < 5 ? t.kind : 0];
    const char* fmt = k.key ? ((SceneTextFn)(uintptr_t)kSceneText)(kTextureSets, nullptr, kToastScene, k.key) : nullptr;
    if (!fmt || !*fmt) fmt = k.english ? k.english : "{0}";
    FormatToast(fmt, t.name, obj->text, (size_t)obj->capacity);
    ((PlayElementFn)(uintptr_t)kPlayElement)(kToastGroup, kToastFrom, kToastTo, 0, 1);
    if (t.kind < 5 && !g.toastLogged[t.kind]) Log(false, L"race: the HUD's ticker shows \"%hs\"", obj->text);
    if (t.kind < 5) g.toastLogged[t.kind] = true;
}

// ---------------------------------------------------------------- the intros (re\m6-intro\FINDINGS.md, re\m7-intro-voice\FINDINGS.md)
// The user's rule (2026-10-22, replacing the vote to skip): as the original, a press at the course intro skips it on that
// screen (the stock skip, CONTINUE as the game shows it). With no press the course intro (on PC its fly-by would loop
// forever) ends once the announcer has finished its course line and the intro music has stopped (on 3 zones the music is
// shorter than the announcer; the racer intro's enter would cut him off), at the latest after kIntroCap. The racer intro is
// the stock one: its own 5 s, or a press. Each game ends its intros by itself; a player who skipped waits at the starting
// line (hold B) until everyone is there.
typedef void(__cdecl* AdvanceFn)();
typedef bool(__thiscall* IsPlayingFn)(void* sound);

// The announcer's queued lines are all spoken (or it has none, or it was stopped: a stopped queue never drains).
bool VoiceDone() {
    const uint8_t* q = *(const uint8_t* const*)(uintptr_t)kVoiceQueue;
    uintptr_t update = ((const uintptr_t*)kVoiceQueueVtable)[1];  // the stock update, or ours (the announcer group hooks it)
    if (!q || *(const uintptr_t*)q != kVoiceQueueVtable || (update != kVoiceQueueUpdate && update != (uintptr_t)&HookSpeechUpdate)) return true;
    return *(const uint16_t*)(q + kVoiceCount) == 0 || q[kVoiceRunState] == 0;
}

// The course intro's music has ended (it doesn't loop), or there is none.
bool MusicDone() {
    const uint8_t* scene = *(const uint8_t* const*)(uintptr_t)kScene;
    void* m = scene ? *(void* const*)(scene + kSceneIntroMusic) : nullptr;
    if (!m || *(const uintptr_t*)m != kSoundVtable || ((const uintptr_t*)kSoundVtable)[kSoundIsPlayingSlot] != kSoundIsPlaying) return true;
    return !((IsPlayingFn)kSoundIsPlaying)(m);
}

int32_t FlowState() {
    const uint8_t* rec = *(const uint8_t* const*)(uintptr_t)kFlowRecord;
    return rec ? *(const int32_t*)(rec + 4) : -1;
}

void TestPlayerIntros();
void IntroUpdate(void* self, int which) {
    if (Active()) Tick();
    if (Active() && g.phase == kIntros) ClockObserve();  // (seen every step before hold B: a release at its first step has its base)
    if (g.phase != kIntros || !self) { Original<UpdateFn>(which)(self); return; }
    bool course = which == kHTrackIntro;
    int32_t state = course ? kStateTrackIntro : kStateCharIntro;
    uint32_t before = *(volatile uint32_t*)(uintptr_t)kEndTimerElapsed, limit = *(volatile uint32_t*)(uintptr_t)kEndTimerLimit;
    bool was = FlowState() == state;
    Original<UpdateFn>(which)(self);
    if (!course && !g.charIntroRan) {  // (its enter, which flushes the speech queue, has run: the others' intro lines can be said)
        g.charIntroRan = true;
        TestPlayerIntros();
    }
    if (!was) return;
    if (FlowState() != state) {  // the stock update moved on: a press here (or the racer intro's own time)
        bool& logged = course ? g.introEnded : g.racerIntroLogged;
        if (!logged)
            Log(false, L"race: intro: the %s intro ends after %u ticks (%s)", course ? L"course" : L"racer", before + 1,
                course || before + 1 < limit ? L"skipped here" : L"its own time");
        logged = true;
        return;
    }
    if (!course || g.introEnded) return;
    uint32_t elapsed = *(volatile uint32_t*)(uintptr_t)kEndTimerElapsed;
    bool music = VoiceDone() && MusicDone();
    if (!music && elapsed < kIntroCap) return;
    ((AdvanceFn)(uintptr_t)kFlowAdvance)();  // once, after the stock update (it queues the state change)
    g.introEnded = true;
    Log(false, L"race: intro: the course intro ends after %u ticks: %s; on to the racer intro", elapsed,
        music ? L"the announcer and the music have finished" : L"its longest wait");
}

// ---------------------------------------------------------------- the announcer (re\m7-announcer\FINDINGS.md)
// The user's goal (2026-10-22): every screen hears the same announcer line at the same moment. The course intro's lines
// are queued on every PC at the same moment (TrackIntro enter): each PC picks the same recorded take from the race's
// seed. From the countdown on the host's game is the only announcer: each line it queues (cue, take, wait) goes to the
// guests, whose own commentary is muted with the game's own switch (scene +0x1350); they queue the host's line the
// stock way (0x46d6b0) with its take forced. A take this PC's language lacks is left to its own pick.
typedef uint32_t(__thiscall* TakeFn)(void* self, uint32_t arg);
typedef uint8_t*(__thiscall* GroupChildFn)(void* takes, int i);
typedef bool(__thiscall* CommentPlayFn)(void* commentMan, void* entry);
typedef void(__cdecl* SpeechFlushFn)();
typedef void(__thiscall* SpeechUpdateFn)(void* queue, uint32_t ms);

bool g_anyResolverCaller = false;  // tests: their calls stand for the resolver's
bool FromResolver(void* ret) { return g_anyResolverCaller || (uintptr_t)ret == kCommentResolverRet; }

uint32_t Mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352d;
    x ^= x >> 15;
    x *= 0x846ca68b;
    return x ^ (x >> 16);
}

// One of a group's takes: `want` if it has it (0 if not); with no `want`, the one whose hash mixed with the seed is
// smallest (the same on every PC, whatever the order of its list).
uint32_t PickTake(uint8_t* group, uint32_t seed, uint32_t want) {
    int n = *(int32_t*)(group + kGroupTakes);
    uint32_t best = 0, bestKey = 0;
    for (int i = 0; i < n && i < 64; ++i) {
        uint8_t* item = ((GroupChildFn)(uintptr_t)kGroupChild)(group + kGroupTakes, i);
        uint32_t h = item ? *(uint32_t*)item : 0;
        if (!h) continue;
        if (want) {
            if (h == want) return h;
            continue;
        }
        uint32_t key = Mix32(seed ^ h);
        if (!best || key < bestKey) best = h, bestKey = key;
    }
    return want ? 0 : best;
}

// A commentary entry by its hash, from the entry list itself (0x46d5e0 would move CommentMan's shared iterator, which
// its update may be using when a line resolves).
uint8_t* CommentEntry(uint32_t cue) {
    uint8_t* cm = *(uint8_t**)(uintptr_t)kCommentMan;
    uint8_t** e = cm ? *(uint8_t***)(cm + kCmEntries) : nullptr;
    int n = cm ? *(int32_t*)(cm + kCmCount) : 0;
    for (int i = 0; e && i < n && i < 4096; ++i)
        if (e[i] && *(uint32_t*)(e[i] + kEntryHash) == cue) return e[i];
    return nullptr;
}

bool AnnounceSending() {  // the host from the countdown on, with another real player in the race
    if (!g.host || g.phase != kRunning || !(g.c.flags & kContractAnnouncer)) return false;
    for (int i = 0; i < g.peerCount; ++i)
        if (!g.peers[i].gone && !g.peers[i].test) return true;
    return false;
}

uint32_t __fastcall HookTakeSelect(uint8_t* group, void*, uint32_t arg) {
    if (group && Active() && FromResolver(_ReturnAddress())) {
        uint32_t cue = *(uint32_t*)(group + kGroupHash);
        if (g.forceCue && cue == g.forceCue) {  // a guest saying the host's line
            if (uint32_t take = PickTake(group, 0, g.forceTake)) return take;
            ++g.announceMissed;
        } else if (g.phase == kIntros) {  // the course intro's lines: the race's takes
            if (uint32_t take = PickTake(group, g.c.seed, 0)) {
                if (!g.introTakeLogged) Log(false, L"race: announcer: the course intro's lines are the same recordings on every screen");
                g.introTakeLogged = true;
                return take;
            }
        } else if (AnnounceSending() || FairCallsOn()) {
            uint32_t take = Original<TakeFn>(kHTakeSelect)(group, arg);
            if (FairCallsOn()) take = FreshTake(group, take);  // no recording twice while the line has another
            if (AnnounceSending()) {
                g.pendingGroup = cue;  // (its take's GetSource comes next, if the take resolves)
                g.pendingTake = take;
            }
            return take;
        }
    }
    return Original<TakeFn>(kHTakeSelect)(group, arg);
}

uint32_t __fastcall HookTakeSource(uint8_t* take, void*, uint32_t arg) {
    uint32_t result = Original<TakeFn>(kHTakeSource)(take, arg);
    if (!take || !Active() || !FromResolver(_ReturnAddress())) return result;
    uint32_t take_ = *(uint32_t*)(take + kGroupHash);
    uint32_t group = g.pendingTake == take_ ? g.pendingGroup : 0;  // (a group whose take didn't resolve is forgotten)
    g.pendingGroup = g.pendingTake = 0;
    if (!AnnounceSending()) return result;
    RaceAnnounce a = {};
    a.race = g.c.race;
    a.tick = SimTick();
    a.take = take_;
    a.cue = group ? group : a.take;  // (a single-take entry is the take itself)
    uint8_t* entry = CommentEntry(a.cue);
    a.wait = entry ? *(int32_t*)(entry + kEntryWait) : 0;
    a.flush = g.flushNext ? 1 : 0;  // a lap call flushed the queue before this line
    g.flushNext = false;
    uint8_t b[kWireMax];
    SendToAll(b, EncodeAnnounce(a, b, sizeof(b)), true);
    if (g.announceSent++ < 20) Log(false, L"race: announcer: sent %08x (take %08x, wait %d ms%s)", a.cue, a.take, a.wait, a.flush ? L", after a flush" : L"");
    return result;
}

// Guest: the host's lines, queued here the stock way (before the queue's own update runs), in order. The speech queue (8
// slots, this game's own driver voices too) takes a line only with a slot to spare (0x46d6b0): a refused line and the ones
// after it wait for room, up to 10 s (in game, m7t: at the finish the guest's own voices filled it and 2 of the host's 4
// lines were refused).
const uint32_t kLineWaitTicks = 500;
void SayHostLines() {
    uint8_t* cm = *(uint8_t**)(uintptr_t)kCommentMan;
    int i = 0;
    for (; i < g.lineCount; ++i) {
        RaceAnnounce& a = g.lines[i];
        if (a.flush) ((SpeechFlushFn)(uintptr_t)kSpeechFlush)();
        a.flush = 0;  // (once, also when the line waits)
        uint8_t* entry = cm ? CommentEntry(a.cue) : nullptr;
        if (!entry) {
            if (g.announceMissed++ < 5) Log(false, L"race: announcer: the host's line %08x isn't one this game has", a.cue);
            continue;
        }
        *(int32_t*)(entry + kEntryWait) = a.wait;
        g.forceCue = a.cue;
        g.forceTake = a.take;
        bool queued = ((CommentPlayFn)(uintptr_t)kCommentPlay)(cm, entry);
        g.forceCue = g.forceTake = 0;
        entry[kEntryActive] = queued ? 1 : 0;  // what 0x46daf0 does after its call
        *(int32_t*)(entry + kEntryElapsed) = 0;
        if (queued) {
            if (g.announceSaid++ < 20) Log(false, L"race: announcer: said the host's %08x", a.cue);
            continue;
        }
        if (SimTick() - g.lineArrived[i] < kLineWaitTicks) {
            if (g.announceWaited++ < 5) Log(false, L"race: announcer: the speech queue is full here; the host's %08x waits for room", a.cue);
            break;
        }
        if (g.announceMissed++ < 5) Log(false, L"race: announcer: the host's %08x found no room here in 10 s (not said)", a.cue);
    }
    memmove(g.lines, g.lines + i, (g.lineCount - i) * sizeof(g.lines[0]));
    memmove(g.lineArrived, g.lineArrived + i, (g.lineCount - i) * sizeof(g.lineArrived[0]));
    g.lineCount -= i;
}

void ForgetPlaceLine();
void CountVoices();
void StartIntros();
void __fastcall HookSpeechUpdate(void* queue, void*, uint32_t ms) {
    bool voices = Active() && queue && queue == *(void**)(uintptr_t)kVoiceQueue;
    if (voices) CountVoices();  // (the frame's physics ticks have run: their lines are queued, the update hasn't started them)
    if (voices && g.lineCount) SayHostLines();
    Original<SpeechUpdateFn>(kHSpeechUpdate)(queue, ms);
    if (voices) StartIntros();  // (after the update: this game's own lines have their streams first)
    if (voices) ForgetPlaceLine();  // (lines end in the update: before the next line can take its sounder)
}

// ---- the lap calls follow the leader (user, 2026-10-22), the wrong-way calls whoever turned
typedef void(__cdecl* CommentRequestFn)(uint32_t cue, int32_t waitMs, int32_t flush);
typedef void(__cdecl* WrongWayCallFn)(int32_t recordIndex);

uint32_t MostCrossings() {
    uint32_t most = 0;
    for (int k = 0; k < Cars(); ++k)
        if (uint8_t* rec = g.racers[k] ? Record(RecordIndex(g.racers[k])) : nullptr)
            if (*(uint32_t*)(rec + kRecCrossings) > most) most = *(uint32_t*)(rec + kRecCrossings);
    return most;
}

// Host: its own player's lap calls are held: their entries look busy for good (0x46daf0 drops a line whose entry is
// active and won't cool down in its wait); the leader's are made here.
void HoldOwnLapCalls() {
    for (int i = 0; i < 4; ++i) {
        uint8_t* e = CommentEntry(kLapCues[i]);
        if (!e) continue;
        g.lapCooldown[i] = *(uint32_t*)(e + kEntryCooldown);
        *(uint32_t*)(e + kEntryCooldown) = 0x7fffffff;
        e[kEntryActive] = 1;
        *(uint32_t*)(e + kEntryElapsed) = 0;
    }
    g.lapsHeld = true;
    g.leaderCrossings = MostCrossings();
}

void ReleaseLapCalls() {
    if (!g.lapsHeld) return;
    g.lapsHeld = false;
    for (int i = 0; i < 4; ++i)
        if (uint8_t* e = CommentEntry(kLapCues[i])) {  // (a held value read back by mistake is no cooldown to keep)
            uint32_t was = g.lapCooldown[i];
            *(uint32_t*)(e + kEntryCooldown) = was && was != 0x7fffffff ? was : 30000;
            e[kEntryActive] = 0;
        }
}

// Host, every race tick: the first car to reach a crossing gets its lap call, as 0x4bc290 makes it for the local player
// (LAP2..LAP4, FLAP at the last lap, a flush first), and every screen hears it.
void LeaderLapCalls() {
    if (!g.lapsHeld || g.c.laps < 2) return;  // (also once the guests are gone: the host's own calls stay held till the end)
    uint32_t most = MostCrossings();
    if (most <= g.leaderCrossings) return;
    g.leaderCrossings = most;
    uint32_t cue = most == g.c.laps ? kLapCues[3] : most >= 2 && most <= 4 && most < g.c.laps ? kLapCues[most - 2] : 0;
    uint8_t* e = cue ? CommentEntry(cue) : nullptr;
    if (!e) return;
    e[kEntryActive] = 0;
    g.flushNext = true;
    ((CommentRequestFn)(uintptr_t)kCommentRequest)(cue, 12000, 1);
    g.flushNext = false;  // (used by the line, if it was queued)
    e[kEntryActive] = 1;
    *(uint32_t*)(e + kEntryCooldown) = 0x7fffffff;
    Log(false, L"race: announcer: the leader's lap call (crossing %u of %u)", most, g.c.laps);
}

// Guest, every race tick: this game's car just turned the wrong way: the host's announcer calls it.
void ReportWrongWay() {
    if (!g.announceMuted || g.local < 0 || !g.racers[g.local]) return;
    bool now = ((uint8_t*)g.racers[g.local])[kRacerWrongWay] != 0;
    Peer* host = PeerByIndex(0);
    if (now && !g.wrongWay && host && !host->gone) {
        uint8_t b[kWireMax];
        size_t n = EncodeWrongWay(g.c.race, (uint8_t)g.local, b, sizeof(b));
        if (n) RaceNetSend(host->id, b, n, true);
    }
    g.wrongWay = now;
}

// Host: another player's car turned the wrong way (their own game's report).
void CallWrongWay(int k) {
    if (!AnnounceSending() || k < 0 || k >= kGridSlots || !g.racers[k]) return;
    ((WrongWayCallFn)(uintptr_t)kWrongWayCall)(RecordIndex(g.racers[k]));
    NoteMention(k);
    Log(false, L"race: announcer: player %d is going the wrong way (their game's report)", k);
}

// The countdown is released: a guest whose host announces mutes its own commentary (its deferred lines too); the host
// holds its own player's lap calls (the leader's are made instead).
void AnnounceStart() {
    if (!g_announcerHooks || !(g.c.flags & kContractAnnouncer)) return;
    if (g.host && AnnounceSending()) HoldOwnLapCalls();
    if (g.host || HostGone()) return;
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    if (!scene) return;
    scene[kSceneCommentMute] = 1;
    g.announceMuted = true;
    uint8_t* cm = *(uint8_t**)(uintptr_t)kCommentMan;
    uint8_t** e = cm ? *(uint8_t***)(cm + kCmEntries) : nullptr;
    for (int i = 0; e && i < *(int32_t*)(cm + kCmCount) && i < 4096; ++i)
        if (e[i]) e[i][kEntryDeferred] = 0;
    Log(false, L"race: announcer: this game says the host's lines from now on (its own commentary is muted)");
}

// The race is over here or the host is gone: this game's own commentary again.
void AnnounceStop(const wchar_t* why) {
    ReleaseLapCalls();
    if (!g.announceMuted) return;
    g.announceMuted = false;
    g.lineCount = 0;
    if (uint8_t* scene = *(uint8_t**)(uintptr_t)kScene) scene[kSceneCommentMute] = 0;
    Log(false, L"race: announcer: this game's own commentary again (%s); %d of the host's lines said here, %d not", why, g.announceSaid,
        g.announceMissed);
}

// ---- fair call-outs (user, 2026-10-22; re\m7-announcer2\FINDINGS.md §1, §2)
// The stock commentary favours the local player: pass lines about it anywhere, about others only near the front; homing
// warnings about it always, about others half the time and only near its camera; at the finish the top three until its line,
// then its own place. The host, whose lines every screen hears, follows the user's rules instead: (a) by race position, the
// leader most, last place least, host or guest alike; (b) a player just mentioned less likely again soon; (c) players before
// computer racers; (d) every player's homing warnings; (e) at the finish the top three, then one player outside them, the
// least mentioned the likeliest (every player in the top three: no more); (f) passes between two players more often; (g) no
// recording twice while the line has another. A departed player's car counts as a computer racer, a test player as a player;
// last-place lines (TRAIL) as the stock says them for its local player.
typedef void(__cdecl* OvertakeCallFn)(int32_t index, uint8_t gained, int32_t place);
typedef void(__thiscall* VoiceLineFn)(void* voice, int32_t unused);

// The host's own draws for these picks (never the game's CRT rand or its shared generator, which the race runs on).
uint32_t g_drawState = 0;
#ifdef RACE_TEST
float g_testRoll = -1.0f;
int g_testCoin = -1;
#endif
uint32_t AnnounceRand() {
    if (!g_drawState) g_drawState = (GetTickCount() * 2654435761u) | 1;
    g_drawState ^= g_drawState << 13;
    g_drawState ^= g_drawState >> 17;
    g_drawState ^= g_drawState << 5;
    return g_drawState;
}

float AnnounceRoll() {  // [0, 1)
#ifdef RACE_TEST
    if (g_testRoll >= 0.0f) return g_testRoll;
#endif
    return (float)(AnnounceRand() >> 8) * (1.0f / 16777216.0f);
}

int AnnounceCoin() {
#ifdef RACE_TEST
    if (g_testCoin >= 0) return g_testCoin;
#endif
    return (int)(AnnounceRand() & 1);
}

bool FairCallsOn() {  // the host from the countdown on, with another player in the race (a test player too)
    if (!g_fairHooks || !g.host || g.phase != kRunning || !(g.c.flags & kContractAnnouncer)) return false;
    for (int i = 0; i < g.peerCount; ++i)
        if (!g.peers[i].gone) return true;
    return false;
}

// Car k is a player's: this game's, or another player's still in the race. A test player's car is the host's AI from hold A
// on (CarLeft, like a departed player's), and still a player's (in game, m7r: their passes went to the stock pick).
bool AnnouncedPlayer(int k) {
    if (k < 0 || k >= g.c.humanCount || k >= kGridSlots || !g.racers[k]) return false;
    if (k == g.local) return true;
    const Peer* p = PeerByIndex(k);
    return p && !p->gone && (p->test || !g.adopted[k]);
}

void NoteMention(int k) {
    if (k < 0 || k >= kGridSlots) return;
    ++g.mentions[k];
    g.mentionTick[k] = SimTick() | 1;
}

float Spread(int k) {  // (b): 0.3 just after a mention, back to 1 over 90 s
    if (!g.mentionTick[k]) return 1.0f;
    int32_t ticks = (int32_t)(SimTick() - g.mentionTick[k]);  // (signed: the stored tick is | 1, one ahead on an even tick)
    float since = ticks > 0 ? (float)ticks / 50.0f : 0.0f;
    return since >= 90.0f ? 1.0f : 0.3f + 0.7f * since / 90.0f;
}

bool Trail(bool gained, int place) { return !gained && place >= Cars() - 1; }

// A player's chance to be a pass's line: (a) 1.0 for the leader down to 0.35 for last place, times (b), times 1.5 for a pass
// between two players (f). Passed into last place (TRAIL): as the stock for its local player (always, on its coin's side).
float PassChance(int k, bool gained, int place, bool duel) {
    int n = Cars();
    if (Trail(gained, place)) return 1.0f;
    float c = (n > 1 ? 1.0f - 0.65f * (float)place / (float)(n - 1) : 1.0f) * Spread(k) * (duel ? 1.5f : 1.0f);
    return c < 1.0f ? c : 1.0f;
}

const wchar_t* PassLine(bool gained, int place) { return gained ? (place == 0 ? L"LEAD" : L"GAIN") : Trail(gained, place) ? L"TRAIL" : L"LOST"; }

struct Pass {
    int r, o;         // the passer and the car now at the place it left (creation indexes)
    int32_t cur, prev;  // the passer's place now and the one it kept (0-based)
};

// What 0x4c4840 sees in this tracker call (the same tests): car R now ahead of the place its tracker kept, O the car there now.
bool NextPass(uint8_t* racer, Pass& p) {
    if (*(int32_t*)(racer + kRacerLife) != 2 || racer[kRacerAiThrottle]) return false;
    const int32_t* tracker = *(const int32_t* const*)(racer + kRacerReactions);
    uint8_t* rec = Record(RecordIndex(racer));
    if (!tracker || !rec || *(volatile uint32_t*)(uintptr_t)kRaceClock <= 8) return false;
    p.prev = tracker[0];
    p.cur = *(int32_t*)(rec + kRecPosition);
    if (p.prev < 0 || p.prev >= kGridSlots || p.cur < 0 || p.cur >= p.prev) return false;
    p.r = RecordIndex(racer);
    p.o = ((const uint8_t*)(uintptr_t)kPlaceList)[p.prev * 8];
    // Not a pass to call: the race is over, the list shows the same car at the place it left (m7w logged "car 2 passed car 2"),
    // or either car has finished (its gates would refuse the line anyway).
    if (RaceOver() || p.o == p.r || p.o < 0 || p.o >= kGridSlots || CarFinished(p.r) || CarFinished(p.o)) return false;
    return AnnouncedPlayer(p.r) || AnnouncedPlayer(p.o);
}

// The pass's line, if any: the stock's coin picks a side (the passer's GAIN, or the passed car's LOST); a player's side first
// (c), said with its chance; a computer racer's only as the stock says it (its coin's side, the passer's new place under 3 for
// GAIN, under 4 for LOST); TRAIL only on the coin's side too (the stock's half the time, user: "same as the game").
void FairPass(const Pass& p) {
    struct Side { int k; bool gained; int32_t place; } sides[2] = {{p.r, true, p.cur}, {p.o, false, p.prev}};
    bool player[2] = {AnnouncedPlayer(p.r), AnnouncedPlayer(p.o)};
    int coin = AnnounceCoin();
    int order[2] = {coin, 1 - coin};
    if (!player[coin] && player[1 - coin]) order[0] = 1 - coin, order[1] = coin;
    int chosen = -1;
    float chance = 0.0f, roll = 0.0f;
    for (int i : order) {
        if (player[i] && Trail(sides[i].gained, sides[i].place) && i != coin) continue;
        if (player[i]) {
            chance = PassChance(sides[i].k, sides[i].gained, sides[i].place, player[0] && player[1]);
            roll = AnnounceRoll();
            if (roll < chance) {
                chosen = i;
                break;
            }
        } else if (i == coin && p.cur < (sides[i].gained ? 3 : 4)) {
            chosen = i;
            chance = 1.0f;
            break;
        }
    }
    bool said = false;
    if (chosen >= 0) {
        const Side& s = sides[chosen];
        ((OvertakeCallFn)(uintptr_t)kOvertakeCall)(s.k, s.gained ? 1 : 0, s.place);
        said = *(volatile float*)(uintptr_t)kOvertakeCooldown == 0.0f;  // (it speaks unless its gates say no, and then starts over)
        if (said && player[chosen]) NoteMention(s.k);
    }
    if (g.passLogged++ >= 30) return;
    if (chosen < 0)
        Log(false, L"race: announcer: car %d (%hs%s) passed car %d (%hs%s) for place %d: no line (a player's chance %.2f, drew %.2f)", p.r,
            RacerName(p.r), player[0] ? L", player" : L"", p.o, RacerName(p.o), player[1] ? L", player" : L"", p.cur + 1, chance, roll);
    else
        Log(false, L"race: announcer: car %d (%hs%s) passed car %d (%hs%s) for place %d: %s about car %d (chance %.2f)%s", p.r, RacerName(p.r),
            player[0] ? L", player" : L"", p.o, RacerName(p.o), player[1] ? L", player" : L"", p.cur + 1,
            PassLine(sides[chosen].gained, sides[chosen].place), sides[chosen].k, chance, said ? L"" : L"; its gates said no");
}

// Racer vtable slot 3 (0x4f1eb0) runs each car's tracker: 0x4c5310 -> 0x4c4840 (a pass) -> 0x4c3410 (the stock's pick) ->
// 0x4bc180, which speaks from 45 s after the last pass line. For a pass with a player in it, while a line is due, the host
// keeps the stock's pick quiet (that cooldown, which only 0x4bc180 reads, held at 0 for the call) and picks by the rules.
void __fastcall HookRacerTrack(uint8_t* racer, void*) {
    Pass p = {};
    volatile float* cooldown = (volatile float*)(uintptr_t)kOvertakeCooldown;
    float due = *cooldown;
    bool ours = racer && due >= kOvertakeReady && FairCallsOn() && NextPass(racer, p);
    if (ours) *cooldown = 0.0f;
    Original<UpdateFn>(kHRacerTrack)(racer);
    if (!ours) return;
    *cooldown = due;
    FairPass(p);
}

// Homing warnings (0x4bc9d0, from a car's lock change in 0x536d10): the host warns about every other player as about its
// own (d), when the stock said nothing (its cooldown still above 46 s): not finished, the homing thing 50 m away or more, the
// player's own cue (a quarter of the time the generic one), a 1 s wait.
void FairAlert(int k, const uint8_t* racer) {
    uintptr_t lock = *(const uintptr_t*)(racer + kRacerLock);
    bool fresh = lock && lock != g.lockSeen[k];
    g.lockSeen[k] = lock;
    if (!fresh || !FairCallsOn() || !AnnouncedPlayer(k) || CarFinished(k)) return;
    volatile float* cooldown = (volatile float*)(uintptr_t)kAlertCooldown;
    if (*cooldown == 0.0f) {  // the stock warned about this player just now (its own player, or another near its camera): a mention
        NoteMention(k);
        return;
    }
    if (k == g.local) return;  // (the stock's)
    if (!(*cooldown > kAlertReady) || *(volatile uint8_t*)(uintptr_t)kCommentOff || !*(volatile uint8_t*)(uintptr_t)kCalloutsLive) return;
    const float* at = (const float*)(racer + kRacerPos);
    const float* from = (const float*)(lock + kRacerPos);
    float dx = at[0] - from[0], dy = at[1] - from[1], dz = at[2] - from[2];
    if (dx * dx + dy * dy + dz * dz < kAlertNear * kAlertNear) return;
    uint32_t generic = *(volatile uint32_t*)(uintptr_t)kGenericAlert;
    uint32_t own = PlayerCue(k, kCueAlert);
    uint32_t cue = generic && (!own || AnnounceRoll() < 0.25f) ? generic : own;
    if (!cue) return;
    ((CommentRequestFn)(uintptr_t)kCommentRequest)(cue, 1000, 0);
    *cooldown = 0.0f;
    NoteMention(k);
    if (g.alertLogged++ < 10) Log(false, L"race: announcer: player %d (%hs) is being targeted: warned as the host's own player would be", k, RacerName(k));
}

bool TakeUsed(uint32_t take) {
    for (int i = 0; i < g.takeCount; ++i)
        if (g.takesUsed[i] == take) return true;
    return false;
}

// (g): the stock's pick, unless this race said it already and the line has a recording it hasn't.
uint32_t FreshTake(uint8_t* group, uint32_t stock) {
    uint32_t pick = stock;
    if (stock && TakeUsed(stock)) {
        uint32_t fresh[64];
        int m = 0, n = *(int32_t*)(group + kGroupTakes);
        for (int i = 0; i < n && i < 64; ++i) {
            uint8_t* item = ((GroupChildFn)(uintptr_t)kGroupChild)(group + kGroupTakes, i);
            uint32_t h = item ? *(uint32_t*)item : 0;
            if (h && !TakeUsed(h)) fresh[m++] = h;
        }
        if (m) pick = fresh[AnnounceRand() % (uint32_t)m];
        if (m && g.freshLogged++ < 5)
            Log(false, L"race: announcer: line %08x's recording %08x was said already: %08x instead (%d left)", *(uint32_t*)(group + kGroupHash), stock,
                pick, m - 1);
    }
    if (pick && !TakeUsed(pick) && g.takeCount < (int)(sizeof(g.takesUsed) / sizeof(g.takesUsed[0]))) g.takesUsed[g.takeCount++] = pick;
    return pick;
}

// Place lines (the user's rule, 2026-10-22, replacing the stock's race-end top three): every player's place is called on every
// screen the moment they cross the line: RESULT1-3 for the top three (25 s, as the stock's race end), RESULT4 below (35 s);
// a player who didn't finish gets FINFAIL (35 s) at the race end. Computer racers get none (user: players only).
// This car's place among the finished: by finish time (record +0x68), as the race end's sort (0x4cc790); ties by list order;
// only real finishes (reason 1) count.
int FinishPlace(int k) {
    int index = RecordIndex(g.racers[k]);
    uint8_t* rec = Record(index);
    if (!rec) return kGridSlots - 1;
    uint32_t t = *(uint32_t*)(rec + kRecTime);
    int n = *(volatile uint8_t*)(uintptr_t)kFinishCount, place = 0;
    const uint8_t* list = (const uint8_t*)(uintptr_t)kFinishList;
    bool seen = false;
    for (int i = 0; i < n && i < 16; ++i) {
        int j = list[i * 8];
        if (j == index) {
            seen = true;
            continue;
        }
        if (*(const uint32_t*)(list + i * 8 + 4) != 1) continue;  // (the race end's estimates for the unfinished)
        uint8_t* other = Record(j);
        uint32_t tj = other ? *(uint32_t*)(other + kRecTime) : 0xffffffff;
        if (tj < t || (tj == t && !seen)) ++place;
    }
    return place;
}

// A commentary entry that would not speak now: said within its cooldown (0x46daf0 defers or drops it).
bool LineBusy(const uint8_t* entry) {
    return entry[kEntryActive] && *(const uint32_t*)(entry + kEntryElapsed) < *(const uint32_t*)(entry + kEntryCooldown);
}

// A player's line about their racer: cue `idx` of its voice. A racer without lines of its own (Metal Sonic, Roster Pack: the
// game's data has none for him) stays as the game has him: no line (user, 2026-10-22: not the game's "PLAYER<n>" lines either,
// which every kind has for racers without names).
uint32_t PlayerCue(int k, uint32_t idx) {
    uint8_t* voice = g.racers[k] ? *(uint8_t**)((uint8_t*)g.racers[k] + kRacerVoice) : nullptr;
    return voice ? *(uint32_t*)(voice + kVoiceCues + idx * 4) : 0;
}

// The speech queue's records (0x10: an array of its slots' records, 0x14 bytes each, +0 the sounder; the head plays; cap +0x14)
// and whether a sounder of the last place line is still in it: the announcer is still saying it. The sounders come from the
// queue's own pool (0x46e2e0: the first one no record uses), so a line's sounder goes to the next line queued once it has
// ended (the independent check): the place line is forgotten the first time it is gone (ForgetPlaceLine, right after each
// queue update, where lines end, and before each finish check).
const uint32_t kQueueSlots = 0x10, kQueueCap = 0x14;
bool PlaceLineTalking() {
    uint8_t* queue = *(uint8_t**)(uintptr_t)kVoiceQueue;
    uint8_t** slots = queue ? *(uint8_t***)(queue + kQueueSlots) : nullptr;
    int count = queue ? *(uint16_t*)(queue + kVoiceCount) : 0, cap = queue ? *(uint16_t*)(queue + kQueueCap) : 0;
    for (int i = 0; slots && i < count && i < cap; ++i)
        for (int j = 0; slots[i] && j < g.placeParts; ++j)
            if (*(void**)slots[i] == g.placeSounders[j]) return true;
    return false;
}

void ForgetPlaceLine() {
    if (g.placeParts && !PlaceLineTalking()) g.placeParts = 0;
}

// The user's rule (2026-10-22): when players finish close together the announcer says the first one's line and skips the ones
// who crossed while he was saying it; once he has finished it, the next finisher's line plays. Every screen hears the same:
// only the host's said lines are sent.
void SayPlace(int k, int place, bool finished, const wchar_t* when) {
    g.placeCalled[k] = true;
    uint32_t idx = !finished ? kCueFinFail : place < 3 ? kCueResult1 + (uint32_t)place : kCueResult4;
    static const wchar_t* const kNames[] = {L"RESULT1", L"RESULT2", L"RESULT3", L"RESULT4"};
    const wchar_t* line = finished ? kNames[place < 3 ? place : 3] : L"FINFAIL";
    if (PlaceLineTalking()) {
        Log(false, L"race: announcer: player %d (%hs) %s, place %d: %s skipped (the announcer is still saying player %d's)", k, RacerName(k),
            when, place + 1, line, g.placeTalker);
        return;
    }
    uint32_t cue = PlayerCue(k, idx);
    uint8_t* queue = *(uint8_t**)(uintptr_t)kVoiceQueue;
    int before = queue ? *(uint16_t*)(queue + kVoiceCount) : -1;
    const uint8_t* entry = cue ? CommentEntry(cue) : nullptr;
    bool busy = entry && LineBusy(entry);  // (read before the request: a queued line is busy afterwards by definition; m7y's log)
    if (cue) ((CommentRequestFn)(uintptr_t)kCommentRequest)(cue, finished && place < 3 ? 25000 : 35000, 0);
    int after = queue ? *(uint16_t*)(queue + kVoiceCount) : -1;
    uint8_t** slots = queue ? *(uint8_t***)(queue + kQueueSlots) : nullptr;
    g.placeParts = 0;
    for (int i = before; slots && i >= 0 && i < after && i < 16 && g.placeParts < 4; ++i)  // (appended at the tail, one per part)
        if (slots[i]) g.placeSounders[g.placeParts++] = *(void**)slots[i];
    g.placeTalker = k;
    if (after > before) NoteMention(k);
    // (refused: no room for its parts, busy before the request, or no such line)
    Log(false, L"race: announcer: player %d (%hs) %s, place %d: %s %s (%08x, %d part(s)%s; speech queue %d)", k, RacerName(k), when,
        place + 1, line, !cue ? L"has no line" : after > before ? L"queued" : L"refused", cue, entry ? *(const int32_t*)(entry + 0x10) : -1,
        busy ? L", busy" : !entry ? L", not loaded" : L"", before);
}

// Host, every race tick: a player who has just finished (in the finish list with a real finish) gets their place line. Not
// once the race is over (the race end's estimated times aren't finishes; FinishLines says what is left).
// Another real player's finish counts once their own Finished is applied (the host's game may list their car at its own view
// of the crossing first, with its own time: a photo finish could be called wrong).
void PlayerFinishLines() {
    if (!FairCallsOn() || RaceOver()) return;
    ForgetPlaceLine();
    for (int k = 0; k < g.c.humanCount && k < kGridSlots; ++k) {
        if (g.placeCalled[k] || !AnnouncedPlayer(k)) continue;
        const Peer* p = k == g.local ? nullptr : PeerByIndex(k);
        if (p && !p->test && !p->applied) continue;
        uint32_t* reason = FinishReason(RecordIndex(g.racers[k]));
        if (reason && *reason == 1) SayPlace(k, FinishPlace(k), true, L"crossed the line");
    }
}

// The race's end on the host: the players not called yet (a finish in the race's last tick: theirs; not finished: FINFAIL;
// by the user's rule only the first of these is said while it plays).
void FinishLines() {
    ForgetPlaceLine();
    int left = 0;
    for (int k = 0; k < g.c.humanCount && k < kGridSlots; ++k) {
        if (g.placeCalled[k] || !AnnouncedPlayer(k)) continue;
        uint32_t* reason = FinishReason(RecordIndex(g.racers[k]));
        bool finished = reason && *reason == 1;
        SayPlace(k, finished ? FinishPlace(k) : kGridSlots - 1, finished, finished ? L"finished as the race ended" : L"didn't finish");
        ++left;
    }
    wchar_t counts[256] = L"";
    for (int k = 0; k < g.c.humanCount && k < kGridSlots; ++k) {
        size_t at = wcslen(counts);
        if (at < 200) _snwprintf_s(counts + at, 256 - at, _TRUNCATE, L"%s%d (%hs) %d", at ? L", " : L"", k, RacerName(k), g.mentions[k]);
    }
    Log(false, L"race: announcer: the finish (mode %d): %d player line(s) at the race end; the stock's top three held (mentions: %s)",
        *(volatile int32_t*)(uintptr_t)kRaceMode, left, counts);
}

// The "race finished" enter (0x5b23b0) runs 0x4bce30, the stock's race-end lines (the top three's, then the local player's).
// On the host its commentary is held with the game's own switch for the call (scene +0x1350, which 0x4bce30 checks before each
// line; its drivers' voices still play, as on every guest, whose commentary is muted the same way) and FinishLines says what
// is left of the players' place lines.
void __fastcall HookRaceEnd(void* self, void*) {
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    bool ours = scene && FairCallsOn() && !scene[kSceneCommentMute] && *(volatile int32_t*)(uintptr_t)kRaceMode != 3;
    if (ours) scene[kSceneCommentMute] = 1;
    Original<UpdateFn>(kHRaceEnd)(self);
    if (!ours) return;
    scene[kSceneCommentMute] = 0;
    FinishLines();
}

// ---- fair item lines (re\m7-item-lines\FINDINGS.md). The stock's HIT line ("...has been hit!") is picked in each car's update
// (racer slot 4 -> 0x4bd3b0 -> 0x4bc390): the first spinning car in update order speaks once the 45 s since the last one are up.
// On the host, another player's car is a network puppet whose spin flag follows only the host's cosmetic view of it: the hits
// their game saw never reach the host, the ones it dodged still can. So each guest reports its own car's spins (ItemLine), the
// host holds the stock's pick for every car while the fair call-outs run and picks the line itself each tick: the players
// first (c), the best placed first, each with its chance by place and recent mentions (a)(b); a computer racer's as the stock
// (always). The ALLSTM line ("...got hit by X's All-Star") is the same story through the racer's message listener (message
// 0x28): the host mutes its own view of a puppet being touched and calls the guest's report. Excitement still accrues (it
// comes before the stock's cooldown gate), and the stock's own gates (call-outs live, the race clock, the game's switch) hold.
typedef void(__cdecl* AllStarHitCallFn)(int32_t recordIndex);
typedef int(__thiscall* ListenerFn)(void* listener, int32_t* message);

bool FairLinesOn() { return g_lineHooks && FairCallsOn(); }

int32_t HitPlace(int k) {  // car k's place now (its lap record), last place when unknown
    uint8_t* rec = g.racers[k] ? Record(RecordIndex(g.racers[k])) : nullptr;
    int32_t place = rec ? *(int32_t*)(rec + kRecPosition) : kGridSlots - 1;
    return place >= 0 && place < kGridSlots ? place : kGridSlots - 1;
}

// Racer vtable slot 4 (0x4f3990): a car's update, with its commentary tests. While the host picks the HIT lines itself, the
// stock's pick sees 0 s since the last line (that cooldown, read only by 0x4bc390, held for the call) and stays quiet.
void __fastcall HookRacerUpdate(uint8_t* racer, void*) {
    volatile float* cooldown = (volatile float*)(uintptr_t)kHitCooldown;
    bool hold = racer && FairLinesOn();
    float was = hold ? *cooldown : 0.0f;
    if (hold) *cooldown = 0.0f;
    Original<UpdateFn>(kHRacerUpdate)(racer);
    if (hold) *cooldown = was;
}

// The HIT line about car k, as 0x4bc390 makes it: the cooldown starts over first (line or not), the generic cue a quarter of
// the time, else the car's own, a 2.5 s wait; a racer without lines of its own (Roster Pack) gets none, as the stock gives it
// its empty cue (and as the user keeps Metal Sonic: no lines).
void HitLine(int k, float chance) {
    volatile float* cooldown = (volatile float*)(uintptr_t)kHitCooldown;
    *cooldown = 0.0f;
    uint32_t generic = *(volatile uint32_t*)(uintptr_t)kGenericHit;
    uint32_t own = PlayerCue(k, kCueHit);
    uint32_t cue = own ? (generic && AnnounceRoll() < 0.25f ? generic : own) : 0;
    if (cue) ((CommentRequestFn)(uintptr_t)kCommentRequest)(cue, 2500, 0);
    bool player = AnnouncedPlayer(k);
    if (player) NoteMention(k);
    ++g.hitSaid;
    if (g.hitLogged++ < 20)
        Log(false, L"race: announcer: car %d (%hs%s) took a hit: HIT about it (chance %.2f%s)", k, RacerName(k), player ? L", player" : L"", chance,
            !cue ? L"; it has no line" : cue == generic ? L"; the generic line" : L"");
}

// Host, every race tick: the cars spinning now (the cars this game drives: their flag, as the stock's test reads it; the other
// players' cars: their games' reports), each spin decided once; while the 45 s are up and the stock's gates allow a line, the
// pick. A spin that outlasts the cooldown gets its line when it is due, as in the stock.
void HostHitLines() {
    if (!FairLinesOn()) {  // (the stock speaks; the other players' reports meanwhile are dropped)
        for (int k = 0; k < kGridSlots; ++k)
            if (g.remote[k]) g.spinNow[k] = false;
        memset(g.spinDecided, 0, sizeof(g.spinDecided));
        return;
    }
    uint8_t count = 0;
    SceneRacerCount(count);
    for (int k = 0; k < Cars() && k < kGridSlots; ++k) {
        const uint8_t* racer = (const uint8_t*)g.racers[k];
        if (!racer) continue;
        if (g.remote[k]) {
            const Peer* p = PeerByIndex(k);
            if (!p || p->gone) g.spinNow[k] = false;
        } else {
            g.spinNow[k] = racer[kRacerSpinning] != 0 && *(const int32_t*)(racer + kRacerLife) == 2;
        }
        if (!g.spinNow[k]) g.spinDecided[k] = false;
    }
    volatile float* cooldown = (volatile float*)(uintptr_t)kHitCooldown;
    bool due = *cooldown >= kHitReady && *(volatile float*)(uintptr_t)kRaceSeconds >= kHitFrom && *(volatile uint8_t*)(uintptr_t)kCalloutsLive &&
               !*(volatile uint8_t*)(uintptr_t)kCommentOff;
    if (!due) return;
    int players[kGridSlots], np = 0, ai[kGridSlots], na = 0;
    for (int k = 0; k < Cars() && k < kGridSlots && k < count; ++k) {
        if (!g.racers[k] || !g.spinNow[k] || g.spinDecided[k]) continue;
        if (AnnouncedPlayer(k)) players[np++] = k;
        else ai[na++] = k;
    }
    if (!np && !na) return;
    for (int i = 1; i < np; ++i)  // the best placed player first (a)
        for (int j = i; j > 0 && HitPlace(players[j]) < HitPlace(players[j - 1]); --j) {
            int t = players[j]; players[j] = players[j - 1]; players[j - 1] = t;
        }
    float chance = 0.0f, roll = 0.0f;
    for (int i = 0; i < np; ++i) {
        int k = players[i];
        g.spinDecided[k] = true;
        chance = PassChance(k, true, HitPlace(k), false);  // 1.0 for the leader .. 0.35 for last place, times (b)
        roll = AnnounceRoll();
        if (roll < chance) {
            HitLine(k, chance);
            return;
        }
    }
    if (na) {
        g.spinDecided[ai[0]] = true;
        HitLine(ai[0], 1.0f);
        return;
    }
    if (g.hitLogged++ < 20)
        Log(false, L"race: announcer: car %d (%hs, player) took a hit: no line (chance %.2f, drew %.2f)", players[0], RacerName(players[0]), chance, roll);
}

// Guest, every race tick: this game's car's spin/flip flag changed: the host is told (spinning, or not any more).
void ReportHits() {
    if (!g.announceMuted || g.local < 0 || !g.racers[g.local]) return;
    bool now = ((const uint8_t*)g.racers[g.local])[kRacerSpinning] != 0;
    Peer* host = PeerByIndex(0);
    if (now != g.spinning && host && !host->gone) {
        RaceItemLine l = {g.c.race, kLineHit, (uint8_t)g.local, (uint8_t)(now ? 1 : 0)};
        uint8_t b[kWireMax];
        size_t n = EncodeItemLine(l, b, sizeof(b));
        if (n) RaceNetSend(host->id, b, n, true);
        if (now) ++g.hitReports;
    }
    g.spinning = now;
}

void ItemLines() {
    if (g.host) HostHitLines();
    else ReportHits();
}

// Host: another player's car was touched by car `other`'s All-Star (their own game's report): the stock's line about the
// All-Star user, through its own gates (not finished, live, near; its entry's 30 s).
void CallAllStarHit(int k, int other) {
    if (!FairLinesOn() || other < 0 || other >= kGridSlots || !g.racers[other] || k == other) return;
    ((AllStarHitCallFn)(uintptr_t)kAllStarHitCall)(RecordIndex(g.racers[other]));
    if (AnnouncedPlayer(other)) NoteMention(other);
    ++g.allStarHitSaid;
    if (g.allStarHitLogged++ < 10)
        Log(false, L"race: announcer: player %d (%hs) was hit by car %d (%hs)'s All-Star (their game's report): ALLSTM about car %d", k, RacerName(k), other,
            RacerName(other), other);
}

// Racer vtable 0x6afba4 slot 1 (0x4ee020): a message to a car's listener (racer+0x2c0; one argument, the message: +0 its kind,
// +8 its sender). Message 0x28, an All-Star touched the car: on the host, about a puppet, the stock's line is held with the
// game's own switch for the call (the trophy stat and the controller's message still run); on a guest, about its own car, the
// host is told who did it.
int __fastcall HookRacerListener(uint8_t* listener, void*, int32_t* message) {
    ListenerFn original = Original<ListenerFn>(kHRacerListener);
    if (!listener || !message || message[0] != kMsgHitByAllStar || g.phase != kRunning) return original(listener, message);
    int k = SlotOf(listener - kListenerInRacer);
    if (k < 0) return original(listener, message);
    if (g.host && g.remote[k] && FairLinesOn()) {
        volatile uint8_t* off = (volatile uint8_t*)(uintptr_t)kCommentOff;
        uint8_t was = *off;
        *off = 1;
        int result = original(listener, message);
        *off = was;
        ++g.allStarHitMuted;
        return result;
    }
    if (!g.host && k == g.local && g.announceMuted) {
        int other = SlotOf((const void*)(uintptr_t)message[2]);
        Peer* host = PeerByIndex(0);
        if (other >= 0 && other != k && host && !host->gone) {
            RaceItemLine l = {g.c.race, kLineAllStarHit, (uint8_t)k, (uint8_t)other};
            uint8_t b[kWireMax];
            size_t n = EncodeItemLine(l, b, sizeof(b));
            if (n) RaceNetSend(host->id, b, n, true);
            ++g.hitReports;
        }
    }
    return original(listener, message);
}

void LogItemLines() {
    if (!g_lineHooks || !(g.c.flags & kContractAnnouncer)) return;
    if (g.host)
        Log(false, L"race: announcer: hit lines: %d HIT said, %d reports from the other players, %d ALLSTM said for them, %d of this screen's ALLSTM "
            L"about them held", g.hitSaid, g.hitReports, g.allStarHitSaid, g.allStarHitMuted);
    else if (g.hitReports) Log(false, L"race: announcer: hit lines: %d of this car's hits reported to the host", g.hitReports);
}

// ---- driver voices (re\m7-vo-triggers\FINDINGS.md): the mirror's first step, log only. Five kinds of driver line fire only for
// a game's own player (drift and stunt POS, respot NEG, the intro, the 4th-or-worse finish NEG), hits from each game's own view of
// them: so each car's own game must decide its lines (as its items). Here every game counts each car's lines from their
// cooldowns, once per frame, and whether each reached its speech queue (a 3D line farther than 2000 m from the listener doesn't),
// to compare the screens.
const wchar_t* const kVoiceKindNames[kVoiceKinds] = {L"POS", L"NEG", L"RCE_WON", L"RCE_LST", L"ATTACK", L"ALLSTAR", L"INTRO"};

bool VoiceQueued(const void* sounder) {  // a record in the speech queue has this sounder (a driver line's is its group)
    uint8_t* queue = *(uint8_t**)(uintptr_t)kVoiceQueue;
    uint8_t** slots = queue ? *(uint8_t***)(queue + kQueueSlots) : nullptr;
    int count = queue ? *(uint16_t*)(queue + kVoiceCount) : 0, cap = queue ? *(uint16_t*)(queue + kQueueCap) : 0;
    for (int i = 0; slots && i < count && i < cap && i < 16; ++i)
        if (slots[i] && *(void**)slots[i] == sounder) return true;
    return false;
}

const wchar_t* VoiceOwner(int k) {
    if (SendsItems(k)) return k == g.local ? L"this game's car" : L"this game's AI";
    return k < g.c.humanCount ? L"another player's car" : L"the host's AI";
}

void LogVoiceGroups(int k, const uint8_t* voice) {  // once per race: the groups' pick, for the mirror's forced takes
    wchar_t text[256] = L"";
    for (int i = 0; i < kVoiceKinds; ++i) {
        const uint8_t* s = *(const uint8_t* const*)(voice + kVoiceGroups + i * 4);
        size_t at = wcslen(text);
        if (s) _snwprintf_s(text + at, 256 - at, _TRUNCATE, L"%s%s %d takes%s", at ? L", " : L"", kVoiceKindNames[i],
                            *(const int32_t*)(s + kGroupTakeCount), s[kGroupRandom] == 1 && s[kGroupShuffle] == 1 ? L"" : L" (not a shuffle)");
        else _snwprintf_s(text + at, 256 - at, _TRUNCATE, L"%s%s none", at ? L", " : L"", kVoiceKindNames[i]);
    }
    Log(false, L"race: voices: car %d (%hs) has %s", k, RacerName(k), text);
}

// The players' racers' lines on every screen (user, 2026-10-22: every player's racer shouts as often as the host's own; each
// player's intro line heard by everyone). Five kinds fire only for a game's own player, hits from each game's own view: so
// each player's game decides its racer's lines and sends them (Voice); the other games mute that racer's own lines (its
// cooldowns held at 0, below the triggers' 20 s gate) and say the sent ones through the game's own line function (0x54d0a0:
// 3D at the car as this screen shows it, or 2D). The podium's lines (RCE_WON, RCE_LST) stay each screen's own; the computer
// racers' lines already follow the items and places every screen shares. Needs another real player in the race.
bool VoicesShared() {
    if (!g_announcerHooks) return false;
    for (int i = 0; i < g.peerCount; ++i)
        if (!g.peers[i].gone && !g.peers[i].test) return true;
    return false;
}

bool VoiceMutedHere(int k) {  // another real player's racer, still theirs: its lines come from their game
    if (k == g.local || k < 0 || k >= g.c.humanCount || k >= kGridSlots || g.adopted[k]) return false;
    const Peer* p = PeerByIndex(k);
    return p && !p->gone && !p->test;
}

int32_t QueuedWait(const void* sounder, uint32_t kind) {  // the line's wait as queued (the update hasn't counted it down yet)
    uint8_t* queue = *(uint8_t**)(uintptr_t)kVoiceQueue;
    uint8_t** slots = queue ? *(uint8_t***)(queue + kQueueSlots) : nullptr;
    int count = queue ? *(uint16_t*)(queue + kVoiceCount) : 0, cap = queue ? *(uint16_t*)(queue + kQueueCap) : 0;
    for (int i = count - 1; slots && i >= 0 && i < cap && i < 16; --i)
        if (slots[i] && *(void**)slots[i] == sounder) return *(int32_t*)(slots[i] + 4);
    return kind == 5 ? 10000 : 3000;  // (not queued here: the trigger's own wait)
}

void SendVoiceLine(int k, uint32_t kind, const uint8_t* group) {
    RaceVoice v = {g.c.race, (uint8_t)k, (uint8_t)kind, (uint8_t)(group && group[kGroupMode] ? 1 : 0), 0};
    int32_t wait = QueuedWait(group, kind);
    v.wait = (uint16_t)(wait < 0 ? 0 : wait > 30000 ? 30000 : wait);
    uint8_t b[kWireMax];
    size_t n = EncodeVoice(v, b, sizeof(b));
    if (!n) return;
    SendToAll(b, n, true);
    ++g.voiceSent;
    if (g.voiceMirrorLogged++ < 40) Log(false, L"race: voices: sent: car %d (%hs) %s (%s, wait %u ms)", k, RacerName(k), kVoiceKindNames[kind],
                                         v.mode ? L"3D" : L"2D", v.wait);
}

typedef void(__thiscall* DriverLineFn)(void* voice, uint32_t kind, uint8_t mode3d, int32_t waitMs);
typedef int32_t(__thiscall* GroupModeFn)(void* group, uint8_t mode3d);
typedef int32_t(__thiscall* GroupPlayFn)(void* group);

// An intro line said at once, straight, not through the speech queue (user, 2026-10-22: with everyone pressing continue, every
// screen "bombarded with 8 racer intro lines ... please keep it that way"): the lines play together, over each other, and the
// countdown's enter (it only empties the queue) doesn't cut them. 2D, as the stock intro line.
bool SayIntroNow(uint8_t* voice, int32_t* result) {
    uint8_t* group = voice ? *(uint8_t**)(voice + kVoiceGroups + kIntroKind * 4) : nullptr;
    const uintptr_t* vt = group ? *(const uintptr_t* const*)group : nullptr;
    *result = -1;
    if (!vt || vt[kGroupPlaySlot] != kGroupPlay) return false;  // (not a sound group as this exe has it)
    ((GroupModeFn)(uintptr_t)kGroupSetMode)(group, 0);
    group[kGroupStarting] = 1;
    *result = ((GroupPlayFn)(uintptr_t)kGroupPlay)(group);
    return true;
}

// Car k's intro line waits here until the sound system has a stream to spare. It plays only a few streams at once (in game about
// 3: a further Play returns 1, its child's start refused), shared with the music, the announcer and every other voice (the
// independent check of m7zb/m7zc): so the lines start right after the speech queue's update (this game's own lines first) and
// only while two of the group's manager's streamers are free (one stays for the game: the countdown's voice, the race music); a
// refused one is tried again, for up to 10 s; every one still waiting is dropped when the countdown is released.
const uint32_t kGroupOwner = 0x78;  // a sound's manager (0x476cb0, 0x470fa0): +0x6c its streamers, +0x70 their count
const uint32_t kStreamers = 0x6c, kStreamerCount = 0x70, kStreamerUsed = 0x7c;  // a streamer's +0x7c: nonzero while in use (0x474430)
const uint32_t kIntroWaitTicks = 500;

int FreeStreamers(const uint8_t* group) {
    const uint8_t* mgr = group ? *(const uint8_t* const*)(group + kGroupOwner) : nullptr;
    const uint8_t* const* list = mgr ? *(const uint8_t* const* const*)(mgr + kStreamers) : nullptr;
    int n = mgr ? *(const int32_t*)(mgr + kStreamerCount) : 0, free = 0;
    if (!list || n <= 0 || n > 64) return 0;
    for (int i = 0; i < n; ++i)
        if (list[i] && *(const uint32_t*)(list[i] + kStreamerUsed) == 0) ++free;
    return free;
}

void QueueIntro(int k) {
    if (k < 0 || k >= kGridSlots || g.introRetry[k]) return;
    g.introRetry[k] = true;
    g.introRetrySince[k] = SimTick();
    g.introTries[k] = 0;
}

void StartIntros() {  // right after the speech queue's update
    int waiting = 0;
    for (int k = 0; k < kGridSlots; ++k) waiting += g.introRetry[k] ? 1 : 0;
    if (!waiting) return;
    if (g.phase >= kRunning) {
        memset(g.introRetry, 0, sizeof(g.introRetry));
        g.voiceUnheard += waiting;
        Log(false, L"race: voices: %d intro line(s) not started before the countdown (dropped)", waiting);
        return;
    }
    for (int k = 0; k < kGridSlots; ++k) {
        if (!g.introRetry[k]) continue;
        uint8_t* voice = g.racers[k] ? *(uint8_t**)((uint8_t*)g.racers[k] + kRacerVoice) : nullptr;
        const uint8_t* group = voice ? *(const uint8_t* const*)(voice + kVoiceGroups + kIntroKind * 4) : nullptr;
        bool late = SimTick() - g.introRetrySince[k] > kIntroWaitTicks;
        if (!late && group && FreeStreamers(group) < 2) continue;
        int32_t result = -1;
        bool sound = !late && SayIntroNow(voice, &result);
        if (sound) ++g.introTries[k];
        if (sound && result != 0) continue;  // (refused: again later)
        g.introRetry[k] = false;
        if (sound) ++g.voicePlayed;
        else ++g.voiceUnheard;
        if (g.voiceMirrorLogged++ < 40)
            Log(false, L"race: voices: car %d (%hs)'s intro line %s", k, RacerName(k),
                sound ? (g.introTries[k] > 1 ? L"started (after a refusal)" : L"started") : late ? L"not started in 10 s (dropped)"
                : L"not a sound group here (dropped)");
    }
}

// Host: its test players' racers say their intro lines at its own racer intro (as if they had just arrived; they are always
// ready), here and on every screen.
void TestPlayerIntros() {
    if (!g.host || !g_announcerHooks) return;
    for (int i = 0; i < g.peerCount; ++i) {
        const Peer& p = g.peers[i];
        if (!p.test || p.gone || p.index >= kGridSlots || !g.racers[p.index]) continue;
        QueueIntro(p.index);
        RaceVoice v = {g.c.race, (uint8_t)p.index, (uint8_t)kIntroKind, 0, 3000};
        uint8_t b[kWireMax];
        size_t n = VoicesShared() ? EncodeVoice(v, b, sizeof(b)) : 0;
        if (n) SendToAll(b, n, true);
        Log(false, L"race: voices: test player car %d (%hs) says its intro line%s", p.index, RacerName(p.index), n ? L"; sent" : L"");
    }
}

// The other players' lines, said here in order. An intro line waits until this game's racer intro has begun (a player still
// at the course intro hears the others' intro lines at their racer intro, all at once) and is played straight; the others go
// through the game's line function and the queue.
void PlayVoiceLines() {
    int kept = 0;
    for (int i = 0; i < g.voiceInCount; ++i) {
        const RaceVoice& v = g.voiceIn[i];
        uint8_t* voice = v.slot < kGridSlots && g.racers[v.slot] ? *(uint8_t**)((uint8_t*)g.racers[v.slot] + kRacerVoice) : nullptr;
        bool intro = v.kind == kIntroKind;
        const Peer* subject = PeerByIndex(v.slot);
        bool testIntro = intro && subject && subject->test && !subject->gone;
        if (voice && intro && g.phase <= kIntros && !g.charIntroRan && SimTick() - g.voiceInTick[i] < kIntroHoldTicks) {
            g.voiceIn[kept] = v;
            g.voiceInTick[kept++] = g.voiceInTick[i];
            continue;
        }
        if (!voice || !(VoiceMutedHere(v.slot) || testIntro) || (intro && g.phase <= kIntros && !g.charIntroRan)) {
            ++g.voiceDropped;
            continue;
        }
        if (intro) {  // (started straight after the queue's update: StartIntros)
            QueueIntro(v.slot);
            continue;
        }
        float* cool = (float*)(voice + kVoiceCooldowns);
        const void* group = *(const void* const*)(voice + kVoiceGroups + v.kind * 4);
        cool[v.kind] = kVoiceReady;  // (held at 0 here: open the gate for this one line)
        int32_t wait = v.wait;
        ((DriverLineFn)(uintptr_t)kVoiceLine)(voice, v.kind, v.mode, wait);
        cool[v.kind] = 0.0f;
        bool queued = group && VoiceQueued(group);
        if (queued) ++g.voicePlayed;
        else ++g.voiceUnheard;
        if (g.voiceMirrorLogged++ < 40)
            Log(false, L"race: voices: said the owner's: car %d (%hs) %s%s", v.slot, RacerName(v.slot), kVoiceKindNames[v.kind],
                queued ? L"" : L" (not queued here: out of range, or the queue was full)");
    }
    g.voiceInCount = kept;
}

void CountVoices() {
    if (g.phase < kSetUp || g.phase > kRunning) return;
    bool shared = VoicesShared();
    for (int k = 0; k < Cars(); ++k) {
        uint8_t* voice = g.racers[k] ? *(uint8_t**)((uint8_t*)g.racers[k] + kRacerVoice) : nullptr;
        if (!voice) continue;
        float* cool = (float*)(voice + kVoiceCooldowns);
        if (!g.voiceSeen[k]) LogVoiceGroups(k, voice);
        if (shared && VoiceMutedHere(k)) {  // (before the look below: a muted racer counts nothing here)
            for (uint8_t i = 0; i < kVoiceKinds; ++i)
                if (VoiceKindShared(i)) cool[i] = 0.0f;
            if (!g.voiceMuteLogged) Log(false, L"race: voices: the other players' racers say their own games' lines here");
            g.voiceMuteLogged = true;
        }
        for (int i = 0; i < kVoiceKinds; ++i) {
            float c = cool[i];
            bool fired = g.voiceSeen[k] && c < g.voiceCool[k][i] && c < kVoiceFresh;
            g.voiceCool[k][i] = c;
            if (!fired) continue;
            const uint8_t* s = *(const uint8_t* const*)(voice + kVoiceGroups + i * 4);
            bool queued = s && VoiceQueued(s);
            ++g.voiceLines[k][i];
            if (queued) ++g.voiceQueued[k][i];
            int& logged = k < g.c.humanCount ? g.voiceLoggedHuman : g.voiceLogged;
            if (logged++ < 40)
                Log(false, L"race: voices: car %d (%hs, %s) %s%s", k, RacerName(k), VoiceOwner(k), kVoiceKindNames[i],
                    queued ? L"" : L" (not queued here: out of range, or the queue was full)");
            if (shared && k == g.local && VoiceKindShared((uint8_t)i)) SendVoiceLine(k, (uint32_t)i, s);
        }
        g.voiceSeen[k] = true;
    }
    if (g.voiceInCount) PlayVoiceLines();
}

void LogVoices() {
    int own = 0, others = 0;
    for (int k = 0; k < Cars(); ++k) {
        wchar_t text[256] = L"";
        int n = 0;
        for (int i = 0; i < kVoiceKinds; ++i) {
            if (!g.voiceLines[k][i]) continue;
            n += g.voiceLines[k][i];
            size_t at = wcslen(text);
            _snwprintf_s(text + at, 256 - at, _TRUNCATE, L"%s%s %u (%u queued)", at ? L", " : L"", kVoiceKindNames[i], g.voiceLines[k][i],
                         g.voiceQueued[k][i]);
        }
        (SendsItems(k) ? own : others) += n;
        if (n) Log(false, L"race: voices: car %d (%hs, %s): %s", k, RacerName(k), VoiceOwner(k), text);
    }
    Log(false, L"race: voices: %d lines of this game's cars, %d of the others' (counted here)", own, others);
    if (g.voiceSent || g.voicePlayed || g.voiceUnheard || g.voiceDropped)
        Log(false, L"race: voices: this player's racer's lines: %d sent; the other players': %d said here, %d not queued here, %d dropped",
            g.voiceSent, g.voicePlayed, g.voiceUnheard, g.voiceDropped);
}
}  // namespace

// The results rows' name (M6, re\m6-race-hud\FINDINGS.md (c)): right after 0912 builds a row, `results`
// 0x425b prints the row's character name into its text buffer (1612(g1f[k], row[1]): 0x5fd200, sprintf "%s"
// into the buffer's +0xc, no bound). A human's row gets their Steam name instead, as on the console; AI rows
// and every other 1612 keep the game's text.
const uint8_t kResultsNameCode[] = {0x07, 0x06, 0x51, 0x01, 0x23, 0x3b, 0x32, 0x02, 0x12, 0x16, 0x00, 0x00, 0x9f, 0x09, 0x18};  // from PC - 14
const ScriptSiteDef kResultsName = {"results row name (1612)", kScriptResults, -14, kResultsNameCode, sizeof(kResultsNameCode),
                                    {0x425b + kResultsInsert + 8, 0x425b + 8}};

static bool ResultsRowName(void* vm, NativeCall* d) {
    if (g.phase != kRunning || !RaceOver() || g.rowHuman < 0 || g.rowHuman >= g.c.humanCount || !g.names[g.rowHuman][0]) return false;
    if (d->argc != 2 || d->types[0] != kTypeObject || !ScriptSite(vm, kResultsName)) return false;
    TextBuffer* out = (TextBuffer*)(uintptr_t)d->values[0];
    if (!out || out->vtable != kTextBufferVtable || out->capacity < 2 || !out->text) {
        if (!g.rowNameLogged) Log(true, L"race: a results row's name box was not recognised; the rows show the racers' names");
        g.rowNameLogged = true;
        return false;
    }
    TagText(g.names[g.rowHuman], out->text, (size_t)out->capacity < sizeof(g.names[0]) ? (size_t)out->capacity : sizeof(g.names[0]));
    if (!g.rowNameLogged) Log(false, L"race: the results rows show the players' names");
    g.rowNameLogged = true;
    return true;
}

// The pause menu's title "PAUSED nP" (`pause_menu` 0x0bec: 0f0f(car) = that car's human number +0x9dc,
// 0x58fb17). Each game numbers only its own player (0), so a guest read "PAUSED 1P"; for that one call the
// car carries its lobby slot (host 1P, guest 2P), as on the console. Put back in RaceNativeAfter.
const uint8_t kPauseTitleCode[] = {0x0b, 0x01, 0x3a, 0x32, 0x01, 0x0f, 0x0f, 0x01, 0x00, 0x0b, 0x02, 0x51, 0x00, 0x27};  // from PC - 11
const ScriptSiteDef kPauseTitle = {"pause title (0f0f)", kScriptPauseMenu, -11, kPauseTitleCode, sizeof(kPauseTitleCode), {0x0bec + 8}};

static void PauseTitleBefore(void* vm, NativeCall* d) {
    g.titleRacer = -1;
    if (g.phase != kRunning || d->argc != 1 || !IntOrHash(d, 0) || d->values[0] >= (uint32_t)g.c.humanCount || d->values[0] >= kMaxHumans) return;
    int k = (int)d->values[0];
    if (!g.racers[k] || !ScriptSite(vm, kPauseTitle)) return;
    int32_t* number = (int32_t*)((uint8_t*)g.racers[k] + kRacerHumanNumber);
    g.titleSaved = *number;
    g.titleRacer = k;
    *number = k;
    if (!g.titleLogged) Log(false, L"race: the pause menu's title shows player %d as %dP", k, k + 1);
    g.titleLogged = true;
}

// The pause menu's EXIT / RETURN TO LOBBY question (re\m8-audit A1): on the consoles the host of an online race was asked "ARE
// YOU SURE YOU WANT TO QUIT? THIS WILL END THE SESSION FOR OTHER PLAYERS." (box AAC0CD27, PS3 pause_menu 0x1300); the PC picks
// D5E3E174 ("...END THE CURRENT RACE") or 21C7CA32, though the box and its texts (every language) are in the PC data. The box
// function (pause_menu 0x1676) writes its body key into two text layers (0x16d5, 0x16e6): for the host with other players still
// in the race, those two calls get the console's key. The buttons are the same (EXIT / BACK).
const uint8_t kQuitBoxTextCode[2][16] = {{0x02, 0x77, 0xa4, 0xc1, 0xf1, 0x0b, 0x01, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x01, 0x00, 0xc4, 0x03},
                                         {0x02, 0xe2, 0x82, 0x75, 0x37, 0x0b, 0x01, 0x3c, 0x32, 0x02, 0x1f, 0x02, 0x01, 0x00, 0xc5, 0x03}};  // from PC - 16
const ScriptSiteDef kQuitBoxText[2] = {{"pause quit box text a (021f)", kScriptPauseMenu, -16, kQuitBoxTextCode[0], 16, {0x16d5 + 8}},
                                       {"pause quit box text b (021f)", kScriptPauseMenu, -16, kQuitBoxTextCode[1], 16, {0x16e6 + 8}}};
const uint32_t kQuitRaceKey = 0xd5e3e174, kQuitProgressKey = 0x21c7ca32, kQuitSessionKey = 0xaac0cd27;

static void QuitBoxTextBefore(void* vm, NativeCall* d) {
    if (g.phase != kRunning || !g.host || d->argc != 2 || d->types[1] != kTypeHash) return;
    if (d->values[1] != kQuitRaceKey && d->values[1] != kQuitProgressKey) return;
    bool others = false;
    for (int i = 0; i < g.peerCount; ++i) others |= !g.peers[i].gone && !g.peers[i].test;
    if (!others) return;
    if (!ScriptSite(vm, kQuitBoxText[0]) && !ScriptSite(vm, kQuitBoxText[1])) return;
    d->values[1] = kQuitSessionKey;
    if (!g.quitBoxLogged) Log(false, L"race: the pause menu's quit question says it ends the session for the other players (the host's, as on the console)");
    g.quitBoxLogged = true;
}

static void PauseTitleAfter() {
    if (g.titleRacer < 0) return;
    if (g.racers[g.titleRacer]) *(int32_t*)((uint8_t*)g.racers[g.titleRacer] + kRacerHumanNumber) = g.titleSaved;
    g.titleRacer = -1;
}

// The loading screen's tips (re\m6-loading\FINDINGS.md 1): fe_game_bridge 0x09ce picks the first of its 4 tips
// with 1d02 (the shared MT19937) before SetupRace reseeds it, so each PC showed its own. For our race that one
// draw starts from the contract's seed: the same tips on every screen. Stock 0x09de, Roster Pack 0x0a80, Metal 1.1.2 0x0a05.
const uint8_t kTipsCode[] = {0x55, 0x00, 0x07, 0x03, 0x3a, 0x32, 0x02, 0x02, 0x1d, 0x01, 0x00, 0xc4, 0x00, 0x57, 0x16, 0x07, 0x02, 0x23};  // from PC - 13
const ScriptSiteDef kTips = {"loading tips draw (1d02)", kScriptBridge, -13, kTipsCode, sizeof(kTipsCode), {0x09de + 8, 0x0a80 + 8, 0x0a05 + 8}};
const uint32_t kTipsSalt = 0x54495053;  // "TIPS": not the grid's own seed

static void LoadingTipsBefore(void* vm) {
    if (!g.launched || g.phase < kArmed || g.phase >= kSetUp) return;
    if (!ScriptSite(vm, kTips)) return;
    ((SeedFn)(uintptr_t)kSeedRng)(g.c.seed ^ kTipsSalt);
    if (!g.tipsLogged) Log(false, L"race: the loading screen's tips come from the race's seed (the same on every screen)");
    g.tipsLogged = true;
}

// A Steam Join Game / invite while racing (the user's rule 5; re\m6-join\FINDINGS.md 3): leave the race the
// way the pause menu's EXIT does (its clean-ups, then QUIT_GAME to the game module), and front_main then opens
// the online search, which takes the join. Checked once per frame at the game module's inbox read (160e(1),
// game +0x048e), only with a join waiting, after a license was picked, in a race with a player on the track
// (not the attract demo), not paused, on the race or the results' retry/quit screen (never during the save).
GAME_ADDRESS kPostMessage = 0x4bb220;  // cdecl(sender, target, msg, const value*, 0) -> al: queued (as native 160c)
GAME_ADDRESS kClearGrandPrix = 0x5fab70, kClearMission = 0x5fa0f0, kClear093f = 0x58e820;  // natives 2116, 210a, 093f: no arguments
GAME_ADDRESS kBlackboard = 0x6eca14, kBlackboardGet = 0x4a4030;  // thiscall(board, key) -> {value, type}* ({0, 2} if missing)
GAME_ADDRESS kWorldPaused = 0xc2a015;  // byte
GAME_ADDRESS kProfiles = 0xc2a520;     // 0x3af0 per license; +0x14 "empty slot" byte (2003(21), 0x545ec0)
const uint32_t kActiveScreenKey = 0xb0567d36, kScreenGame = 0xbed107e7, kScreenRetryQuit = 0x603658c4;
const uint32_t kGameModule = 0xbed107e7, kQuitGame = 0x682e3dfe;
const uint8_t kGameInboxCode[] = {0x51, 0x01, 0x32, 0x01, 0x0e, 0x16, 0x02, 0x00, 0x2f, 0x00};  // from PC - 10
const ScriptSiteDef kGameInbox = {"game inbox read (160e)", kScriptGame, -10, kGameInboxCode, sizeof(kGameInboxCode), {0x048e + 8}};
struct ScriptValue { uint32_t value, type; };
typedef const ScriptValue*(__thiscall* BlackboardGetFn)(uintptr_t board, uint32_t key);
typedef bool(__cdecl* PostMessageFn)(uint32_t sender, uint32_t target, uint32_t msg, const ScriptValue* payload, uint32_t zero);
typedef void(__cdecl* PlainFn)();

static void NoteLicense(const NativeCall* d) {  // 2004(20, license): the license select's pick; 2004(21, ..): a new one made
    if (d->argc >= 1 && IntOrHash(d, 0) && d->values[0] == 21) {
        if (!g_licensePicked) Log(false, L"menus: a new license was made (a Steam join can now leave a race)");
        g_licensePicked = true;
        return;
    }
    if (d->argc < 2 || !IntOrHash(d, 0) || !IntOrHash(d, 1) || d->values[0] != 20 || d->values[1] > 3) return;
    bool used = *(volatile uint8_t*)(kProfiles + d->values[1] * 0x3af0 + 0x14) == 0;
    if (used && !g_licensePicked) Log(false, L"menus: license %u picked (a Steam join can now leave a race)", d->values[1] + 1);
    g_licensePicked = g_licensePicked || used;
}

static uint32_t ActiveScreen() { return ((BlackboardGetFn)(uintptr_t)kBlackboardGet)(kBlackboard, kActiveScreenKey)->value; }

// Leaves the race the way the pause menu's EXIT does: QUIT_GAME to the game module (payload 1 from the results' retry/quit
// screen, as the results screen quits: it has saved), then its clean-ups. False: the message queue was full (next frame).
// Once per online race (a Steam join and a session message could both ask in one frame).
static uint32_t g_quitRace = 0;
static bool QuitRace(uint32_t screen) {
    if (Active() && g.c.race && g_quitRace == g.c.race) return true;
    ScriptValue payload = {screen == kScreenRetryQuit ? 1u : 0u, kTypeInt};
    if (!((PostMessageFn)(uintptr_t)kPostMessage)(0, kGameModule, kQuitGame, &payload, 0)) return false;
    ((PlainFn)(uintptr_t)kClearGrandPrix)();  // the pause menu's EXIT clean-ups (the game module quits on its next frame)
    ((PlainFn)(uintptr_t)kClearMission)();
    ((PlainFn)(uintptr_t)kClear093f)();
    if (Active()) g_quitRace = g.c.race;
    return true;
}

static void LeaveRaceForJoin(void* vm) {
    if (!g_licensePicked || !ScriptSite(vm, kGameInbox)) return;
    LobbySnapshot s;
    LobbyRead(s);
    if (!s.pendingJoin || s.state == kLobbyOff || s.pendingJoin == g_joinQuitFor) return;
    if (*(volatile uint8_t*)(uintptr_t)kWorldPaused || g_pauseOpen) return;
    uint32_t screen = ActiveScreen();
    bool racing = screen == kScreenGame && SimTick() - g_raceUpdateTick <= 25 && !RaceOver();  // (not the race end's handover)
    if (!racing && screen != kScreenRetryQuit) return;
    uint8_t* scene = *(uint8_t**)(uintptr_t)kScene;
    if (!scene || *(uint32_t*)(scene + 0x1104) < 1) return;  // nobody on the track: the attract demo
    if (!QuitRace(screen)) return;  // queue full: next frame
    g_joinQuitFor = s.pendingJoin;
    Log(true, L"race: leaving the race for a Steam join of lobby %llu%s", s.pendingJoin, Active() ? L" (counts as quitting the online race)" : L"");
}

// The console's session messages (M7, re\m7-messages\FINDINGS.md). Mid-race, when the last other player is gone (host)
// or the host is gone (guest), the console's own dialog shows over the running race, as on the console: "The connection
// to the other player(s) has been lost. The match cannot continue." with RETURN TO LOBBY, "The connection to the host
// has been lost...", or "The host has ended the match. Returning to the lobby." when the host quit the race but is
// still in the lobby. The dialog is posted to the `dialogs` script module the way the exe posts its achievement popup
// (an array [definition, none, none]); the OK is seen at the dialogs module's close site, and the race is left the way
// the pause menu's EXIT does; front_main then returns to the lobby, or to the game search when the lobby is gone.
namespace {
GAME_ADDRESS kArrayHelper = 0xc28b20;   // the script array helper's `this`; +4 the array type id (registered at run time)
GAME_ADDRESS kArrayCreate = 0x48ed30;   // thiscall(helper, ScriptValue* out, count), ret 8 -> al
GAME_ADDRESS kArraySet = 0x48cb00;      // thiscall(helper, ScriptValue* array, index, const ScriptValue* element), ret 0xc (addref)
GAME_ADDRESS kValueRelease = 0x48c8b0;  // thiscall(ScriptValue*): releases our reference
GAME_ADDRESS kNoneValue = 0xc28b34;     // the game's canonical none value {0, 0}
const uint32_t kDialogsModule = 0x694fe697, kDialogShow = 0xdfea0f93, kDialogClose = 0x3cb781ef, kDialogScene = 0x424b961f;
const uint32_t kDialogPlayersLeft = 0xcb5d217e;  // (edited by tools\build_menus.py) the other player(s) lost; RETURN TO LOBBY
const uint32_t kDialogHostLost = 0x8374a07b;     // (edited) the connection to the host lost, the session no longer available; OK
const uint32_t kDialogHostEnded = 0x168056c7;    // the host has ended the match, returning to the lobby; OK
// The dialogs module's OK path: HASH 424b961f; 0206/1 (the scene's hide). Our shipped module (Preview5) and the stock one.
const uint8_t kDialogCloseCode[] = {0x02, 0x1f, 0x96, 0x4b, 0x42, 0x32, 0x01, 0x06, 0x02, 0x01, 0x00, 0x5e, 0x01};  // from PC - 13
const ScriptSiteDef kDialogCloseSite = {"dialogs OK path (0206)", kScriptDialogs, -13, kDialogCloseCode, sizeof(kDialogCloseCode),
                                        {0x749 + 8, 0x72d + 8}};  // ours, the stock's
const DWORD kDialogMaxOpenMs = 300000;  // an answer not seen in 5 min (the dialog was replaced or closed another way): forgotten
typedef bool(__fastcall* ArrayCreateFn)(uintptr_t helper, void*, ScriptValue* out, uint32_t count);
typedef void(__fastcall* ArraySetFn)(uintptr_t helper, void*, ScriptValue* array, uint32_t index, const ScriptValue* element);
typedef void(__fastcall* ValueReleaseFn)(ScriptValue* value, void*);

struct NetDialog {
    uint32_t race = 0;       // the race it belongs to (one per race)
    uint32_t def = 0;
    bool pending = false;    // to be posted (retried every frame until the post is taken)
    bool open = false;       // posted; its OK not seen yet (an OK after the forget time no longer counts)
    bool shown = false;      // posted; closed with the race if still up
    bool answered = false;   // the player's OK closed it
    bool quitPending = false;  // answered; the quit message not taken yet
    bool closePending = false;  // the close message not taken yet (retried from the menus' poll too)
    DWORD openedAt = 0;
} g_dialog;

bool PostDialogClose() {
    const ScriptValue* none = (const ScriptValue*)(uintptr_t)kNoneValue;
    return ((PostMessageFn)(uintptr_t)kPostMessage)(0, kDialogsModule, kDialogClose, none, 0);
}

const wchar_t* DialogName(uint32_t def) {
    return def == kDialogPlayersLeft ? L"the other player(s) lost" : def == kDialogHostLost ? L"the host lost" : L"the host ended the match";
}

bool PostDialog(uint32_t def) {
    ScriptValue arr = {0, 0};
    if (!((ArrayCreateFn)(uintptr_t)kArrayCreate)(kArrayHelper, nullptr, &arr, 3)) return false;
    ScriptValue definition = {def, kTypeHash};
    const ScriptValue* none = (const ScriptValue*)(uintptr_t)kNoneValue;
    ((ArraySetFn)(uintptr_t)kArraySet)(kArrayHelper, nullptr, &arr, 0, &definition);
    ((ArraySetFn)(uintptr_t)kArraySet)(kArrayHelper, nullptr, &arr, 1, none);
    ((ArraySetFn)(uintptr_t)kArraySet)(kArrayHelper, nullptr, &arr, 2, none);
    bool ok = ((PostMessageFn)(uintptr_t)kPostMessage)(0, kDialogsModule, kDialogShow, &arr, 0);
    ((ValueReleaseFn)(uintptr_t)kValueRelease)(&arr, nullptr);
    return ok;
}

// Pump: mid-race (the countdown on, the race not over), once per race.
void CheckSessionEnd(const LobbySnapshot& s) {
    if (g.phase != kRunning || RaceOver() || g_dialog.race == g.c.race || g.peerCount == 0) return;
    uint32_t def = 0;
    if (g.host) {
        bool all = true, real = false;  // every other (real) player gone; test players don't keep a session going
        for (int i = 0; i < g.peerCount; ++i) {
            real = real || !g.peers[i].test;
            all = all && (g.peers[i].gone || g.peers[i].test);
        }
        if (all && real) def = kDialogPlayersLeft;
    } else if (Peer* h = PeerByIndex(0)) {
        if (h->gone) {
            bool inLobby = false;  // quit the race but still in the lobby: the host ended the match
            for (int j = 0; s.state == kLobbyInLobby && j < s.memberCount; ++j) inLobby = inLobby || s.members[j].id == h->id;
            def = h->abortReason == kAbortQuit && inLobby ? kDialogHostEnded : kDialogHostLost;
        }
    }
    if (!def) return;
    g_dialog = NetDialog();
    g_dialog.race = g.c.race;
    g_dialog.def = def;
    g_dialog.pending = true;
    Log(true, L"race: session: %s; the console's message is shown, and the race is left when it is answered", DialogName(def));
}

// Every frame at the game module's inbox read: post a pending dialog (not while paused, not once the race is over),
// retry a pending quit (not while paused; on the game screen only while the race runs, else from the results'
// retry/quit screen, as a Steam join does), retry a pending close, forget a dialog whose answer never comes.
void NetDialogFrame() {
    if (g_dialog.closePending && PostDialogClose()) g_dialog.closePending = false;
    if (g_dialog.open && GetTickCount() - g_dialog.openedAt > kDialogMaxOpenMs) {
        g_dialog.open = false;
        Log(false, L"race: session: the message was not answered in 5 minutes; the race goes on");
    }
    bool paused = *(volatile uint8_t*)(uintptr_t)kWorldPaused || g_pauseOpen;
    if (g_dialog.quitPending) {
        if (paused) return;
        uint32_t screen = ActiveScreen();
        bool racing = screen == kScreenGame && !RaceOver() && SimTick() - g_raceUpdateTick <= 25;  // (not the race end's handover)
        if ((racing || screen == kScreenRetryQuit) && QuitRace(screen)) {
            g_dialog.quitPending = false;
            Log(true, L"race: session: the message was answered; leaving the race");
        }
        return;
    }
    if (!g_dialog.pending) return;
    if (g_dialog.race != g.c.race || g.phase != kRunning || RaceOver()) { g_dialog.pending = false; return; }
    if (paused) return;
    if (!PostDialog(g_dialog.def)) return;  // the message queue is full: next frame
    g_dialog.pending = false;
    g_dialog.open = g_dialog.shown = true;
    g_dialog.openedAt = GetTickCount();
    Log(false, L"race: session: the message is up (%s)", DialogName(g_dialog.def));
}

void RetryDialogClose() {
    if (g_dialog.closePending && PostDialogClose()) g_dialog.closePending = false;
}

// 1.1.1, the auto-update's notices (re\update-dialogs\FINDINGS.md): two definitions nothing else posts, their bodies rewritten
// (tools\build_menus.py DIALOG_EDITS, tools\text_tables.py). Their own state (never g_dialog: its answer quits a race). Our
// fe_mainmenu calls 7a1c only when the menu is idle, no dialog is up and a license is picked; the call site is checked (another
// module's 7a1c, e.g. the game search, never posts one). A waiting Steam join goes first; one notice at a time, 1.5 s apart (a
// post the dialogs module hasn't read yet would be replaced by the next one).
const uint32_t kNoticeReadyDef = 0x9dc5ca23, kNoticeAppliedDef = 0x75cf8f27;
const uint8_t kMainMenuJoinCode[] = {0x57, 0x16, 0x02, 0x85, 0x3a, 0xb8, 0x6c, 0x4b, 0x14, 0x3b,
                                     0x00, 0x32, 0x00, 0x1c, 0x7a, 0x01, 0x00, 0xfe, 0x7f};  // from PC - 19
const ScriptSiteDef kMainMenuJoin = {"main menu join check (7a1c)", kScriptMainMenu, -19, kMainMenuJoinCode, sizeof(kMainMenuJoinCode), {0x1c1a}};
const DWORD kNoticeGapMs = 1500;
DWORD g_noticePostedAt = 0;
bool g_noticeSiteLogged = false;

// 0206 at the dialogs module's OK path with our dialog up: the player answered.
void NetDialogClosed(void* vm, const NativeCall* d) {
    if (!g_dialog.open || d->argc < 1 || d->types[0] != kTypeHash || d->values[0] != kDialogScene) return;
    if (!ScriptSite(vm, kDialogCloseSite)) return;
    g_dialog.open = false;
    g_dialog.answered = true;
    g_dialog.quitPending = true;
    NetDialogFrame();  // the quit now, if the game screen allows it
}

// The race is closed: a dialog still up is closed (its answer would quit a race that no longer runs; left up, it
// would block the lobby's busy-gated screens).
void CloseNetDialog() {
    if (g_dialog.shown && !g_dialog.answered) {  // (answered: the OK closed it)
        if (PostDialogClose()) Log(false, L"race: session: the message was closed with the race");
        else g_dialog.closePending = true;
    }
    g_dialog.open = g_dialog.shown = g_dialog.answered = g_dialog.pending = g_dialog.quitPending = false;
}
}  // namespace

void RaceMainMenuJoinCheck(void* vm) {
    int kind = AutoUpdateNoticePending();
    if (!kind || !vm) return;
    if (!ScriptSite(vm, kMainMenuJoin)) return;
    if (!g_noticeSiteLogged) {
        g_noticeSiteLogged = true;
        Log(false, L"auto-update: the main menu can show the notice (our fe_mainmenu's join check)");
    }
    if (g_noticePostedAt && GetTickCount() - g_noticePostedAt < kNoticeGapMs) return;
    LobbySnapshot s;
    LobbyRead(s);
    if (s.state != kLobbyOff && s.pendingJoin) return;  // a Steam join leaves the main menu first
    if (!PostDialog(kind == kUpdateNoticeApplied ? kNoticeAppliedDef : kNoticeReadyDef)) return;  // the queue is full: next frame
    g_noticePostedAt = GetTickCount();
    AutoUpdateNoticeShown(kind);
}

// True: this call was handled here (the game's handler is not called).
bool RaceNativeBefore(void* vm, NativeCall* d) {
    if (d && d->id == 0x2004) {
        NoteLicense(d);
        return false;
    }
    if (vm && d && d->id == 0x160e) {
        LeaveRaceForJoin(vm);
        if (ScriptSite(vm, kGameInbox)) NetDialogFrame();
        return false;
    }
    if (vm && d && d->id == 0x0206) {
        NetDialogClosed(vm, d);
        return false;
    }
    if (vm && d && d->id == 0x1b03) {
        if (g.phase == kRunning && !g.resultsStarted && ScriptSite(vm, kResultsStart)) {
            g.resultsStarted = true;
            if (g_pauseOpen) Log(false, L"race: this game's results started with the online pause open: it closes now, as on the console");
            // The lobby's race bar (a player waiting there): this player views the results now (the console's "VIEWING RESULTS"
            // status, fe_net_lobby_EN 1C2A9041), with its final progress; the record is cleared when the results are left (EndRace).
            int32_t v[6];
            if (g.progressSent[0] >= 0 && ProgressOf(g.local, v)) {
                v[5] = 1;
                if (LobbyPostArgs(kCmdSetProgress, v, 6)) memcpy(g.progressSent, v, sizeof(g.progressSent));
                else Log(false, L"race: the lobby's race bar: \"viewing results\" could not be posted (the queue is full)");
            }
        }
        return false;
    }
    if (vm && d && d->id == 0x0f0f) {
        PauseTitleBefore(vm, d);
        return false;
    }
    if (vm && d && d->id == 0x1d02) {
        LoadingTipsBefore(vm);
        return false;
    }
    if (d && d->id == 0x1f15) {  // the progress bar's frame: our entries first, so it places them this frame
        UpdateProgressBar();
        return false;
    }
    if (vm && d && d->id == 0x1612) return ResultsRowName(vm, d);
    if (vm && d && d->id == 0x021f) QuitBoxTextBefore(vm, d);  // (then the HUD tags' check below: another site)
    if (!vm || !d || d->id != 0x021f || d->argc != 2 || d->types[0] != kTypeHash || d->types[1] != kTypeHash || !TagsOn()) return false;
    if (!ScriptSite(vm, kHudTagText)) return false;
    int k = -1;
    for (int i = 0; i < kGridSlots; ++i)
        if (d->values[1] == kTagLabels[i]) k = i;
    if (k < 0) {
        bool& unknownLogged = g.tagLogged[3];
        if (!unknownLogged) Log(true, L"race: a HUD tag label %08x was not recognised; the tags show player numbers", d->values[1]);
        unknownLogged = true;
        return false;
    }
    static char cpuName[32];
    char* text = nullptr;
    bool cpu = CpuTagged(k);
    if (cpu) {
        strncpy_s(cpuName, RacerDisplayName(k), _TRUNCATE);
        text = cpuName;
    } else if (k < g.c.humanCount && TaggedPeer(k) && g.names[k][0]) {
        text = g.names[k];
    }
    if (!text) return false;
    static TextBuffer buffer = {kTextBufferVtable, 0, 0, nullptr};
    buffer.capacity = cpu ? (int32_t)sizeof(cpuName) : (int32_t)sizeof(g.names[k]);
    buffer.text = text;
    bool ok = ((SetElementTextFn)(uintptr_t)kSetElementText)(d->values[0], &buffer);
    bool& logged = g.tagLogged[cpu ? 4 : 2];
    if (!logged) Log(false, cpu ? L"race: the HUD tag over computer racer %d's car shows its racer's name%s" : L"race: the HUD tag over player %d's car shows their name%s",
                     k, ok ? L"" : L" (the tag's text was not found)");
    logged = true;
    return ok;  // not found: the game's own label
}

#ifdef RACE_TEST
void RaceTestSetTextAddress(uintptr_t setElementText) { kSetElementText = setElementText; }
void RaceTestSetIntroAddresses(uintptr_t flowRecord, uintptr_t flowAdvance, uintptr_t findText) {
    kFlowRecord = flowRecord; kFlowAdvance = flowAdvance; kFindTextElement = findText;
}
void RaceTestSetIntroSoundAddresses(uintptr_t voiceQueue, uintptr_t voiceVtable, uintptr_t voiceUpdate, uintptr_t soundVtable, uintptr_t isPlaying) {
    kVoiceQueue = voiceQueue; kVoiceQueueVtable = voiceVtable; kVoiceQueueUpdate = voiceUpdate; kSoundVtable = soundVtable; kSoundIsPlaying = isPlaying;
}
void RaceTestSetAnnouncer(bool hooks, uintptr_t commentMan, uintptr_t play, uintptr_t flush, uintptr_t child, uintptr_t request,
                          uintptr_t wrongWay) {
    g_announcerHooks = g_anyResolverCaller = hooks;
    kCommentMan = commentMan; kCommentPlay = play; kSpeechFlush = flush; kGroupChild = child; kCommentRequest = request; kWrongWayCall = wrongWay;
}
void RaceTestSetFairAddresses(uintptr_t overtakeCall, uintptr_t overtakeCooldown, uintptr_t alertCooldown, uintptr_t placeList, uintptr_t raceClock,
                              uintptr_t commentOff, uintptr_t raceMode, uintptr_t calloutsLive, uintptr_t voiceWin, uintptr_t voiceLose,
                              uintptr_t genericAlert) {
    kOvertakeCall = overtakeCall; kOvertakeCooldown = overtakeCooldown; kAlertCooldown = alertCooldown; kPlaceList = placeList;
    kRaceClock = raceClock; kCommentOff = commentOff; kRaceMode = raceMode; kCalloutsLive = calloutsLive; kVoiceWin = voiceWin;
    kVoiceLose = voiceLose; kGenericAlert = genericAlert;
}
// The fair call-outs group on or off; fixed draws (roll in [0, 1) for every chance, coin 0/1; -1: random).
void RaceTestSetFair(bool hooks, float roll, int coin) {
    g_fairHooks = hooks;
    g_testRoll = roll;
    g_testCoin = coin;
}
int RaceTestMentions(int k) { return k >= 0 && k < kGridSlots ? g.mentions[k] : -1; }
void RaceTestSetLineAddresses(uintptr_t hitCooldown, uintptr_t raceSeconds, uintptr_t genericHit, uintptr_t allStarHitCall, uintptr_t opaLineCall,
                              uintptr_t opaVtable) {
    kHitCooldown = hitCooldown; kRaceSeconds = raceSeconds; kGenericHit = genericHit; kAllStarHitCall = allStarHitCall; kOpaLineCall = opaLineCall;
    kOpaVtable = opaVtable;
}
void RaceTestSetLineHooks(bool lines, bool opa) { g_lineHooks = lines; g_opaHooks = opa; }
void RaceTestSetPulseHooks(bool on) { g_pulseHooks = on; }
void RaceTestSetGateHooks(bool on) { g_gateHooks = on; }
void RaceTestSetProjectileSerial(uintptr_t address) { kProjectileSerial = address; }
void RaceTestSetOpaRamAddress(uintptr_t fn) { kOpaRamStart = fn; }
void RaceTestSetVoiceLineAddress(uintptr_t fn) { kVoiceLine = fn; }
void RaceTestSetGroupPlayAddresses(uintptr_t setMode, uintptr_t play) { kGroupSetMode = setMode; kGroupPlay = play; }
int RaceTestVoiceLines(int k, int kind, bool queued) {
    if (k < 0 || k >= kGridSlots || kind < 0 || kind >= kVoiceKinds) return -1;
    return queued ? g.voiceQueued[k][kind] : g.voiceLines[k][kind];
}
void RaceTestSetDialogAddresses(uintptr_t helper, uintptr_t create, uintptr_t set, uintptr_t release, uintptr_t none) {
    kArrayHelper = helper; kArrayCreate = create; kArraySet = set; kValueRelease = release; kNoneValue = none;
}
void RaceTestSetToastAddresses(uintptr_t idle, uintptr_t play, uintptr_t sceneText) { kElementIdle = idle; kPlayElement = play; kSceneText = sceneText; }
void RaceTestSetProgressAddress(uintptr_t distance) { kRecordDistance = distance; }
void RaceTestSetJoinAddresses(uintptr_t post, uintptr_t clearGp, uintptr_t clearMission, uintptr_t clear093f, uintptr_t blackboardGet,
                              uintptr_t worldPaused, uintptr_t profiles) {
    kPostMessage = post; kClearGrandPrix = clearGp; kClearMission = clearMission; kClear093f = clear093f; kBlackboardGet = blackboardGet;
    kWorldPaused = worldPaused; kProfiles = profiles;
}
void RaceTestSetCatchupAddresses(uintptr_t distance, uintptr_t nearCheck, uintptr_t arena, uintptr_t nearMetres, uintptr_t defaults) {
    kRaceDistance = distance; kNearCheck = nearCheck; kArenaCourse = arena; kNearMetres = nearMetres; kScaleDefaults = defaults;
}
void RaceTestSetNavPosition(uintptr_t navPosition) { kNavPosition = navPosition; }
void RaceTestSetBarAddresses(uintptr_t localPlayers, uintptr_t iconHash, uintptr_t setImage) {
    kLocalPlayers = localPlayers; kIconHash = iconHash; kSetImage = setImage;
}
#endif

void RaceNativeAfter(void* vm, NativeCall* d) {
    if (d && d->id == 0x0f0f) {
        PauseTitleAfter();
        return;
    }
    if (d && d->id == 0x1f16) {  // the progress bar was (re)built: every entry is the game's again
        for (Race::BarCopy& b : g.bar) b.human = 0;
        UpdateProgressBar();
        return;
    }
    if (vm && d && d->id == 0x0938 && HudTagGate(vm)) ToastFrame();  // once per HUD frame
    if (!vm || !d || (d->id != 0x0938 && d->id != 0x0911) || !TagsOn() || !Number(d)) return;
    if (d->id == 0x0938) {
        if (!HudTagGate(vm) || (int32_t)d->retValue >= 2) return;
        d->retValue = 2;  // value only; type kept (none: int)
    } else {
        if (d->argc != 1 || !IntOrHash(d, 0) || (!TaggedPeer((int)d->values[0]) && !CpuTagged((int)d->values[0]))) return;
        if (!ScriptSite(vm, kHudTagHuman)) return;
        d->retValue = 1;
    }
    if (d->retType == kTypeNone) d->retType = kTypeInt;
    bool& gateLogged = g.tagLogged[0], &carLogged = g.tagLogged[1];
    bool& logged = d->id == 0x0938 ? gateLogged : carLogged;
    if (!logged) Log(false, d->id == 0x0938 ? L"race: the HUD's player tags are on" : L"race: the HUD tags player %d's car", d->argc ? (int)d->values[0] : -1);
    logged = true;
}

// ---------------------------------------------------------------- VIEW PROFILE / GAMER CARD (re\m7-profile\FINDINGS.md)
// The console's results list had a cursor over the rows (Up/Down; the gold row) and a VIEW PROFILE / GAMER CARD button
// (square = the PC's F1) for that player's profile. The PC `results` module kept the row highlight and lost the handler; our
// module (tools\build_menus.py results_with_profile) calls this every list frame. The cursor visits only the rows whose car is
// a real player's (this player's own included; computer racers' and test players' rows have no profile), starts on the first
// of them, and F1 asks the Steam worker for that player's profile page in the overlay (kCmdProfile), at most twice a second.
// The footer is set once the cursor appears and again every 50 frames (in case the screen's own footer call comes after ours).
enum { kProfileStep = 0, kProfileRowOff = 1, kProfileRowOn = 2, kProfileFooter = 3 };
enum { kFooterCard = 1, kFooterPlain = 2 };
const DWORD kProfileRepeatMs = 500;
const int kProfileFooterEvery = 50;

static bool ProfileRow(int row) {  // the results row (a place) shows a real player's car
    if (row < 0 || row >= g.placeRows || row >= kGridSlots) return false;
    int k = g.placeHuman[row];
    if (k < 0 || k >= g.c.humanCount) return false;
    if (k == g.local) return true;
    const Peer* p = PeerByIndex(k);
    return p && !p->test;  // (a departed player's row still has their profile)
}

static int NextProfileRow(int from, int step) {
    for (int r = from + step; r >= 0 && r < g.placeRows && r < kGridSlots; r += step)
        if (ProfileRow(r)) return r;
    return -1;
}

int32_t RaceProfileNative(int32_t field, int32_t up, int32_t down, int32_t f1) {
    if (field != kProfileStep) return field == kProfileRowOff ? g.profileOff : field == kProfileRowOn ? g.profileOn : field == kProfileFooter ? g.profileFooter : -1;
    g.profileOff = g.profileOn = -1;
    g.profileFooter = 0;
    if (g.phase != kRunning || !RaceOver() || !g.resultsStarted || g.placeRows <= 0) return -1;
    if (g.profileRow < 0) {
        int first = NextProfileRow(-1, 1);
        if (first < 0) return -1;
        g.profileRow = g.profileOn = first;
        g.profileFooter = kFooterCard;
        g.profileFrames = 1;
        Log(false, L"race: results: the profile cursor starts on row %d (player %d, %hs); F1 opens the Steam profile", first + 1, g.placeHuman[first],
            RacerName(g.placeHuman[first]));
        return first;
    }
    if (++g.profileFrames % kProfileFooterEvery == 0) g.profileFooter = kFooterCard;
    int moved = up ? NextProfileRow(g.profileRow, -1) : down ? NextProfileRow(g.profileRow, 1) : -1;
    if (moved >= 0) {
        g.profileOff = g.profileRow;
        g.profileOn = g.profileRow = moved;
    }
    if (f1 && GetTickCount() - g.profileF1At > kProfileRepeatMs) {
        g.profileF1At = GetTickCount();
        int k = g.placeHuman[g.profileRow];
        uint64_t id = k >= 0 && k < g.c.humanCount ? g.c.humans[k] : 0;
        int32_t args[2] = {(int32_t)(uint32_t)id, (int32_t)(uint32_t)(id >> 32)};
        if (id && !IsTestPlayer(id) && LobbyPostArgs(kCmdProfile, args, 2))
            Log(false, L"race: results: VIEW PROFILE: player %d's Steam profile asked for (row %d)", k, g.profileRow + 1);
    }
    return g.profileRow;
}

uint32_t RaceRacerHash(int index) { return g_racerList && index >= 0 && index < g_racerCount ? g_racerList[index].hash : 0; }
bool RaceRacerOn(int index) { return RacerOn(index); }
int RaceTestPlayerRacer(int n, const int* taken, int takenCount) {
    for (int i = 0; g_racerList && i < g_racerCount; ++i) {
        bool used = !g_racerList[i].ai || !RacerOn(i);
        for (int j = 0; j < takenCount && !used; ++j) used = taken[j] == i;
        if (!used && n-- == 0) return i;
    }
    return -1;
}
uint32_t RaceLastId() { return g_lastRaceId; }

#ifdef RACE_TEST
// Every script call site this file checks (ScriptSite), for tests/test_race.cpp to verify against the real modules: each site's
// bytes once in its module (at a known offset) and in no other module of the package; the `results` sites against our built
// module (menus\out\results.smosbin), where every site after 0x25e1 moved with the profile cursor's insertion (kResultsInsert).
int RaceTestSites(const ScriptSiteDef** out, int cap) {
    const ScriptSiteDef* sites[] = {
        &kResultsRow, &kFinishPanel, &kResultsList, &kResultsMenu, &kResultsStart, &kResultsName,
        &kPauseMenu, &kPauseOpen, &kPauseSites[0], &kPauseSites[1], &kPauseSites[2], &kPauseSites[3], &kPauseSites[4], &kPauseSites[5],
        &kPauseEndReads[0], &kPauseEndReads[1], &kPauseTitle, &kQuitBoxText[0], &kQuitBoxText[1],
        &kFrontMainRead, &kBridgeDifficulty, &kBridgeRacers, &kTips, &kHudTagGate, &kHudTagHuman, &kHudTagText,
        &kGameInbox, &kDialogCloseSite, &kMainMenuJoin,
    };
    int n = 0;
    for (const ScriptSiteDef* s : sites)
        if (n < cap) out[n++] = s;
    return n;
}

uint32_t RaceTestSitePc(const uint8_t* module, const ScriptSiteDef& s) { return SitePc(module, s); }
#endif
