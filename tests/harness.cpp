// Loads a packaged runtime like ChaoGarage does and calls its entry points.
// usage: harness.exe <mod folder> [run-seconds]
// Prints the HRESULTs; the runtime's own log is <mod folder>\SteamMultiplayer.log.
// If the runtime's steam_api.dll check passes, this WILL initialize Steam as AppID 34190.
#include <windows.h>
#include <stdio.h>
#include <string>
#include "mod_api.h"

static void __cdecl HostLog(const wchar_t* m) { wprintf(L"[host log] %s\n", m); }
static BOOL WINAPI Active() { return TRUE; }

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    std::wstring dir = argv[1];
    // Optional decoy: a different module named steam_api.dll loaded first (tests import binding).
    wchar_t decoy[MAX_PATH];
    if (GetEnvironmentVariableW(L"HARNESS_DECOY", decoy, MAX_PATH))
        wprintf(L"decoy %s: %s\n", decoy,LoadLibraryW(decoy) ? L"loaded" : L"FAILED");
    int seconds = argc > 2 ? _wtoi(argv[2]) : 0;
    HMODULE m = LoadLibraryExW((dir + L"\\runtime\\SteamMultiplayer.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { wprintf(L"load failed %lu\n", GetLastError()); return 3; }
    auto init = (SsasrModInitializeFn)GetProcAddress(m, "SsasrModInitialize");
    auto rollback = (SsasrModRollbackFn)GetProcAddress(m, "SsasrModRollback");
    if (!init || !rollback) return 4;
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    SsasrHostApiV1 api{sizeof(api), 1, GetModuleHandleW(nullptr), cwd, SSASR_HOST_NATIVE_CRT_GATE, GetCurrentThreadId(), 0x248F45, Active, HostLog};
    HRESULT bad = init(nullptr, dir.c_str());
    wprintf(L"init(null api) = 0x%08lX\n", (unsigned long)bad);
    HRESULT hr = init(&api, dir.c_str());
    wprintf(L"init = 0x%08lX\n", (unsigned long)hr);
    wprintf(L"steam_api.dll loaded: %s\n", GetModuleHandleW(L"steam_api.dll") ? L"yes" : L"no");
    Sleep(seconds * 1000);
    hr = rollback();
    wprintf(L"rollback = 0x%08lX\n", (unsigned long)hr);
    wprintf(L"SteamAppId after rollback: %s\n", GetEnvironmentVariableW(L"SteamAppId", cwd, MAX_PATH) ? cwd : L"(unset)");
    return 0;
}
