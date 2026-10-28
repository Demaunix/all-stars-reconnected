#pragma once
// The Game Search list (LobbyRow, lobby.h) as the Steam worker builds it (steam_mp.cpp Search) from two sources: Steam's
// public lobby list (Play with Anyone) and the friends' current lobbies (their games' lobby ids, both modes). Since 2026-10-22
// (the user's wish) a friend's friends-only lobby shows up in Play with Anyone too, and a lobby of another Steam Multiplayer
// build is listed, marked, and refused before any join (the console's list had a compatibility column). Pure logic, tested
// offline (tests\test_search_rows.cpp); the worker adds the Steam data and the mode.
#include <string.h>
#include "lobby.h"

struct SearchRows {
    LobbyRow rows[kMaxRows] = {};
    int count = 0;

    void Clear() { count = 0; }

    int Find(uint64_t lobby) const {
        for (int i = 0; i < count; ++i)
            if (rows[i].lobby == lobby) return i;
        return -1;
    }

    // The list's order: friends' lobbies first, then strangers' of this build with the same mods, then strangers' with other
    // mods, then strangers' of other builds (joinable only with their build), each group in arrival order.
    static int Rank(const LobbyRow& r) { return r.friendRow ? 0 : r.otherVersion ? 3 : r.otherMods ? 2 : 1; }

    // Adds a lobby whose data is known, or merges it with its row (a friend's public lobby comes from both sources).
    // `viaFriend`: found through a friend's game, so every seat counts, as in Play with Friends; a stranger's lobby needs
    // one of its `publicSeats` (the host's PRIVATE SLOTS rule) free. Full lobbies and our own are never listed. With the
    // list full, a lobby replaces the last one ranked below it, else it isn't listed. True when the lobby is listed.
    bool Add(const LobbyRow& in, bool viaFriend, int publicSeats, uint64_t ownLobby) {
        LobbyRow row = in;
        if (viaFriend) row.friendRow = 1;
        if (!row.lobby || row.lobby == ownLobby) return false;
        int at = Find(row.lobby);
        if (at >= 0) {  // seen already (the other source): the newer count, a friend's flag kept, then placed again (or gone)
            row.friendRow |= rows[at].friendRow;
            Remove(at);
        }
        if (row.members >= row.maxMembers) return false;  // full (the newer count)
        if (!row.friendRow && row.members >= publicSeats) return false;
        if (count == kMaxRows) {
            int drop = -1;
            for (int i = count - 1; i >= 0 && drop < 0; --i)
                if (Rank(rows[i]) > Rank(row)) drop = i;
            if (drop < 0) return false;
            Remove(drop);
        }
        int pos = count;
        while (pos > 0 && Rank(rows[pos - 1]) > Rank(row)) --pos;
        for (int i = count; i > pos; --i) rows[i] = rows[i - 1];
        rows[pos] = row;
        ++count;
        return true;
    }

private:
    void Remove(int at) {
        for (int i = at; i + 1 < count; ++i) rows[i] = rows[i + 1];
        --count;
        memset(&rows[count], 0, sizeof(rows[count]));
    }
};
