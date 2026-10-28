// Offline test of the Game Search list's rows (src/search_rows.h): the public list and the friends' lobbies merged, the
// PRIVATE SLOTS rule for strangers, friends first, other mod builds last, the full list.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "search_rows.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static LobbyRow Row(uint64_t lobby, const char* host, int members, int maxMembers = 8, int otherVersion = 0) {
    LobbyRow r = {};
    r.lobby = lobby;
    strcpy_s(r.host, host);
    r.members = members;
    r.maxMembers = maxMembers;
    r.otherVersion = otherVersion;
    return r;
}

int main() {
    const uint64_t own = 0x1000;
    SearchRows s;
    // A stranger's lobby with a public seat free: listed; one whose public seats are taken (3 of 8 seats public, 3 in it):
    // not; a friend's lobby in the same state: listed (every seat counts); a full lobby never; our own never.
    CHECK(s.Add(Row(1, "A", 1), false, 8, own) && s.count == 1 && s.rows[0].lobby == 1 && !s.rows[0].friendRow);
    CHECK(!s.Add(Row(2, "B", 3), false, 3, own) && s.count == 1);
    CHECK(s.Add(Row(2, "B", 3), true, 3, own) && s.count == 2 && s.rows[0].lobby == 2 && s.rows[0].friendRow && s.rows[1].lobby == 1);
    CHECK(!s.Add(Row(3, "C", 8), true, 8, own) && !s.Add(Row(3, "C", 8), false, 8, own) && s.count == 2);
    CHECK(!s.Add(Row(own, "me", 1), false, 8, own) && !s.Add(Row(own, "me", 1), true, 8, own) && s.count == 2);
    CHECK(!s.Add(Row(0, "none", 1), true, 8, own) && s.count == 2);
    // The same lobby from both sources: one row, a friend's flag kept whichever came first, the newer member count.
    CHECK(s.Add(Row(1, "A", 2), true, 8, own) && s.count == 2 && s.rows[0].lobby == 2 && s.rows[1].lobby == 1 && s.rows[1].friendRow &&
          s.rows[1].members == 2);
    CHECK(s.Add(Row(1, "A", 3), false, 8, own) && s.count == 2 && s.rows[1].lobby == 1 && s.rows[1].friendRow && s.rows[1].members == 3);
    CHECK(s.Add(Row(4, "D", 1), false, 8, own) && s.Add(Row(4, "D", 2), true, 8, own) && s.count == 3 && s.rows[2].lobby == 4 && s.rows[2].friendRow);
    // A lobby of another mod build: listed after this build's strangers; a friend's of another build stays with the friends.
    CHECK(s.Add(Row(5, "E", 1, 8, 1), false, 8, own) && s.count == 4 && s.rows[3].lobby == 5 && s.rows[3].otherVersion);
    CHECK(s.Add(Row(6, "F", 1), false, 8, own) && s.count == 5 && s.rows[3].lobby == 6 && s.rows[4].lobby == 5);
    CHECK(s.Add(Row(7, "G", 1, 8, 1), true, 8, own) && s.count == 6 && s.rows[3].lobby == 7 && s.rows[3].friendRow && s.rows[3].otherVersion &&
          s.rows[4].lobby == 6 && s.rows[5].lobby == 5);
    // Order so far: friends 2, 1, 4, 7; stranger 6; other build 5.
    CHECK(s.rows[0].lobby == 2 && s.rows[1].lobby == 1 && s.rows[2].lobby == 4);
    // The list fills up (8 rows): a stranger of this build arriving full replaces the last other-build row, not a friend's;
    // another other-build stranger isn't listed; a friend's replaces the last stranger.
    CHECK(s.Add(Row(8, "H", 1), false, 8, own) && s.Add(Row(9, "I", 1), false, 8, own) && s.count == 8);
    CHECK(s.rows[7].lobby == 5);
    CHECK(s.Add(Row(10, "J", 1), false, 8, own) && s.count == 8 && s.Find(5) < 0 && s.rows[7].lobby == 10 && s.rows[4].lobby == 6);
    CHECK(!s.Add(Row(11, "K", 1, 8, 1), false, 8, own) && s.count == 8 && s.Find(11) < 0);
    CHECK(!s.Add(Row(12, "L", 1), false, 8, own) && s.count == 8 && s.Find(12) < 0);  // (no row ranked below a stranger of this build)
    CHECK(s.Add(Row(13, "M", 1), true, 8, own) && s.count == 8 && s.Find(10) < 0 && s.rows[4].lobby == 13 && s.rows[4].friendRow);
    CHECK(s.Find(2) == 0 && s.Find(13) == 4 && s.Find(6) == 5 && s.Find(99) < 0);
    // A friend's lobby already listed, seen again from the public list while full: merged, not dropped or duplicated.
    CHECK(s.Add(Row(13, "M", 2), false, 8, own) && s.count == 8 && s.Find(13) == 4 && s.rows[4].members == 2 && s.rows[4].friendRow);
    // A listed lobby that the second source shows full: its row goes (the newer count).
    CHECK(!s.Add(Row(13, "M", 8), false, 8, own) && s.count == 7 && s.Find(13) < 0);
    CHECK(!s.Add(Row(6, "F", 8), true, 8, own) && s.count == 6 && s.Find(6) < 0);
    // Clear.
    s.Clear();
    CHECK(s.count == 0 && s.Find(2) < 0);
    // Other mods: after this build's strangers with the same mods, before other builds; a friend's with other mods stays first.
    LobbyRow mods = Row(30, "P", 1);
    mods.otherMods = 1;
    CHECK(s.Add(Row(31, "Q", 1, 8, 1), false, 8, own) && s.Add(mods, false, 8, own) && s.Add(Row(32, "R", 1), false, 8, own));
    CHECK(s.count == 3 && s.rows[0].lobby == 32 && s.rows[1].lobby == 30 && s.rows[2].lobby == 31);
    mods.lobby = 33;
    CHECK(s.Add(mods, true, 8, own) && s.rows[0].lobby == 33 && s.rows[0].friendRow && s.rows[0].otherMods);
    s.Clear();
    // A friend's lobby whose public seats are taken but with a seat left: listed in Play with Friends terms (viaFriend true).
    CHECK(s.Add(Row(20, "N", 7, 8), true, 1, own) && s.count == 1);
    // KICK (M7, lobby.h): the host's "banned" list and the "kick" id as the lobby data holds them.
    CHECK(ParseSteamId("76561198012345678") == 76561198012345678ull);
    CHECK(ParseSteamId("") == 0 && ParseSteamId(nullptr) == 0 && ParseSteamId("12a") == 0 && ParseSteamId("-5") == 0 && ParseSteamId(" 5") == 0);
    CHECK(ParseSteamId("99999999999999999999") == 0 && ParseSteamId("18446744073709551616") == 0);  // past 64 bits (2^64 too)
    CHECK(ParseSteamId("18446744073709551615") == 0xFFFFFFFFFFFFFFFFull);
    CHECK(IdListHas("76561198012345678", 76561198012345678ull));
    CHECK(IdListHas("1,76561198012345678,3", 76561198012345678ull) && IdListHas("1,76561198012345678,3", 3) && IdListHas("1,76561198012345678,3", 1));
    CHECK(!IdListHas("1,76561198012345678,3", 7656119801234567ull) && !IdListHas("", 3) && !IdListHas(nullptr, 3) && !IdListHas("1,,3", 0) && !IdListHas("1,,3", 2));
    printf("search rows: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
