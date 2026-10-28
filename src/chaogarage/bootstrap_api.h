#pragma once
#include <windows.h>
#include <stdint.h>

/* Called synchronously outside DllMain, before the game's own CRT setup.
   No native game function may be called yet. Steam/system threads may exist.
   All strings/bytes remain valid for the duration of this call only. */
#define SSASR_BOOTSTRAP_ABI_V1 1u
#define SSASR_BOOTSTRAP_ABI_V2 2u
#define SSASR_BOOTSTRAP_DISK_AUTHENTICATED 1u
#define SSASR_BOOTSTRAP_NATIVE_CRT_GATE 2u
#define SSASR_BOOTSTRAP_GAME_INITIALIZATION_NOT_STARTED 4u
#define SSASR_HOST_ROLLBACK_FAILED ((HRESULT)0x8004A001L)
/* Bootstrap V2 only: mods were not started and the host has already shown the
   player its one warning for this start. Any other failure is unexpected. */
#define CHAOGARAGE_HOST_NOT_STARTED_WARNED ((HRESULT)0x8004A010L)

struct SsasrBootstrapV1 {
    uint32_t size;
    uint32_t version;
    HMODULE game_image;
    HMODULE bootstrap_image;
    const wchar_t* game_directory;
    const uint8_t* executable_sha256; /* exactly 32 bytes */
    uint32_t flags;
    uint32_t initializing_thread_id;
    uint32_t native_gate_rva;
    BOOL (WINAPI* initialization_is_active)();
};

/* Version 2 keeps the version 1 layout as its prefix (size = sizeof(SsasrBootstrapV2),
   version = 2) and adds the proxy's release, so the host can refuse a proxy from a
   different release with a message instead of guessing. Released 1.0 hosts accept
   only size = sizeof(SsasrBootstrapV1) and version = 1; a 1.1 proxy therefore never
   calls a host that lacks ChaoGarageGetLoaderInfo (see loader_info.h). */
struct SsasrBootstrapV2 {
    SsasrBootstrapV1 v1;
    const wchar_t* bootstrap_release; /* e.g. L"1.1.0" */
};

/* Host must validate ABI and initialization_is_active(). On failure it must
   undo all of its own publication. The proxy retains the host DLL until exit.
   Export this exact undecorated name from the x86 host. The argument points to a
   SsasrBootstrapV1 or to the v1 prefix of a SsasrBootstrapV2. */
typedef HRESULT (__cdecl* SsasrModsHostInitializeFn)(const SsasrBootstrapV1*);
