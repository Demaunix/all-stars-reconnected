// Offline test of src/script_subst.cpp's pre-parse step on a real package, without the game.
// The hook acts on the first FrontEndScripts parse only, so each mode runs in its own process.
// usage: test_script_subst.exe pack  <Roster Pack FrontEndScripts.sbn>
//        test_script_subst.exe stock <the game's own Scripts/FrontEndScripts.sbn>
//        test_script_subst.exe metal <the standalone Metal Sonic 1.1.2's FrontEndScripts.sbn> (its 22-racer module)
//          After BeforeParse and a simulated parse (entry+0x1c += its own address, as 0x54b780
//          does), the four target entries resolve to our buffers (the 25- or 21-racer network
//          module respectively); every other entry is unchanged.
//        test_script_subst.exe other <either package>
//          One byte of its fe_network_game is changed first: nothing is substituted and the log
//          says why.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

bool ScriptSubstTestPrepare();
void ScriptSubstTestBeforeParse(uint8_t* pkg);
const uint8_t* ScriptSubstTestBuffer(size_t i);
bool ScriptSubstTestIsStock(size_t i);
bool ScriptSubstTestIsMetal(size_t i);

static std::vector<std::wstring> g_log;
void Log(bool, const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    g_log.push_back(buf);
    wprintf(L"  log: %s\n", buf);
}

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static std::vector<uint8_t> ReadAll(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    v.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(v.data(), 1, v.size(), f);
    fclose(f);
    return v;
}

static uint32_t U32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

// The game's parse of each entry's data field (notes/SCRIPT-LOADER.md, `add [edi],edi` at 0x54b7bc).
static std::vector<const uint8_t*> SimulateParse(uint8_t* pkg) {
    std::vector<const uint8_t*> mods;
    for (uint32_t i = 0; i < U32(pkg + 12); ++i) {
        uint8_t* field = pkg + 40 + 32 * i + 28;
        uint32_t abs = U32(field) + (uint32_t)(uintptr_t)field;
        memcpy(field, &abs, 4);
        mods.push_back((const uint8_t*)(uintptr_t)abs);
    }
    return mods;
}

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool stock = strcmp(argv[1], "stock") == 0;
    const bool metal = strcmp(argv[1], "metal") == 0;  // the standalone Metal Sonic 1.1.2's package
    const bool pack = stock || metal || strcmp(argv[1], "pack") == 0;  // "pack": substitution expected
    CHECK(ScriptSubstTestPrepare());
    std::vector<uint8_t> pkg = ReadAll(argv[2]);
    if (!pack) {  // corrupt one code byte of fe_network_game (entry hash 8398d0bc)
        for (uint32_t i = 0; i < U32(pkg.data() + 12); ++i) {
            uint8_t* e = pkg.data() + 40 + 32 * i;
            if (U32(e + 16) == 0x8398d0bc) (e + 28 + U32(e + 28))[0x100] ^= 1;
        }
    }
    std::vector<uint8_t> before = pkg;
    ScriptSubstTestBeforeParse(pkg.data());
    std::vector<const uint8_t*> got = SimulateParse(pkg.data());
    std::vector<const uint8_t*> orig = SimulateParse(before.data());
    // Our buffers, in script_subst.cpp's target order: fe_mainmenu, dialogs, fe_network_game, alias, results.
    const uint32_t targets[5] = {0xee8a2362, 0x694fe697, 0x8398d0bc, 0xc04d334f, 0x8a972456};
    int substituted = 0;
    for (uint32_t i = 0; i < U32(pkg.data() + 12); ++i) {
        uint32_t hash = U32(pkg.data() + 40 + 32 * i + 16);
        int t = -1;
        for (int k = 0; k < 5; ++k) if (targets[k] == hash) t = k;
        bool isOurs = t >= 0 && got[i] == ScriptSubstTestBuffer(t);
        bool sameAsOriginal = got[i] - pkg.data() == orig[i] - before.data();
        substituted += isOurs;
        if (pack) {
            CHECK(t >= 0 ? isOurs : sameAsOriginal);
            if (isOurs) CHECK(memcmp(got[i], "smosbinL", 8) == 0);
        } else {
            CHECK(sameAsOriginal);
        }
    }
    printf("%s: %d entr%s point at our modules\n", argv[1], substituted, substituted == 1 ? "y" : "ies");
    CHECK(pack ? substituted == 5 : substituted == 0);
    if (pack) CHECK(ScriptSubstTestBuffer(4) && memcmp(ScriptSubstTestBuffer(4), "smosbinL", 8) == 0);  // the results module
    CHECK(pack ? ScriptSubstTestBuffer(2) == ScriptSubstTestBuffer(3) : true);  // the alias shares one buffer
    if (pack) CHECK(ScriptSubstTestIsStock(2) == stock && ScriptSubstTestIsStock(3) == stock);
    if (pack) CHECK(ScriptSubstTestIsMetal(2) == metal && ScriptSubstTestIsMetal(3) == metal);
    if (!pack) CHECK(!g_log.empty() && g_log.back().find(L"not one this version was built for") != std::wstring::npos);
    printf("script substitution test (%s): %s\n", argv[1], g_fail ? "FAIL" : "PASS");
    return g_fail;
}
