// Checks every background-running patch site (src/bg_sites.h) against the decrypted exe copy:
// the original bytes must be exactly what the runtime expects to find in memory.
// usage: test_bg_sites.exe <path to re\bin\SSASR-unpacked.exe>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "bg_sites.h"

static std::vector<uint8_t> g_file;

static const uint8_t* At(uint32_t va, size_t n) {
    uint32_t pe = *(const uint32_t*)&g_file[0x3c];
    uint16_t nsec = *(const uint16_t*)&g_file[pe + 6];
    uint16_t optsz = *(const uint16_t*)&g_file[pe + 20];
    uint32_t base = *(const uint32_t*)&g_file[pe + 24 + 28];
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = &g_file[pe + 24 + optsz + 40 * i];
        uint32_t vsz = *(const uint32_t*)(s + 8), rva = *(const uint32_t*)(s + 12);
        uint32_t rawsz = *(const uint32_t*)(s + 16), raw = *(const uint32_t*)(s + 20);
        if (va >= base + rva && va + n <= base + rva + (vsz > rawsz ? rawsz : vsz))
            return &g_file[raw + (va - base - rva)];
    }
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    g_file.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(g_file.data(), 1, g_file.size(), f);
    fclose(f);
    int fail = 0;
    for (const BgSite& s : kBgSites) {
        const uint8_t* p = At(s.va, s.len);
        bool ok = p && memcmp(p, s.orig, s.len) == 0;
        printf("%s 0x%06x %-40s\n", ok ? "OK  " : "FAIL", s.va, s.what);
        fail += !ok;
    }
    // The XInput stub the pad wrapper calls must still be jmp [0x67d24c].
    static const uint8_t stub[] = {0xff, 0x25, 0x4c, 0xd2, 0x67, 0x00};
    const uint8_t* p = At(kXInputGetStateStub, sizeof(stub));
    bool ok = p && memcmp(p, stub, sizeof(stub)) == 0;
    printf("%s 0x%06x XInputGetState stub\n", ok ? "OK  " : "FAIL", kXInputGetStateStub);
    fail += !ok;
    // The script package parse thunk that src/script_subst.cpp detours.
    static const uint8_t thunk[] = {0x8b, 0x44, 0x24, 0x04, 0x50, 0xb9, 0x98, 0x7b, 0xce, 0x00, 0xe8, 0xe1, 0x07, 0x09, 0x00, 0xc3};
    p = At(0x4baf90, sizeof(thunk));
    ok = p && memcmp(p, thunk, sizeof(thunk)) == 0;
    printf("%s 0x4baf90 script package parse thunk\n", ok ? "OK  " : "FAIL");
    fail += !ok;
    printf("%d failure(s)\n", fail);
    return fail;
}
