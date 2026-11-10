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

    // Found by its id alone, also when another mod's mod.ini would stop ScanMods first (the folder sorts before it).
    std::wstring modsLocal = root + L"\\mods-local";
    CreateDirectoryW(modsLocal.c_str(), nullptr);
    WriteIni(modsLocal + L"\\A Broken", "[mod]\r\nid=broken\r\n");
    CreateDirectoryW((modsLocal + L"\\B Not A Mod").c_str(), nullptr);
    WriteIni(modsLocal + L"\\C Pack", "[mod]\r\nid=" + std::string(kLocalRacerPack) + "\r\n");
    CHECK(ModInstalled(modsLocal, kLocalRacerPack));
    CHECK(!ModInstalled(modsLocal, "metal-sonic"));
    CHECK(!ModInstalled(root + L"\\no-such-folder", kLocalRacerPack));

    // A racer pack that stays local: never in a lobby's list, never a difference, in either direction.
    std::vector<ModEntry> hostLocal = mine;
    hostLocal.push_back({kLocalRacerPack, "1.0.3", H1, H1});
    std::vector<ModEntry> reqLocal = RequiredMods(hostLocal, {});
    CHECK(!ModListed(reqLocal, kLocalRacerPack) && reqLocal.size() == mine.size());
    CHECK(DescribeModDifferences(mine, reqLocal).empty());       // a host with it, a joiner without
    CHECK(DescribeModDifferences(hostLocal, RequiredMods(mine, {})).empty());  // a joiner with it, a host without
    CHECK(DescribeModDifferences(hostLocal, reqLocal).empty());  // both, even at different versions
    hostLocal.back().version = "1.0.4";
    CHECK(DescribeModDifferences(hostLocal, reqLocal).empty());
    CHECK(!ModsDifferBeyondIcons(hostLocal, RequiredMods(mine, {})));
    CHECK(RefusedMods(hostLocal, RequiredMods(mine, {})) == 0);
    std::vector<ModEntry> withMetal = RequiredMods(mine, {});     // a host's lobby with Metal Sonic on: the pack doesn't stand in for it
    withMetal.push_back({"metal-sonic", "1.1.5", H2, H1});
    CHECK(DescribeModDifferences(hostLocal, withMetal).size() == 1);
    CHECK(ModStateFor(hostLocal, withMetal, "metal-sonic") == kModMissing);

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
    const int32_t kRest = kRefusedOtherMods;
    CHECK(RefusedMods(mineIcons, hostAll) == (kModOtherVersion | kModMissing << 2 | kRest));
    mineIcons[0].runtime = runtime0;
    CHECK(RefusedMods(mineIcons, hostAll) == (kModOtherVersion | kModMissing << 2));
    // (steam.py's order: Egg's mark, then Metal's, then On-Foot's, each none / dash / X, each without and with the other mods)
    CHECK(RefusalText(0) == 0 && RefusalText(kModSame | kModSame << 2 | kModSame << 4) == 0 && RefusalText(kRest) == 0);  // the plain text
    CHECK(RefusalText(kModOtherVersion << 4) == 1);                  // On-Foot another version
    CHECK(RefusalText(kModMissing << 4) == 3);                       // On-Foot not installed
    CHECK(RefusalText(kModMissing << 2) == 11);                      // Metal not installed
    CHECK(RefusalText(kModOtherVersion << 2 | kRest) == 6);           // Metal another version, other mods too
    CHECK(RefusalText(kModOtherVersion) == 17);                       // Egg another version
    CHECK(RefusalText(kModOtherVersion | kModMissing << 2) == 29);    // Egg another version, Metal not installed
    CHECK(RefusalText(kModMissing | kModSame << 2) == 35);            // Egg not installed (Metal the same build)
    CHECK(RefusalText(kModMissing | kModMissing << 2 | kModMissing << 4 | kRest) == kRefusalTexts);  // the last text
    for (int32_t code = 0; code < 128; ++code) CHECK(RefusalText(code) >= 0 && RefusalText(code) <= kRefusalTexts);

    // On-Foot Sonic (notes\ONFOOT-ONLINE.md): one switch with his add-on; his icon is the worse of the two.
    std::vector<ModEntry> hostFoot = mine;
    hostFoot.push_back({kOnFootId, "?", H1, H2});
    hostFoot.push_back({kOnFootAddonId, "1.0.1", H2, H1});
    std::vector<ModEntry> reqFoot = RequiredMods(hostFoot, {kOnFootId, kOnFootAddonId});
    CHECK(reqFoot.size() == mine.size() && DescribeModDifferences(mine, reqFoot).empty());  // off: anyone joins
    CHECK(DescribeModDifferences(hostFoot, RequiredMods(mine, {})).empty());               // a joiner with him, a host without
    CHECK(OnFootStateFor(mine, reqFoot) == kModUnused && ModIcons(hostFoot, reqFoot) == 0);
    reqFoot = RequiredMods(hostFoot, {});
    CHECK(DescribeModDifferences(mine, reqFoot).size() == 2);  // on: both missing
    CHECK(OnFootStateFor(mine, reqFoot) == kModMissing && OnFootStateFor(hostFoot, reqFoot) == kModSame);
    std::vector<ModEntry> noAddon(hostFoot.begin(), hostFoot.end() - 1);
    CHECK(OnFootStateFor(noAddon, reqFoot) == kModMissing);    // him without his add-on
    std::vector<ModEntry> oldAddon = hostFoot;
    oldAddon.back().version = "1.0.0";
    CHECK(OnFootStateFor(oldAddon, reqFoot) == kModOtherVersion && ModIcons(oldAddon, reqFoot) == kModOtherVersion << 4);
    CHECK(RefusalText(RefusedMods(mine, reqFoot)) == 3);
    // His add-on inside his folder (a `merged` build, 2026-11-10): complete alone; an unknown build still needs the add-on.
    CHECK(OnFootComplete(hostFoot) && !OnFootComplete(noAddon) && !OnFootComplete(mine));
    const OnFootBuildPair& mergedPair = kOnFootBuilds[sizeof(kOnFootBuilds) / sizeof(kOnFootBuilds[0]) - 1];
    CHECK(mergedPair.merged && !kOnFootBuilds[0].merged && !kOnFootBuilds[1].merged);
    std::vector<ModEntry> hostMerged = mine;
    hostMerged.push_back({kOnFootId, "?", H1, mergedPair.same});
    CHECK(OnFootComplete(hostMerged) && OnFootMergedRuntime(mergedPair.same) && !OnFootMergedRuntime(kOnFootBuilds[1].same));
    std::vector<ModEntry> reqMerged = RequiredMods(hostMerged, {});
    CHECK(OnFootStateFor(hostMerged, reqMerged) == kModSame && OnFootStateFor(mine, reqMerged) == kModMissing);
    CHECK(OnFootStateFor(hostFoot, reqMerged) == kModOtherVersion);  // an old two-folder copy joining a merged host: refused
    CHECK(!DescribeModDifferences(hostFoot, reqMerged).empty() && DescribeModDifferences(hostMerged, reqMerged).empty());
    CHECK(OnFootStateFor(hostMerged, RequiredMods(hostMerged, {kOnFootId, kOnFootAddonId})) == kModUnused);

    // His tester copies: the 16 stamp bytes after the marker don't count; the tester build counts as the public build.
    {
        std::vector<unsigned char> dll(4096, 0x5a), dll2;
        memcpy(dll.data() + 1000, kOnFootStampMarker, 16);
        for (int i = 0; i < 16; ++i) dll[1016 + i] = (unsigned char)(i * 7 + 1);  // tester A's stamp
        dll2 = dll;
        for (int i = 0; i < 16; ++i) dll2[1016 + i] = (unsigned char)(i * 3 + 9);  // tester B's stamp
        std::string pinA, pinB, zeroed;
        CHECK(Sha256Bytes(dll.data(), dll.size(), pinA) && Sha256Bytes(dll2.data(), dll2.size(), pinB) && pinA != pinB);
        std::string a = OnFootLobbyRuntime(dll, pinA), b = OnFootLobbyRuntime(dll2, pinB);
        std::vector<unsigned char> blank = dll;
        memset(blank.data() + 1016, 0, 16);
        CHECK(Sha256Bytes(blank.data(), blank.size(), zeroed));
        CHECK(a == b && a == zeroed && a != pinA);
        CHECK(OnFootLobbyRuntime(dll, pinB) == pinB);  // a file that isn't the pinned one: its pin, which no copy matches
        dll[2000] ^= 1;                                // a change outside the stamp: another build
        std::string pinC;
        CHECK(Sha256Bytes(dll.data(), dll.size(), pinC) && OnFootLobbyRuntime(dll, pinC) != a);
        std::vector<unsigned char> twice = dll2;       // two markers: not a tester copy we know, the plain hash
        memcpy(twice.data() + 3000, kOnFootStampMarker, 16);
        std::string pinT;
        CHECK(Sha256Bytes(twice.data(), twice.size(), pinT) && OnFootLobbyRuntime(twice, pinT) == pinT);
        std::vector<unsigned char> plainDll(4096, 0x33);  // no marker (the public build): the plain hash
        std::string pinP;
        CHECK(Sha256Bytes(plainDll.data(), plainDll.size(), pinP) && OnFootLobbyRuntime(plainDll, pinP) == pinP);
        // Read through ScanMods: two folders with different stamps list the same runtime.
        std::wstring modsFoot = root + L"\\mods-foot";
        CreateDirectoryW(modsFoot.c_str(), nullptr);
        CreateDirectoryW((modsFoot + L"\\On-Foot Sonic").c_str(), nullptr);
        CreateDirectoryW((modsFoot + L"\\On-Foot Sonic\\runtime").c_str(), nullptr);
        HANDLE f = CreateFileW((modsFoot + L"\\On-Foot Sonic\\runtime\\OnFootSonic.dll").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD wrote = 0;
        CHECK(f != INVALID_HANDLE_VALUE && WriteFile(f, dll2.data(), (DWORD)dll2.size(), &wrote, nullptr) && wrote == dll2.size());
        CloseHandle(f);
        WriteIni(modsFoot + L"\\On-Foot Sonic", "; (c) a comment line first, as a tester copy's\r\n[mod]\r\nid=on-foot-sonic\r\nruntime=runtime\\OnFootSonic.dll\r\nplan_sha256=" +
                                                 std::string(H1) + "\r\nruntime_sha256=" + pinB + "\r\n");
        std::vector<ModEntry> scanned;
        std::wstring badFoot;
        CHECK(ScanMods(modsFoot, scanned, badFoot) && scanned.size() == 1 && scanned[0].runtime == a && scanned[0].version == "?");
    }
    // The real pair: a tester template (stamp zeroed) lists as the public build.
    CHECK(std::string(kOnFootBuilds[0].tester).size() == 64 && std::string(kOnFootBuilds[0].same).size() == 64);

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
