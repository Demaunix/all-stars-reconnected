// Front-end script substitution (M2): makes the game use our builds of a few modules of
// Scripts/FrontEndScripts.sbn without replacing the file (the All-Stars Roster Pack may own it).
// Works with the stock package and with the Roster Pack's; each gets its own network module.
// See notes/SCRIPT-LOADER.md. The package is parsed once, on the main thread, during game init by
// the thunk 0x4baf90 -> 0x54b780, which turns each 32-byte entry's self-relative offsets into
// pointers and relocates every module in place. Our detour points the target entries' data offsets
// (entry+0x1c) at our own copies just before that parse; the game then relocates and uses them.
#include "script_subst.h"
#include "race.h"
#include <bcrypt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "menus_embed.h"

static const uint32_t kParseThunk = 0x4baf90;  // cdecl(package): mov eax,[esp+4]; push eax; mov ecx,0xce7b98; call 0x54b780; ret
static const uint8_t kParseThunkOrig[16] = {0x8b, 0x44, 0x24, 0x04, 0x50, 0xb9, 0x98, 0x7b, 0xce, 0x00, 0xe8, 0xe1, 0x07, 0x09, 0x00, 0xc3};
// The hook calls 0x54b780 thiscall(tree = 0xce7b98, package), ret 4, exactly as the thunk did.

// Our replacement modules (tools/build_menus.py), each copied once into a writable buffer that is
// never freed (the game keeps pointers into it).
struct Module {
    const uint8_t* bytes;
    size_t size;
    uint8_t* buffer;
};
enum { kModMainMenu, kModDialogs, kModNetRoster, kModNetStock, kModResults, kModNetMetal, kModuleCount };
static Module g_modules[kModuleCount] = {
    {kMenu_fe_mainmenu, sizeof(kMenu_fe_mainmenu)},
    {kMenu_dialogs, sizeof(kMenu_dialogs)},
    {kMenu_fe_network_game, sizeof(kMenu_fe_network_game)},              // 25 racers, for the Roster Pack
    {kMenu_fe_network_game_stock, sizeof(kMenu_fe_network_game_stock)},  // the stock 21 racers
    {kMenu_results, sizeof(kMenu_results)},                              // the results screen with the profile cursor (M7)
    {kMenu_fe_network_game_metal, sizeof(kMenu_fe_network_game_metal)},  // the stock 21 + Metal, for the standalone Metal Sonic 1.1.2
};

// An original module the game may load, and the module of ours that replaces it.
struct Accept {
    const char* origSha256;
    int module;
};
static const char kRosterNet[] = "dda10019b222845c33e0918a6c407a99ad683c9a4741a42de01b4320ca00d36d";  // All-Stars Roster Pack 0.1.0-alpha.2
static const char kStockNet[] = "929d14fb7e413970167f001a5fa84f997eb3df683c1db915af2ea213e796d0c3";   // the game's own Scripts/FrontEndScripts.sbn
static const char kMetalNet[] = "04ad1a0c7a603b725001ffa78ddf29f89abe22ec5e47235cfdb80cd992c1c854";   // the standalone Metal Sonic 1.1.2's
static const char kStockResults[] = "d582a4aaaf3ba5af94ada225b22f08188bd6cb42f47899a8966d68d596b252e3";  // `results`, the same in both packages

struct Target {
    uint32_t nameHash;
    const char* name;
    Accept accept[3];  // fe_mainmenu, dialogs and results are the same in every package (re\m9-metal112\FINDINGS.md)
};
static const Target g_targets[] = {
    {0xee8a2362, "fe_mainmenu", {{"885d11dacd6e404dfa07e454e03b3d736bb0dab48235391e94443e59062633c8", kModMainMenu}}},
    {0x694fe697, "dialogs", {{"ddb2e0e7797d0acd0f19fd47ac6cad6837d3d9dbcc35f38add97f52bb4464185", kModDialogs}}},
    {0x8398d0bc, "fe_network_game", {{kRosterNet, kModNetRoster}, {kStockNet, kModNetStock}, {kMetalNet, kModNetMetal}}},
    // Alias of fe_network_game: same original bytes, same replacement buffer.
    {0xc04d334f, "fe_network_error_handler", {{kRosterNet, kModNetRoster}, {kStockNet, kModNetStock}, {kMetalNet, kModNetMetal}}},
    // The results screen: the console's profile cursor (VIEW PROFILE / GAMER CARD) put back (re\m7-profile\FINDINGS.md).
    {0x8a972456, "results", {{kStockResults, kModResults}}},
};
static const size_t kTargetCount = sizeof(g_targets) / sizeof(g_targets[0]);

static bool g_installed = false;
static bool g_done = false;  // substitution happens on the first FrontEndScripts parse only
static uint8_t* g_entryField[kTargetCount];
static int g_chosen[kTargetCount] = {-1, -1, -1, -1, -1};  // module index per target once substituted

static bool Sha256Hex(const void* data, size_t n, char out[65]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char d[32];
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) &&
              BCRYPT_SUCCESS(BCryptHashData(h, (PUCHAR)data, (ULONG)n, 0)) && BCRYPT_SUCCESS(BCryptFinishHash(h, d, 32, 0));
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    for (int i = 0; i < 32; ++i) sprintf_s(out + 2 * i, 3, "%02x", d[i]);
    return true;
}

static uint32_t U32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Runs inside the game's parse of every script package, before the original parse.
static void __cdecl BeforeParse(uint8_t* pkg) {
    __try {
        if (g_done || memcmp(pkg, "smospkg2", 8) != 0 || U32(pkg + 8) != 32) return;
        uint32_t count = U32(pkg + 12);
        uint8_t* entry[kTargetCount] = {};
        for (uint32_t i = 0; i < count && i < 4096; ++i) {
            uint8_t* e = pkg + 40 + 32 * i;
            for (size_t t = 0; t < kTargetCount; ++t)
                if (U32(e + 16) == g_targets[t].nameHash) entry[t] = e;
        }
        size_t found = 0;
        for (size_t t = 0; t < kTargetCount; ++t) found += entry[t] != nullptr;
        if (found == 0) return;  // another package (SetupObjects, Behaviours, ...)
        g_done = true;
        if (found != kTargetCount) {
            Log(true, L"multiplayer menus: off (FrontEndScripts has %u of %u expected modules)", (unsigned)found, (unsigned)kTargetCount);
            return;
        }
        // All or nothing: every original must be exactly a module one of our builds was made from.
        int chosen[kTargetCount];
        for (size_t t = 0; t < kTargetCount; ++t) {
            const uint8_t* mod = entry[t] + 28 + U32(entry[t] + 28);  // self-relative before the parse
            char sha[65];
            chosen[t] = -1;
            bool isModule = memcmp(mod, "smosbinL", 8) == 0 && Sha256Hex(mod, U32(mod + 8), sha);
            for (const Accept& a : g_targets[t].accept)
                if (isModule && a.origSha256 && strcmp(sha, a.origSha256) == 0) chosen[t] = a.module;
            if (chosen[t] < 0) {
                Log(true, L"multiplayer menus: off (the installed %hs script is not one this version was built for; sha256 %hs)",
                    g_targets[t].name, isModule ? sha : "not a module");
                return;
            }
        }
        for (size_t t = 0; t < kTargetCount; ++t) {
            uint8_t* buf = g_modules[chosen[t]].buffer;
            *(uint32_t*)(entry[t] + 28) = (uint32_t)(uintptr_t)buf - (uint32_t)(uintptr_t)(entry[t] + 28);  // 32-bit wrap reaches anywhere
            g_entryField[t] = entry[t] + 28;
            g_chosen[t] = chosen[t];
        }
        Log(true, L"multiplayer menus: using our front-end scripts (fe_mainmenu, dialogs, results, fe_network_game: %s)",
            g_chosen[2] == kModNetRoster ? L"Roster Pack, 25 racers"
            : g_chosen[2] == kModNetMetal ? L"the standalone Metal Sonic (1.1.2 or newer), 22 racers"
                                          : L"stock game, 21 racers");
#ifndef SCRIPT_SUBST_TEST
        // The race contract maps lobby racer numbers through it.
        RaceUseRacerList(g_chosen[2] == kModNetRoster ? kRacerListRoster : g_chosen[2] == kModNetMetal ? kRacerListMetal : kRacerListStock);
#endif
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(true, L"multiplayer menus: off (fault while reading the script package)");
    }
}

// After the parse the game has made entry+0x1c absolute and relocated our copies (+0x38 = 1).
static void __cdecl AfterParse() {
    static bool checked = false;
    if (checked || !g_entryField[0]) return;
    checked = true;
    for (size_t t = 0; t < kTargetCount; ++t) {
        uint8_t* buf = g_modules[g_chosen[t]].buffer;
        if (U32(g_entryField[t]) != (uint32_t)(uintptr_t)buf || U32(buf + 0x38) != 1)
            Log(true, L"multiplayer menus: WARNING %hs not bound as expected (entry %08x, relocated %u)", g_targets[t].name,
                U32(g_entryField[t]), U32(buf + 0x38));
    }
    Log(false, L"multiplayer menus: scripts relocated by the game");
}

__declspec(naked) static void ParseThunkHook() {
    __asm {
        push dword ptr [esp + 4]
        call BeforeParse
        add esp, 4
        push dword ptr [esp + 4]
        mov ecx, 0x00ce7b98  // kModuleTree
        mov eax, 0x0054b780  // kParsePackage
        call eax
        push eax
        call AfterParse
        pop eax
        ret
    }
}

static bool WriteCode(uint32_t va, const uint8_t* bytes, size_t n) {
    DWORD old, ignored;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(uintptr_t)va, bytes, n);
    VirtualProtect((void*)(uintptr_t)va, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)va, n);
    return true;
}

// One writable, page-aligned copy per module, kept for the life of the process.
static bool PrepareBuffers() {
    for (Module& m : g_modules) {
        if (m.buffer) continue;
        m.buffer = (uint8_t*)VirtualAlloc(nullptr, m.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!m.buffer) return false;
        memcpy(m.buffer, m.bytes, m.size);
    }
    return true;
}

#ifdef SCRIPT_SUBST_TEST
// tests/test_script_subst.cpp: run the real pre-parse step on a package copy in memory.
bool ScriptSubstTestPrepare() { return PrepareBuffers(); }
void ScriptSubstTestBeforeParse(uint8_t* pkg) { BeforeParse(pkg); }
// The buffer target i now points at (nullptr if not substituted), and whether it is the stock build.
const uint8_t* ScriptSubstTestBuffer(size_t i) { return i < kTargetCount && g_chosen[i] >= 0 ? g_modules[g_chosen[i]].buffer : nullptr; }
bool ScriptSubstTestIsStock(size_t i) { return i < kTargetCount && g_chosen[i] == kModNetStock; }
bool ScriptSubstTestIsMetal(size_t i) { return i < kTargetCount && g_chosen[i] == kModNetMetal; }
#endif

void ScriptSubstInstall(HMODULE gameImage, const std::wstring& modDir) {
    std::wstring ini = modDir + L"\\config.ini";
    if (GetPrivateProfileIntW(L"Menus", L"Enabled", 1, ini.c_str()) == 0) {
        Log(false, L"multiplayer menus: off (config.ini [Menus] Enabled=0)");
        return;
    }
    if ((uintptr_t)gameImage != 0x400000) {
        Log(true, L"multiplayer menus: off (game image at %p, expected 0x400000)", gameImage);
        return;
    }
    if (memcmp((void*)(uintptr_t)kParseThunk, kParseThunkOrig, sizeof(kParseThunkOrig)) != 0) {
        Log(true, L"multiplayer menus: off (code at 0x%06x is not the expected original; another mod may patch it)", kParseThunk);
        return;
    }
    if (!PrepareBuffers()) {
        Log(true, L"multiplayer menus: off (out of memory)");
        return;
    }
    uint8_t jmp[5] = {0xe9};
    uint32_t rel = (uint32_t)(uintptr_t)ParseThunkHook - (kParseThunk + 5);
    memcpy(jmp + 1, &rel, 4);
    if (!WriteCode(kParseThunk, jmp, sizeof(jmp))) {
        Log(true, L"multiplayer menus: off (could not patch 0x%06x, error %lu)", kParseThunk, GetLastError());
        return;
    }
    g_installed = true;
    Log(false, L"multiplayer menus: script hook installed");
}

void ScriptSubstRollback() {
    if (!g_installed) return;
    WriteCode(kParseThunk, kParseThunkOrig, 5);
    g_installed = false;
}
