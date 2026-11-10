#pragma once
// Same-side drift re-press (the keyboard drift fix; the user's pick and OK, 2026-11-09; re\drift-cause\FINDINGS.md fix option 1,
// notes\KEYBOARD-DRIFT.md section 3f). The game treats keyboard and controller alike: a new drift takes its side from the steering on the
// tick the drift button goes down (latch = -steer, store 0x5cbf7d). A keyboard player who lets go of the drift with the counter
// arrow held and taps the drift button again within a few ticks therefore starts a new drift on the OTHER side: the car swings
// across (log: 24 of 24 such re-presses). A key's counter-steer is always 1.0, above the 0.65 drift-start threshold, so it
// happens on a keyboard far more often than with a stick. The fix: on that one tick, the steering points the old drift's own
// way, so the new hop keeps the old side. An arrow pressed afresh after the release still changes sides (a deliberate change).
// Replaces Majority Drift Parity's release remap (dp1), whose premise (the devices release differently) the log disproved.
// Header-only for tests/test_steer.cpp.
#include <math.h>
#include <stdint.h>

// Drift states (vehicle +0x2d0, 0x5cb4b0): 0 none, 1 entry / hop, 2 drifting, 3 releasing, 4 end.
const uint8_t kSameSideWindow = 10;  // ticks of state 3 (20 ms each) in which a re-press keeps the old side

struct SameSideState {
    float lastBrake = 0;         // the drift (brake) input last tick
    uint8_t counterHeld = 0;     // the counter arrow has been held without a break since the drift (state 1 / 2)
    uint8_t releaseTicks = 0;    // ticks in state 3 so far (255: many)
};

// The steering this keyboard car's physics should see this tick. state: the drift state the previous tick's physics left (the
// controller update runs first); latch: the running drift's side (vehicle +0x2a0, +-1; 0 none); brake / steer: this tick's
// inputs. Returns `steer` unchanged except on a quick re-press with the counter arrow held through, where it is the old side's.
inline float SameSideSteer(SameSideState& s, int32_t state, float latch, float brake, float steer) {
    bool counter = latch != 0 && steer * latch > 0;  // the arrow opposite the hop's (latch = -steer at the hop)
    float out = steer;
    if (state == 1 || state == 2) {
        s.counterHeld = counter ? 1 : 0;
        s.releaseTicks = 0;
    } else if (state == 3) {
        if (!counter) s.counterHeld = 0;
        if (s.releaseTicks < 255) ++s.releaseTicks;
        if (s.counterHeld && !(s.lastBrake > 0) && brake > 0 && s.releaseTicks <= kSameSideWindow)
            out = latch > 0 ? -fabsf(steer) : fabsf(steer);  // the hop latches -steer = the old side
    } else {
        s.counterHeld = 0;
        s.releaseTicks = 0;
    }
    s.lastBrake = brake;
    return out;
}
