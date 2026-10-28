// Strict parsing of Steam lobby IDs from the command line and rich presence connect strings.
// Header-only so tests/test_parse.cpp can exercise it without Steam.
#pragma once
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "steam/steamclientpublic.h"

// Unsigned decimal, digits only, no sign/space/overflow, and a public-universe lobby SteamID; else 0.
template <typename Ch> inline uint64_t ParseLobbyId(const Ch* s, size_t len) {
    if (!s || len == 0 || len > 20) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return 0;
        unsigned d = (unsigned)(s[i] - '0');
        if (v > (UINT64_MAX - d) / 10) return 0;
        v = v * 10 + d;
    }
    CSteamID id(v);
    return (id.IsLobby() && id.GetEUniverse() == k_EUniversePublic) ? v : 0;
}

// argv form: an argument exactly "+connect_lobby" followed by an argument that is exactly the ID.
inline uint64_t LobbyFromArgs(int argc, wchar_t** argv) {
    for (int i = 1; argv && i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"+connect_lobby") == 0) return ParseLobbyId(argv[i + 1], wcslen(argv[i + 1]));
    return 0;
}

// Rich presence form: exactly "+connect_lobby <id>" (the value this mod publishes), nothing else.
inline uint64_t LobbyFromConnectString(const char* s) {
    static const char kPrefix[] = "+connect_lobby ";
    const size_t n = sizeof(kPrefix) - 1;
    if (!s || strncmp(s, kPrefix, n) != 0) return 0;
    return ParseLobbyId(s + n, strlen(s + n));
}
