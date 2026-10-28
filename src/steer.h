#pragma once
// Drift+ off (the user's rule, 2026-10-27; re\drift-plus\FINDINGS.md): the stock Human update adds a controller's stick and
// d-pad into one steering value (0x5af150: right + (stick - left), unclamped), so both at once give 2.0, four times the turning
// (the community's "Drift+"). While a keyboard player races online, a controller steers with ONE of them: the first one used
// keeps control until it is let go; the other is ignored meanwhile. Header-only for tests/test_steer.cpp.
#include <math.h>
#include <stdint.h>

enum SteerOwner : uint8_t { kSteerNone = 0, kSteerStick = 1, kSteerDpad = 2 };

// stick: the stick's value (-1..1, its dead zone already applied); left / right: the d-pad's (0 or 1). `owner` carries the
// choice from one tick to the next. Returns the steering value to use instead of the stock sum.
inline float SteerOneInput(float stick, float left, float right, uint8_t& owner) {
    bool stickOn = stick != 0, dpadOn = left != 0 || right != 0;
    float dpad = right - left;
    if ((owner == kSteerStick && !stickOn) || (owner == kSteerDpad && !dpadOn)) owner = kSteerNone;
    if (owner == kSteerNone) {
        if (stickOn && dpadOn) owner = fabsf(dpad) >= fabsf(stick) ? kSteerDpad : kSteerStick;  // (both in the same tick)
        else if (dpadOn) owner = kSteerDpad;
        else if (stickOn) owner = kSteerStick;
    }
    return owner == kSteerStick ? stick : owner == kSteerDpad ? dpad : 0.0f;
}
