// RANDOM racer slots (src/random_slots.h): the hidden pick, the tie rule, the host's view at Start, the member data.
#include <string.h>
#include <stdio.h>
#include "random_slots.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static LobbyMember Member(int32_t racer, int32_t random = -1, int32_t dup = 0) {
    LobbyMember m = {};
    m.racer = racer;
    m.random = random;
    m.randomDup = dup;
    return m;
}

int main() {
    const int count = 6;
    bool all[count] = {true, true, true, true, true, true};
    bool dup = false;

    // Seats: 0 host (fixed racer 2), 1 RANDOM (me), 2 fixed racer 4.
    LobbyMember m[3] = {Member(2), Member(kRacerRandom), Member(4)};
    bool ok = true;
    for (uint32_t r = 0; r < 200; ++r) {
        int32_t p = RandomRoll(m, 3, 1, all, count, r, dup);
        ok = ok && p >= 0 && p < count && p != 2 && p != 4 && !dup;
    }
    Check(ok, "a roll never gives another player's racer");
    bool seen[count] = {};
    for (uint32_t r = 0; r < 200; ++r) seen[RandomRoll(m, 3, 1, all, count, r, dup)] = true;
    Check(seen[0] && seen[1] && seen[3] && seen[5], "every free racer can come up");

    bool some[count] = {false, true, false, true, false, false};  // this game's list offers only racers 1 and 3
    ok = true;
    for (uint32_t r = 0; r < 50; ++r) {
        int32_t p = RandomRoll(m, 3, 1, some, count, r, dup);
        ok = ok && (p == 1 || p == 3);
    }
    Check(ok, "only racers this game's own list offers (locked ones never)");

    // Two RANDOM players: the earlier seat keeps its pick, the later one rolls again.
    LobbyMember two[3] = {Member(0), Member(kRacerRandom, 3), Member(kRacerRandom, 3)};
    Check(RandomPickValid(two, 3, 1, 3, false, all, count), "two RANDOM players on one racer: the earlier seat keeps it");
    Check(!RandomPickValid(two, 3, 2, 3, false, all, count), "... and the later seat's pick no longer holds");
    ok = true;
    for (uint32_t r = 0; r < 50; ++r) {
        int32_t p = RandomRoll(two, 3, 2, all, count, r, dup);
        ok = ok && p != 3 && p != 0;
    }
    Check(ok, "the later seat's new pick avoids the earlier seat's pick and the fixed racer");
    Check(RandomHostRacer(two, 3, 1, count) == 3 && RandomHostRacer(two, 3, 2, count) == -1, "the host waits for the later seat's new pick");
    two[2].random = 5;
    Check(RandomHostRacer(two, 3, 2, count) == 5 && RandomHostRacer(two, 3, 0, count) == 0, "settled: the host races both picks and the fixed racer");

    // A fixed pick always beats a RANDOM one (even from a later seat).
    LobbyMember fixedLater[2] = {Member(kRacerRandom, 4), Member(4)};
    Check(!RandomPickValid(fixedLater, 2, 0, 4, false, all, count), "a later player picking my hidden racer: my pick no longer holds");
    Check(RandomHostRacer(fixedLater, 2, 0, count) == -1, "the host waits until that RANDOM player rolled again");

    // Nothing free: a duplicate, which holds only while nothing is free.
    bool one[count] = {false, false, true, false, false, false};
    int32_t p = RandomRoll(m, 3, 1, one, count, 7, dup);
    Check(p == 2 && dup, "every offered racer taken: a duplicate of one offered here");
    Check(RandomPickValid(m, 3, 1, 2, true, one, count), "the duplicate holds while nothing is free");
    bool oneMore[count] = {false, true, true, false, false, false};
    Check(!RandomPickValid(m, 3, 1, 2, true, oneMore, count), "a racer frees up: roll again (no duplicate when avoidable)");
    LobbyMember dupM[2] = {Member(2), Member(kRacerRandom, 2, 1)};
    Check(RandomHostRacer(dupM, 2, 1, count) == 2, "the host accepts a pick marked duplicate");
    bool none[count] = {};
    Check(RandomRoll(m, 3, 1, none, count, 1, dup) == -1 && !dup, "this game offers no racer: no pick");
    Check(RandomHostRacer(m, 3, 1, count) == -1, "a RANDOM player with no pick: the host waits");

    Check(!RandomPickValid(m, 3, 1, 9, false, all, count) && RandomPickValid(m, 3, 1, 1, false, some, count),
          "a pick outside the list never holds; an offered free one does");
    Check(!RandomPickValid(m, 3, 1, 0, false, some, count), "a pick this game no longer offers (a mod switched off): roll again");

    // Member data "rnd".
    char t[16];
    RandomData(-1, false, t, sizeof(t));
    Check(!strcmp(t, "-1"), "no pick: \"-1\"");
    RandomData(12, false, t, sizeof(t));
    Check(!strcmp(t, "12"), "a pick: its index");
    RandomData(3, true, t, sizeof(t));
    Check(!strcmp(t, "3d"), "a duplicate: the index and d");
    int32_t pick, d;
    RandomParse("12", pick, d);
    Check(pick == 12 && !d, "parsed: a pick");
    RandomParse("3d", pick, d);
    Check(pick == 3 && d, "parsed: a duplicate");
    const char* bad[] = {nullptr, "", "-1", "x", "64", "1000", "3dd", "3x", "d", " 3"};
    ok = true;
    for (const char* b : bad) {
        RandomParse(b, pick, d);
        ok = ok && pick == -1 && !d;
    }
    Check(ok, "parsed: none, garbage, out of range and a dropped member (null) are no pick");

    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
