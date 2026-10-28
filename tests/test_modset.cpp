// Unit tests for src/modset.h. usage: test_modset.exe <scratch dir> [real mods dir to scan read-only]
#include <stdio.h>
#include <string>
#include "modset.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static const char* H1 = "1111111111111111111111111111111111111111111111111111111111111111";
static const char* H2 = "2222222222222222222222222222222222222222222222222222222222222222";

static void WriteIni(const std::wstring& dir, const std::string& body) {
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring p = dir + L"\\mod.ini";
    HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(f, body.data(), (DWORD)body.size(), &n, nullptr);
    CloseHandle(f);
}

static std::string Ini(const char* id, const char* ver, const char* plan, const char* rt) {
    return std::string("[mod]\r\nschema=1\r\nid=") + id + "\r\nname=x\r\nversion=" + ver + "\r\nruntime=runtime\\x.dll\r\nplan=plan.json\r\nplan_sha256=" +
           plan + "\r\nruntime_sha256=" + rt + "\r\n";
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, w, MAX_PATH);
    std::wstring root = w;
    CreateDirectoryW(root.c_str(), nullptr);
    std::wstring mods = root + L"\\mods";
    CreateDirectoryW(mods.c_str(), nullptr);
    std::string upper = H2;
    for (char& c : upper) c = 'A';  // uppercase pin must be lowercased
    WriteIni(mods + L"\\Zeta Mod", Ini("zeta", "1.0", H1, H1));
    WriteIni(mods + L"\\Alpha Mod", Ini("alpha", "2.0-beta", H1, upper.c_str()));
    CreateDirectoryW((mods + L"\\Not A Mod").c_str(), nullptr);  // no mod.ini: ignored, like the loader

    std::vector<ModEntry> mine;
    std::wstring bad;
    CHECK(ScanMods(mods, mine, bad));
    CHECK(mine.size() == 2);
    CHECK(mine.size() == 2 && mine[0].id == "alpha" && mine[1].id == "zeta");
    CHECK(mine.size() == 2 && mine[0].runtime == std::string(64, 'a'));

    std::string enc = EncodeMods(mine);
    std::vector<ModEntry> back;
    CHECK(DecodeMods(enc, back) && back.size() == 2 && back[0] == mine[0] && back[1] == mine[1]);
    CHECK(DescribeModDifferences(mine, back).empty());

    // Each kind of difference, described in plain words.
    std::vector<ModEntry> host = mine;
    host[1].version = "1.1";
    host.push_back({"egg-hangar", "1.1.0", H1, H2});
    std::vector<ModEntry> mine2 = mine;
    mine2[0].runtime = H2;
    mine2.push_back({"extra-mod", "0.1", H1, H1});
    std::vector<std::string> d = DescribeModDifferences(mine2, host);
    for (const std::string& s : d) printf("  diff: %s\n", s.c_str());
    CHECK(d.size() == 4);
    CHECK(d.size() == 4 && d[0].find("different files: alpha") == 0);
    CHECK(d.size() == 4 && d[1] == "different version: zeta (host 1.1, you 1.0)");
    CHECK(d.size() == 4 && d[2].find("missing: egg-hangar 1.1.0") == 0);
    CHECK(d.size() == 4 && d[3].find("extra: extra-mod 0.1") == 0);

    // 1.1 ENABLE MODIFICATIONS: a host's off toggleable mods leave its list; a joiner may have them (any version), not other extras.
    std::vector<ModEntry> hostAll = mine;
    hostAll.push_back({"egg-hangar", "1.2.0", H1, H2});
    hostAll.push_back({"metal-sonic", "1.1.0", H2, H1});
    std::vector<ModEntry> req = RequiredMods(hostAll, {"egg-hangar", "alpha"});  // ("alpha" isn't toggleable: it stays)
    CHECK(req.size() == hostAll.size() - 1 && !ModListed(req, "egg-hangar") && ModListed(req, "metal-sonic") && ModListed(req, "alpha"));
    std::vector<ModEntry> joiner = mine;  // no Egg Hangar, no Metal Sonic
    CHECK(DescribeModDifferences(joiner, req).size() == 1);  // missing: metal-sonic (it's on)
    joiner.push_back({"metal-sonic", "1.1.0", H2, H1});
    CHECK(DescribeModDifferences(joiner, req).empty());
    joiner.push_back({"egg-hangar", "0.9", H2, H2});  // another Egg Hangar build: off in this lobby, so allowed
    CHECK(DescribeModDifferences(joiner, req).empty());
    joiner.back().id = "egg-hangar-fork";             // a mod that isn't toggleable: still an extra
    CHECK(DescribeModDifferences(joiner, req).size() == 1);
    CHECK(RequiredMods(hostAll, {}).size() == hostAll.size());  // nothing off = the 1.0.0 list

    // The game list's mod icons: each used toggleable mod's state here; "(other mods)" only for the other mods.
    std::vector<ModEntry> mineIcons = mine;
    mineIcons.push_back({"egg-hangar", "1.2.0", H1, H2});  // the host's build
    CHECK(ModStateFor(mineIcons, hostAll, "egg-hangar") == kModSame);
    CHECK(ModStateFor(mineIcons, hostAll, "metal-sonic") == kModMissing);
    CHECK(ModStateFor(mineIcons, req, "egg-hangar") == kModUnused);  // (off in that lobby)
    mineIcons.back().version = "1.1.0";
    CHECK(ModStateFor(mineIcons, hostAll, "egg-hangar") == kModOtherVersion);
    CHECK(!ModsDifferBeyondIcons(mineIcons, hostAll));  // only the icons' mods differ
    const std::string runtime0 = mineIcons[0].runtime;
    mineIcons[0].runtime = H2;
    CHECK(ModsDifferBeyondIcons(mineIcons, hostAll));   // another mod's build differs too

    // The join refusal pop-up: the code, then the body it picks (steam.py MODS_REFUSALS: Egg's mark, Metal's, then the others).
    CHECK(RefusedMods(mineIcons, hostAll) == (kModOtherVersion | kModMissing << 2 | 16));
    mineIcons[0].runtime = runtime0;
    CHECK(RefusedMods(mineIcons, hostAll) == (kModOtherVersion | kModMissing << 2));
    CHECK(RefusalText(0) == 0 && RefusalText(kModSame | kModSame << 2) == 0 && RefusalText(16) == 0);  // nothing to name: the plain text
    CHECK(RefusalText(kModMissing << 2) == 3);                    // Metal not installed
    CHECK(RefusalText(kModOtherVersion << 2 | 16) == 2);           // Metal another version, other mods too
    CHECK(RefusalText(kModOtherVersion) == 5);                      // Egg another version
    CHECK(RefusalText(kModOtherVersion | kModMissing << 2) == 9);   // Egg another version, Metal not installed
    CHECK(RefusalText(kModMissing | kModSame << 2) == 11);          // Egg not installed (Metal the same build)
    CHECK(RefusalText(kModMissing | kModMissing << 2 | 16) == 16);  // the last text

    // Malformed lobby data is rejected, never half-read.
    CHECK(!DecodeMods("", back));
    CHECK(!DecodeMods("a|1|" + std::string(H1) + "|short", back));
    CHECK(!DecodeMods("a|1|" + std::string(H1), back));
    CHECK(!DecodeMods("a|1|" + std::string(H1) + "|" + H1 + "|extra", back));
    CHECK(!DecodeMods(enc + ";", back));

    // A mod.ini without a usable pin makes the scan fail and names the folder.
    WriteIni(mods + L"\\Broken", Ini("broken", "1", H1, "nothex|"));
    CHECK(!ScanMods(mods, mine, bad) && bad == L"Broken");

    if (argc > 2) {  // read-only scan of a real mods folder
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, w, MAX_PATH);
        std::vector<ModEntry> real;
        bool ok = ScanMods(w, real, bad);
        printf("real mods: %s, %u mod(s), lobby value %u bytes\n", ok ? "ok" : "FAILED", (unsigned)real.size(), (unsigned)EncodeMods(real).size());
        for (const ModEntry& m : real) printf("  %s %s\n", m.id.c_str(), m.version.c_str());
        CHECK(ok);
    }
    printf("modset tests: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
