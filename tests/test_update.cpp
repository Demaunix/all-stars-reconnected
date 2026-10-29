// The auto-update's checks (src/update.h): signatures, versions, paths, the manifest, the STORED zip, staging and the
// folder swap on a fake game folder (with a DLL loaded from the live folder, as in the game).
// usage: test_update.exe <scratch folder>
#include <stdio.h>
#include <string>
#include <vector>
#include "update.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// ---------------------------------------------------------------- a test signing key (CNG, as tools/release_sign.py)
struct TestKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    uint8_t pub[64] = {};
    TestKey() {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        BCryptGenerateKeyPair(alg, &key, 256, 0);
        BCryptFinalizeKeyPair(key, 0);
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        ULONG n = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &n, 0);
        memcpy(pub, blob + sizeof(BCRYPT_ECCKEY_BLOB), 64);
    }
    ~TestKey() {
        BCryptDestroyKey(key);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    std::vector<uint8_t> Sign(const std::string& text) {
        uint8_t d[32];
        UpdateHash(text.data(), text.size(), d);
        std::vector<uint8_t> sig(64);
        ULONG n = 0;
        BCryptSignHash(key, nullptr, d, 32, sig.data(), 64, &n, 0);
        return sig;
    }
};

// ---------------------------------------------------------------- a STORED zip writer (as tools/release_zip.py writes)
struct ZipFile {
    std::string name, data;
    uint16_t method = 0;
};

static void Put16(std::string& s, uint32_t v) { s += (char)(v & 0xff), s += (char)(v >> 8 & 0xff); }
static void Put32(std::string& s, uint32_t v) { Put16(s, v & 0xffff), Put16(s, v >> 16); }

static std::string MakeZip(const std::vector<ZipFile>& files) {
    std::string out, cd;
    for (auto& f : files) {
        uint32_t off = (uint32_t)out.size();
        Put32(out, 0x04034b50), Put16(out, 20), Put16(out, 0), Put16(out, f.method), Put32(out, 0), Put32(out, 0);
        Put32(out, (uint32_t)f.data.size()), Put32(out, (uint32_t)f.data.size()), Put16(out, (uint32_t)f.name.size()), Put16(out, 0);
        out += f.name + f.data;
        Put32(cd, 0x02014b50), Put16(cd, 20), Put16(cd, 20), Put16(cd, 0), Put16(cd, f.method), Put32(cd, 0), Put32(cd, 0);
        Put32(cd, (uint32_t)f.data.size()), Put32(cd, (uint32_t)f.data.size()), Put16(cd, (uint32_t)f.name.size());
        Put16(cd, 0), Put16(cd, 0), Put16(cd, 0), Put16(cd, 0), Put32(cd, 0), Put32(cd, off);
        cd += f.name;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    Put32(out, 0x06054b50), Put16(out, 0), Put16(out, 0), Put16(out, (uint32_t)files.size()), Put16(out, (uint32_t)files.size());
    Put32(out, (uint32_t)cd.size()), Put32(out, cdOff), Put16(out, 0);
    return out;
}

static UpdateReader Reader(const std::string& zip) {
    return [&zip](uint64_t off, void* buf, size_t n) {
        if (off > zip.size() || n > zip.size() - off) return false;
        memcpy(buf, zip.data() + off, n);
        return true;
    };
}

static std::string Hex(const uint8_t* d) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

static std::string FileLine(const std::string& path, const std::string& data) {
    uint8_t d[32];
    UpdateHash(data.data(), data.size(), d);
    return "file=" + Hex(d) + " " + std::to_string(data.size()) + " " + path + "\n";
}

static std::string ReadAll(const std::wstring& path) {
    std::string s;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) s.append(buf, n);
    CloseHandle(h);
    return s;
}

static void WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &n, nullptr);
    CloseHandle(h);
}

// --check <folder> <scratch>: the DLL's own checks on real release files (tools/release_update.py's output): the built-in
// keys, the manifest, the zip's hash, its files, and a full staging copy.
static int CheckRelease(const std::wstring& dir, const std::wstring& scratch) {
    std::string text = ReadAll(dir + L"\\update.txt"), sig = ReadAll(dir + L"\\update.txt.sig");
    int key = UpdateSignedBy(text, (const uint8_t*)sig.data(), sig.size());
    Check(key >= 0, "release: update.txt is signed with a built-in key");
    UpdateManifest m;
    const char* why = UpdateParse(text, m);
    Check(!why, why ? why : "release: update.txt parses");
    if (why) return g_fail;
    std::string zip = ReadAll(dir + L"\\" + UpdateWide(m.zipName));
    uint8_t got[32];
    Check(zip.size() == m.zipSize && UpdateHash(zip.data(), zip.size(), got) && !memcmp(got, m.zipSha, 32), "release: the zip's size and SHA-256 match");
    std::vector<UpdateZipEntry> entries;
    why = UpdateReadZip(zip.size(), Reader(zip), entries);
    Check(!why, why ? why : "release: the zip reads");
    why = why ? why : UpdateMatch(m, entries);
    Check(!why, why ? why : "release: the zip's files are the manifest's");
    why = why ? why : UpdateStage(m, entries, Reader(zip), scratch + L"\\staged");
    Check(!why, why ? why : "release: every file stages with its SHA-256");
    printf("release %s: %u files, %s\n", m.version.c_str(), (unsigned)m.files.size(), g_fail ? "SOME FAILED" : "ALL PASS");
    UpdateRemoveTree(scratch + L"\\staged");
    return g_fail;
}

int main(int argc, char** argv) {
    if (argc == 4 && !strcmp(argv[1], "--check"))
        return CheckRelease(std::wstring(argv[2], argv[2] + strlen(argv[2])), std::wstring(argv[3], argv[3] + strlen(argv[3])));
    if (argc < 2) {
        printf("usage: test_newversion.exe <scratch folder> | --check <release folder> <scratch folder>\n");
        return 2;
    }
    // ---- signatures
    TestKey a, b;
    uint8_t keys[2][64];
    memcpy(keys[0], a.pub, 64), memcpy(keys[1], b.pub, 64);
    std::string text = "format=1\nversion=1.2.0\n";
    auto sigA = a.Sign(text), sigB = b.Sign(text);
    Check(UpdateSignedBy(text, sigA.data(), 64, keys) == 0, "signature: the current key's is accepted (index 0)");
    Check(UpdateSignedBy(text, sigB.data(), 64, keys) == 1, "signature: the spare key's is accepted (index 1)");
    Check(UpdateSignedBy(text + " ", sigA.data(), 64, keys) == -1, "signature: one byte added to the text is refused");
    auto bad = sigA;
    bad[10] ^= 1;
    Check(UpdateSignedBy(text, bad.data(), 64, keys) == -1, "signature: one bit flipped is refused");
    Check(UpdateSignedBy(text, sigA.data(), 63, keys) == -1, "signature: a short signature is refused");
    Check(UpdateSignedBy(text, sigA.data(), 64) == -1, "signature: a test key's signature is refused by the built-in keys");
    uint8_t zero[64] = {};
    Check(UpdateSignedBy(text, zero, 64, keys) == -1, "signature: all zeros is refused");

    // ---- versions
    Check(UpdateIsRelease("1.1.1") && UpdateIsRelease("2") && UpdateIsRelease("1.2.3.4"), "versions: releases");
    Check(!UpdateIsRelease("1.1.1-dp9") && !UpdateIsRelease("1..1") && !UpdateIsRelease("1.") && !UpdateIsRelease("") &&
              !UpdateIsRelease("1.2.3.4.5") && !UpdateIsRelease(".1") && !UpdateIsRelease("v1.1") && !UpdateIsRelease("1.1 "),
          "versions: developer builds and malformed ones are not releases");
    Check(UpdateNewer("1.1.2", "1.1.1") && UpdateNewer("1.2", "1.1.9") && UpdateNewer("1.10", "1.9") && UpdateNewer("2.0.0", "1.99"),
          "versions: newer");
    Check(!UpdateNewer("1.1.1", "1.1.1") && !UpdateNewer("1.1.1.0", "1.1.1") && !UpdateNewer("1.1.0", "1.1.1") &&
              !UpdateNewer("1.1.2", "1.1.1-dp9") && !UpdateNewer("1.1.2-dp1", "1.1.1"),
          "versions: equal, older, or a developer build on either side is never newer");

    // ---- paths
    Check(UpdateSafePath("mod.ini") && UpdateSafePath("runtime/SteamMultiplayer.dll") &&
              UpdateSafePath("payloads/SumoToolResources/0B11E779.metal-sonic.dat") && UpdateSafePath("README.txt"),
          "paths: the mod's own files");
    const char* badPaths[] = {"../x", "a/../b", "/x", "C:x", "a:b", "a\\b", "a//b", "con.txt", "a/nul", "x.", " x", "x ", "",
                              "a/", "./a", "a/./b", "COM1", "lpt1.dat", "x\t", "\xc3\xa9.txt"};
    bool allRefused = true;
    for (auto p : badPaths) allRefused = allRefused && !UpdateSafePath(p);
    Check(allRefused, "paths: parent, absolute, drive, stream, backslash, empty part, device, trailing dot/space, non-ASCII refused");
    Check(!UpdateSafePath(std::string(201, 'a')), "paths: too long refused");

    // ---- the manifest
    std::string modIni = "[mod]\r\nversion=1.2.0\r\n", dll = std::string("MZ") + std::string(5000, 'x'), cfg = "[Update]\r\nAuto=1\r\n";
    std::vector<ZipFile> files = {{"Steam Multiplayer/", ""},
                                  {"Steam Multiplayer/mod.ini", modIni},
                                  {"Steam Multiplayer/runtime/", ""},
                                  {"Steam Multiplayer/runtime/SteamMultiplayer.dll", dll},
                                  {"Steam Multiplayer/config.ini", cfg}};
    std::string zip = MakeZip(files);
    uint8_t zipSha[32];
    UpdateHash(zip.data(), zip.size(), zipSha);
    std::string manifest = "format=1\nversion=1.2.0\ndate=2026-10-28\nloader_caps=xpac-member-variants,xpac-member-optional\nzip=ALL-STARS RECONNECTED-1.2.0.zip " +
                           std::to_string(zip.size()) + " " + Hex(zipSha) + "\n" + FileLine("mod.ini", modIni) +
                           FileLine("runtime/SteamMultiplayer.dll", dll) + FileLine("config.ini", cfg);
    UpdateManifest m;
    const char* why = UpdateParse(manifest, m);
    Check(!why && m.version == "1.2.0" && m.files.size() == 3 && m.loaderCaps.size() == 2 && m.zipSize == zip.size() &&
              m.zipName == "ALL-STARS RECONNECTED-1.2.0.zip",
          "manifest: a good one parses");
    UpdateManifest crlf;
    std::string withCr;
    for (char c : manifest) withCr += c == '\n' ? std::string("\r\n") : std::string(1, c);
    Check(!UpdateParse(withCr, crlf) && crlf.files.size() == 3, "manifest: CRLF line ends parse the same");
    struct Bad {
        const char* what;
        std::string text;
    };
    auto replaced = [&](const std::string& from, const std::string& to) {
        std::string t = manifest;
        size_t at = t.find(from);
        return at == std::string::npos ? std::string("<no change>") : t.replace(at, from.size(), to);
    };
    Bad bads[] = {
        {"no format line", replaced("format=1\n", "")},
        {"format 2", replaced("format=1", "format=2")},
        {"an unknown key", manifest + "extra=1\n"},
        {"a developer version", replaced("version=1.2.0", "version=1.2.0-dp1")},
        {"no date", replaced("date=2026-10-28\n", "")},
        {"no loader_caps line", replaced("loader_caps=xpac-member-variants,xpac-member-optional\n", "")},
        {"an uppercase capability", replaced("xpac-member-optional", "XPAC")},
        {"a zip that isn't .zip", replaced("1.2.0.zip", "1.2.0.exe")},
        {"a zip name with a folder", replaced("zip=ALL-STARS", "zip=x/ALL-STARS")},
        {"a zero zip size", replaced(" " + std::to_string(zip.size()) + " ", " 0 ")},
        {"an uppercase hash", replaced(Hex(zipSha), std::string(64, 'A'))},
        {"a file leaving the folder", replaced(" mod.ini\n", " ../mod.ini\n")},
        {"a file listed twice", manifest + FileLine("MOD.INI", modIni)},
        {"no runtime DLL", replaced(FileLine("runtime/SteamMultiplayer.dll", dll), "")},
        {"a control character", replaced("date=2026-10-28", "date=2026-10\x01" "28")},
        {"a line without '='", manifest + "junk\n"},
        {"too big", manifest + std::string(70000, '\n')},
    };
    for (auto& t : bads) {
        UpdateManifest x;
        bool refused = t.text != "<no change>" && UpdateParse(t.text, x) != nullptr;
        Check(refused, (std::string("manifest refused: ") + t.what).c_str());
    }

    // ---- the zip
    std::vector<UpdateZipEntry> entries;
    why = UpdateReadZip(zip.size(), Reader(zip), entries);
    Check(!why && entries.size() == 3 && entries[1].path == "runtime/SteamMultiplayer.dll" && entries[1].size == dll.size(),
          "zip: a stored zip's files are read (directories skipped)");
    Check(!UpdateMatch(m, entries), "zip: its files match the manifest");
    std::string cut = zip.substr(0, zip.size() - 5);
    Check(UpdateReadZip(cut.size(), Reader(cut), entries) != nullptr, "zip: a truncated zip is refused");
    auto deflated = files;
    deflated[1].method = 8;
    std::string dz = MakeZip(deflated);
    Check(UpdateReadZip(dz.size(), Reader(dz), entries) != nullptr, "zip: a compressed entry is refused");
    auto outside = files;
    outside.push_back({"Other Mod/mod.ini", "x"});
    std::string oz = MakeZip(outside);
    Check(UpdateReadZip(oz.size(), Reader(oz), entries) != nullptr, "zip: an entry outside \"Steam Multiplayer/\" is refused");
    auto escape = files;
    escape.push_back({"Steam Multiplayer/../dinput8.dll", "x"});
    std::string ez = MakeZip(escape);
    Check(UpdateReadZip(ez.size(), Reader(ez), entries) != nullptr, "zip: an entry escaping the folder is refused");
    auto twice = files;
    twice.push_back({"Steam Multiplayer/MOD.INI", "x"});
    std::string tz = MakeZip(twice);
    Check(UpdateReadZip(tz.size(), Reader(tz), entries) != nullptr, "zip: a duplicate name is refused");
    auto extra = files;
    extra.push_back({"Steam Multiplayer/extra.txt", "x"});
    std::string xz = MakeZip(extra);
    Check(!UpdateReadZip(xz.size(), Reader(xz), entries) && UpdateMatch(m, entries) != nullptr, "zip: a file the manifest doesn't list is refused");
    Check(UpdateReadZip(10, Reader(zip), entries) != nullptr, "zip: too small to be a zip is refused");

    // ---- capabilities
    const char* have[] = {"xpac-member-variants", "xpac-member-optional", "other", nullptr};
    const char* old[] = {"xpac-member-variants", nullptr};
    Check(UpdateHasCaps(m.loaderCaps, have) && !UpdateHasCaps(m.loaderCaps, old) && !UpdateHasCaps(m.loaderCaps, nullptr),
          "loader: every capability the release needs must be in the running loader's list");

    // ---- staging for ChaoGarage on a fake game folder (notes/AUTO-UPDATE.md "The handshake")
    std::wstring scratch(argv[1], argv[1] + strlen(argv[1]));
    UpdateRemoveTree(scratch);
    UpdatePaths p;
    p.live = scratch + L"\\game\\mods\\Steam Multiplayer";
    p.root = scratch + L"\\game\\mods-updates";
    UpdateMakeDirs(p.live + L"\\payloads");
    UpdateMakeDirs(p.root + L"\\staged");
    WriteAll(p.live + L"\\mod.ini", "[mod]\r\nversion=1.1.1\r\n");
    std::string playerCfg = "[Update]\r\nAuto=1\r\n[Voice]\r\nKey=B\r\n";
    WriteAll(p.live + L"\\config.ini", playerCfg);
    WriteAll(p.live + L"\\payloads\\member.dat", std::string(4096, 'p'));
    // ChaoGarage 1.1.0+ keeps every payload open share-read for the whole session (its virtual archives).
    HANDLE held = CreateFileW((p.live + L"\\payloads\\member.dat").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    Check(held != INVALID_HANDLE_VALUE, "game: a payload is held open the way the loader holds it");
    bool renamed = MoveFileExW(p.live.c_str(), (scratch + L"\\game\\aside").c_str(), 0) != 0;
    if (renamed) MoveFileExW((scratch + L"\\game\\aside").c_str(), p.live.c_str(), 0);
    Check(!renamed, "game: while a payload is open the mod folder can't be renamed (why ChaoGarage does the swap at start)");

    UpdateManifest wrong = m;
    wrong.files[0].sha[0] ^= 1;
    UpdateReadZip(zip.size(), Reader(zip), entries);
    why = UpdateStageForLoader(wrong, entries, Reader(zip), p);
    Check(why && !UpdateExists(p.Ready()) && ReadAll(p.live + L"\\mod.ini").find("1.1.1") != std::string::npos,
          "stage: a file whose hash differs is refused, no marker, the live folder unchanged");
    int asked = 0;
    why = UpdateStageForLoader(m, entries, Reader(zip), p, [&] { return ++asked > 1; });
    Check(why && !strcmp(why, "stopped") && !UpdateExists(p.Ready()), "stage: a stop (rollback) ends it early, no marker");

    why = UpdateStageForLoader(m, entries, Reader(zip), p);
    Check(!why, why ? why : "stage: staged, re-checked, marked ready");
    Check(ReadAll(p.Staged() + L"\\mod.ini") == modIni && ReadAll(p.Staged() + L"\\runtime\\SteamMultiplayer.dll") == dll &&
              ReadAll(p.Staged() + L"\\config.ini") == cfg,
          "stage: the staged folder holds exactly the release's files (its default config.ini: ChaoGarage keeps the player's at the swap)");
    Check(ReadAll(p.live + L"\\mod.ini").find("1.1.1") != std::string::npos && ReadAll(p.live + L"\\config.ini") == playerCfg,
          "stage: the live folder is untouched (the game keeps running it)");
    std::string ready = ReadAll(p.Ready());
    std::vector<UpdateFile> listed;
    Check(!UpdateListFolder(p.Staged(), "", listed) && listed.size() == 3, "stage: the staged folder has exactly the release's 3 files");
    std::string head = "format=1\nfolder=Steam Multiplayer\nversion=1.2.0\nreplaces=" + UpdateFileSha(p.live + L"\\mod.ini") + "\nkeep=config.ini\n";
    bool allListed = ready.rfind(head, 0) == 0;
    for (auto& f : listed) allListed = allListed && ready.find("file=" + UpdateHexOf(f.sha) + " " + std::to_string(f.size) + " " + f.path + "\n") != std::string::npos;
    Check(allListed && std::count(ready.begin(), ready.end(), '\n') == 8,
          "stage: the marker names the installed mod.ini it replaces, keeps config.ini, and lists every staged file as on disk");
    Check(!UpdateExists(p.Ready() + L".tmp"), "stage: no temporary marker left");

    // The next start (the mod's side): a waiting update is kept, nothing is downloaded again.
    Check(UpdateCleanUp(p) && UpdateExists(p.Staged() + L"\\mod.ini"), "next start: a marked staging is kept and reported as waiting");
    // A swap ChaoGarage began (its .swapping journal): the mod never touches it.
    DeleteFileW(p.Ready().c_str());
    WriteAll(p.root + L"\\staged\\Steam Multiplayer.swapping", "format=1\n");
    Check(UpdateCleanUp(p) && UpdateExists(p.Staged() + L"\\mod.ini"), "next start: a staging under ChaoGarage's .swapping journal is left alone");
    DeleteFileW((p.root + L"\\staged\\Steam Multiplayer.swapping").c_str());
    // A staging cut short (no marker): removed.
    WriteAll(p.Ready() + L".tmp", "format=1\n");
    Check(!UpdateCleanUp(p) && !UpdateExists(p.Staged()) && !UpdateExists(p.Ready() + L".tmp"),
          "next start: a staging without its marker (and a half-written marker) is removed");
    UpdateMakeDirs(p.root + L"\\download");
    WriteAll(p.root + L"\\download\\x.zip.part", "x");
    UpdateCleanUp(p);
    Check(!UpdateExists(p.root + L"\\download"), "next start: a download cut short is removed");

    // A link inside the staged folder: the listing refuses it.
    UpdateMakeDirs(p.Staged());
    WriteAll(p.Staged() + L"\\mod.ini", "x");
    bool linkMade = CreateSymbolicLinkW((p.Staged() + L"\\link").c_str(), p.live.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2) != 0;
    std::vector<UpdateFile> withLink;
    Check(!linkMade || UpdateListFolder(p.Staged(), "", withLink) != nullptr, linkMade ? "stage: a link in the staged folder is refused"
                                                                                       : "stage: (no link made: symbolic links need Developer Mode; skipped)");
    UpdateRemoveTree(p.Staged());
    Check(ReadAll(p.live + L"\\config.ini") == playerCfg, "clean-up: removing a staging with a link never touches the link's target");

    // Next process start: nothing holds the payload any more, so ChaoGarage's start-up step can rename the folder.
    CloseHandle(held);
    renamed = MoveFileExW(p.live.c_str(), (scratch + L"\\game\\aside").c_str(), 0) != 0;
    Check(renamed, "loader start: with no payload open the mod folder can be renamed");
    if (renamed) MoveFileExW((scratch + L"\\game\\aside").c_str(), p.live.c_str(), 0);

    // The main menu's "updated" notice: once per version, only after ChaoGarage put a different version in place.
    std::wstring prevDir = p.root + L"\\previous\\Steam Multiplayer";
    UpdateRemoveTree(p.root + L"\\previous");
    DeleteFileW((p.root + L"\\notice.txt").c_str());
    Check(!UpdateJustApplied(p.root, "1.1.2"), "notice: no previous copy, no notice");
    UpdateMakeDirs(prevDir);
    WriteAll(prevDir + L"\\mod.ini", "[mod]\r\nversion=1.1.1\r\n");
    Check(UpdateJustApplied(p.root, "1.1.2"), "notice: the previous copy is 1.1.1 and 1.1.2 runs: the notice is due");
    Check(!UpdateJustApplied(p.root, "1.1.1"), "notice: the previous copy is the running version: no notice");
    Check(UpdateNoticeMark(p.root, "1.1.2") && UpdateNoticeShownFor(p.root) == "1.1.2", "notice: shown for 1.1.2 is recorded");
    Check(!UpdateJustApplied(p.root, "1.1.2"), "notice: the next start of 1.1.2 doesn't show it again");
    Check(UpdateJustApplied(p.root, "1.1.3"), "notice: a later update (1.1.3) shows it again");
    WriteAll(prevDir + L"\\mod.ini", "[mod]\r\nschema=1\r\n");
    Check(!UpdateJustApplied(p.root, "1.1.3"), "notice: a previous copy without a version: no notice");
    UpdateRemoveTree(p.root + L"\\previous");

    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail;
}
