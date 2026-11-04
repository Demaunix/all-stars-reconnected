// Fuzzing every decoder of what other players and Steam lobbies send (the user's safety priority, 2026-10-25): the race
// messages (race_wire.h), voice packets (voice.h), lobby data ("mods", "rules", "kick"/"banned", the figure keys) and the
// +connect_lobby argument. Random and mutated inputs, each in a heap buffer of exactly its size, so the address sanitizer
// (/fsanitize=address) stops at the first byte read past it. A decoder may refuse anything; what it accepts must stay in
// range and encode back to the very same bytes (only well-formed values get through to the game).
//
//   fuzz_decoders.exe [iterations] [seed]      (defaults: 300000, 1)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "race_wire.h"
#include "voice.h"
#include "lobby.h"
#include "modset.h"
#include "parse.h"
#include "figure_presence.h"

static int g_fail = 0;
static long long g_accepted[32] = {};
#define CHECK(c, what) do { if (!(c)) { if (g_fail++ < 20) printf("FAIL %s: %s (iteration %lld)\n", what, #c, g_iter); } } while (0)
static long long g_iter = 0;

// xorshift64*: the same run for the same seed.
static uint64_t g_rng = 1;
static uint64_t Next() {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 0x2545F4914F6CDD1Dull;
}
static uint32_t Below(uint32_t n) { return n ? (uint32_t)(Next() % n) : 0; }
static uint8_t Byte() { return (uint8_t)Next(); }

// A float that is often a corner case.
static float AnyFloat() {
    switch (Below(8)) {
    case 0: return 0.0f;
    case 1: { uint32_t b = 0x7fc00000; float f; memcpy(&f, &b, 4); return f; }      // NaN
    case 2: { uint32_t b = 0x7f800000; float f; memcpy(&f, &b, 4); return f; }      // +inf
    case 3: { uint32_t b = (uint32_t)Next(); float f; memcpy(&f, &b, 4); return f; } // any bits
    case 4: return (float)(int32_t)Below(200001) - 100000.0f;
    default: return ((float)Below(2000001) - 1000000.0f) / 1000.0f;
    }
}

// The input in a buffer of exactly `n` bytes (malloc(0) for none): a read past it is a sanitizer error.
struct Exact {
    uint8_t* p;
    size_t n;
    Exact(const uint8_t* src, size_t size) : p((uint8_t*)malloc(size ? size : 1)), n(size) { if (size) memcpy(p, src, size); }
    ~Exact() { free(p); }
};

// ---------------------------------------------------------------- race messages
// Every decoder on one input; each accepted one must re-encode to the same bytes.
static void DecodeAll(const uint8_t* src, size_t size) {
    Exact in(src, size);
    uint8_t out[kWireMax + 64];
    auto same = [&](size_t n) { return n == size && memcmp(out, in.p, size) == 0; };
    {
        RaceContract c;
        if (DecodeContract(in.p, in.n, c)) { ++g_accepted[kMsgContract]; CHECK(ContractValid(c) && same(EncodeContract(c, out, sizeof(out))), "contract"); }
    }
    {
        uint32_t race; uint8_t stage, slot;
        if (DecodeLoaded(in.p, in.n, race, stage, slot)) { ++g_accepted[kMsgLoaded]; CHECK(slot < kGridSlots && same(EncodeLoaded(race, stage, slot, out, sizeof(out))), "loaded"); }
        if (DecodeGo(in.p, in.n, race, stage)) { ++g_accepted[kMsgGo]; CHECK(same(EncodeGo(race, stage, out, sizeof(out))), "go"); }
        uint8_t reason;
        if (DecodeAbort(in.p, in.n, race, reason)) { ++g_accepted[kMsgAbort]; CHECK(same(EncodeAbort(race, (AbortReason)reason, out, sizeof(out))), "abort"); }
        if (DecodeIntroVote(in.p, in.n, race, slot)) { ++g_accepted[kMsgIntroVote]; CHECK(slot < kMaxHumans && same(EncodeIntroVote(race, slot, out, sizeof(out))), "intro vote"); }
        if (DecodeWrongWay(in.p, in.n, race, slot)) { ++g_accepted[kMsgWrongWay]; CHECK(slot < kMaxHumans && same(EncodeWrongWay(race, slot, out, sizeof(out))), "wrong way"); }
    }
    {
        RaceVoice v;
        if (DecodeVoice(in.p, in.n, v)) { ++g_accepted[kMsgVoice]; CHECK(v.slot < kMaxHumans && same(EncodeVoice(v, out, sizeof(out))), "voice line"); }
        RaceItemLine l;
        if (DecodeItemLine(in.p, in.n, l)) { ++g_accepted[kMsgItemLine]; CHECK(ItemLineValid(l) && same(EncodeItemLine(l, out, sizeof(out))), "item line"); }
        RaceAnnounce a;
        if (DecodeAnnounce(in.p, in.n, a)) { ++g_accepted[kMsgAnnounce]; CHECK(a.wait >= 0 && same(EncodeAnnounce(a, out, sizeof(out))), "announce"); }
    }
    {
        RacePose p;
        if (DecodePose(in.p, in.n, p)) { ++g_accepted[kMsgPose]; CHECK(PoseValid(p) && same(EncodePose(p, out, sizeof(out))), "pose"); }
        RaceCars c;
        if (DecodeCars(in.p, in.n, c)) {
            ++g_accepted[kMsgCars];
            CHECK(c.count >= 1 && c.count <= kMaxCars && same(EncodeCars(c, out, sizeof(out))), "cars");
            for (int i = 0; i < c.count; ++i) CHECK(c.cars[i].slot < kGridSlots && PoseValid(c.cars[i]), "cars slot");
        }
    }
    {
        RaceResults r;
        if (DecodeResults(in.p, in.n, r)) { ++g_accepted[kMsgResults]; CHECK(ResultsValid(r) && same(EncodeResults(r, out, sizeof(out))), "results"); }
        RaceFinish f;
        if (DecodeFinished(in.p, in.n, f)) { ++g_accepted[kMsgFinished]; CHECK(f.slot < kGridSlots && same(EncodeFinished(f, out, sizeof(out))), "finished"); }
        RaceEvent e;
        if (DecodeEvent(in.p, in.n, e)) {
            ++g_accepted[kMsgEvent];
            CHECK(e.slot < kGridSlots && e.size <= kEventBodyMax && e.size == EventBodySize(e.kind) && same(EncodeEvent(e, out, sizeof(out))), "event");
            if (FireLaunchValid(e) && (e.kind == 1 || e.kind == 2)) ++g_accepted[24];
        }
    }
    WireType t;
    uint32_t race;
    if (WirePeek(in.p, in.n, t, race)) CHECK(race != 0, "peek");
}

// The exact size the decoder of `type` wants (a Cars or Event size from its count / kind), or a random one.
static size_t SizeFor(uint8_t type) {
    switch (type) {
    case kMsgContract: return wire::kContractSize;
    case kMsgLoaded: return wire::kLoadedSize;
    case kMsgGo: return wire::kGoSize;
    case kMsgPose: return wire::kPoseSize;
    case kMsgAbort: return wire::kAbortSize;
    case kMsgFinished: return wire::kFinishedSize;
    case kMsgCars: return wire::CarsSize((int)Below(kMaxCars + 2));
    case kMsgResults: return wire::kResultsSize;
    case kMsgEvent: { static const uint8_t kinds[] = {0, 1, 2, 3, 4, 5, 6, 10, 11, 0x40, 0x41, 0x42, 0x44, 7, 0xff};
                      return wire::EventSize(EventBodySize(kinds[Below(sizeof(kinds))])); }
    case kMsgIntroVote: case kMsgWrongWay: return wire::kIntroVoteSize;
    case kMsgAnnounce: return wire::kAnnounceSize;
    case kMsgVoice: return wire::kVoiceSize;
    case kMsgItemLine: return wire::kItemLineSize;
    default: return Below(kWireMax + 16);
    }
}

// A header that passes Open for some type, then a body of bytes that are often in range.
static void FuzzWellFramed() {
    uint8_t buf[kWireMax + 64] = {};
    uint8_t type = Below(10) ? (uint8_t)(1 + Below(14)) : Byte();
    size_t size = Below(8) ? SizeFor(type) : Below(kWireMax + 32);
    if (size > sizeof(buf)) size = sizeof(buf);
    for (size_t i = 0; i < size; ++i) buf[i] = Below(3) ? (uint8_t)Below(9) : Byte();  // small values pass the range checks
    if (size >= wire::kHeader) {
        uint32_t magic = Below(16) ? kWireMagic : (uint32_t)Next();
        memcpy(buf, &magic, 4);
        buf[4] = type;
        buf[5] = Below(16) ? kWireVersion : Byte();
        uint16_t s = Below(16) ? (uint16_t)size : (uint16_t)Next();
        memcpy(buf + 6, &s, 2);
        uint32_t race = Below(16) ? 0x1234 + Below(3) : (uint32_t)Next();
        memcpy(buf + 8, &race, 4);
    }
    // Floats where a pose has them: often corner cases.
    if ((type == kMsgPose || type == kMsgCars) && Below(2))
        for (size_t at = wire::kHeader; at + 4 <= size; at += 4) { float f = Below(3) ? AnyFloat() : 0.5f; memcpy(buf + at, &f, 4); }
    DecodeAll(buf, size);
}

// A valid message of a random type, then mutated: bytes changed, cut short, or grown.
static size_t ValidMessage(uint8_t* out, size_t cap) {
    uint32_t race = 1 + Below(0xfffffffe);
    switch (Below(13)) {
    case 0: {
        RaceContract c = {};
        c.race = race; c.seed = (uint32_t)Next(); c.course = Byte(); c.courseHash = (uint32_t)Next();
        c.laps = (uint8_t)(1 + Below(9)); c.items = (uint8_t)Below(2); c.catchup = (uint8_t)Below(2); c.ai = (uint8_t)Below(4);
        c.flags = (uint8_t)(Below(2) ? kContractHostAi | kContractItems : Below(2) * kContractAnnouncer) | (uint8_t)(Below(2) * kContractNoDriftPlus);
        c.humanCount = (uint8_t)(2 + Below(kMaxHumans - 1));
        for (int i = 0; i < c.humanCount; ++i) c.humans[i] = 76561197960265728ull + (uint64_t)i * 7 + 1;
        for (int i = 0; i < kGridSlots; ++i) c.roster[i] = 0x10000 + (uint32_t)i * 3 + 1;
        return EncodeContract(c, out, cap);
    }
    case 1: return EncodeLoaded(race, (uint8_t)(1 + Below(2)), (uint8_t)Below(kGridSlots), out, cap);
    case 2: return EncodeGo(race, (uint8_t)(1 + Below(2)), out, cap);
    case 3: return EncodeAbort(race, (AbortReason)(1 + Below(4)), out, cap);
    case 4: {
        RacePose p = {};
        p.race = race; p.slot = (uint8_t)Below(kGridSlots); p.flags = (uint8_t)Below(2); p.tick = (uint32_t)Next();
        p.rot[3] = 1; p.pos[0] = ((float)Below(200001) - 100000.0f); p.vel[1] = ((float)Below(20001) - 10000.0f) / 1.5f;
        p.nav = (uint16_t)Next(); p.fx = (uint8_t)Below(128);
        return EncodePose(p, out, cap);
    }
    case 5: {
        RaceCars c = {};
        c.race = race; c.tick = (uint32_t)Next(); c.count = (uint8_t)(1 + Below(kMaxCars));
        for (int i = 0; i < c.count; ++i) { c.cars[i].race = race; c.cars[i].tick = c.tick; c.cars[i].slot = (uint8_t)i; c.cars[i].rot[3] = 1; }
        return EncodeCars(c, out, cap);
    }
    case 6: {
        RaceResults r = {};
        r.race = race;
        for (int i = 0; i < kGridSlots; ++i) { r.cars[i].reason = (uint8_t)Below(7); r.cars[i].rank = (uint8_t)i; r.cars[i].time = (uint32_t)Next(); }
        return EncodeResults(r, out, cap);
    }
    case 7: { RaceFinish f = {race, (uint8_t)Below(kGridSlots), (uint8_t)(1 + Below(6)), (uint32_t)Next(), (uint32_t)Next()}; return EncodeFinished(f, out, cap); }
    case 8: {
        static const uint8_t kinds[] = {0, 1, 2, 3, 4, 5, 6, 10, 11, 0x40, 0x41, 0x42, 0x44};
        RaceEvent e = {};
        e.race = race; e.tick = (uint32_t)Next(); e.kind = kinds[Below(sizeof(kinds))]; e.slot = (uint8_t)Below(kGridSlots);
        e.size = EventBodySize(e.kind);
        for (uint16_t i = 0; i < e.size; ++i) e.body[i] = Byte();
        return EncodeEvent(e, out, cap);
    }
    case 9: { RaceVoice v = {race, (uint8_t)Below(kMaxHumans), (uint8_t)(Below(2) ? 0 : 6), (uint8_t)Below(2), (uint16_t)Below(30001)}; return EncodeVoice(v, out, cap); }
    case 10: { RaceItemLine l = {race, kLineHit, (uint8_t)Below(kMaxHumans), (uint8_t)Below(2)}; return EncodeItemLine(l, out, cap); }
    case 11: { RaceAnnounce a = {race, (uint32_t)Next(), 1 + (uint32_t)Below(0xfffffffe), (uint32_t)Next(), (int32_t)Below(120001), (uint8_t)Below(2)}; return EncodeAnnounce(a, out, cap); }
    default: return EncodeWrongWay(race, (uint8_t)Below(kMaxHumans), out, cap);
    }
}

static void FuzzMutated() {
    uint8_t buf[kWireMax + 64] = {};
    size_t n = ValidMessage(buf, kWireMax);
    if (!n) { ++g_fail; printf("FAIL: a valid message didn't encode (iteration %lld)\n", g_iter); return; }
    DecodeAll(buf, n);  // the unchanged message is accepted by its decoder (counted above)
    int edits = 1 + (int)Below(4);
    for (int i = 0; i < edits; ++i) {
        switch (Below(4)) {
        case 0: buf[Below((uint32_t)n)] = Byte(); break;
        case 1: buf[Below((uint32_t)n)] ^= (uint8_t)(1 << Below(8)); break;
        case 2: n = Below((uint32_t)n + 1); break;                                       // cut short
        default: { size_t grow = Below(16); for (size_t k = 0; k < grow && n < kWireMax + 32; ++k) buf[n++] = Byte(); }  // grown
        }
        if (!n) break;
    }
    DecodeAll(buf, n);
}

static void FuzzRandomBytes() {
    uint8_t buf[kWireMax + 64];
    size_t n = Below(sizeof(buf));
    for (size_t i = 0; i < n; ++i) buf[i] = Byte();
    DecodeAll(buf, n);
}

// ---------------------------------------------------------------- voice packets
static void FuzzVoice() {
    std::vector<uint8_t> v(Below(10) ? Below(64) : Below(kVoicePacketMax + 32));
    for (uint8_t& b : v) b = Byte();
    if (v.size() >= 5 && Below(2)) { memcpy(v.data(), "ASV1", 4); v[4] = kVoiceVersion; }
    Exact in(v.data(), v.size());
    uint16_t seq;
    const uint8_t* data = nullptr;
    uint32_t len = 0;
    if (VoiceDecode(in.p, in.n, seq, data, len)) {
        ++g_accepted[20];
        CHECK(data >= in.p && len > 0 && len <= kVoiceDataMax && data + len == in.p + in.n, "voice packet");
    }
}

// ---------------------------------------------------------------- lobby data (text)
// A string from an alphabet that hits the parsers' separators and digits often.
static std::string AnyText(size_t maxLen, const char* alphabet) {
    std::string s;
    size_t n = Below((uint32_t)maxLen + 1);
    size_t k = strlen(alphabet);
    for (size_t i = 0; i < n; ++i) s += Below(16) ? alphabet[Below((uint32_t)k)] : (char)(1 + Below(255));
    return s;
}

static void FuzzLobbyText() {
    // "mods": id|version|plan|runtime;...
    std::string m = AnyText(Below(4) ? 400 : 9000, "ab-|;|;0123456789abcdef");
    if (Below(3) == 0) {  // a well-formed list, then maybe changed
        m.clear();
        int count = 1 + (int)Below(4);
        for (int i = 0; i < count; ++i) {
            if (i) m += ';';
            m += "mod" + std::to_string(Below(5)) + "|1." + std::to_string(Below(9)) + "|" + std::string(64, "0af"[Below(3)]) + "|" + std::string(64, "1b"[Below(2)]);
        }
        if (Below(2)) m[Below((uint32_t)m.size())] = "|;x"[Below(3)];
    }
    std::vector<ModEntry> mods;
    if (DecodeMods(m, mods)) {
        ++g_accepted[21];
        CHECK(EncodeMods(mods) == m, "mods round trip");
        for (const ModEntry& e : mods) CHECK(!e.id.empty() && e.plan.size() == 64 && e.runtime.size() == 64, "mods entry");
        std::vector<ModEntry> other;
        DecodeMods(m, other);
        DescribeModDifferences(mods, other);  // (the join refusal's text: must not throw on any accepted list)
        RefusedMods(mods, other);
    }
    // "rules", "kick", "banned", +connect_lobby: C strings.
    std::string r = AnyText(40, "0123456789,-+ ");
    Exact rules((const uint8_t*)r.c_str(), r.size() + 1);
    int seats = PublicSeats((const char*)rules.p);
    CHECK(seats >= 1 && seats <= kConsoleSeats, "public seats");
    std::string k = AnyText(40, "0123456789");
    Exact kick((const uint8_t*)k.c_str(), k.size() + 1);
    uint64_t id = ParseSteamId((const char*)kick.p);
    if (id) {
        ++g_accepted[22];
        CHECK(std::to_string(id) == k || k.size() > 1 && k[0] == '0', "steam id");
        std::string list = AnyText(200, "0123456789,") + "," + k + "," + AnyText(30, "0123456789,");
        Exact banned((const uint8_t*)list.c_str(), list.size() + 1);
        CHECK(k.size() >= 24 || IdListHas((const char*)banned.p, id), "banned list");
    }
    std::string list = AnyText(300, "0123456789,");
    Exact banned((const uint8_t*)list.c_str(), list.size() + 1);
    IdListHas((const char*)banned.p, 76561197960265729ull);
    std::string c = (Below(2) ? "+connect_lobby " : "") + AnyText(40, "0123456789 +connect_lobby");
    Exact connect((const uint8_t*)c.c_str(), c.size() + 1);
    LobbyFromConnectString((const char*)connect.p);
}

// The figure keys: a header "1;b13" / "1;d573;1a2b3c4d" and base64 chunks.
static void FuzzFigure() {
    std::string header, chunk[kFigureChunks];
    if (Below(3) == 0) {  // a real design, then maybe changed
        FigureDesign d;
        d.builtin = -1;
        d.descSize = (int32_t)(1 + Below(kFigureDescMax));
        for (int i = 0; i < d.descSize; ++i) d.desc[i] = Byte();
        header = EncodeFigureHeader(d);
        std::string ch[kFigureChunks];
        EncodeFigureChunks(d, ch);
        for (int i = 0; i < kFigureChunks; ++i) chunk[i] = ch[i];
        if (Below(2)) {
            std::string& s = Below(2) ? header : chunk[Below(kFigureChunks)];
            if (!s.empty()) s[Below((uint32_t)s.size())] = "0;=Ab+/"[Below(7)];
        }
    } else {
        header = AnyText(30, "0123456789;bd abcdef");
        for (int i = 0; i < kFigureChunks; ++i) chunk[i] = Below(3) ? AnyText(kFigureChunkChars + 8, "ABCxyz019+/=") : "";
    }
    std::vector<Exact*> held;
    const char* ptrs[kFigureChunks];
    for (int i = 0; i < kFigureChunks; ++i) {
        held.push_back(new Exact((const uint8_t*)chunk[i].c_str(), chunk[i].size() + 1));
        ptrs[i] = (const char*)held.back()->p;
    }
    Exact h((const uint8_t*)header.c_str(), header.size() + 1);
    uint32_t version;
    FigureDesign d;
    if (DecodeFigure((const char*)h.p, ptrs, version, d)) {
        ++g_accepted[23];
        // (numbers with leading zeros are accepted, "01;b3": the same design, so compare what it means, not the text)
        uint32_t again = 0;
        FigureDesign d2;
        std::string h2 = EncodeFigureHeader(d, version);
        CHECK(version > 0 && DecodeFigure(h2.c_str(), ptrs, again, d2) && again == version && FigureDesignSame(d, d2), "figure header round trip");
        if (d.builtin < 0) {
            CHECK(d.descSize >= 1 && d.descSize <= kFigureDescMax, "figure size");
            std::string ch[kFigureChunks];
            EncodeFigureChunks(d, ch);
            std::string a, used;
            for (int i = 0; i < kFigureChunks; ++i) a += ch[i];
            for (int i = 0; i < kFigureChunks && !chunk[i].empty(); ++i) used += chunk[i];
            CHECK(a == used, "figure chunks round trip");
        } else {
            CHECK(d.builtin < kFigureBuiltins, "figure builtin");
        }
        FigureFriendRec r;
        r.id = (uint64_t)Next();
        std::string name = AnyText(80, "abc ");
        CopyUtf8Name(r.name, sizeof(r.name), name.c_str());
        r.design = d;
        DescribeFigureFriend(r);
        AsrFigureFriend o;
        FigureFriendOut(r, o);
    }
    for (Exact* e : held) delete e;
    std::vector<uint8_t> bytes;
    std::string b64 = AnyText(64, "ABCabc019+/=");
    if (Base64Decode(b64, bytes)) CHECK(Base64Encode(bytes.data(), bytes.size()) == b64, "base64 round trip");
}

int main(int argc, char** argv) {
    long long iterations = argc > 1 ? _atoi64(argv[1]) : 300000;
    g_rng = argc > 2 ? (uint64_t)_atoi64(argv[2]) * 0x9E3779B97F4A7C15ull + 1 : 1;
    DWORD start = GetTickCount();
    for (g_iter = 0; g_iter < iterations; ++g_iter) {
        switch (g_iter % 6) {
        case 0: FuzzWellFramed(); break;
        case 1: FuzzMutated(); break;
        case 2: FuzzRandomBytes(); break;
        case 3: FuzzVoice(); break;
        case 4: FuzzLobbyText(); break;
        default: FuzzFigure(); break;
        }
    }
    printf("fuzz: %lld inputs in %lu ms; accepted:", iterations, GetTickCount() - start);
    static const char* names[32] = {nullptr, "contract", "loaded", "go", "pose", "abort", "finished", "cars", "results", "event", "intro", "announce",
                                     "wrongway", "voice line", "item line", nullptr, nullptr, nullptr, nullptr, nullptr, "voice packet", "mods", "steam id", "figure",
                                     "fire launch"};
    for (int i = 0; i < 32; ++i)
        if (names[i]) printf(" %s %lld", names[i], g_accepted[i]);
    printf("\nfuzz decoders: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
