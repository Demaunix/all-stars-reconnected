// Unit tests for src/figure_presence.h (friend figures, P1): the published value's encoding and decoding,
// its limits, the friends' order, the config value, the ABI layout and, given the built DLL, its exports with Steam off.
// usage: test_figure.exe <scratch dir> [path to the built SteamMultiplayer.dll]
#include <stdio.h>
#include <string>
#include <vector>
#include "figure_presence.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static FigureDesign Builtin(int32_t i) {
    FigureDesign d;
    d.builtin = i;
    return d;
}
static FigureDesign Desc(int size, uint8_t seed) {
    FigureDesign d;
    d.builtin = -1;
    d.descSize = size;
    for (int i = 0; i < size && i < kFigureDescMax; ++i) d.desc[i] = (uint8_t)(seed + i * 7);
    return d;
}
static void Pointers(const std::string (&c)[kFigureChunks], const char* (&out)[kFigureChunks]) {
    for (int i = 0; i < kFigureChunks; ++i) out[i] = c[i].c_str();
}
static FigureFriendRec Rec(uint64_t id, bool lobby, int seat, uint32_t order, int32_t builtin = 13) {
    FigureFriendRec r;
    r.id = id;
    sprintf_s(r.name, "player%llu", id);
    r.sameLobby = lobby;
    r.seat = seat;
    r.order = order;
    r.design.builtin = builtin;
    return r;
}
static void WriteFile(const std::wstring& path, const std::string& body) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n;
    WriteFile(f, body.data(), (DWORD)body.size(), &n, nullptr);
    CloseHandle(f);
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, w, MAX_PATH);
    std::wstring scratch = w;
    CreateDirectoryW(scratch.c_str(), nullptr);

    // ---- base64: every length round-trips, the known vectors, the strict decoder
    for (int n = 0; n <= 10; ++n) {
        uint8_t b[10];
        for (int i = 0; i < n; ++i) b[i] = (uint8_t)(i * 37 + n);
        std::string s = Base64Encode(b, (size_t)n);
        std::vector<uint8_t> back;
        CHECK(s.size() == (size_t)(n + 2) / 3 * 4);
        CHECK(Base64Decode(s, back) && back.size() == (size_t)n && (n == 0 || memcmp(back.data(), b, (size_t)n) == 0));
    }
    CHECK(Base64Encode((const uint8_t*)"Man", 3) == "TWFu");
    CHECK(Base64Encode((const uint8_t*)"Ma", 2) == "TWE=");
    CHECK(Base64Encode((const uint8_t*)"M", 1) == "TQ==");
    std::vector<uint8_t> bytes;
    CHECK(Base64Decode("", bytes) && bytes.empty());
    CHECK(!Base64Decode("A", bytes));
    CHECK(!Base64Decode("AB=C", bytes));
    CHECK(!Base64Decode("@@@@", bytes));
    CHECK(!Base64Decode("TWE=TWFu", bytes));  // padding only at the end
    CHECK(!Base64Decode("TWF=", bytes));      // padding bits not zero ("Ma" is TWE=)
    CHECK(!Base64Decode("TWFu\n", bytes));
    CHECK(FigureCrc32((const uint8_t*)"123456789", 9) == 0xcbf43926u);  // the standard check value
    CHECK(FigureCrc32(nullptr, 0) == 0);

    // ---- the header: built-ins
    uint32_t ver = 0;
    FigureDesign d;
    const char* none[kFigureChunks] = {"", "", ""};
    CHECK(EncodeFigureHeader(Builtin(13)) == "1;b13");
    CHECK(EncodeFigureHeader(Builtin(0)) == "1;b0");
    CHECK(EncodeFigureHeader(Builtin(16), 3) == "3;b16");
    CHECK(DecodeFigure("1;b13", none, ver, d) && ver == 1 && d.builtin == 13 && d.descSize == 0 && FigureDesignValid(d));
    CHECK(DecodeFigure("1;b16", nullptr, ver, d) && d.builtin == 16);
    CHECK(DecodeFigure("7;b5", none, ver, d) && ver == 7 && d.builtin == 5);  // a newer version's built-in still reads
    CHECK(DecodeFigure("4294967295;b1", none, ver, d) && ver == 0xffffffffu);
    const char* bad[] = {"", "1;b17", "1;b-1", "1;bx", "1;b", "b13", "1;z9", "1;b13;x", "0;b1", ";b1", "1;b13 ", " 1;b13", "1;B13",
                         "4294967296;b1", "99999999999;b1", "1;b1000000", "1;b0000000", "1;d0", "1;d574", "1;d300", "1;d300;", "1;d300;1234567",
                         "1;d300;123456789", "1;d300;1234567G", "1;d300;ABCDEF01", "1", "1;", "1;;b1", "1;b1;", nullptr};
    for (const char* s : bad) {
        bool ok = DecodeFigure(s, none, ver, d);
        if (ok) printf("  accepted \"%s\"\n", s ? s : "(null)");
        CHECK(!ok && d.builtin == -1 && d.descSize == 0);
    }
    CHECK(!FigureDesignValid(Builtin(17)) && !FigureDesignValid(Builtin(-1)) && FigureDesignValid(Builtin(0)) && FigureDesignValid(Desc(1, 0)));
    CHECK(!FigureDesignValid(Desc(kFigureDescMax + 1, 0)));
    {
        FigureDesign mixed = Desc(10, 1);
        mixed.builtin = 2;  // a built-in with description bytes: not a design
        CHECK(!FigureDesignValid(mixed));
    }

    // ---- descriptions: the chunks, the size limit, the CRC
    FigureDesign full = Desc(kFigureDescMax, 3);
    std::string header = EncodeFigureHeader(full);
    CHECK(header.size() == 15 && header.compare(0, 6, "1;d573") == 0 && header[6] == ';');
    std::string c[kFigureChunks];
    const char* cp[kFigureChunks];
    EncodeFigureChunks(full, c);
    Pointers(c, cp);
    CHECK(c[0].size() == 255 && c[1].size() == 255 && c[2].size() == 254);  // 573 bytes = 764 characters
    CHECK(DecodeFigure(header.c_str(), cp, ver, d) && ver == 1 && d.builtin == -1 && d.descSize == 573 && memcmp(d.desc, full.desc, 573) == 0 &&
          FigureDesignValid(d));
    FigureDesign part = Desc(300, 9);
    header = EncodeFigureHeader(part);
    EncodeFigureChunks(part, c);
    Pointers(c, cp);
    CHECK(c[0].size() == 255 && c[1].size() == 145 && c[2].empty());  // 300 bytes = 400 characters
    CHECK(DecodeFigure(header.c_str(), cp, ver, d) && d.descSize == 300 && memcmp(d.desc, part.desc, 300) == 0);
    {
        std::string wrongSize = header;
        wrongSize.replace(3, 3, "299");
        CHECK(!DecodeFigure(wrongSize.c_str(), cp, ver, d));  // the declared size must match the chunks
        std::string tampered[kFigureChunks] = {c[0], c[1], c[2]};
        tampered[1][10] = tampered[1][10] == 'A' ? 'B' : 'A';  // one character changed: the CRC refuses it
        const char* tp[kFigureChunks];
        Pointers(tampered, tp);
        CHECK(!DecodeFigure(header.c_str(), tp, ver, d));
        const char* halfUpdated[kFigureChunks] = {c[0].c_str(), "", ""};  // chunk 1 not in yet: too short
        CHECK(!DecodeFigure(header.c_str(), halfUpdated, ver, d));
        const char* gap[kFigureChunks] = {c[0].c_str(), "", c[1].c_str()};  // a gap ends the value
        CHECK(!DecodeFigure(header.c_str(), gap, ver, d));
        CHECK(!DecodeFigure(header.c_str(), nullptr, ver, d) && !DecodeFigure(header.c_str(), none, ver, d));
        std::string longChunk(256, 'A');  // 192 bytes of zero, but the chunk is longer than Steam's value limit
        FigureDesign zeros = Desc(192, 0);
        for (int i = 0; i < 192; ++i) zeros.desc[i] = 0;
        const char* lp[kFigureChunks] = {longChunk.c_str(), "", ""};
        CHECK(!DecodeFigure(EncodeFigureHeader(zeros).c_str(), lp, ver, d));
        const char* twoChunks[kFigureChunks] = {longChunk.c_str() + 1, "A", ""};  // the same 256 characters split 255 + 1: fine (the reader joins them)
        CHECK(DecodeFigure(EncodeFigureHeader(zeros).c_str(), twoChunks, ver, d) && d.descSize == 192);
    }
    EncodeFigureChunks(Builtin(2), c);
    CHECK(c[0].empty() && c[1].empty() && c[2].empty());  // a built-in has no chunks
    EncodeFigureChunks(Desc(kFigureDescMax + 1, 1), c);
    CHECK(c[0].empty());  // too big to carry: nothing goes out
    int fails = 0;
    for (int n = 1; n <= kFigureDescMax; ++n) {  // every size round-trips, every chunk within Steam's limit
        FigureDesign x = Desc(n, (uint8_t)n);
        EncodeFigureChunks(x, c);
        Pointers(c, cp);
        bool ok = DecodeFigure(EncodeFigureHeader(x).c_str(), cp, ver, d) && d.descSize == n && memcmp(d.desc, x.desc, (size_t)n) == 0;
        for (int k = 0; k < kFigureChunks; ++k) ok = ok && c[k].size() <= (size_t)kFigureChunkChars;
        if (!ok) ++fails;
    }
    CHECK(fails == 0);
    for (int i = 0; i < kFigureBuiltins; ++i) {
        EncodeFigureChunks(Builtin(i), c);
        Pointers(c, cp);
        if (!(DecodeFigure(EncodeFigureHeader(Builtin(i)).c_str(), cp, ver, d) && d.builtin == i)) ++fails;
    }
    CHECK(fails == 0);
    CHECK(FigureDesignSame(part, part) && !FigureDesignSame(part, full) && !FigureDesignSame(part, Builtin(13)) && FigureDesignSame(Builtin(4), Builtin(4)));

    // ---- the order: this lobby's friends by seat, then the others newest first, ties by id; at most kFigureFriendsMax
    std::vector<FigureFriendRec> v = {Rec(105, false, 0, 3), Rec(101, true, 2, 1), Rec(104, false, 0, 9), Rec(102, true, 1, 7), Rec(103, false, 0, 3)};
    OrderFigureFriends(v);
    CHECK(v.size() == 5 && v[0].id == 102 && v[1].id == 101 && v[2].id == 104 && v[3].id == 103 && v[4].id == 105);
    std::vector<FigureFriendRec> many;
    for (uint64_t i = 0; i < 20; ++i) many.push_back(Rec(200 + i, false, 0, (uint32_t)(20 - i)));
    OrderFigureFriends(many);
    CHECK(many.size() == (size_t)kFigureFriendsMax && many[0].id == 200 && many[15].id == 215);
    std::vector<FigureFriendRec> a = {Rec(1, true, 0, 1), Rec(2, false, 0, 2)}, b = a;
    CHECK(!FigureFriendsDiffer(a, b));
    b[1].design.builtin = 4;
    CHECK(FigureFriendsDiffer(a, b));
    b = a;
    b[0].sameLobby = false;
    CHECK(FigureFriendsDiffer(a, b));
    b = {a[1], a[0]};
    CHECK(FigureFriendsDiffer(a, b));
    b = a;
    b.pop_back();
    CHECK(FigureFriendsDiffer(a, b));
    b = a;
    strcpy_s(b[0].name, "renamed");
    CHECK(!FigureFriendsDiffer(a, b));  // a name change alone is no change of what the export tells
    CHECK(DescribeFigureFriend(Rec(7, true, 0, 1, 13)) == "player7 (7) built-in 13 [lobby]");
    CHECK(DescribeFigureFriend(Rec(8, false, 0, 1, 4)) == "player8 (8) built-in 4");
    {
        FigureFriendRec r = Rec(9, false, 0, 1);
        r.design = Desc(300, 1);
        CHECK(DescribeFigureFriend(r) == "player9 (9) a 300-byte design");
    }

    // ---- the export's record
    {
        FigureFriendRec r = Rec(76561197960265729ull, true, 0, 1, 6);  // (a placeholder id)
        std::string longName(70, 'n');
        strncpy_s(r.name, longName.c_str(), _TRUNCATE);
        r.version = 1;
        AsrFigureFriend o;
        memset(&o, 0xaa, sizeof(o));
        FigureFriendOut(r, o);
        CHECK(o.steamId == 76561197960265729ull && o.featureVersion == 1 && o.builtin == 6 && o.sameLobby == 1 && o.descSize == 0);
        CHECK(strlen(o.name) == 63 && o.name[63] == 0);
        // A name cut never splits a UTF-8 character: 62 ASCII bytes + a 3-byte character (65 bytes) keeps the 62.
        std::string utf8 = std::string(62, 'a') + "\xe2\x82\xac";
        CopyUtf8Name(r.name, sizeof(r.name), utf8.c_str());
        CHECK(strlen(r.name) == 62);
        utf8 = std::string(61, 'a') + "\xe2\x82\xac";  // 64 bytes: the character would end at byte 64, one too many
        CopyUtf8Name(r.name, sizeof(r.name), utf8.c_str());
        CHECK(strlen(r.name) == 61);
        utf8 = std::string(60, 'a') + "\xe2\x82\xac";  // 63 bytes: fits whole
        CopyUtf8Name(r.name, sizeof(r.name), utf8.c_str());
        CHECK(strlen(r.name) == 63 && (unsigned char)r.name[62] == 0xac);
        CopyUtf8Name(r.name, sizeof(r.name), nullptr);
        CHECK(r.name[0] == 0);
        CopyUtf8Name(r.name, sizeof(r.name), "short");
        CHECK(strcmp(r.name, "short") == 0);
        char tiny[1];
        CopyUtf8Name(tiny, 1, "abc");
        CHECK(tiny[0] == 0);
        bool zero = true;
        for (uint8_t x : o.desc) zero = zero && x == 0;
        CHECK(zero);
        r.design = Desc(300, 5);
        FigureFriendOut(r, o);
        CHECK(o.builtin == -1 && o.descSize == 300 && memcmp(o.desc, r.design.desc, 300) == 0 && o.desc[300] == 0);
    }
    printf("ABI: AsrFigureFriend %u bytes (name +%u, featureVersion +%u, builtin +%u, sameLobby +%u, descSize +%u, desc +%u), AsrFigureInfo %u bytes\n",
           (unsigned)sizeof(AsrFigureFriend), (unsigned)offsetof(AsrFigureFriend, name), (unsigned)offsetof(AsrFigureFriend, featureVersion),
           (unsigned)offsetof(AsrFigureFriend, builtin), (unsigned)offsetof(AsrFigureFriend, sameLobby), (unsigned)offsetof(AsrFigureFriend, descSize),
           (unsigned)offsetof(AsrFigureFriend, desc), (unsigned)sizeof(AsrFigureInfo));

    // ---- config.ini [Friends] Figure
    std::wstring ini = scratch + L"\\config.ini";
    WriteFile(ini, "[Menus]\r\nEnabled=1\r\n\r\n[Friends]\r\nFigure=5\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == 5);
    WriteFile(ini, "[Friends]\r\nFigure=0\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == 0);
    WriteFile(ini, "[Friends]\r\nFigure=16\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == 16);
    WriteFile(ini, "[Friends]\r\nFigure=17\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    WriteFile(ini, "[Friends]\r\nFigure=-1\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    WriteFile(ini, "[Friends]\r\nFigure=abc\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    WriteFile(ini, "[Friends]\r\nFigure=5x\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    WriteFile(ini, "[Friends]\r\nFigure=\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    WriteFile(ini, "[Friends]\r\nFigure= 7 \r\n");  // (the INI reader trims the spaces)
    CHECK(FigureConfigBuiltin(ini.c_str()) == 7);
    WriteFile(ini, "[Friends]\r\nFigure=007\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == 7);
    WriteFile(ini, "[Friends]\r\nFigure=0013\r\n");
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);  // (more than 3 digits: not a design number)
    WriteFile(ini, "[Menus]\r\nEnabled=1\r\n");  // an older config.ini kept through an update: no section
    CHECK(FigureConfigBuiltin(ini.c_str()) == kFigureDefaultBuiltin);
    CHECK(FigureConfigBuiltin((scratch + L"\\missing.ini").c_str()) == kFigureDefaultBuiltin);

    // ---- the built DLL's exports, with Steam off (the mod is never started here)
    if (argc > 2) {
        wchar_t given[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, given, MAX_PATH);
        GetFullPathNameW(given, MAX_PATH, w, nullptr);  // (the LOAD_LIBRARY_SEARCH_* flags need an absolute path)
        HMODULE m = LoadLibraryExW(w, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) printf("  load of %s failed (%lu)\n", argv[2], GetLastError());
        CHECK(m != nullptr);
        if (m) {
            auto query = (int(__cdecl*)(AsrFigureInfo*, int))GetProcAddress(m, "AsrFigureQuery");
            auto friends = (int(__cdecl*)(AsrFigureFriend*, int))GetProcAddress(m, "AsrFigureFriends");
            auto setSelf = (int(__cdecl*)(int))GetProcAddress(m, "AsrFigureSetSelf");
            CHECK(query && friends && setSelf);
            // 1.1.1-dp13: the script call sites are found by their bytes (race.cpp ScriptSite); a mod that rebuilds a menu script in
            // memory asks for this before inserting a line ahead of one of them.
            auto sitesByPattern = (int(__cdecl*)())GetProcAddress(m, "AsrScriptSitesByPattern");
            CHECK(sitesByPattern && sitesByPattern() == 1);
            if (query && friends && setSelf) {
                AsrFigureInfo info;
                memset(&info, 0xaa, sizeof(info));
                CHECK(query(&info, sizeof(info)) == (int)sizeof(AsrFigureInfo));
                CHECK(info.abi == kAsrFigureAbi && info.friendSize == sizeof(AsrFigureFriend) && info.featureVersion == kFigureFeatureVersion);
                CHECK(info.steamUp == 0 && info.inLobby == 0 && info.racingOnline == 0 && info.selfBuiltin == kFigureDefaultBuiltin &&
                      info.selfDescSize == 0 && info.selfSteamId == 0 && info.friendCount == 0 && info.enabled == 0);
                uint32_t two[3] = {0, 0, 0xdeadbeef};  // a caller with a shorter struct gets what fits
                CHECK(query((AsrFigureInfo*)two, 8) == (int)sizeof(AsrFigureInfo) && two[0] == kAsrFigureAbi && two[1] == sizeof(AsrFigureFriend) &&
                      two[2] == 0xdeadbeef);
                CHECK(query(nullptr, 48) == 0 && query(&info, 0) == 0);
                AsrFigureFriend f[2];
                CHECK(friends(f, 2) == -1);  // the feature is off (the default), and Steam is not running in this process
                CHECK(friends(nullptr, 0) == -1);
                CHECK(setSelf(5) == 0);  // off: refused
                query(&info, sizeof(info));
                CHECK(info.selfBuiltin == kFigureDefaultBuiltin && info.enabled == 0);
                auto enable = (void(__cdecl*)(int))GetProcAddress(m, "SteamMpTestFigureEnable");
                CHECK(enable != nullptr);
                if (enable) {
                    enable(1);  // as config.ini [Friends] Enabled=1 would
                    query(&info, sizeof(info));
                    CHECK(info.enabled == 1 && info.steamUp == 0 && info.friendCount == 0);
                    CHECK(friends(f, 2) == -1);  // on, but Steam is still not running here
                    CHECK(setSelf(5) == 1);
                    query(&info, sizeof(info));
                    CHECK(info.selfBuiltin == 5);
                    CHECK(setSelf(17) == 0 && setSelf(-2) == 0);
                    query(&info, sizeof(info));
                    CHECK(info.selfBuiltin == 5);
                    CHECK(setSelf(-1) == 1);  // back to config.ini's value (the default: the mod read no config here)
                    query(&info, sizeof(info));
                    CHECK(info.selfBuiltin == kFigureDefaultBuiltin);
                    enable(0);  // off again: unavailable, the figure kept
                    query(&info, sizeof(info));
                    CHECK(info.enabled == 0 && info.steamUp == 0 && setSelf(3) == 0 && friends(f, 2) == -1);
                }
            }
            FreeLibrary(m);
        }
    } else {
        printf("  (no DLL given: the export checks were skipped)\n");
    }

    printf("%s (%d failure(s))\n", g_fail ? "SOME FAILED" : "ALL PASS", g_fail);
    return g_fail;
}
