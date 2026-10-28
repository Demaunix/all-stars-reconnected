#pragma once
// Smooth playback of another player's car from its Pose messages (notes/RACE-BRIDGE.md).
//
// Paced by sim ticks only: both games step their simulation 50 times per second. The sender stamps
// each pose with its own tick; the receiver notes its own tick when it takes the pose from the
// network queue. The car is shown a small, steady delay behind the newest data, so there is
// almost always a later pose to blend towards. Codex's lesson (its v17-v19 tests): pace playback
// by the game's ticks, never by the wall clock, or the car jerks.
//
// How the delay is chosen: d = (our tick at arrival) - (their tick) is the arrival delay plus the
// difference between the two tick counters. Its minimum over the last few seconds is the best
// case; showing the car at (our tick - min d - buffer) leaves `buffer` ticks for jitter. The
// playback cursor follows that target at up to +-5% speed, and jumps only after a long gap.
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>

struct PlayoutPose {
    float pos[3], rot[4], vel[3], spin[3];
    uint16_t nav;  // the sender's nav path (not blended: the nearer pose's)
    uint8_t fx;    // the sender's exhaust effect (race_wire.h kFx*; not blended: the nearer pose's)
};

class Playout {
public:
    struct Settings {
        int bufferTicks = 3;     // least extra delay for jitter (60 ms); grows with measured jitter
        int maxBufferTicks = 15; // most extra delay (300 ms)
        int windowTicks = 150;   // min-delay window (3 s)
        int maxExtrapTicks = 5;  // beyond the newest pose, keep moving this long (100 ms), then hold
        int staleTicks = 50;     // no new pose for this long (1 s): stop showing (sender quiet)
        int jumpTicks = 25;      // cursor this far from its target: jump instead of easing
        double slew = 0.05;      // the cursor may run 5% fast or slow to reach its target
    };

    Playout() { Reset(); }
    explicit Playout(const Settings& s) : m_set(s) { Reset(); }

    void Reset() {
        m_count = 0;
        m_delayCount = 0;
        m_started = false;
        m_haveLocal = false;
        m_haveDelay = false;
    }

    // A pose taken from the network at our tick `localTick`. Older or duplicate ticks are dropped,
    // a late one that still fits between kept poses is slotted in. Returns false if dropped.
    bool Push(uint32_t remoteTick, uint32_t localTick, const PlayoutPose& p, bool snap) {
        if (m_haveLocal && (int32_t)(localTick - m_lastLocal) < 0) Reset();  // our counter went back: start over
        if (m_count && (int32_t)(localTick - m_lastArrival) > m_set.staleTicks) Reset();  // after a silence, start fresh
        if (m_count && (int32_t)(remoteTick - m_s[0].tick) <= 0) return false;  // older than everything kept
        int at = m_count;
        while (at > 0 && (int32_t)(remoteTick - m_s[at - 1].tick) < 0) --at;
        if (at > 0 && m_s[at - 1].tick == remoteTick) return false;  // duplicate
        if (m_count == kCap) {  // full: forget the oldest
            memmove(&m_s[0], &m_s[1], sizeof(Entry) * (kCap - 1));
            --m_count;
            if (at > 0) --at;
        }
        memmove(&m_s[at + 1], &m_s[at], sizeof(Entry) * (m_count - at));
        m_s[at] = Entry{remoteTick, snap, p};
        ++m_count;
        m_lastArrival = localTick;
        NoteDelay(localTick, (int32_t)(localTick - remoteTick));
        return true;
    }

    // The pose to show at our tick `localTick` (call once per sim tick). False while warming up
    // (not enough data yet) or when the sender has gone quiet; the caller then leaves the car as is.
    // `clock`: another car of the same sender (same messages, same tick counter) whose playback delay
    // this one uses, so the sender's cars are shown at the same moment of its race.
    bool Sample(uint32_t localTick, PlayoutPose& out, const Playout* clock = nullptr) {
        int32_t dt = m_haveLocal ? (int32_t)(localTick - m_lastLocal) : 0;
        if (dt < 0) { Reset(); return false; }
        m_lastLocal = localTick;
        m_haveLocal = true;
        if (!m_count || (int32_t)(localTick - m_lastArrival) > m_set.staleTicks) {
            m_started = false;
            return false;
        }
        // Work relative to the newest pose's tick, so tick wrap-around is harmless.
        uint32_t base = m_s[m_count - 1].tick;
        double delay = clock && clock != this && clock->m_haveDelay ? clock->m_delay : TargetDelay(localTick);
        m_delay = delay;
        m_haveDelay = true;
        double targetRel = (double)(int32_t)(localTick - base) - delay;
        if (!m_started) {
            if (m_count < 2 || targetRel < (double)(int32_t)(m_s[0].tick - base)) return false;  // still filling
            m_cursorRel = targetRel;
            m_cursorBase = base;
            m_started = true;
        } else {
            m_cursorRel += (double)(int32_t)(m_cursorBase - base) + dt;  // re-base, then advance one step per tick
            m_cursorBase = base;
            double err = targetRel - m_cursorRel;
            double step = m_set.slew * (dt > 0 ? dt : 1);
            if (fabs(err) > m_set.jumpTicks) m_cursorRel = targetRel;
            else m_cursorRel += err > step ? step : (err < -step ? -step : err);
        }
        Interpolate(m_cursorRel, base, out);
        return true;
    }

    // Diagnostics: playback delay behind the newest pose, in ticks (valid after Sample returned true).
    double Lag() const { return m_started ? -m_cursorRel : 0.0; }

    // The sender's tick the last Sample showed (rounded down). False while nothing is shown (warming up,
    // or the sender went quiet). Something the sender did at its tick t is on screen once t <= this.
    bool ShownTick(uint32_t& tick) const {
        if (!m_started) return false;
        tick = m_cursorBase + (uint32_t)(int32_t)floor(m_cursorRel);
        return true;
    }
    int Count() const { return m_count; }

private:
    static const int kCap = 64;
    struct Entry {
        uint32_t tick;
        bool snap;
        PlayoutPose p;
    };

    void NoteDelay(uint32_t localTick, int32_t d) {
        if (m_delayCount == kDelayCap) {
            memmove(&m_d[0], &m_d[1], sizeof(Delay) * (kDelayCap - 1));
            --m_delayCount;
        }
        m_d[m_delayCount++] = Delay{localTick, d};
    }

    // How far behind (our tick - their tick) to show the car: the best arrival delay of the window
    // plus a jitter margin that covers 95% of arrivals (at least bufferTicks, at most maxBufferTicks).
    double TargetDelay(uint32_t localTick) {
        int keep = 0;  // drop entries older than the window, but always keep the newest
        while (keep < m_delayCount - 1 && (int32_t)(localTick - m_d[keep].at) > m_set.windowTicks) ++keep;
        if (keep) {
            memmove(&m_d[0], &m_d[keep], sizeof(Delay) * (m_delayCount - keep));
            m_delayCount -= keep;
        }
        int32_t sorted[kDelayCap];
        for (int i = 0; i < m_delayCount; ++i) sorted[i] = m_d[i].d;
        std::sort(sorted, sorted + m_delayCount);
        int32_t lo = sorted[0], p95 = sorted[(m_delayCount - 1) * 95 / 100];
        int margin = p95 - lo + 1;
        if (margin < m_set.bufferTicks) margin = m_set.bufferTicks;
        if (margin > m_set.maxBufferTicks) margin = m_set.maxBufferTicks;
        return (double)lo + margin;
    }

    // rel: cursor in ticks relative to `base` (the newest pose's tick).
    void Interpolate(double rel, uint32_t base, PlayoutPose& out) const {
        auto relOf = [&](int i) { return (double)(int32_t)(m_s[i].tick - base); };
        if (rel <= relOf(0)) { out = m_s[0].p; return; }
        int n = m_count - 1;
        if (rel >= relOf(n)) {  // past the newest pose: keep moving briefly, then hold where it got to
            double over = rel - relOf(n);
            if (n < 1 || m_s[n].snap) { out = m_s[n].p; return; }
            Extrapolate(n, over < m_set.maxExtrapTicks ? over : m_set.maxExtrapTicks, out);
            return;
        }
        int b = 1;
        while (relOf(b) < rel) ++b;
        int a = b - 1;
        if (m_s[b].snap) { out = m_s[a].p; return; }  // a reset happened between them: no blending across it
        double t = (rel - relOf(a)) / (relOf(b) - relOf(a));
        Blend(m_s[a].p, m_s[b].p, (float)t, out);
    }

    void Extrapolate(int n, double over, PlayoutPose& out) const {
        const Entry& a = m_s[n - 1];
        const Entry& b = m_s[n];
        double span = (double)(int32_t)(b.tick - a.tick);
        float k = (float)(over / span);
        out = b.p;
        for (int i = 0; i < 3; ++i) out.pos[i] = b.p.pos[i] + (b.p.pos[i] - a.p.pos[i]) * k;
    }

    static void Blend(const PlayoutPose& a, const PlayoutPose& b, float t, PlayoutPose& out) {
        out.nav = t < 0.5f ? a.nav : b.nav;
        out.fx = t < 0.5f ? a.fx : b.fx;
        for (int i = 0; i < 3; ++i) {
            out.pos[i] = a.pos[i] + (b.pos[i] - a.pos[i]) * t;
            out.vel[i] = a.vel[i] + (b.vel[i] - a.vel[i]) * t;
            out.spin[i] = a.spin[i] + (b.spin[i] - a.spin[i]) * t;
        }
        // Normalised lerp along the shorter arc (rotation as a quaternion).
        float dot = 0;
        for (int i = 0; i < 4; ++i) dot += a.rot[i] * b.rot[i];
        float sign = dot < 0 ? -1.0f : 1.0f, len = 0;
        for (int i = 0; i < 4; ++i) {
            out.rot[i] = a.rot[i] + (sign * b.rot[i] - a.rot[i]) * t;
            len += out.rot[i] * out.rot[i];
        }
        if (len > 1e-12f) {
            len = 1.0f / sqrtf(len);
            for (float& r : out.rot) r *= len;
        } else {
            memcpy(out.rot, b.rot, sizeof(out.rot));
        }
    }

    static const int kDelayCap = 192;
    struct Delay {
        uint32_t at;
        int32_t d;
    };

    Settings m_set;
    Entry m_s[kCap];
    int m_count = 0;
    Delay m_d[kDelayCap];
    int m_delayCount = 0;
    bool m_started = false, m_haveLocal = false, m_haveDelay = false;
    uint32_t m_lastLocal = 0, m_lastArrival = 0, m_cursorBase = 0;
    double m_cursorRel = 0;
    double m_delay = 0;  // the playback delay used by the last Sample (in ticks)
};
