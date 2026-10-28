// Unit tests for src/race_wire.h (message format) and src/playout.h (remote car smoothing).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "race_wire.h"
#include "playout.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static RaceContract SampleContract() {
    RaceContract c = {};
    c.race = 0x12345678;
    c.seed = 0xcafef00d;
    c.course = 3;
    c.courseHash = 0xd622d9d9;
    c.laps = 3;
    c.items = 1;
    c.catchup = 0;
    c.ai = 2;
    c.flags = kContractHostAi;
    c.humanCount = 2;
    c.humans[0] = 76561198500000002ull;
    c.humans[1] = 76561198400000001ull;
    for (int i = 0; i < kGridSlots; ++i) c.roster[i] = 0xaabb0000u + i;
    return c;
}

static void TestContract() {
    uint8_t buf[kWireMax];
    RaceContract c = SampleContract(), d;
    size_t n = EncodeContract(c, buf, sizeof(buf));
    CHECK(n == wire::kContractSize && n == 123);  // 12 header + 4 + 1 + 4 + 4 + 1 + 1 + 8 humans * 8 + 8 racers * 4
    CHECK(DecodeContract(buf, n, d));
    CHECK(d.race == c.race && d.seed == 0xcafef00d && d.course == 3 && d.courseHash == 0xd622d9d9 && d.laps == 3 && d.items == 1 && d.ai == 2);
    CHECK(d.flags == kContractHostAi);
    CHECK(d.humanCount == 2 && d.humans[1] == c.humans[1] && d.humans[2] == 0 && d.roster[7] == 0xaabb0007u);
    WireType t;
    uint32_t race;
    CHECK(WirePeek(buf, n, t, race) && t == kMsgContract && race == c.race);
    // Every truncation, and every change to magic, type, version or size, is refused.
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeContract(buf, k, d));
    for (size_t k = 0; k < 8; ++k) {
        buf[k] ^= 0x40;
        CHECK(!DecodeContract(buf, n, d));
        buf[k] ^= 0x40;
    }
    CHECK(DecodeContract(buf, n, d));
    // Invalid contracts are not encoded.
    RaceContract bad = c;
    bad.roster[5] = bad.roster[2];  // one racer twice
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.roster[3] = bad.roster[0];  // a computer racer that is a player's racer
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    {   // players may share a racer (cars are made per grid slot; the user's choice)
        RaceContract same = c;
        same.roster[1] = same.roster[0];
        n = EncodeContract(same, buf, sizeof(buf));
        CHECK(n == wire::kContractSize && DecodeContract(buf, n, d) && d.roster[0] == d.roster[1]);
    }
    bad = c;
    bad.roster[7] = 0;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.humans[1] = bad.humans[0];
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.humans[2] = 5;  // an ID beyond humanCount
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.humanCount = 1;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    // A full lobby: 8 humans and no AI (kMaxHumans = kGridSlots); 9 is refused, so is an empty ID among the 8.
    RaceContract full = c;
    full.humanCount = kMaxHumans;
    for (int i = 0; i < kMaxHumans; ++i) full.humans[i] = 76561198000000000ull + i;
    n = EncodeContract(full, buf, sizeof(buf));
    CHECK(n == wire::kContractSize && DecodeContract(buf, n, d) && d.humanCount == 8 && d.humans[7] == full.humans[7]);
    {   // 8 players on one racer (the user's 8-Sonic test)
        RaceContract eight = full;
        for (int i = 0; i < kGridSlots; ++i) eight.roster[i] = 0x5011c000u;
        n = EncodeContract(eight, buf, sizeof(buf));
        CHECK(n == wire::kContractSize && DecodeContract(buf, n, d) && d.roster[7] == 0x5011c000u);
    }
    bad = full;
    bad.humanCount = 9;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = full;
    bad.humans[5] = 0;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.race = 0;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.laps = 0;
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad = c;
    bad.flags = 2;  // an unknown flag
    CHECK(EncodeContract(bad, buf, sizeof(buf)) == 0);
    bad.flags = 0;  // no flags: each game drives its own AI
    CHECK(DecodeContract(buf, EncodeContract(bad, buf, sizeof(buf)), d) && d.flags == 0);
    CHECK(EncodeContract(c, buf, wire::kContractSize - 1) == 0);  // too small an output buffer
    // A nonzero unused human entry on the wire is refused.
    n = EncodeContract(c, buf, sizeof(buf));
    buf[wire::kHeader + 4 + 1 + 4 + 4 + 1 + 1 + 2 * 8] = 1;  // low byte of human 2
    CHECK(!DecodeContract(buf, n, d));
    n = EncodeContract(c, buf, sizeof(buf));
    buf[wire::kHeader + 4 + 1 + 4 + 4] = 0x80;  // unknown flag on the wire
    CHECK(!DecodeContract(buf, n, d));
}

static RacePose CarPose(uint8_t slot, float x) {
    RacePose p = {};
    p.slot = slot;
    p.pos[0] = x;
    p.rot[3] = 1;
    p.vel[2] = -3;
    p.nav = (uint16_t)(slot + 1);
    return p;
}

static void TestCars() {
    uint8_t buf[kWireMax];
    RaceCars c = {}, d;
    c.race = 0x99;
    c.tick = 0x10203040;
    c.count = kMaxCars;
    for (int i = 0; i < kMaxCars; ++i) {
        c.cars[i] = CarPose((uint8_t)(i == 0 ? 0 : i + 1), 10.0f * i);  // the host's car and the AI 2..7
        c.cars[i].race = c.race;
        c.cars[i].tick = c.tick;
    }
    c.cars[3].flags = kPoseSnap;
    c.cars[3].fx = (uint8_t)(2 | (15 << 3));  // charge level 2, serial 15, beside the snap flag
    c.cars[4].fx = kFxOff;
    size_t n = EncodeCars(c, buf, sizeof(buf));
    CHECK(n == wire::CarsSize(kMaxCars) && n == 409 && n <= kWireMax && DecodeCars(buf, n, d));
    CHECK(d.cars[3].fx == c.cars[3].fx && d.cars[4].fx == kFxOff && d.cars[0].fx == kFxNone);
    CHECK(d.race == 0x99 && d.tick == 0x10203040 && d.count == kMaxCars);
    for (int i = 0; i < kMaxCars; ++i)
        CHECK(d.cars[i].slot == c.cars[i].slot && d.cars[i].pos[0] == 10.0f * i && d.cars[i].nav == c.cars[i].nav && d.cars[i].race == 0x99 &&
              d.cars[i].tick == 0x10203040 && d.cars[i].vel[2] == -3 && d.cars[i].flags == (i == 3 ? kPoseSnap : 0));
    WireType t;
    uint32_t race;
    CHECK(WirePeek(buf, n, t, race) && t == kMsgCars && race == 0x99);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeCars(buf, k, d));  // every truncation
    RacePose pose;
    CHECK(!DecodePose(buf, n, pose));
    // One car (the host's own on an odd tick).
    c.count = 1;
    n = EncodeCars(c, buf, sizeof(buf));
    CHECK(n == wire::CarsSize(1) && DecodeCars(buf, n, d) && d.count == 1 && d.cars[0].slot == 0);
    buf[wire::kHeader + 4] = 2;  // a count that doesn't match the size
    CHECK(!DecodeCars(buf, n, d));
    // Refused: no cars, one slot twice, a bad pose, a slot out of range.
    c.count = 0;
    CHECK(EncodeCars(c, buf, sizeof(buf)) == 0);
    c.count = 3;
    c.cars[2].slot = c.cars[1].slot;
    CHECK(EncodeCars(c, buf, sizeof(buf)) == 0);
    c.cars[2].slot = 3;
    c.cars[1].pos[1] = NAN;
    CHECK(EncodeCars(c, buf, sizeof(buf)) == 0);
    c.cars[1].pos[1] = 0;
    n = EncodeCars(c, buf, sizeof(buf));
    CHECK(DecodeCars(buf, n, d));
    buf[wire::kHeader + 4 + 1 + wire::kCarSize] = kGridSlots;  // the second car's slot
    CHECK(!DecodeCars(buf, n, d));
    CHECK(EncodeCars(c, buf, wire::CarsSize(3) - 1) == 0);
}

static void TestResults() {
    uint8_t buf[kWireMax];
    RaceResults r = {}, d;
    r.race = 0x77;
    for (int k = 0; k < kGridSlots; ++k) r.cars[k] = {(uint8_t)(k == 1 ? 6 : 1), (uint8_t)(7 - k), 0x00100000u * (k + 1), 0x1000u + k};
    r.cars[5].reason = 0;  // no result for that car
    size_t n = EncodeResults(r, buf, sizeof(buf));
    CHECK(n == wire::kResultsSize && n == 92 && DecodeResults(buf, n, d));
    CHECK(d.race == 0x77 && d.cars[1].reason == 6 && d.cars[5].reason == 0 && d.cars[7].time == 0x00800000u && d.cars[7].best == 0x1007u);
    CHECK(d.cars[0].rank == 7 && d.cars[7].rank == 0);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeResults(buf, k, d));
    buf[wire::kHeader + 10 * 2] = 7;  // car 2: unknown reason
    CHECK(!DecodeResults(buf, n, d));
    buf[wire::kHeader + 10 * 2] = 1;
    buf[wire::kHeader + 10 * 2 + 1] = 8;  // car 2: a place beyond the grid
    CHECK(!DecodeResults(buf, n, d));
    r.cars[3].rank = r.cars[4].rank;  // one place twice
    CHECK(EncodeResults(r, buf, sizeof(buf)) == 0);
    for (RaceResultEntry& e : r.cars) e.rank = kRankNone;  // no places at all: fine
    CHECK(DecodeResults(buf, EncodeResults(r, buf, sizeof(buf)), d) && d.cars[3].rank == kRankNone);
    r.race = 0;
    CHECK(EncodeResults(r, buf, sizeof(buf)) == 0);
}

static void TestSmall() {
    uint8_t buf[kWireMax];
    uint32_t race;
    uint8_t stage, slot, reason;
    size_t n = EncodeLoaded(7, kStageCountdown, 5, buf, sizeof(buf));
    CHECK(n == wire::kLoadedSize && DecodeLoaded(buf, n, race, stage, slot) && race == 7 && stage == kStageCountdown && slot == 5);
    CHECK(!DecodeGo(buf, n, race, stage));  // a Loaded is not a Go
    CHECK(EncodeLoaded(7, kStageLoaded, 8, buf, sizeof(buf)) == 0);
    CHECK(EncodeLoaded(7, 3, 1, buf, sizeof(buf)) == 0);
    n = EncodeGo(9, kStageLoaded, buf, sizeof(buf));
    CHECK(n == wire::kGoSize && DecodeGo(buf, n, race, stage) && race == 9 && stage == kStageLoaded);
    buf[wire::kHeader] = 0;
    CHECK(!DecodeGo(buf, n, race, stage));
    CHECK(EncodeGo(0, kStageLoaded, buf, sizeof(buf)) == 0);
    n = EncodeAbort(9, kAbortLost, buf, sizeof(buf));
    CHECK(n == wire::kAbortSize && DecodeAbort(buf, n, race, reason) && reason == kAbortLost);
    buf[wire::kHeader] = 99;
    CHECK(!DecodeAbort(buf, n, race, reason));
    // IntroVote: the race and the voter's slot (a human's: below kMaxHumans).
    n = EncodeIntroVote(11, 2, buf, sizeof(buf));
    CHECK(n == wire::kIntroVoteSize && DecodeIntroVote(buf, n, race, slot) && race == 11 && slot == 2);
    CHECK(!DecodeAbort(buf, n, race, reason) && !DecodeGo(buf, n, race, stage));  // same size, another type
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeIntroVote(buf, k, race, slot));
    buf[wire::kHeader] = kMaxHumans;
    CHECK(!DecodeIntroVote(buf, n, race, slot));
    CHECK(EncodeIntroVote(11, kMaxHumans, buf, sizeof(buf)) == 0 && EncodeIntroVote(0, 0, buf, sizeof(buf)) == 0);
    // Voice: a player's racer's driver line (the shared kinds only; 2D or 3D; a wait up to 30 s).
    RaceVoice v = {12, 3, 6, 0, 3000}, w = {};
    n = EncodeVoice(v, buf, sizeof(buf));
    CHECK(n == wire::kVoiceSize && DecodeVoice(buf, n, w) && w.race == 12 && w.slot == 3 && w.kind == 6 && w.mode == 0 && w.wait == 3000);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeVoice(buf, k, w));
    buf[wire::kHeader + 1] = 2;  // the podium's RCE_WON: each screen's own
    CHECK(!DecodeVoice(buf, n, w));
    for (uint8_t kind : {0, 1, 4, 5}) {
        v.kind = kind;
        v.mode = 1;
        CHECK(EncodeVoice(v, buf, sizeof(buf)) == n && DecodeVoice(buf, n, w) && w.kind == kind && w.mode == 1);
    }
    for (uint8_t kind : {2, 3, 7}) {
        v.kind = kind;
        CHECK(EncodeVoice(v, buf, sizeof(buf)) == 0);
    }
    v.kind = 0;
    v.mode = 2;
    CHECK(EncodeVoice(v, buf, sizeof(buf)) == 0);
    v.mode = 0;
    v.wait = 30001;
    CHECK(EncodeVoice(v, buf, sizeof(buf)) == 0);
    v.wait = 0;
    v.slot = kMaxHumans;
    CHECK(EncodeVoice(v, buf, sizeof(buf)) == 0);
    // ItemLine: a hit the sender's own game saw: its car spins (1) / stopped (0); an All-Star's touch names the All-Star user's
    // car (never its own).
    RaceItemLine l = {13, kLineHit, 2, 1}, m = {};
    n = EncodeItemLine(l, buf, sizeof(buf));
    CHECK(n == wire::kItemLineSize && DecodeItemLine(buf, n, m) && m.race == 13 && m.kind == kLineHit && m.slot == 2 && m.other == 1);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeItemLine(buf, k, m));
    l.other = 0;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == n && DecodeItemLine(buf, n, m) && m.other == 0);
    l.other = 3;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);  // a spin report says spinning or not, nothing else
    l.kind = kLineAllStarHit;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == n && DecodeItemLine(buf, n, m) && m.kind == kLineAllStarHit && m.slot == 2 && m.other == 3);
    l.other = 2;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);  // its own car
    l.other = kGridSlots;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);
    l.other = 0xff;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);  // an All-Star's touch names its user
    l.kind = 2;
    l.other = 3;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);
    l.kind = kLineHit;
    l.other = 1;
    l.slot = kMaxHumans;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);
    l.slot = 0;
    l.race = 0;
    CHECK(EncodeItemLine(l, buf, sizeof(buf)) == 0);
    // The All-Star phase body: the phase in the low byte; Opa-Opa's weapon + 1 in the next once known (0-2, or the game's 4
    // "none"); anything else reads as not known.
    CHECK(AllStarPhaseBody(2, kNoWeapon) == 2 && AllStarPhaseOf(2) == 2 && AllStarWeaponOf(2) == kNoWeapon);
    CHECK(AllStarPhaseBody(3, 2) == (3u | 3u << 8) && AllStarPhaseOf(AllStarPhaseBody(3, 2)) == 3 && AllStarWeaponOf(AllStarPhaseBody(3, 2)) == 2);
    CHECK(AllStarWeaponOf(AllStarPhaseBody(4, 0)) == 0 && AllStarWeaponOf(AllStarPhaseBody(4, 3)) == kNoWeapon && AllStarWeaponOf(4u | 9u << 8) == kNoWeapon);
    CHECK(AllStarPhaseBody(4, kWeaponNone) == (4u | 5u << 8) && AllStarWeaponOf(AllStarPhaseBody(4, kWeaponNone)) == kWeaponNone);
    CHECK(AllStarWeaponOf(4u | 4u << 8) == kNoWeapon && AllStarWeaponOf(4u | 6u << 8) == kNoWeapon && WeaponValid(4) && !WeaponValid(3) && !WeaponValid(-1));
    CHECK(AllStarPhaseOf(0xffffff05) == 5);
}

static void TestPose() {
    uint8_t buf[kWireMax];
    RacePose p = {}, q;
    p.race = 1;
    p.slot = 7;
    p.flags = kPoseSnap;
    p.tick = 0xfffffff0u;
    float v[10] = {1.5f, -2, 3, 0, 0, 0.7071f, 0.7071f, 10, 0, -1};
    memcpy(p.pos, v, sizeof(v));
    p.nav = 0x1234;
    size_t n = EncodePose(p, buf, sizeof(buf));
    CHECK(n == wire::kPoseSize && n == 72 && DecodePose(buf, n, q));
    CHECK(q.slot == 7 && q.flags == kPoseSnap && q.tick == 0xfffffff0u && q.pos[0] == 1.5f && q.rot[3] == 0.7071f && q.vel[2] == -1);
    CHECK(q.nav == 0x1234 && buf[n - 2] == 0x34 && buf[n - 1] == 0x12);  // last on the wire, little-endian
    p.nav = kNavNone;
    CHECK(DecodePose(buf, EncodePose(p, buf, sizeof(buf)), q) && q.nav == kNavNone);
    buf[5] = 1;  // a version-1 pose (no nav field) is refused
    CHECK(!DecodePose(buf, n, q));
    p.nav = 0x1234;
    p.pos[1] = NAN;
    CHECK(EncodePose(p, buf, sizeof(buf)) == 0);
    p.pos[1] = 0;
    float r0 = p.rot[3];
    p.rot[2] = p.rot[3] = 0;  // not a rotation
    CHECK(EncodePose(p, buf, sizeof(buf)) == 0);
    p.rot[2] = 0.7071f;
    p.rot[3] = r0;
    p.pos[0] = 2e5f;  // far outside any course
    CHECK(EncodePose(p, buf, sizeof(buf)) == 0);
    p.pos[0] = 1.5f;
    CHECK(EncodePose(p, buf, sizeof(buf)) != 0);
    p.pos[1] = 0;
    p.flags = 2;  // unknown flag
    CHECK(EncodePose(p, buf, sizeof(buf)) == 0);
    // The exhaust fx rides in the flags byte's bits 1-7, beside the snap flag; the size is unchanged.
    p.flags = kPoseSnap;
    CHECK(p.fx == kFxNone);  // the default: not known
    n = EncodePose(p, buf, sizeof(buf));
    CHECK(n == 72 && DecodePose(buf, n, q) && q.fx == kFxNone && q.flags == kPoseSnap);
    p.fx = (uint8_t)(4 | (9 << 3));  // the flame, serial 9
    n = EncodePose(p, buf, sizeof(buf));
    CHECK(n == 72 && buf[wire::kHeader + 1] == (uint8_t)(kPoseSnap | (p.fx << 1)));
    CHECK(DecodePose(buf, n, q) && q.fx == p.fx && q.flags == kPoseSnap && FxState(q.fx) == 4 && FxSerial(q.fx) == 9);
    p.flags = 0;
    p.fx = kFxOff;
    CHECK(DecodePose(buf, EncodePose(p, buf, sizeof(buf)), q) && q.fx == kFxOff && q.flags == 0);
}

static void TestFinished() {
    uint8_t buf[kWireMax];
    RaceFinish f = {0x55, 1, 1, 0x00c80000u, 0x00320000u}, g;
    size_t n = EncodeFinished(f, buf, sizeof(buf));
    CHECK(n == wire::kFinishedSize && n == 22 && DecodeFinished(buf, n, g));
    CHECK(g.race == 0x55 && g.slot == 1 && g.reason == 1 && g.time == 0x00c80000u && g.best == 0x00320000u);
    WireType t;
    uint32_t race;
    CHECK(WirePeek(buf, n, t, race) && t == kMsgFinished && race == 0x55);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeFinished(buf, k, g));
    RacePose pose;
    CHECK(!DecodePose(buf, n, pose));  // a Finished is not a Pose
    buf[wire::kHeader + 1] = 7;        // unknown reason
    CHECK(!DecodeFinished(buf, n, g));
    buf[wire::kHeader + 1] = 6;
    buf[wire::kHeader] = kGridSlots;   // slot out of range
    CHECK(!DecodeFinished(buf, n, g));
    f.reason = 0;
    CHECK(EncodeFinished(f, buf, sizeof(buf)) == 0);
    f.reason = 6;
    f.race = 0;
    CHECK(EncodeFinished(f, buf, sizeof(buf)) == 0);
}

// ---------------------------------------------------------------- playout
// A car moving along x at 1 unit per tick, rotating slowly. The sender's tick counter differs
// from ours by `offset`; packets arrive after `delay` ticks plus jitter, sometimes lost/reordered.
static PlayoutPose Truth(double t) {
    PlayoutPose p = {};
    p.pos[0] = (float)(t + 20 * sin(t * 0.05));  // weaving: speed varies 0..2 units per tick
    double a = t * 0.01;
    p.rot[2] = (float)sin(a / 2);
    p.rot[3] = (float)cos(a / 2);
    p.vel[0] = 50;
    return p;
}

struct Scenario {
    int delay, jitter, lossPct, reorderPct;
    uint32_t offset;
    int ticks;
};

// Returns the largest change of the shown car's speed between consecutive ticks after warm-up
// (jerk: the truth's own is at most 0.05), and the largest delay behind the truth.
static double RunScenario(const Scenario& s, unsigned seed, double& maxLag, int& shown) {
    srand(seed);
    Playout play;
    struct InFlight { uint32_t sendTick; int arrive; };
    InFlight q[512];
    int qn = 0;
    double lastX = 0, lastStep = 0, maxStep = 0;
    int have = 0;
    shown = 0;
    maxLag = 0;
    const uint32_t localBase = 1000;
    for (int t = 0; t < s.ticks; ++t) {
        uint32_t remoteTick = (uint32_t)t + s.offset;
        if (rand() % 100 >= s.lossPct && qn < 512) {
            int arrive = t + s.delay + (s.jitter ? rand() % (s.jitter + 1) : 0);
            if (rand() % 100 < s.reorderPct) arrive += 2;
            q[qn++] = {remoteTick, arrive};
        }
        // deliver everything due this tick (any order the network chose)
        for (int i = 0; i < qn;) {
            if (q[i].arrive <= t) {
                uint32_t rt = q[i].sendTick;
                play.Push(rt, localBase + t, Truth((double)(uint32_t)(rt - s.offset)), false);
                q[i] = q[--qn];
            } else {
                ++i;
            }
        }
        PlayoutPose out;
        if (play.Sample(localBase + t, out)) {
            ++shown;
            double step = out.pos[0] - lastX;
            if (have >= 2 && t > 200) {  // jerk: change of speed between consecutive ticks
                double jerk = fabs(step - lastStep);
                if (jerk > maxStep) maxStep = jerk;
            }
            lastStep = step;
            lastX = out.pos[0];
            ++have;
            // how far behind the truth the shown car is, in ticks of travel
            double lag = 0;
            while (lag < 100 && Truth(t - lag).pos[0] > out.pos[0]) lag += 0.25;
            if (t > 200 && lag > maxLag) maxLag = lag;
            float len = 0;
            for (float r : out.rot) len += r * r;
            CHECK(fabsf(len - 1) < 1e-3f);
        }
    }
    return maxStep;
}

static void TestPlayout() {
    double lag;
    int shown;
    // Clean link, 100 ms each way: steps exactly 1 unit per tick, steady delay.
    double step = RunScenario({5, 0, 0, 0, 123456, 1500}, 1, lag, shown);
    printf("  clean: max jerk %.3f, max lag %.2f ticks, shown %d\n", step, lag, shown);
    CHECK(step < 0.06 && lag <= 5 + 3 + 0.3 && shown > 1400);
    // Jittery link (0-6 ticks extra), 5% loss, 5% reordering, counter wrap-around in the sender.
    step = RunScenario({5, 6, 5, 5, 0xffffff00u, 3000}, 2, lag, shown);
    printf("  rough: max jerk %.3f, max lag %.2f ticks, shown %d\n", step, lag, shown);
    CHECK(step < 0.2 && lag < 5 + 15 + 1 && shown > 2900);

    // Sender goes quiet: playback stops after staleTicks, then restarts cleanly.
    Playout p;
    PlayoutPose out;
    for (uint32_t t = 0; t < 100; ++t) {
        p.Push(t, 5000 + t, Truth(t), false);
        p.Sample(5000 + t, out);
    }
    CHECK(p.Sample(5100, out));
    CHECK(!p.Sample(5100 + 60, out));       // > 50 ticks since the last arrival
    for (uint32_t t = 0; t < 10; ++t) p.Push(400 + t, 5200 + t, Truth(400 + t), false);
    CHECK(p.Sample(5212, out) && out.pos[0] > 390);  // started again on the new data

    // Duplicates and too-old poses are refused.
    Playout d;
    CHECK(d.Push(10, 1, Truth(10), false));
    CHECK(!d.Push(10, 2, Truth(10), false));
    CHECK(d.Push(12, 3, Truth(12), false));
    CHECK(d.Push(11, 4, Truth(11), false));   // late but fits between kept poses
    CHECK(!d.Push(9, 5, Truth(9), false));    // older than everything kept
    CHECK(d.Count() == 3);

    // A snap pose is not blended across: the shown car stays at the old pose until the snap tick.
    Playout s;
    for (uint32_t t = 0; t < 40; ++t) {
        PlayoutPose pose = Truth(t);
        bool snap = t == 30;
        if (t >= 30) pose.pos[0] += 1000;
        s.Push(t, 100 + t, pose, snap);
        if (s.Sample(100 + t, out)) CHECK(out.pos[0] < 100 || out.pos[0] > 1000);  // never in between
    }

    // The nav path rides along unblended: the shown nav is that of a pose next to the shown position.
    Playout n;
    int navShown[2] = {};
    for (uint32_t t = 0; t < 60; ++t) {
        PlayoutPose pose = Truth(t);
        pose.nav = t < 30 ? 4 : 9;
        n.Push(t, 700 + t, pose, false);
        if (!n.Sample(700 + t, out)) continue;
        CHECK(out.nav == 4 || out.nav == 9);
        CHECK(!(out.nav == 4 && navShown[1]));  // once on the new path, never back
        ++navShown[out.nav == 9];
        if (out.nav == 4) CHECK(out.pos[0] <= (Truth(29).pos[0] + Truth(30).pos[0]) / 2 + 1e-3f);
    }
    CHECK(navShown[0] > 20 && navShown[1] > 20);

    // The host's AI (every 2nd tick) shown with its own car's clock: the same moment of the host's race
    // as its car (every tick), over a jittery link where both arrive in the same messages.
    srand(7);
    Playout car, ai;
    int compared = 0;
    double worst = 0;
    struct Msg { uint32_t tick; int arrive; };
    std::vector<Msg> flight;
    for (int t = 0; t < 2000; ++t) {
        flight.push_back({(uint32_t)t, t + 4 + rand() % 5});
        for (size_t i = 0; i < flight.size();) {
            if (flight[i].arrive > t) { ++i; continue; }
            uint32_t rt = flight[i].tick;
            car.Push(rt, 9000 + t, Truth(rt), false);
            if (rt % 2 == 0) {
                PlayoutPose a = Truth(rt);
                a.pos[0] += 500;  // the AI car: the same motion, 500 units further on
                ai.Push(rt, 9000 + t, a, false);
            }
            flight.erase(flight.begin() + i);
        }
        PlayoutPose oc, oa;
        bool sc = car.Sample(9000 + t, oc), sa = ai.Sample(9000 + t, oa, &car);
        if (sc && sa && t > 300) {
            ++compared;
            double gap = fabs((oa.pos[0] - 500) - oc.pos[0]);
            if (gap > worst) worst = gap;
            // The tick on screen: the car shown lies between that tick's pose and the next one's.
            uint32_t shown = 0;
            CHECK(car.ShownTick(shown) && shown < (uint32_t)t);
            CHECK(Truth(shown).pos[0] <= oc.pos[0] + 1e-3f && oc.pos[0] <= Truth(shown + 1).pos[0] + 1e-3f);
        }
    }
    printf("  shared clock: AI vs car worst gap %.3f units over %d ticks\n", worst, compared);
    CHECK(compared > 1600 && worst < 0.05);  // interpolation error only (the truth moves 0..2 units per tick)
    uint32_t none;
    CHECK(!Playout().ShownTick(none));  // nothing shown yet
}

static void TestEvent() {
    uint8_t buf[kWireMax];
    RaceEvent e = {}, d;
    e.race = 0x12345678;
    e.tick = 4242;
    e.kind = 1;
    e.slot = 3;
    e.size = EventBodySize(1);
    for (int i = 0; i < e.size; ++i) e.body[i] = (uint8_t)(i * 7 + 1);
    size_t n = EncodeEvent(e, buf, sizeof(buf));
    CHECK(n == wire::kHeader + 6 + 0xd8 && n <= kWireMax);
    CHECK(DecodeEvent(buf, n, d));
    CHECK(d.race == e.race && d.tick == 4242 && d.kind == 1 && d.slot == 3 && d.size == 0xd8 && !memcmp(d.body, e.body, 0xd8));
    WireType t;
    uint32_t race;
    CHECK(WirePeek(buf, n, t, race) && t == kMsgEvent && race == e.race);
    for (size_t k = 0; k < n; ++k) CHECK(!DecodeEvent(buf, k, d));  // every truncation
    // Every kind has its own size: a kind-1 body labelled kind 0 (or a kind that stays home) is refused.
    buf[wire::kHeader + 4] = 0;
    CHECK(!DecodeEvent(buf, n, d));
    buf[wire::kHeader + 4] = 7;
    CHECK(!DecodeEvent(buf, n, d));
    buf[wire::kHeader + 4] = 2;
    CHECK(DecodeEvent(buf, n, d) && d.kind == 2);
    buf[wire::kHeader + 5] = kGridSlots;  // no such car
    CHECK(!DecodeEvent(buf, n, d));
    // Small kinds round-trip; carried kinds only.
    const uint8_t carried[] = {0, 5, 6, 10, 11, kEventAllStarPhase, kEventReaction, kEventAllStarShot, kEventAllStarGate};
    // The gate All-Stars' shot: class | count | flags in the head, two payload words (12 bytes).
    CHECK(PseudoEvent(kEventAllStarGate) && EventBodySize(kEventAllStarGate) == 12 && AllStarGateHead(5, 3, kGateFlagHold) == (5u | 3u << 8 | 1u << 16) &&
          AllStarGateClass(AllStarGateHead(5, 3, kGateFlagHold)) == 5 && AllStarGateFlags(AllStarGateHead(5, 3, kGateFlagHold)) == kGateFlagHold &&
          AllStarGateFlags(AllStarGateHead(0, 255, 0)) == 0 && AllStarGateClass(AllStarGateHead(2, 255, 0)) == 2);
    CHECK(PseudoEvent(kEventAllStarShot) && AllStarShotBody(2, 7) == (3u | 7u << 8) && AllStarShotWeapon(AllStarShotBody(0, 1)) == 0 &&
          AllStarShotWeapon(AllStarShotBody(1, 255)) == 1 && AllStarShotWeapon(0) == kNoWeapon && AllStarShotWeapon(4) == kNoWeapon);
    CHECK(AllStarPulseBody(2, 5) == (0x12u | 5u << 8) && AllStarPulseClass(AllStarPulseBody(2, 5)) == 2 && AllStarPulseClass(AllStarShotBody(1, 1)) < 0 &&
          AllStarShotWeapon(AllStarPulseBody(0, 1)) == kNoWeapon && AllStarShotKind(AllStarPulseBody(0, 9)) == 0x10 && AllStarPulseClass(0x20) < 0);
    for (uint8_t kind : carried) {
        RaceEvent s = e;
        s.kind = kind;
        s.size = EventBodySize(kind);
        n = EncodeEvent(s, buf, sizeof(buf));
        CHECK(n == wire::kHeader + 6 + s.size && DecodeEvent(buf, n, d) && d.kind == kind && !memcmp(d.body, s.body, s.size));
    }
    for (uint8_t kind : {7, 8, 9, 12, 13, 15, 16, 17, 255}) CHECK(EventBodySize(kind) == 0);
    RaceEvent bad = e;
    bad.size = 4;  // not kind 1's size
    CHECK(EncodeEvent(bad, buf, sizeof(buf)) == 0);
    bad = e;
    bad.race = 0;
    CHECK(EncodeEvent(bad, buf, sizeof(buf)) == 0);
    CHECK(EncodeEvent(e, buf, wire::EventSize(e.size) - 1) == 0);  // too small an output buffer
    // The item flag needs the host's AI.
    RaceContract c = SampleContract();
    c.flags = kContractHostAi | kContractItems;
    CHECK(EncodeContract(c, buf, sizeof(buf)) != 0);
    c.flags = kContractItems;
    CHECK(EncodeContract(c, buf, sizeof(buf)) == 0);
}

int main() {
    TestContract();
    TestEvent();
    TestSmall();
    TestPose();
    TestFinished();
    TestCars();
    TestResults();
    TestPlayout();
    printf("race wire / playout: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
