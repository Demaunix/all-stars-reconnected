// Friend figures: the Steam side (P1; notes\FIGURE-FRIENDS.md).
// A racer mod can show the player's friends as figures (each player's own look: one of the game's built-in designs today). With
// this mod, every game publishes its player's figure as rich presence (and as lobby member data while in a lobby), the Steam
// worker gathers the friends' figures, and the racer mod's runtime reads them at race start through the AsrFigure* exports
// (steam_mp.cpp). Header-only: the encoding, the decoding, the order and the ABI structs, so tests\test_figure.cpp runs them
// without Steam. Players without such a mod see no change (nothing here touches the game).
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

// ---------------------------------------------------------------- the published value
// Rich presence key kFigureKey = "<version>;b<index>" (a built-in design 0..16) or "<version>;d<bytes>;<crc32>" (a compact
// description of that many bytes, its CRC-32 as 8 hex digits, base64 in kFigureChunkKeys, each chunk at most kFigureChunkChars
// characters, consecutive; P4; the CRC refuses chunks that arrived half-updated). The same strings go into the lobby's member data
// under the same keys. A reader takes any version whose design letter it knows; an unknown letter
// or an unreadable value means "no design": that friend is left out. Steam's limits (isteamfriends.h): 30 keys per player, a key
// 64 characters, a value 256 (with the terminator); member data 8192 bytes a value. This mod uses connect, status and these 4.
constexpr uint32_t kFigureFeatureVersion = 1;
constexpr char kFigureKey[] = "asr_figure";
constexpr int kFigureChunks = 3;
constexpr const char* kFigureChunkKeys[kFigureChunks] = {"asr_fg0", "asr_fg1", "asr_fg2"};
constexpr int kFigureChunkChars = 255;
constexpr int kFigureDescMax = kFigureChunks * kFigureChunkChars / 4 * 3;  // 573 bytes: what 3 chunks of base64 carry
constexpr int kFigureBuiltins = 17;                                        // the built-in figure designs 0..16
constexpr int kFigureDefaultBuiltin = 13;                                  // (the common design today)
constexpr int kFigureFriendsMax = 16;                                      // friends kept for the export (a mod shows up to 4)
static_assert(kFigureDescMax == 573, "the documented limit");

// ---------------------------------------------------------------- the in-process ABI (what a racer mod binds to)
// Exports of SteamMultiplayer.dll (GetModuleHandleW(L"SteamMultiplayer.dll") + GetProcAddress), all extern "C" __cdecl:
//   int AsrFigureQuery(AsrFigureInfo* out, int size);     fills min(size, sizeof(AsrFigureInfo)) bytes; returns sizeof(AsrFigureInfo)
//   int AsrFigureFriends(AsrFigureFriend* out, int max);  the friends with a design, in order; returns how many were written
//                                                         (0..max), or -1 while the feature is off in this game (config.ini
//                                                         [Friends] Enabled, default off) or Steam is not running in it
//   int AsrFigureSetSelf(int builtin);                    this player's figure from the racer mod (0..16; -1 = back to config.ini);
//                                                         returns 1 when taken, 0 when refused or the feature is off; republished
//                                                         within a second
// Later ABIs add functions and never change these structs (kAsrFigureAbi says which ones exist).
constexpr uint32_t kAsrFigureAbi = 1;
#pragma pack(push, 8)
struct AsrFigureFriend {
    uint64_t steamId;         // +0
    char name[64];            // +8   the friend's Steam persona name, UTF-8, NUL-terminated (cut to fit)
    uint32_t featureVersion;  // +72  the version field of its asr_figure value
    int32_t builtin;          // +76  its built-in design 0..16, or -1 when desc holds a description
    int32_t sameLobby;        // +80  1 = a member of this player's Steam Multiplayer lobby
    int32_t descSize;         // +84  bytes valid in desc (0 for a built-in; P4 fills it)
    uint8_t desc[576];        // +88  the compact description, zero beyond descSize
};                            // sizeof 664
struct AsrFigureInfo {
    uint32_t abi;             // +0   kAsrFigureAbi
    uint32_t friendSize;      // +4   sizeof(AsrFigureFriend)
    uint32_t featureVersion;  // +8   the version this mod publishes
    int32_t steamUp;          // +12  1 = Steam runs in this game (the worker is up); else AsrFigureFriends answers -1
    int32_t inLobby;          // +16  1 = this game is in a Steam Multiplayer lobby
    int32_t racingOnline;     // +20  1 = an online race of this mod runs (the original showed no friends in online races: callers show none)
    int32_t selfBuiltin;      // +24  this player's published built-in (-1: a description)
    int32_t selfDescSize;     // +28
    uint64_t selfSteamId;     // +32  0 until Steam is up
    uint32_t friendCount;     // +40  friends with a design known right now (what a big AsrFigureFriends would return)
    int32_t enabled;          // +44  1 = the friend figures are on in this game (config.ini [Friends] Enabled=1); 0 = off: nothing is
                              //      published or gathered, steamUp reads 0 and AsrFigureFriends answers -1
};                            // sizeof 48
#pragma pack(pop)
static_assert(sizeof(AsrFigureFriend) == 664 && offsetof(AsrFigureFriend, name) == 8 && offsetof(AsrFigureFriend, featureVersion) == 72 &&
                  offsetof(AsrFigureFriend, builtin) == 76 && offsetof(AsrFigureFriend, sameLobby) == 80 &&
                  offsetof(AsrFigureFriend, descSize) == 84 && offsetof(AsrFigureFriend, desc) == 88,
              "AsrFigureFriend layout (documented in notes\\FIGURE-FRIENDS.md)");
static_assert(sizeof(AsrFigureInfo) == 48 && offsetof(AsrFigureInfo, selfSteamId) == 32 && offsetof(AsrFigureInfo, friendCount) == 40 &&
                  offsetof(AsrFigureInfo, enabled) == 44,
              "AsrFigureInfo layout (documented in notes\\FIGURE-FRIENDS.md)");
static_assert(sizeof(AsrFigureFriend::desc) >= kFigureDescMax, "desc holds every description the chunks can carry");

// ---------------------------------------------------------------- a design
struct FigureDesign {
    int32_t builtin = kFigureDefaultBuiltin;  // 0..16, or -1 when desc holds a description
    int32_t descSize = 0;
    uint8_t desc[kFigureDescMax] = {};
};
inline bool FigureDesignValid(const FigureDesign& d) {
    return (d.builtin >= 0 && d.builtin < kFigureBuiltins && d.descSize == 0) ||
           (d.builtin == -1 && d.descSize > 0 && d.descSize <= kFigureDescMax);
}
inline bool FigureDesignSame(const FigureDesign& a, const FigureDesign& b) {
    return a.builtin == b.builtin && a.descSize == b.descSize && (a.descSize <= 0 || memcmp(a.desc, b.desc, (size_t)a.descSize) == 0);
}

// Copies a UTF-8 name into a fixed buffer, NUL-terminated, never cutting a character in half (Steam names can be 128 bytes).
inline void CopyUtf8Name(char* dst, size_t size, const char* src) {
    if (!size) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= size) {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80) --n;  // the first dropped byte continues a character: drop it whole
    }
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

// config.ini [Friends] Figure: the built-in design this player publishes (the digits 0..16; anything else, or no key: the default 13).
inline int32_t FigureConfigBuiltin(const wchar_t* configIni) {
    wchar_t s[32] = {};
    GetPrivateProfileStringW(L"Friends", L"Figure", L"", s, 32, configIni);
    int v = 0, digits = 0;
    const wchar_t* p = s;
    for (; *p >= L'0' && *p <= L'9' && digits < 3; ++p, ++digits) v = v * 10 + (*p - L'0');
    return digits && *p == 0 && v < kFigureBuiltins ? v : kFigureDefaultBuiltin;
}

// ---------------------------------------------------------------- base64 (RFC 4648, with padding; the decoder is strict)
inline std::string Base64Encode(const uint8_t* p, size_t n) {
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16;
        if (i + 1 < n) v |= (uint32_t)p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        s += kAlphabet[(v >> 18) & 63];
        s += kAlphabet[(v >> 12) & 63];
        s += i + 1 < n ? kAlphabet[(v >> 6) & 63] : '=';
        s += i + 2 < n ? kAlphabet[v & 63] : '=';
    }
    return s;
}
inline int Base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
// False for anything but whole groups of 4 with '=' padding only at the end (and the padding bits zero).
inline bool Base64Decode(const std::string& s, std::vector<uint8_t>& out) {
    out.clear();
    if (s.size() % 4 != 0) return false;
    for (size_t i = 0; i < s.size(); i += 4) {
        bool last = i + 4 == s.size();
        int pad = s[i + 3] == '=' ? (s[i + 2] == '=' ? 2 : 1) : 0;
        if (pad && !last) return false;
        int v[4] = {};
        for (int k = 0; k < 4; ++k) {
            if (k >= 4 - pad) continue;
            v[k] = Base64Value(s[i + k]);
            if (v[k] < 0) return false;
        }
        uint32_t bits = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        if ((pad == 2 && (bits & 0xffff)) || (pad == 1 && (bits & 0xff))) return false;  // non-canonical padding bits
        out.push_back((uint8_t)(bits >> 16));
        if (pad < 2) out.push_back((uint8_t)(bits >> 8));
        if (pad < 1) out.push_back((uint8_t)bits);
    }
    return true;
}

// CRC-32 (IEEE, as zip's): the description header's check value.
inline uint32_t FigureCrc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// ---------------------------------------------------------------- encoding
// The kFigureKey value for a design: "1;b13", or "1;d573;1a2b3c4d" with the bytes in the chunks.
inline std::string EncodeFigureHeader(const FigureDesign& d, uint32_t version = kFigureFeatureVersion) {
    char s[48];
    if (d.builtin >= 0) sprintf_s(s, "%u;b%d", version, d.builtin);
    else sprintf_s(s, "%u;d%d;%08x", version, d.descSize, FigureCrc32(d.desc, d.descSize > 0 ? (size_t)d.descSize : 0));
    return s;
}

// The chunk values for a design: base64 of the description cut into pieces of kFigureChunkChars; empty for a built-in (and the
// unused tail), which means "delete the key".
inline void EncodeFigureChunks(const FigureDesign& d, std::string (&chunks)[kFigureChunks]) {
    for (std::string& c : chunks) c.clear();
    if (d.builtin >= 0 || d.descSize <= 0 || d.descSize > kFigureDescMax) return;
    std::string all = Base64Encode(d.desc, (size_t)d.descSize);
    for (int i = 0; i < kFigureChunks && (size_t)i * kFigureChunkChars < all.size(); ++i)
        chunks[i] = all.substr((size_t)i * kFigureChunkChars, kFigureChunkChars);
}

// Reads a friend's value. `chunks` are its chunk values (nullptr or "" = absent; only read for a description). False = no design
// here (an empty or unreadable header, a design letter this reader doesn't know, chunks that don't decode to the declared size).
inline bool DecodeFigure(const char* header, const char* const chunks[kFigureChunks], uint32_t& version, FigureDesign& d) {
    d = FigureDesign();
    d.builtin = -1;
    version = 0;
    if (!header || !*header) return false;
    const char* s = header;
    unsigned long long ver = 0;
    int digits = 0;
    for (; *s >= '0' && *s <= '9'; ++s, ++digits) {
        ver = ver * 10 + (unsigned)(*s - '0');
        if (ver > 0xffffffffull) return false;
    }
    if (!digits || digits > 10 || ver == 0 || *s++ != ';') return false;
    char kind = *s++;
    if (kind != 'b' && kind != 'd') return false;
    long value = 0;
    digits = 0;
    for (; *s >= '0' && *s <= '9'; ++s, ++digits) {
        value = value * 10 + (*s - '0');
        if (value > 100000) return false;
    }
    if (!digits || digits > 6) return false;
    if (kind == 'b') {
        if (*s != 0 || value >= kFigureBuiltins) return false;
        version = (uint32_t)ver;
        d.builtin = (int32_t)value;
        return true;
    }
    if (*s++ != ';') return false;
    uint32_t crc = 0;
    for (int i = 0; i < 8; ++i, ++s) {
        int h = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : -1;
        if (h < 0) return false;
        crc = crc << 4 | (uint32_t)h;
    }
    if (*s != 0 || value < 1 || value > kFigureDescMax) return false;
    std::string all;
    for (int i = 0; i < kFigureChunks; ++i) {
        const char* c = chunks ? chunks[i] : nullptr;
        if (!c || !*c) break;
        size_t n = strlen(c);
        if (n > (size_t)kFigureChunkChars) return false;
        all += c;
    }
    std::vector<uint8_t> bytes;
    if (!Base64Decode(all, bytes) || bytes.size() != (size_t)value || FigureCrc32(bytes.data(), bytes.size()) != crc) return false;
    version = (uint32_t)ver;
    d.descSize = (int32_t)value;
    memcpy(d.desc, bytes.data(), bytes.size());
    return true;
}

// ---------------------------------------------------------------- the friends found (the Steam worker's records)
struct FigureFriendRec {
    uint64_t id = 0;
    char name[64] = {};
    uint32_t version = 0;
    FigureDesign design;
    bool sameLobby = false;
    int32_t seat = 0;    // its place in the lobby's member list (same-lobby friends keep that order)
    uint32_t order = 0;  // the n-th design seen this run (the others come newest first)
};

// The export's order (the design's selection rule, P0: no cache): this lobby's friends in seat order, then the others newest
// first, ties by Steam id; at most kFigureFriendsMax.
inline void OrderFigureFriends(std::vector<FigureFriendRec>& v) {
    std::stable_sort(v.begin(), v.end(), [](const FigureFriendRec& a, const FigureFriendRec& b) {
        if (a.sameLobby != b.sameLobby) return a.sameLobby;
        if (a.sameLobby && a.seat != b.seat) return a.seat < b.seat;
        if (!a.sameLobby && a.order != b.order) return a.order > b.order;
        return a.id < b.id;
    });
    if (v.size() > (size_t)kFigureFriendsMax) v.resize(kFigureFriendsMax);
}

inline void FigureFriendOut(const FigureFriendRec& r, AsrFigureFriend& o) {
    memset(&o, 0, sizeof(o));
    o.steamId = r.id;
    CopyUtf8Name(o.name, sizeof(o.name), r.name);
    o.featureVersion = r.version;
    o.builtin = r.design.builtin;
    o.sameLobby = r.sameLobby ? 1 : 0;
    o.descSize = r.design.descSize;
    if (r.design.descSize > 0 && r.design.descSize <= kFigureDescMax) memcpy(o.desc, r.design.desc, (size_t)r.design.descSize);
}

// Whether two gathered lists differ in what the export tells (ids, designs, lobby membership, order): the log line's trigger.
inline bool FigureFriendsDiffer(const std::vector<FigureFriendRec>& a, const std::vector<FigureFriendRec>& b) {
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].sameLobby != b[i].sameLobby || !FigureDesignSame(a[i].design, b[i].design)) return true;
    return false;
}

// One friend for the log: "Name (id) built-in 13" or "Name (id) a 300-byte design", plus " [lobby]".
inline std::string DescribeFigureFriend(const FigureFriendRec& r) {
    char s[160];
    if (r.design.builtin >= 0) sprintf_s(s, "%s (%llu) built-in %d%s", r.name, r.id, r.design.builtin, r.sameLobby ? " [lobby]" : "");
    else sprintf_s(s, "%s (%llu) a %d-byte design%s", r.name, r.id, r.design.descSize, r.sameLobby ? " [lobby]" : "");
    return s;
}
