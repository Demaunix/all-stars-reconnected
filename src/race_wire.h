#pragma once
// Messages between the games for one race, sent with ISteamNetworkingMessages on kRaceChannel.
// Our own format (notes/RACE-BRIDGE.md), not a game format: little-endian, fixed sizes, and every
// message is checked completely before anything uses it. A message from another wire version, or
// one that fails a check, is dropped by the caller (and logged).
//
//   Contract  host -> guests   the race everyone loads: id, course, rules, humans and grid slots
//   Loaded    each -> host     "I reached hold <stage>" (1 = race loaded, 2 = countdown reached)
//   Go        host -> guests   release hold <stage> now
//   Pose      each -> each     one sim tick of the sender's car (unreliable, ~50 per second)
//   Abort     any  -> all      leave this race (reason code)
//   Finished  each -> each     the sender's car is in its game's finish list: its time (once)
//   Cars      host -> guests   one sim tick of every car the host's game drives: its player and,
//                              when the contract says so, the AI (unreliable; replaces its Pose)
//   Results   host -> guests   the host's final results for all 8 cars, once its race is over
//   Event     each -> each     one item event (give, fire, hit...) of a car the sender owns, as the
//                              game built it (notes/ITEMS-SYNC.md); the receiver runs it for that car
//   IntroVote each -> each     the sender's player voted to skip the course intro (once; with more
//                              than half the votes it ends once the announcer has finished)
//   Announce  host -> guests   one line of the host's announcer (cue, take, wait): the guests' own
//                              announcer is muted, they say the host's lines (re\m7-announcer)
//   WrongWay  guest -> host    the sender's car just turned the wrong way: the host's announcer calls it
//   Voice     each -> each     the sender's player's racer said a driver line (kind, 2D/3D, wait): the others,
//                              whose own lines for that racer are muted, say it too (re\m7-vo-triggers)
//   ItemLine  guest -> host    the sender's car took a hit its own game saw (a spin, or an All-Star's touch, with
//                              the All-Star user): the host's announcer calls it (re\m7-item-lines)
// (IntroVote is no longer sent: since 0.7.0-m7z each screen skips its own intros.)
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

constexpr int kRaceChannel = 1;
constexpr uint32_t kWireMagic = 0x31525341;  // bytes "ASR1"
constexpr uint8_t kWireVersion = 12;         // 2: Pose carries the nav path; Finished. 3: Cars, Results, contract flags. 4: Event. 5: exhaust fx. 6: reactions. 7: IntroVote, 8 humans. 8: Announce. 9: Voice. 10: ItemLine, the All-Star weapon in the phase. 11: the gate All-Stars' shots. 12: kContractNoDriftPlus
constexpr int kMaxHumans = 8;                // = the lobby size (kMaxMembers, lobby.h): the console's 8 seats
constexpr int kGridSlots = 8;                // a single race always has 8 racers
constexpr int kMaxCars = kGridSlots - 1;     // the host's cars in one Cars message (never all 8: 2+ humans)
constexpr size_t kWireMax = 416;             // the largest message (Cars with 7 cars) fits

enum WireType : uint8_t { kMsgContract = 1, kMsgLoaded = 2, kMsgGo = 3, kMsgPose = 4, kMsgAbort = 5, kMsgFinished = 6, kMsgCars = 7, kMsgResults = 8,
                          kMsgEvent = 9, kMsgIntroVote = 10, kMsgAnnounce = 11, kMsgWrongWay = 12, kMsgVoice = 13, kMsgItemLine = 14 };

// Contract flags.
enum ContractFlags : uint8_t {
    kContractHostAi = 1,  // the host's game drives the AI; the guests show them from its Cars and take its Results
    kContractItems = 2,   // item events are mirrored: each car's owner decides its items (needs kContractHostAi)
    kContractAnnouncer = 4,  // the host's announcer speaks for every screen (Announce; the guests mute their own)
    kContractNoDriftPlus = 8,  // a keyboard player races: each controller steers with its stick OR its d-pad, never their sum (re\drift-plus)
};

enum AbortReason : uint8_t {
    kAbortQuit = 1,      // the player left the race
    kAbortTimeout = 2,   // the others did not load in time
    kAbortMismatch = 3,  // the contract could not be loaded here (course, racer, rules)
    kAbortLost = 4,      // the connection to the other game was lost
};

// The race every PC loads. Racers are created in roster order on every PC: humans first, in
// lobby order (host first), then AI. With the same order, states and RNG seed, every PC builds the
// same starting grid (notes/RACE-BRIDGE.md).
struct RaceContract {
    uint32_t race;        // nonzero; new for every start
    uint32_t seed;        // the game's RNG is reseeded with this before the grid is built
    uint8_t course;       // course ordinal in the front end's course table
    uint32_t courseHash;  // that course's name hash, checked on both sides
    uint8_t laps, items, catchup, ai;
    uint8_t flags;        // ContractFlags
    uint8_t humanCount;   // 2..kMaxHumans
    uint64_t humans[kMaxHumans];   // Steam IDs; human k is roster entry k
    uint32_t roster[kGridSlots];   // character hash per creation call
};

enum HoldStage : uint8_t { kStageLoaded = 1, kStageCountdown = 2 };

enum PoseFlags : uint8_t { kPoseSnap = 1 };  // do not blend from the previous pose (respawn, reset)

// The car's exhaust effect on its owner's screen (re\m6-race-hud\EXHAUST-FINDINGS.md): state bits 0-2 =
// the exhaust component's mode (0-3 boost charge level, 4 boost flame, 7 off; kFxNone: not known), bits
// 3-6 = a serial that changes each time the owner's flame fires again. Sent in the flags byte's bits 1-7.
constexpr uint8_t kFxNone = 5;
constexpr uint8_t kFxOff = 7;
inline uint8_t FxState(uint8_t fx) { return fx & 7; }
inline uint8_t FxSerial(uint8_t fx) { return (fx >> 3) & 15; }

constexpr uint16_t kNavNone = 0xffff;  // the sender's car is on no nav path (or the course has none)

struct RacePose {
    uint32_t race;
    uint8_t slot, flags;
    uint32_t tick;  // the sender's sim tick
    float pos[3], rot[4], vel[3], spin[3];  // metres, quaternion x y z w, m/s, angular velocity
    uint16_t nav;   // index of the nav path the sender's track cursor is on (notes/LAPS.md), or kNavNone
    uint8_t fx = kFxNone;  // the exhaust effect (kFx*, FxState / FxSerial)
};

// A car reached its game's finish list. Times are the game's lap-record units (1/4096 sim tick
// since GO, record +0x68 / +0x60); reason is the finish-list reason (1 finished .. 6 estimated).
struct RaceFinish {
    uint32_t race;
    uint8_t slot, reason;
    uint32_t time, best;
};

// One sim tick of the host's cars: each entry is a pose (its race and tick are the batch's).
struct RaceCars {
    uint32_t race;
    uint32_t tick;  // the host's sim tick
    uint8_t count;  // 1..kMaxCars
    RacePose cars[kMaxCars];
};

// The host's final results: one entry per creation slot 0..7, in slot order. reason 0 = no result
// (the guest leaves that car as its own game has it); otherwise as in RaceFinish. rank: the car's
// place after the host's sort (0-based), or kRankNone; it settles the order of equal times.
constexpr uint8_t kRankNone = 0xff;
struct RaceResultEntry {
    uint8_t reason, rank;
    uint32_t time, best;
};
struct RaceResults {
    uint32_t race;
    RaceResultEntry cars[kGridSlots];
};

// One item event (notes/ITEMS-SYNC.md): the game's event object from +8 (after its vtable and kind),
// `size` bytes, fixed per kind. slot: the car whose owner decided it (the firing car, or the victim of
// a hit), which the receiver checks against the sender. tick: the sender's sim tick when it ran.
constexpr uint16_t kEventBodyMax = 0xe0 - 8;
struct RaceEvent {
    uint32_t race;
    uint32_t tick;
    uint8_t kind, slot;
    uint16_t size;
    uint8_t body[kEventBodyMax];
};

// Not a game kind (those are 0..0x11): the owner's All-Star phase (racer +0x1ff0) changed; body u32: the phase in its low
// byte and, for Opa-Opa's All-Star once it is active (phase 4: the weapon is final then, re\m7-opa\FINDINGS.md), the
// weapon + 1 in the next byte (1..3 = weapons 0..2, 5 = the game's 4 "none": the owner fires nothing; 0: not known yet).
// It travels in the same ordered queue as the events, so it comes after the All-Star's fire.
constexpr uint8_t kEventAllStarPhase = 0x40;
constexpr int kNoWeapon = -1;    // not known (yet)
constexpr int kWeaponNone = 4;   // the game's "no weapon chosen": fires nothing, says nothing
inline bool WeaponValid(int w) { return (w >= 0 && w <= 2) || w == kWeaponNone; }
// Bit 16 (1.1): Metal Sonic's All-Star ran its time out on its owner's screen (a phase-4 message sent again with it): the copies
// close their beam then (race.cpp ForceMetalTimeUp; re\m9-metal-allstar\FINDINGS.md).
constexpr uint32_t kAllStarTimeUp = 1u << 16;
inline uint32_t AllStarPhaseBody(uint32_t phase, int weapon, bool timeUp = false) {
    return (phase & 0xff) | (WeaponValid(weapon) ? (uint32_t)(weapon + 1) << 8 : 0) | (timeUp ? kAllStarTimeUp : 0);
}
inline uint32_t AllStarPhaseOf(uint32_t body) { return body & 0xff; }
inline int AllStarWeaponOf(uint32_t body) {
    uint32_t w = (body >> 8) & 0xff;
    return (w >= 1 && w <= 3) || w == kWeaponNone + 1 ? (int)w - 1 : kNoWeapon;
}
// Not a game kind either: the owner's driver started a reaction (a taunt, a celebration...); body u32 the
// animation node's hash (re\m6-reactions\FINDINGS.md). Same queue, shown when the car is shown at that tick.
constexpr uint8_t kEventReaction = 0x41;
// Nor this: Opa-Opa's All-Star fired a shot on its owner's screen (re\m7-allstar-shots\FINDINGS.md §3): body u32 = the
// weapon + 1 (1..3) in the low byte, a running count in the next. The copies fire at the owner's shots, not on their own.
// Also the other All-Star classes whose fire is a timed pulse (Super Sonic, AiAi, B.D. Joe): kind byte kShotPulseBase + the class's
// index in race.cpp's table.
constexpr uint8_t kEventAllStarShot = 0x42;
constexpr uint32_t kShotPulseBase = 0x10;
inline uint32_t AllStarShotBody(int weapon, uint8_t count) { return (uint32_t)(weapon + 1) | (uint32_t)count << 8; }
inline uint32_t AllStarPulseBody(int classIndex, uint8_t count) { return (kShotPulseBase + (uint32_t)classIndex) | (uint32_t)count << 8; }
inline uint32_t AllStarShotKind(uint32_t body) { return body & 0xff; }
inline int AllStarShotWeapon(uint32_t body) { uint32_t w = body & 0xff; return w >= 1 && w <= 3 ? (int)w - 1 : kNoWeapon; }
inline int AllStarPulseClass(uint32_t body) { uint32_t k = body & 0xff; return k >= kShotPulseBase && k < kShotPulseBase + 16 ? (int)(k - kShotPulseBase) : -1; }
// Nor this: a "gate" All-Star's shot (Eggman, Shadow, Alex Kidd, Zobio, Ryo, Knuckles; re\m7-allstar-shots2\FINDINGS.md): the
// owner's All-Star fired (or, Knuckles, started a hold: flag kGateFlagHold). Body 12 bytes: u32 head = the class's index in
// race.cpp's table (low byte) | a running count << 8 | flags << 16, then two u32 payload words (Eggman's rocket pattern, Shadow's
// steer/vertical floats, Alex Kidd's steering float, Knuckles' punch level; raw bits).
constexpr uint8_t kEventAllStarGate = 0x44;
constexpr uint32_t kGateFlagHold = 1;     // Knuckles' hold started; Beat's spray held (payload: the steer)
constexpr uint32_t kGateFlagRelease = 2;  // Beat's spray released
constexpr uint32_t kGateFlagAttack = 4;   // Bonanza Bros: the blimp's Attack started (the bomb's throw comes as a plain shot: aim now, aim before)
constexpr uint32_t kGateFlagAim = 8;      // Bonanza Bros: the blimp's aim angle (payload a), sent as it turns
inline uint32_t AllStarGateHead(int classIndex, uint8_t count, uint32_t flags) { return ((uint32_t)classIndex & 0xff) | (uint32_t)count << 8 | (flags & 0xff) << 16; }
inline int AllStarGateClass(uint32_t head) { return (int)(head & 0xff); }
inline uint32_t AllStarGateFlags(uint32_t head) { return (head >> 16) & 0xff; }
inline bool PseudoEvent(uint8_t kind) { return kind == kEventAllStarPhase || kind == kEventReaction || kind == kEventAllStarShot || kind == kEventAllStarGate; }

// The body size of each event kind the games exchange (the event's size minus its 8-byte head), or 0
// for a kind that stays on its own PC.
inline uint16_t EventBodySize(uint8_t kind) {
    switch (kind) {
    case kEventAllStarPhase: case kEventReaction: case kEventAllStarShot: return 4;
    case kEventAllStarGate: return 12;
    case 0: return 0x14 - 8;          // give an item
    case 1: case 2: return 0xe0 - 8;  // fire (2: aimed)
    case 3: case 4: return 0xe0 - 8;  // sub-projectiles
    case 5: return 0x14 - 8;          // a hit
    case 6: return 0x0c - 8;          // a latch
    case 10: case 11: return 0x10 - 8;  // homing target set / clear
    default: return 0;
    }
}

// ---------------------------------------------------------------- byte helpers
namespace wire {

struct Writer {
    uint8_t* p;
    size_t cap, n;
    bool ok;
    void Bytes(const void* v, size_t k) {
        if (!ok || cap - n < k) { ok = false; return; }
        memcpy(p + n, v, k);
        n += k;
    }
    void U8(uint8_t v) { Bytes(&v, 1); }
    void U16(uint16_t v) { Bytes(&v, 2); }
    void U32(uint32_t v) { Bytes(&v, 4); }
    void U64(uint64_t v) { Bytes(&v, 8); }
    void F32(float v) { Bytes(&v, 4); }
};

struct Reader {
    const uint8_t* p;
    size_t size, n;
    bool ok;
    void Bytes(void* v, size_t k) {
        if (!ok || size - n < k) { ok = false; memset(v, 0, k); return; }
        memcpy(v, p + n, k);
        n += k;
    }
    uint8_t U8() { uint8_t v; Bytes(&v, 1); return v; }
    uint16_t U16() { uint16_t v; Bytes(&v, 2); return v; }
    uint32_t U32() { uint32_t v; Bytes(&v, 4); return v; }
    uint64_t U64() { uint64_t v; Bytes(&v, 8); return v; }
    float F32() { float v; Bytes(&v, 4); return v; }
};

// Header: magic u32, type u8, version u8, total size u16, race u32 (12 bytes).
constexpr size_t kHeader = 12;

inline void Header(Writer& w, WireType type, uint16_t size, uint32_t race) {
    w.U32(kWireMagic);
    w.U8(type);
    w.U8(kWireVersion);
    w.U16(size);
    w.U32(race);
}

// Checks the header against the expected type and exact size; leaves the reader after it.
inline bool Open(Reader& r, WireType type, uint16_t size, uint32_t& race) {
    if (r.size != size) return false;
    uint32_t magic = r.U32();
    uint8_t t = r.U8(), v = r.U8();
    uint16_t s = r.U16();
    race = r.U32();
    return r.ok && magic == kWireMagic && t == type && v == kWireVersion && s == size && race != 0;
}

inline bool Finite(float f) { return isfinite(f) != 0; }

constexpr uint16_t kContractSize = (uint16_t)(kHeader + 4 + 1 + 4 + 4 + 1 + 1 + kMaxHumans * 8 + kGridSlots * 4);
constexpr uint16_t kLoadedSize = (uint16_t)(kHeader + 2);
constexpr uint16_t kGoSize = (uint16_t)(kHeader + 1);
constexpr uint16_t kPoseSize = (uint16_t)(kHeader + 1 + 1 + 4 + 13 * 4 + 2);
constexpr uint16_t kAbortSize = (uint16_t)(kHeader + 1);
constexpr uint16_t kIntroVoteSize = (uint16_t)(kHeader + 1);
constexpr uint16_t kAnnounceSize = (uint16_t)(kHeader + 4 + 4 + 4 + 4 + 1);
constexpr uint16_t kVoiceSize = (uint16_t)(kHeader + 1 + 1 + 1 + 2);
constexpr uint16_t kItemLineSize = (uint16_t)(kHeader + 1 + 1 + 1);
constexpr uint16_t kFinishedSize = (uint16_t)(kHeader + 1 + 1 + 4 + 4);
constexpr uint16_t kCarSize = (uint16_t)(1 + 1 + 13 * 4 + 2);  // one Cars entry: slot, flags, pose, nav
constexpr uint16_t CarsSize(int count) { return (uint16_t)(kHeader + 4 + 1 + count * kCarSize); }
constexpr uint16_t kResultsSize = (uint16_t)(kHeader + kGridSlots * (1 + 1 + 4 + 4));
static_assert(kContractSize <= kWireMax && CarsSize(kMaxCars) <= kWireMax && kResultsSize <= kWireMax, "every message fits kWireMax");

}  // namespace wire

// The message type and race id, if `data` starts with a valid header of our version (size not checked).
inline bool WirePeek(const void* data, size_t size, WireType& type, uint32_t& race) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    uint32_t magic = r.U32();
    uint8_t t = r.U8(), v = r.U8();
    uint16_t s = r.U16();
    race = r.U32();
    type = (WireType)t;
    return r.ok && magic == kWireMagic && v == kWireVersion && s == size && race != 0;
}

// ---------------------------------------------------------------- Contract
// Valid: race nonzero, 2..kMaxHumans distinct nonzero Steam IDs, 8 distinct nonzero characters, laps 1..9,
// the on/off rules 0/1, AI setting 0..3, known flags only.
inline bool ContractValid(const RaceContract& c) {
    if (!c.race || c.humanCount < 2 || c.humanCount > kMaxHumans) return false;
    if (c.laps < 1 || c.laps > 9 || c.items > 1 || c.catchup > 1 || c.ai > 3 || (c.flags & ~(kContractHostAi | kContractItems | kContractAnnouncer | kContractNoDriftPlus))) return false;
    if ((c.flags & kContractItems) && !(c.flags & kContractHostAi)) return false;  // item owners need one owner for the AI
    for (int i = 0; i < kMaxHumans; ++i) {
        if (i >= c.humanCount) { if (c.humans[i]) return false; continue; }
        if (!c.humans[i]) return false;
        for (int j = 0; j < i; ++j) if (c.humans[j] == c.humans[i]) return false;
    }
    // Cars are made per grid slot, so a racer may appear more than once among the players (the user's choice, 2026-10-23);
    // a computer racer is never one of the players' racers or another AI's.
    for (int i = 0; i < kGridSlots; ++i) {
        if (!c.roster[i]) return false;
        for (int j = 0; j < i; ++j) if (c.roster[j] == c.roster[i] && i >= c.humanCount) return false;
    }
    return true;
}

inline size_t EncodeContract(const RaceContract& c, void* out, size_t cap) {
    if (!ContractValid(c)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgContract, wire::kContractSize, c.race);
    w.U32(c.seed);
    w.U8(c.course);
    w.U32(c.courseHash);
    w.U8(c.laps); w.U8(c.items); w.U8(c.catchup); w.U8(c.ai);
    w.U8(c.flags);
    w.U8(c.humanCount);
    for (uint64_t h : c.humans) w.U64(h);
    for (uint32_t r : c.roster) w.U32(r);
    return w.ok && w.n == wire::kContractSize ? w.n : 0;
}

inline bool DecodeContract(const void* data, size_t size, RaceContract& c) {
    c = RaceContract{};
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgContract, wire::kContractSize, c.race)) return false;
    c.seed = r.U32();
    c.course = r.U8();
    c.courseHash = r.U32();
    c.laps = r.U8(); c.items = r.U8(); c.catchup = r.U8(); c.ai = r.U8();
    c.flags = r.U8();
    c.humanCount = r.U8();
    for (uint64_t& h : c.humans) h = r.U64();
    for (uint32_t& x : c.roster) x = r.U32();
    return r.ok && r.n == size && ContractValid(c);
}

// ---------------------------------------------------------------- Loaded / Go / Abort
inline bool StageValid(uint8_t stage) { return stage == kStageLoaded || stage == kStageCountdown; }

inline size_t EncodeLoaded(uint32_t race, uint8_t stage, uint8_t slot, void* out, size_t cap) {
    if (!race || !StageValid(stage) || slot >= kGridSlots) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgLoaded, wire::kLoadedSize, race);
    w.U8(stage);
    w.U8(slot);
    return w.ok ? w.n : 0;
}

inline bool DecodeLoaded(const void* data, size_t size, uint32_t& race, uint8_t& stage, uint8_t& slot) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgLoaded, wire::kLoadedSize, race)) return false;
    stage = r.U8();
    slot = r.U8();
    return r.ok && StageValid(stage) && slot < kGridSlots;
}

inline size_t EncodeGo(uint32_t race, uint8_t stage, void* out, size_t cap) {
    if (!race || !StageValid(stage)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgGo, wire::kGoSize, race);
    w.U8(stage);
    return w.ok ? w.n : 0;
}

inline bool DecodeGo(const void* data, size_t size, uint32_t& race, uint8_t& stage) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgGo, wire::kGoSize, race)) return false;
    stage = r.U8();
    return r.ok && StageValid(stage);
}

inline size_t EncodeAbort(uint32_t race, AbortReason reason, void* out, size_t cap) {
    if (!race) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgAbort, wire::kAbortSize, race);
    w.U8(reason);
    return w.ok ? w.n : 0;
}

inline bool DecodeAbort(const void* data, size_t size, uint32_t& race, uint8_t& reason) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgAbort, wire::kAbortSize, race)) return false;
    reason = r.U8();
    return r.ok && reason >= kAbortQuit && reason <= kAbortLost;
}

// ---------------------------------------------------------------- IntroVote
// The sender's player (creation index `slot`) voted to skip the course intro.
inline size_t EncodeIntroVote(uint32_t race, uint8_t slot, void* out, size_t cap) {
    if (!race || slot >= kMaxHumans) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgIntroVote, wire::kIntroVoteSize, race);
    w.U8(slot);
    return w.ok ? w.n : 0;
}

inline bool DecodeIntroVote(const void* data, size_t size, uint32_t& race, uint8_t& slot) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgIntroVote, wire::kIntroVoteSize, race)) return false;
    slot = r.U8();
    return r.ok && slot < kMaxHumans;
}

// ---------------------------------------------------------------- WrongWay
// The sender's car (creation index `slot`) just turned the wrong way.
inline size_t EncodeWrongWay(uint32_t race, uint8_t slot, void* out, size_t cap) {
    if (!race || slot >= kMaxHumans) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgWrongWay, wire::kIntroVoteSize, race);  // (the same body: one slot byte)
    w.U8(slot);
    return w.ok ? w.n : 0;
}

inline bool DecodeWrongWay(const void* data, size_t size, uint32_t& race, uint8_t& slot) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgWrongWay, wire::kIntroVoteSize, race)) return false;
    slot = r.U8();
    return r.ok && slot < kMaxHumans;
}

// ---------------------------------------------------------------- Voice
// The sender's player's racer (creation index `slot`) said a driver line: its voice group `kind` (0 POS, 1 NEG, 4 ATTACK,
// 5 ALLSTAR, 6 INTRO; the podium's 2 and 3 stay each screen's own), 3D at its car or 2D, with the queue wait in ms.
struct RaceVoice {
    uint32_t race;
    uint8_t slot, kind, mode;
    uint16_t wait;
};

inline bool VoiceKindShared(uint8_t kind) { return kind == 0 || kind == 1 || kind == 4 || kind == 5 || kind == 6; }

inline size_t EncodeVoice(const RaceVoice& v, void* out, size_t cap) {
    if (!v.race || v.slot >= kMaxHumans || !VoiceKindShared(v.kind) || v.mode > 1 || v.wait > 30000) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgVoice, wire::kVoiceSize, v.race);
    w.U8(v.slot);
    w.U8(v.kind);
    w.U8(v.mode);
    w.U16(v.wait);
    return w.ok ? w.n : 0;
}

inline bool DecodeVoice(const void* data, size_t size, RaceVoice& v) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgVoice, wire::kVoiceSize, v.race)) return false;
    v.slot = r.U8();
    v.kind = r.U8();
    v.mode = r.U8();
    v.wait = r.U16();
    return r.ok && v.slot < kMaxHumans && VoiceKindShared(v.kind) && v.mode <= 1 && v.wait <= 30000;
}

// ---------------------------------------------------------------- ItemLine
// The sender's car (creation index `slot`) took a hit in its own game: kLineHit, its car's spin/flip flag (racer +0xba0)
// changed, `other` = 1 spinning now, 0 not any more (the host's pick reads "spinning now", as the stock's test does);
// kLineAllStarHit, an All-Star touched it, `other` the All-Star user's creation index.
enum ItemLineKind : uint8_t { kLineHit = 0, kLineAllStarHit = 1 };
struct RaceItemLine {
    uint32_t race;
    uint8_t kind, slot, other;
};

inline bool ItemLineValid(const RaceItemLine& l) {
    if (!l.race || l.slot >= kMaxHumans || l.kind > kLineAllStarHit) return false;
    return l.kind == kLineAllStarHit ? l.other < kGridSlots && l.other != l.slot : l.other <= 1;
}

inline size_t EncodeItemLine(const RaceItemLine& l, void* out, size_t cap) {
    if (!ItemLineValid(l)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgItemLine, wire::kItemLineSize, l.race);
    w.U8(l.kind);
    w.U8(l.slot);
    w.U8(l.other);
    return w.ok ? w.n : 0;
}

inline bool DecodeItemLine(const void* data, size_t size, RaceItemLine& l) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgItemLine, wire::kItemLineSize, l.race)) return false;
    l.kind = r.U8();
    l.slot = r.U8();
    l.other = r.U8();
    return r.ok && ItemLineValid(l);
}

// ---------------------------------------------------------------- Announce
// The host's announcer said a line (re\m7-announcer\FINDINGS.md): the commentary entry (cue), the recorded take, its
// wait in ms, the host's tick; flush: the host's queue was flushed first (lap calls).
struct RaceAnnounce {
    uint32_t race, tick, cue, take;
    int32_t wait;
    uint8_t flush;
};

inline size_t EncodeAnnounce(const RaceAnnounce& a, void* out, size_t cap) {
    if (!a.race || !a.cue || a.flush > 1) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgAnnounce, wire::kAnnounceSize, a.race);
    w.U32(a.tick);
    w.U32(a.cue);
    w.U32(a.take);
    w.U32((uint32_t)a.wait);
    w.U8(a.flush);
    return w.ok ? w.n : 0;
}

inline bool DecodeAnnounce(const void* data, size_t size, RaceAnnounce& a) {
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgAnnounce, wire::kAnnounceSize, a.race)) return false;
    a.tick = r.U32();
    a.cue = r.U32();
    a.take = r.U32();
    a.wait = (int32_t)r.U32();
    a.flush = r.U8();
    return r.ok && a.cue && a.flush <= 1 && a.wait >= 0 && a.wait <= 120000;
}

// ---------------------------------------------------------------- Pose
inline bool PoseValid(const RacePose& p) {
    if (!p.race || p.slot >= kGridSlots || (p.flags & ~kPoseSnap)) return false;
    for (float f : p.pos) if (!wire::Finite(f)) return false;
    for (float f : p.rot) if (!wire::Finite(f)) return false;
    for (float f : p.vel) if (!wire::Finite(f)) return false;
    for (float f : p.spin) if (!wire::Finite(f)) return false;
    // Sane ranges, so garbage can't give the game a degenerate rotation or a far-off position.
    float q = 0;
    for (float f : p.rot) q += f * f;
    if (q < 0.81f || q > 1.21f) return false;
    for (float f : p.pos) if (f < -1e5f || f > 1e5f) return false;
    for (float f : p.vel) if (f < -1e4f || f > 1e4f) return false;
    for (float f : p.spin) if (f < -1e4f || f > 1e4f) return false;
    return true;
}

namespace wire {
inline void PoseBody(Writer& w, const RacePose& p) {
    for (float f : p.pos) w.F32(f);
    for (float f : p.rot) w.F32(f);
    for (float f : p.vel) w.F32(f);
    for (float f : p.spin) w.F32(f);
    w.U16(p.nav);
}
inline void PoseBody(Reader& r, RacePose& p) {
    for (float& f : p.pos) f = r.F32();
    for (float& f : p.rot) f = r.F32();
    for (float& f : p.vel) f = r.F32();
    for (float& f : p.spin) f = r.F32();
    p.nav = r.U16();
}
// The flags byte on the wire: bit 0 kPoseSnap, bits 1-7 the exhaust fx (an fx above 127 is sent as kFxNone).
inline uint8_t FlagsByte(const RacePose& p) { return (uint8_t)((p.flags & kPoseSnap) | ((p.fx < 128 ? p.fx : kFxNone) << 1)); }
inline void ReadFlags(Reader& r, RacePose& p) {
    uint8_t b = r.U8();
    p.flags = b & kPoseSnap;
    p.fx = b >> 1;
}
}  // namespace wire

inline size_t EncodePose(const RacePose& p, void* out, size_t cap) {
    if (!PoseValid(p)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgPose, wire::kPoseSize, p.race);
    w.U8(p.slot);
    w.U8(wire::FlagsByte(p));
    w.U32(p.tick);
    wire::PoseBody(w, p);
    return w.ok && w.n == wire::kPoseSize ? w.n : 0;
}

inline bool DecodePose(const void* data, size_t size, RacePose& p) {
    p = RacePose{};
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgPose, wire::kPoseSize, p.race)) return false;
    p.slot = r.U8();
    wire::ReadFlags(r, p);
    p.tick = r.U32();
    wire::PoseBody(r, p);
    return r.ok && r.n == size && PoseValid(p);
}

// ---------------------------------------------------------------- Cars
// Valid: 1..kMaxCars entries, distinct slots, each a valid pose of the batch's race.
inline bool CarsValid(const RaceCars& c) {
    if (!c.race || c.count < 1 || c.count > kMaxCars) return false;
    for (int i = 0; i < c.count; ++i) {
        const RacePose& p = c.cars[i];
        if (p.race != c.race || p.tick != c.tick || !PoseValid(p)) return false;
        for (int j = 0; j < i; ++j) if (c.cars[j].slot == p.slot) return false;
    }
    return true;
}

inline size_t EncodeCars(const RaceCars& c, void* out, size_t cap) {
    if (!CarsValid(c)) return 0;
    uint16_t size = wire::CarsSize(c.count);
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgCars, size, c.race);
    w.U32(c.tick);
    w.U8(c.count);
    for (int i = 0; i < c.count; ++i) {
        w.U8(c.cars[i].slot);
        w.U8(wire::FlagsByte(c.cars[i]));
        wire::PoseBody(w, c.cars[i]);
    }
    return w.ok && w.n == size ? w.n : 0;
}

inline bool DecodeCars(const void* data, size_t size, RaceCars& c) {
    c = RaceCars{};
    if (size < wire::CarsSize(1) || size > wire::CarsSize(kMaxCars) || (size - wire::CarsSize(0)) % wire::kCarSize) return false;
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgCars, (uint16_t)size, c.race)) return false;
    c.tick = r.U32();
    c.count = r.U8();
    if (!r.ok || wire::CarsSize(c.count) != size) return false;
    for (int i = 0; i < c.count; ++i) {
        RacePose& p = c.cars[i];
        p.race = c.race;
        p.tick = c.tick;
        p.slot = r.U8();
        wire::ReadFlags(r, p);
        wire::PoseBody(r, p);
    }
    return r.ok && r.n == size && CarsValid(c);
}

// ---------------------------------------------------------------- Results
// Valid: reasons 0..6; ranks are kRankNone or distinct places 0..7.
inline bool ResultsValid(const RaceResults& res) {
    if (!res.race) return false;
    bool used[kGridSlots] = {};
    for (const RaceResultEntry& e : res.cars) {
        if (e.reason > 6) return false;
        if (e.rank == kRankNone) continue;
        if (e.rank >= kGridSlots || used[e.rank]) return false;
        used[e.rank] = true;
    }
    return true;
}

inline size_t EncodeResults(const RaceResults& res, void* out, size_t cap) {
    if (!ResultsValid(res)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgResults, wire::kResultsSize, res.race);
    for (const RaceResultEntry& e : res.cars) {
        w.U8(e.reason);
        w.U8(e.rank);
        w.U32(e.time);
        w.U32(e.best);
    }
    return w.ok && w.n == wire::kResultsSize ? w.n : 0;
}

inline bool DecodeResults(const void* data, size_t size, RaceResults& res) {
    res = RaceResults{};
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgResults, wire::kResultsSize, res.race)) return false;
    for (RaceResultEntry& e : res.cars) {
        e.reason = r.U8();
        e.rank = r.U8();
        e.time = r.U32();
        e.best = r.U32();
    }
    return r.ok && r.n == size && ResultsValid(res);
}

// ---------------------------------------------------------------- Finished
inline bool FinishValid(const RaceFinish& f) { return f.race && f.slot < kGridSlots && f.reason >= 1 && f.reason <= 6; }

inline size_t EncodeFinished(const RaceFinish& f, void* out, size_t cap) {
    if (!FinishValid(f)) return 0;
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgFinished, wire::kFinishedSize, f.race);
    w.U8(f.slot);
    w.U8(f.reason);
    w.U32(f.time);
    w.U32(f.best);
    return w.ok && w.n == wire::kFinishedSize ? w.n : 0;
}

inline bool DecodeFinished(const void* data, size_t size, RaceFinish& f) {
    f = RaceFinish{};
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgFinished, wire::kFinishedSize, f.race)) return false;
    f.slot = r.U8();
    f.reason = r.U8();
    f.time = r.U32();
    f.best = r.U32();
    return r.ok && r.n == size && FinishValid(f);
}

// ---------------------------------------------------------------- Event
// Valid: a carried kind with its exact body size, a grid slot.
inline bool EventValid(const RaceEvent& e) { return e.race && e.slot < kGridSlots && EventBodySize(e.kind) && e.size == EventBodySize(e.kind); }

namespace wire {
constexpr uint16_t EventSize(uint16_t body) { return (uint16_t)(kHeader + 4 + 1 + 1 + body); }
static_assert(EventSize(kEventBodyMax) <= kWireMax, "every message fits kWireMax");
}  // namespace wire

inline size_t EncodeEvent(const RaceEvent& e, void* out, size_t cap) {
    if (!EventValid(e)) return 0;
    uint16_t size = wire::EventSize(e.size);
    wire::Writer w{(uint8_t*)out, cap, 0, true};
    wire::Header(w, kMsgEvent, size, e.race);
    w.U32(e.tick);
    w.U8(e.kind);
    w.U8(e.slot);
    w.Bytes(e.body, e.size);
    return w.ok && w.n == size ? w.n : 0;
}

inline bool DecodeEvent(const void* data, size_t size, RaceEvent& e) {
    e = RaceEvent{};
    if (size < wire::EventSize(0) || size > wire::EventSize(kEventBodyMax)) return false;
    wire::Reader r{(const uint8_t*)data, size, 0, true};
    if (!wire::Open(r, kMsgEvent, (uint16_t)size, e.race)) return false;
    e.tick = r.U32();
    e.kind = r.U8();
    e.slot = r.U8();
    e.size = (uint16_t)(size - wire::EventSize(0));
    if (!r.ok || e.size != EventBodySize(e.kind)) return false;
    r.Bytes(e.body, e.size);
    return r.ok && r.n == size && EventValid(e);
}
