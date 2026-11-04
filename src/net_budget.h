#pragma once
// Limits for what the other players' games send (notes\SAFETY.md F2). A member's game could otherwise flood this game with
// tiny messages: a few thousand a second push everyone else's poses, finishes and results out of the race queue and keep the
// Steam worker busy. Each sender gets a message budget, and a full queue drops the biggest sender's messages first.
#include <stdint.h>

// Per sender: kNetRatePerSecond messages a second on average, up to kNetBurst at once; the rest are dropped before they are
// queued. A race needs far less: a pose (or the host's cars) every tick, item events (at most 16 per 15 ticks), a few lines.
constexpr int kNetSenders = 16;
constexpr double kNetRatePerSecond = 400.0, kNetBurst = 400.0;

struct NetBudget {
    struct Sender {
        uint64_t id;
        double tokens;
        uint32_t last;     // ms (GetTickCount) of the last message
        uint32_t dropped;  // messages over its budget
    };
    Sender s[kNetSenders] = {};

    // Whether a message from `id` at `now` (ms) may go on; `firstDrop` is set when this is the sender's first message dropped
    // since its last one taken (the caller logs that, not every drop).
    bool Take(uint64_t id, uint32_t now, bool* firstDrop = nullptr) {
        if (firstDrop) *firstDrop = false;
        Sender* e = nullptr;
        for (Sender& x : s)
            if (x.id == id) { e = &x; break; }
        if (!e) {  // a new sender: a free entry, else the one quiet the longest
            e = &s[0];
            for (Sender& x : s) {
                if (!x.id) { e = &x; break; }
                if ((int32_t)(now - x.last) > (int32_t)(now - e->last)) e = &x;
            }
            *e = Sender{id, kNetBurst, now, 0};
        }
        double add = (double)(uint32_t)(now - e->last) * kNetRatePerSecond / 1000.0;
        if ((int32_t)(now - e->last) < 0) add = 0;  // (a clock going back gives nothing)
        e->tokens = e->tokens + add > kNetBurst ? kNetBurst : e->tokens + add;
        e->last = now;
        if (e->tokens < 1.0) {
            if (firstDrop) *firstDrop = e->dropped == 0;
            ++e->dropped;
            return false;
        }
        e->tokens -= 1.0;
        e->dropped = 0;
        return true;
    }
};

// When the inbound queue (`count` messages, oldest first, each with .peer and .reliable) is full and a message from `incoming`
// arrives: the index of the message to drop, from the sender with the most queued (counting the incoming one): its oldest
// unreliable message, else its oldest; -1 = drop the incoming message itself (its sender has the most and nothing queued).
template <class Msg>
int NetEvictIndex(const Msg* q, int count, uint64_t incoming) {
    uint64_t ids[kNetSenders + 1];
    int counts[kNetSenders + 1], n = 0;
    auto add = [&](uint64_t id) {
        for (int i = 0; i < n; ++i)
            if (ids[i] == id) { ++counts[i]; return; }
        if (n < kNetSenders + 1) { ids[n] = id; counts[n++] = 1; }
    };
    add(incoming);
    for (int i = 0; i < count; ++i) add(q[i].peer);
    int top = 0;
    for (int i = 1; i < n; ++i)
        if (counts[i] > counts[top]) top = i;
    uint64_t victim = ids[top];
    int oldest = -1;
    for (int i = 0; i < count; ++i) {
        if (q[i].peer != victim) continue;
        if (!q[i].reliable) return i;
        if (oldest < 0) oldest = i;
    }
    return oldest;
}
