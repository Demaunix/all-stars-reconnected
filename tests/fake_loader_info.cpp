// A stand-in ChaoGarage.dll for the auto-update's end-to-end test (tests/update_e2e.py): only ChaoGarageGetLoaderInfo,
// with ChaoGarage 1.3.0's capabilities that matter here (or 1.2.0's, without pending-mod-updates, with FAKE_OLD_LOADER).
// Never installed anywhere.
#include <stdint.h>

struct LoaderInfoV1 {
    uint32_t size, version;
    const wchar_t* release;
    uint32_t bootstrapAbi, hostAbi, modIniSchema, planSchema;
    const char* const* capabilities;
    const wchar_t* releasesUrl;
};

#ifdef FAKE_OLD_LOADER
static const char* const kCaps[] = {"xpac-member-variants", "xpac-member-optional", nullptr};
static const LoaderInfoV1 kInfo = {sizeof(LoaderInfoV1), 1, L"1.2.0-test", 2, 1, 1, 1, kCaps, L"(test)"};
#else
static const char* const kCaps[] = {"xpac-member-variants", "xpac-member-optional", "pending-mod-updates", nullptr};
static const LoaderInfoV1 kInfo = {sizeof(LoaderInfoV1), 1, L"1.3.0-test", 2, 1, 1, 1, kCaps, L"(test)"};
#endif

extern "C" __declspec(dllexport) const LoaderInfoV1* __cdecl ChaoGarageGetLoaderInfo(void) { return &kInfo; }
