// Mod-set check (M2): both players must run exactly the same mods, because roster and course mods
// change racer lists, archives and save layouts. The host publishes its list in lobby data; a
// joiner compares it with its own and refuses a mismatch with a readable list of differences.
// 1.1: the toggleable mods (ModToggleable) a host turned off are left out of the list, and a joiner may have them.
// Header-only so tests/test_modset.cpp can exercise it without Steam.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

struct ModEntry {
    std::string id, version, plan, runtime;  // plan/runtime: the 64-hex pins from mod.ini, lowercased
    bool operator==(const ModEntry& o) const { return id == o.id && version == o.version && plan == o.plan && runtime == o.runtime; }
};

// On-Foot Sonic (the user's mod, 2026-11-09; notes\ONFOOT-ONLINE.md) is one switch with his add-on: both on or both off.
const char kOnFootId[] = "on-foot-sonic", kOnFootAddonId[] = "on-foot-sonic-addon";
inline bool ModToggleable(const std::string& id) {
    return id == "egg-hangar" || id == "metal-sonic" || id == kOnFootId || id == kOnFootAddonId;
}

// On-Foot Sonic's runtime as the lobby compares it. Each tester copy's OnFootSonic.dll carries its own 16-byte stamp (tester
// number, expiry, Steam lock, check word) right after a fixed 16-byte marker, so every copy's runtime_sha256 differs. The lobby
// hashes his DLL with those 16 bytes zeroed instead (exactly one marker, else the plain file), and counts the tester build and
// the public build of the same source as one: they differ only in the tester checks and the trail colours, no gameplay
// (A:\ASR-OnFoot-Visuals, symbol-map diff 2026-11-09). A file that isn't the one mod.ini pins keeps mod.ini's own hash, which
// matches no one's (fail closed); the loader refuses such a file anyway.
const unsigned char kOnFootStampMarker[16] = {0x3A, 0x9F, 0x51, 0xC2, 0x07, 0xE8, 0x6D, 0xB4,
                                              0x29, 0x73, 0xD0, 0x1E, 0x85, 0x4C, 0xFA, 0x62};
// `merged`: that build carries his add-on inside his own folder (it starts runtime\OnFootAddon.dll itself), so no separate
// on-foot-sonic-addon folder is needed (the user, 2026-11-10; tools\onfoot_merge.py).
struct OnFootBuildPair { const char* tester; const char* same; bool merged; };  // a tester template (stamp zeroed) and its public build
const OnFootBuildPair kOnFootBuilds[] = {
    {"9f2abe7cedfcd19c7b0deffe1c4cbfc029e2386dda3e659a04d0e6e1a2d198fd",
     "22aaba9a3585366db2d868e0ea9389abebcd6bd3b750bde3b77870073da3b812", false},
    // (2026-11-10: the network-car build, look exports v1; notes\ONFOOT-ONLINE.md "OnFootSonic.dll side (v1) DONE")
    {"698fe30083286331711006e4bf04ab6261a29a224937e6decb56f626ed66d6df",
     "a57aef01c84d3938111ab4165a3faf913f48dc25007d19f9b256521fe6f094b1", false},
    // (2026-11-10: the same + the add-on starter, A:\ASR-OnFoot-Visuals runtime-src\addon_loader.hpp: the merged folder)
    {"ba27ca3cd57202524ffd594e0f88fc81b3178374fe68f64ae852cdfd11b96bc9",
     "70641c60ef3a4afc9a4ab19984cf211f42f0c60690d2421cc77ff77499c1bb9f", true},
    // (2026-11-10, 1.2.2: the same + add-on 1.0.2, his save record by the shared extra-racer rule; A:\ASR-OnFoot-Visuals 5f86ad8)
    {"b2d4c7a9046eeed6c6ae309ca4a7c947055ea656e28661b070d930b34411ce90",
     "8c1f38d65f0b33148e563b6f99c7e87447c0e135e8b7017f4f1e7120c53df436", true},
};

inline bool Sha256Bytes(const void* data, size_t n, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    bool ok = n <= 0xffffffffu && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)data, (ULONG)n, 0)) &&
              BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static const char kHex[] = "0123456789abcdef";
    hex.clear();
    for (unsigned char b : digest) hex += kHex[b >> 4], hex += kHex[b & 15];
    return true;
}

// The stamp-blind hash of DLL bytes (see above); the pinned hash `pinned` must be the plain file's.
inline std::string OnFootLobbyRuntime(std::vector<unsigned char> dll, const std::string& pinned) {
    std::string plain, masked;
    if (!Sha256Bytes(dll.data(), dll.size(), plain) || plain != pinned) return pinned;
    const unsigned char* b = dll.data();
    size_t hits = 0, at = 0;
    for (size_t i = 0; i + 32 <= dll.size(); ++i)
        if (memcmp(b + i, kOnFootStampMarker, 16) == 0) ++hits, at = i;
    if (hits == 1) memset(dll.data() + at + 16, 0, 16);
    if (hits > 1 || !Sha256Bytes(dll.data(), dll.size(), masked)) return pinned;
    for (const OnFootBuildPair& p : kOnFootBuilds)
        if (masked == p.tester) return p.same;
    return masked;
}

inline bool ReadWholeFile(const std::wstring& path, std::vector<unsigned char>& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024;
    DWORD got = 0;
    if (ok) out.resize((size_t)size.QuadPart);
    ok = ok && ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(f);
    return ok;
}

// One mod.ini field, read the way the ChaoGarage loader reads it (GetPrivateProfileStringW, [mod]).
inline std::string ModIniField(const std::wstring& ini, const wchar_t* key, bool lower = false) {
    wchar_t w[512] = {};
    DWORD n = GetPrivateProfileStringW(L"mod", key, L"", w, 512, ini.c_str());
    std::string s;
    for (DWORD i = 0; i < n; ++i) {
        wchar_t c = w[i];
        if (c < 0x21 || c > 0x7e || c == ';' || c == '|') return "";  // reserved by the encoding below
        s += (char)(lower && c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    return s;
}

// Every mods\<folder>\mod.ini, as the loader sees them (it loads all of them or none). Sorted by id.
// Returns false (and `bad` names the folder) if a mod.ini lacks a field the check needs.
inline bool ScanMods(const std::wstring& modsDir, std::vector<ModEntry>& out, std::wstring& bad) {
    out.clear();
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileW((modsDir + L"\\*").c_str(), &d);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        std::wstring name = d.cFileName;
        if (name == L"." || name == L".." || !(d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring ini = modsDir + L"\\" + name + L"\\mod.ini";
        if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        ModEntry m{ModIniField(ini, L"id"), ModIniField(ini, L"version"), ModIniField(ini, L"plan_sha256", true),
                   ModIniField(ini, L"runtime_sha256", true)};
        if (m.id.empty() || m.plan.size() != 64 || m.runtime.size() != 64) {
            bad = name;
            ok = false;
            break;
        }
        if (m.version.empty()) m.version = "?";
        if (m.id == kOnFootId) {  // (tester copies: OnFootLobbyRuntime)
            wchar_t w[MAX_PATH] = {};
            GetPrivateProfileStringW(L"mod", L"runtime", L"", w, MAX_PATH, ini.c_str());
            std::vector<unsigned char> dll;
            if (w[0] && ReadWholeFile(modsDir + L"\\" + name + L"\\" + w, dll)) m.runtime = OnFootLobbyRuntime(std::move(dll), m.runtime);
        }
        out.push_back(m);
    } while (FindNextFileW(f, &d));
    FindClose(f);
    std::sort(out.begin(), out.end(), [](const ModEntry& a, const ModEntry& b) { return a.id < b.id; });
    return ok;
}

// Lobby-data form: "id|version|plan|runtime" entries joined by ';' (about 150 bytes per mod;
// Steam allows 8 KB per value, so about 50 mods).
inline std::string EncodeMods(const std::vector<ModEntry>& mods) {
    std::string s;
    for (const ModEntry& m : mods) {
        if (!s.empty()) s += ';';
        s += m.id + "|" + m.version + "|" + m.plan + "|" + m.runtime;
    }
    return s;
}

inline bool DecodeMods(const std::string& s, std::vector<ModEntry>& out) {
    out.clear();
    if (s.empty()) return false;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        std::string e = s.substr(pos, end - pos);
        ModEntry m;
        size_t a = e.find('|'), b = a == std::string::npos ? a : e.find('|', a + 1), c = b == std::string::npos ? b : e.find('|', b + 1);
        if (c == std::string::npos || e.find('|', c + 1) != std::string::npos) return false;
        m.id = e.substr(0, a);
        m.version = e.substr(a + 1, b - a - 1);
        m.plan = e.substr(b + 1, c - b - 1);
        m.runtime = e.substr(c + 1);
        if (m.id.empty() || m.plan.size() != 64 || m.runtime.size() != 64) return false;
        out.push_back(m);
        pos = end + 1;
    }
    return true;
}

// ENABLE MODIFICATIONS (1.1, notes\ENABLE-MODS.md): the mods a host may turn off for its lobby. Off = installed but unused (Egg
// Hangar's course not offered, Metal Sonic not picked and never an AI racer), so the lobby's list leaves it out and a joiner
// needn't have it. Every other mod stays exact-match.
// 1.2: a racer pack that stays local: its racers are never offered online (the game uses the stock list, ScriptSubstUseStockRacers),
// so it never joins a lobby's list, and a game with it may join anyone and be joined by anyone.
const char kLocalRacerPack[] = "five-racers";
inline bool ModLocalOnly(const std::string& id) { return id == kLocalRacerPack; }

// Whether a mods\<folder>\mod.ini has this id; unlike ScanMods it needs nothing else from the folder, or from the other mods.
inline bool ModInstalled(const std::wstring& modsDir, const char* id) {
    WIN32_FIND_DATAW d;
    HANDLE f = FindFirstFileW((modsDir + L"\\*").c_str(), &d);
    if (f == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        std::wstring name = d.cFileName;
        if (name != L"." && name != L".." && (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            found = ModIniField(modsDir + L"\\" + name + L"\\mod.ini", L"id") == id;
    } while (!found && FindNextFileW(f, &d));
    FindClose(f);
    return found;
}

// The lobby's required set (lobby data "mods"): every mod but the local ones and the toggleable ones named in `off`.
inline std::vector<ModEntry> RequiredMods(const std::vector<ModEntry>& mods, const std::vector<std::string>& off) {
    std::vector<ModEntry> out;
    for (const ModEntry& m : mods)
        if (!ModLocalOnly(m.id) && (!ModToggleable(m.id) || std::find(off.begin(), off.end(), m.id) == off.end())) out.push_back(m);
    return out;
}

inline bool ModListed(const std::vector<ModEntry>& mods, const char* id) {
    return std::any_of(mods.begin(), mods.end(), [&](const ModEntry& m) { return m.id == id; });
}

// On-Foot Sonic is complete (him + his racer data) with his add-on folder, or as one merged folder whose (stamp-blind) runtime
// is a build marked `merged` in kOnFootBuilds. Unknown builds need the add-on folder, as before (fail closed).
inline bool OnFootMergedRuntime(const std::string& runtime) {
    for (const OnFootBuildPair& p : kOnFootBuilds)
        if (p.merged && runtime == p.same) return true;
    return false;
}
inline bool OnFootComplete(const std::vector<ModEntry>& mods) {
    auto of = std::find_if(mods.begin(), mods.end(), [](const ModEntry& m) { return m.id == kOnFootId; });
    return of != mods.end() && (ModListed(mods, kOnFootAddonId) || OnFootMergedRuntime(of->runtime));
}

// The game list's mod icons (the user's design): how this game has toggleable mod `id` that the host's lobby uses.
enum : int { kModUnused = 0, kModSame = 1, kModOtherVersion = 2, kModMissing = 3 };
inline int ModStateFor(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host, const char* id) {
    auto h = std::find_if(host.begin(), host.end(), [&](const ModEntry& m) { return m.id == id; });
    if (h == host.end()) return kModUnused;
    auto m = std::find_if(mine.begin(), mine.end(), [&](const ModEntry& e) { return e.id == id; });
    return m == mine.end() ? kModMissing : *m == *h ? kModSame : kModOtherVersion;
}
// On-Foot Sonic's icon covers him and his add-on: the worse of the two (the states' order is also how bad each is).
inline int OnFootStateFor(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    return (std::max)(ModStateFor(mine, host, kOnFootId), ModStateFor(mine, host, kOnFootAddonId));
}
// The game list row's mod icons (LobbyRow::mods): Egg Hangar's state in bits 0-1, Metal Sonic's in 2-3, On-Foot Sonic's in 4-5.
inline int32_t ModIcons(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    return ModStateFor(mine, host, "egg-hangar") | ModStateFor(mine, host, "metal-sonic") << 2 | OnFootStateFor(mine, host) << 4;
}

// Empty when this game may join (every mod the host's lobby needs, at the same build, and nothing else but toggleable mods the
// lobby leaves off); otherwise one line per difference, in plain words, for the player.
inline std::vector<std::string> DescribeModDifferences(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    std::vector<std::string> out;
    for (const ModEntry& h : host) {
        auto it = std::find_if(mine.begin(), mine.end(), [&](const ModEntry& m) { return m.id == h.id; });
        if (it == mine.end()) out.push_back("missing: " + h.id + " " + h.version + " (the host has it)");
        else if (it->version != h.version) out.push_back("different version: " + h.id + " (host " + h.version + ", you " + it->version + ")");
        else if (!(*it == h)) out.push_back("different files: " + h.id + " " + h.version + " (same version, different build)");
    }
    for (const ModEntry& m : mine)
        if (!ModToggleable(m.id) && !ModLocalOnly(m.id) && std::none_of(host.begin(), host.end(), [&](const ModEntry& h) { return h.id == m.id; }))
            out.push_back("extra: " + m.id + " " + m.version + " (the host does not have it)");
    return out;
}

// Whether the sets differ in a mod the icons don't show (every mod but the toggleable ones): the row's "(other mods)".
// The join refusal pop-up (1.1): the icons' states (ModIcons: Egg Hangar, Metal Sonic, On-Foot Sonic), 64 when any other mod differs.
const int32_t kRefusedOtherMods = 64;
inline bool ModsDifferBeyondIcons(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host);
inline int32_t RefusedMods(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    return ModIcons(mine, host) | (ModsDifferBeyondIcons(mine, host) ? kRefusedOtherMods : 0);
}

// The pop-up's body for a RefusedMods code: 0 = the plain text (no Egg Hangar / Metal Sonic / On-Foot Sonic difference), else
// 1 + 2 * ((Egg's mark * 3 + Metal's mark) * 3 + On-Foot's mark - 1) + (other mods differ), each mark 0 none, 1 the dash (another
// version), 2 the X (not installed): tools\netmenu\steam.py MODS_REFUSALS lists the 52 texts in this order.
const int32_t kRefusalTexts = 52;
inline int32_t RefusalText(int32_t code) {
    auto mark = [](int state) { return state == kModOtherVersion ? 1 : state == kModMissing ? 2 : 0; };
    int e = mark(code & 3), m = mark((code >> 2) & 3), o = mark((code >> 4) & 3);
    return e || m || o ? 1 + 2 * ((e * 3 + m) * 3 + o - 1) + ((code & kRefusedOtherMods) ? 1 : 0) : 0;
}

inline bool ModsDifferBeyondIcons(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    auto fixed = [](const std::vector<ModEntry>& v) {
        std::vector<ModEntry> out;
        for (const ModEntry& m : v)
            if (!ModToggleable(m.id)) out.push_back(m);
        return out;
    };
    return !DescribeModDifferences(fixed(mine), fixed(host)).empty();
}
