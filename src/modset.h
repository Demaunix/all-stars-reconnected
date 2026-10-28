// Mod-set check (M2): both players must run exactly the same mods, because roster and course mods
// change racer lists, archives and save layouts. The host publishes its list in lobby data; a
// joiner compares it with its own and refuses a mismatch with a readable list of differences.
// 1.1: the toggleable mods (ModToggleable) a host turned off are left out of the list, and a joiner may have them.
// Header-only so tests/test_modset.cpp can exercise it without Steam.
#pragma once
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

struct ModEntry {
    std::string id, version, plan, runtime;  // plan/runtime: the 64-hex pins from mod.ini, lowercased
    bool operator==(const ModEntry& o) const { return id == o.id && version == o.version && plan == o.plan && runtime == o.runtime; }
};

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
inline bool ModToggleable(const std::string& id) { return id == "egg-hangar" || id == "metal-sonic"; }

// The lobby's required set (lobby data "mods"): every mod but the toggleable ones named in `off`.
inline std::vector<ModEntry> RequiredMods(const std::vector<ModEntry>& mods, const std::vector<std::string>& off) {
    std::vector<ModEntry> out;
    for (const ModEntry& m : mods)
        if (!ModToggleable(m.id) || std::find(off.begin(), off.end(), m.id) == off.end()) out.push_back(m);
    return out;
}

inline bool ModListed(const std::vector<ModEntry>& mods, const char* id) {
    return std::any_of(mods.begin(), mods.end(), [&](const ModEntry& m) { return m.id == id; });
}

// The game list's mod icons (the user's design): how this game has toggleable mod `id` that the host's lobby uses.
enum : int { kModUnused = 0, kModSame = 1, kModOtherVersion = 2, kModMissing = 3 };
inline int ModStateFor(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host, const char* id) {
    auto h = std::find_if(host.begin(), host.end(), [&](const ModEntry& m) { return m.id == id; });
    if (h == host.end()) return kModUnused;
    auto m = std::find_if(mine.begin(), mine.end(), [&](const ModEntry& e) { return e.id == id; });
    return m == mine.end() ? kModMissing : *m == *h ? kModSame : kModOtherVersion;
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
        if (!ModToggleable(m.id) && std::none_of(host.begin(), host.end(), [&](const ModEntry& h) { return h.id == m.id; }))
            out.push_back("extra: " + m.id + " " + m.version + " (the host does not have it)");
    return out;
}

// Whether the sets differ in a mod the icons don't show (every mod but the toggleable ones): the row's "(other mods)".
// The join refusal pop-up (1.1): Egg Hangar's ModStateFor in bits 0-1, Metal Sonic's in bits 2-3, 16 when any other mod differs.
inline bool ModsDifferBeyondIcons(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host);
inline int32_t RefusedMods(const std::vector<ModEntry>& mine, const std::vector<ModEntry>& host) {
    return ModStateFor(mine, host, "egg-hangar") | ModStateFor(mine, host, "metal-sonic") << 2 | (ModsDifferBeyondIcons(mine, host) ? 16 : 0);
}

// The pop-up's body for a RefusedMods code: 0 = the plain text (no Egg Hangar / Metal Sonic difference), else 1 + 2 * (Egg's mark
// * 3 + Metal's mark - 1) + (other mods differ), each mark 0 none, 1 the dash (another version), 2 the X (not installed):
// tools\netmenu\steam.py MODS_REFUSALS lists the 16 texts in this order.
inline int32_t RefusalText(int32_t code) {
    auto mark = [](int state) { return state == kModOtherVersion ? 1 : state == kModMissing ? 2 : 0; };
    int e = mark(code & 3), m = mark((code >> 2) & 3);
    return e || m ? 1 + 2 * (e * 3 + m - 1) + ((code & 16) ? 1 : 0) : 0;
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
