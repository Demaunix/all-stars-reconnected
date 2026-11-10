#pragma once
// RANDOM racer / course slots in the online lobby (the user's request 2026-11-09, an addition the original never had;
// notes\RANDOM-SLOTS.md). A player on RANDOM publishes member data "racer" = kRacerRandom, so every screen shows the stock "?"
// until the loading screen. Each such player's own game keeps a hidden pick (member data "rnd") from the racers its own list
// offers, never one another player already has: a fixed pick always wins, and between two RANDOM players the earlier seat (host
// first, then seat order) keeps its pick and the later one rolls again. The host's Start races that pick. A new pick every race.
// Header-only for tests/test_random.cpp.
#include <stdint.h>
#include <stdio.h>
#include "lobby.h"

constexpr int32_t kRacerRandom = 99;  // member data "racer" of a player on RANDOM (no racer list is this long)
constexpr int kRandomMaxRacers = 64;

// Is `pick` free for member `me`: not another member's fixed racer, nor the pick of a RANDOM member in an earlier seat.
inline bool RandomPickFree(const LobbyMember* m, int n, int me, int32_t pick) {
    for (int j = 0; j < n; ++j) {
        if (j == me) continue;
        if (m[j].racer == pick) return false;
        if (j < me && m[j].racer == kRacerRandom && m[j].random == pick) return false;
    }
    return true;
}

// Any racer this game offers that is free for `me`?
inline bool RandomAnyFree(const LobbyMember* m, int n, int me, const bool* offered, int count) {
    for (int i = 0; i < count && i < kRandomMaxRacers; ++i)
        if (offered[i] && RandomPickFree(m, n, me, i)) return true;
    return false;
}

// This game's pick still holds: offered here and free, or a duplicate rolled because nothing was free and still nothing is.
inline bool RandomPickValid(const LobbyMember* m, int n, int me, int32_t pick, bool dup, const bool* offered, int count) {
    if (pick < 0 || pick >= count || pick >= kRandomMaxRacers || !offered[pick]) return false;
    if (RandomPickFree(m, n, me, pick)) return true;
    return dup && !RandomAnyFree(m, n, me, offered, count);
}

// A new pick for `me` from the free racers this game offers (r: a random number). None free: any racer offered here, as a
// duplicate (dup = true; a tiny unlocked list). -1: this game offers no racer at all.
inline int32_t RandomRoll(const LobbyMember* m, int n, int me, const bool* offered, int count, uint32_t r, bool& dup) {
    int pool[kRandomMaxRacers], k = 0;
    for (int i = 0; i < count && i < kRandomMaxRacers; ++i)
        if (offered[i] && RandomPickFree(m, n, me, i)) pool[k++] = i;
    dup = false;
    if (!k) {
        for (int i = 0; i < count && i < kRandomMaxRacers; ++i)
            if (offered[i]) pool[k++] = i;
        dup = k > 0;
    }
    return k ? pool[r % (uint32_t)k] : -1;
}

// The host at Start: the racer member i races; -1 = its RANDOM pick is not settled yet (none published, or it still collides:
// an update in flight), so Start waits.
inline int32_t RandomHostRacer(const LobbyMember* m, int n, int i, int count) {
    if (m[i].racer != kRacerRandom) return m[i].racer;
    int32_t pick = m[i].random;
    if (pick < 0 || pick >= count) return -1;
    if (!m[i].randomDup && !RandomPickFree(m, n, i, pick)) return -1;
    return pick;
}

// Member data "rnd": the pick, with a "d" when it is a duplicate; "-1" for none.
inline void RandomData(int32_t pick, bool dup, char* out, size_t size) {
    if (pick < 0) sprintf_s(out, size, "-1");
    else sprintf_s(out, size, dup ? "%dd" : "%d", pick);
}
inline void RandomParse(const char* s, int32_t& pick, int32_t& dup) {
    pick = -1;
    dup = 0;
    if (!s || !*s) return;
    int v = 0, digits = 0;
    for (; *s >= '0' && *s <= '9' && digits < 3; ++s, ++digits) v = v * 10 + (*s - '0');
    if (!digits || v >= kRandomMaxRacers) return;
    bool d = *s == 'd';
    if (d) ++s;
    if (*s) return;
    pick = v;
    dup = d ? 1 : 0;
}
