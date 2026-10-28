#pragma once
#include <windows.h>
#include <stdint.h>
#include "bootstrap_api.h"
#define SSASR_HOST_ABI_V1 1u
// Game initialization has not begun; this does not assert no Steam threads exist.
#define SSASR_HOST_NATIVE_CRT_GATE 1u
struct SsasrHostApiV1 {
    uint32_t size;
    uint32_t version;
    HMODULE game_image;
    const wchar_t* game_directory;
    uint32_t flags;
    uint32_t initializing_thread_id;
    uint32_t native_gate_rva;
    BOOL (WINAPI* initialization_is_active)();
    void (__cdecl* log)(const wchar_t* message);
};
// Called outside DllMain. No native game functions may be invoked yet.
// The host installs validated resource redirection before this entry, affecting
// IAT entries only; native .text remains unchanged for the first runtime guard.
typedef HRESULT (__cdecl* SsasrModInitializeFn)(const SsasrHostApiV1*, const wchar_t* mod_directory);
// Called only before game initialization if another mod's initialization fails.
// Revert owned hooks and configuration; retain memory/DLLs until process exit.
// A failed rollback aborts startup; never continue with a partial publication.
typedef HRESULT (__cdecl* SsasrModRollbackFn)();
