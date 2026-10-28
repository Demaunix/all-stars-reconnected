// Unit tests for src/parse.h (no Steam needed). Exit code = number of failures.
#include <stdio.h>
#include "parse.h"

static int g_fail = 0;
#define CHECK(expr, want)                                                                        \
    do {                                                                                         \
        unsigned long long got_ = (expr);                                                        \
        if (got_ != (unsigned long long)(want)) {                                                \
            printf("FAIL line %d: %s = %llu, want %llu\n", __LINE__, #expr, got_, (unsigned long long)(want)); \
            ++g_fail;                                                                            \
        }                                                                                        \
    } while (0)

int main() {
    const uint64_t L = 109775241600000042ull;  // a lobby ID in Steam's format
    // connect strings
    CHECK(LobbyFromConnectString("+connect_lobby 109775241600000042"), L);
    CHECK(LobbyFromConnectString("junk+connect_lobby 109775241600000042"), 0);
    CHECK(LobbyFromConnectString("+connect_lobby109775241600000042"), 0);
    CHECK(LobbyFromConnectString("+connect_lobby 109775241600000042 junk"), 0);
    CHECK(LobbyFromConnectString("+connect_lobby  109775241600000042"), 0);
    CHECK(LobbyFromConnectString("+connect_lobby -18337531782745804743"), 0);  // wraps to a lobby ID with strtoui64
    CHECK(LobbyFromConnectString("+connect_lobby +109775241600000042"), 0);
    CHECK(LobbyFromConnectString("+connect_lobby 99999999999999999999"), 0);    // overflow
    CHECK(LobbyFromConnectString("+connect_lobby 76561198500000002"), 0);       // individual account, not a lobby
    CHECK(LobbyFromConnectString("+connect_lobby "), 0);
    CHECK(LobbyFromConnectString(""), 0);
    CHECK(LobbyFromConnectString(nullptr), 0);
    // argv
    wchar_t a0[] = L"game.exe", a1[] = L"+connect_lobby", a2[] = L"109775241600000042", bad[] = L"10977524160000004x",
            upper[] = L"+CONNECT_LOBBY";
    wchar_t* ok[] = {a0, a1, a2};
    CHECK(LobbyFromArgs(3, ok), L);
    wchar_t* missing[] = {a0, a1};
    CHECK(LobbyFromArgs(2, missing), 0);
    wchar_t* badId[] = {a0, a1, bad};
    CHECK(LobbyFromArgs(3, badId), 0);
    wchar_t* wrongCase[] = {a0, upper, a2};
    CHECK(LobbyFromArgs(3, wrongCase), 0);
    wchar_t* first[] = {a0, a2, a1, a2};
    CHECK(LobbyFromArgs(4, first), L);
    CHECK(LobbyFromArgs(0, nullptr), 0);
    printf("parse tests: %s (%d failure(s))\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail;
}
