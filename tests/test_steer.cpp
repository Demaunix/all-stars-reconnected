// Drift+ off (src/steer.h): a controller steers with its stick OR its d-pad; the first one used keeps control until let go.
// The keyboard drift fix (src/drift_style.h): a quick re-press keeps the drift's side.
#include <stdio.h>
#include "steer.h"
#include "drift_style.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

int main() {
    uint8_t owner = kSteerNone;
    Check(SteerOneInput(0, 0, 0, owner) == 0 && owner == kSteerNone, "nothing used: no steering");
    Check(SteerOneInput(0.6f, 0, 0, owner) == 0.6f && owner == kSteerStick, "the stick alone steers as it is");
    Check(SteerOneInput(1.0f, 0, 1, owner) == 1.0f && owner == kSteerStick, "stick first, then the d-pad the same way: the stick only (not 2.0)");
    Check(SteerOneInput(0.3f, 1, 0, owner) == 0.3f && owner == kSteerStick, "the d-pad the other way doesn't pull the stick's steering");
    Check(SteerOneInput(0, 1, 0, owner) == -1.0f && owner == kSteerDpad, "stick let go while the d-pad is held: the d-pad takes over");
    Check(SteerOneInput(1.0f, 1, 0, owner) == -1.0f && owner == kSteerDpad, "d-pad first, then the stick: the d-pad only");
    Check(SteerOneInput(1.0f, 0, 0, owner) == 1.0f && owner == kSteerStick, "d-pad let go: the stick steers");
    Check(SteerOneInput(0, 0, 0, owner) == 0 && owner == kSteerNone, "both let go: no steering, nobody owns it");
    Check(SteerOneInput(0.5f, 0, 1, owner) == 1.0f && owner == kSteerDpad, "both in the same tick: the larger (the d-pad's full 1)");
    owner = kSteerNone;
    Check(SteerOneInput(-1.0f, 0, 1, owner) == 1.0f && owner == kSteerDpad, "both in the same tick, equal: the d-pad (never their sum, 0)");

    // Same-side drift re-press (src/drift_style.h). A left drift: the hop arrow was left (-1), so the latch is +1 (latch = -steer).
    // The rows follow the logged re-presses (re\drift-parity\drift-log-20261108.csv ticks 3961-3964, 4048-4051).
    SameSideState s;
    Check(SameSideSteer(s, 2, 1.0f, 1.0f, 1.0f) == 1.0f && s.counterHeld, "drifting with the counter arrow held: left as it is");
    Check(SameSideSteer(s, 2, 1.0f, 0.0f, 1.0f) == 1.0f, "the drift let go (the release tick): left as it is");
    Check(SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f) == 1.0f && SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f) == 1.0f, "releasing, no drift button: left as it is");
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == -1.0f, "quick re-press, counter arrow held through: steers the old side for that tick");
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == 1.0f, "the button still held next tick: left as it is (only the press tick)");
    Check(SameSideSteer(s, 1, 1.0f, 1.0f, 1.0f) == 1.0f && s.counterHeld, "the new hop on the old side, counter arrow still held: left as it is");
    // The mirror: a right drift (latch -1), counter arrow = left (-1).
    s = SameSideState();
    SameSideSteer(s, 2, -1.0f, 1.0f, -1.0f);
    SameSideSteer(s, 2, -1.0f, 0.0f, -1.0f);
    SameSideSteer(s, 3, -1.0f, 0.0f, -1.0f);
    Check(SameSideSteer(s, 3, -1.0f, 1.0f, -1.0f) == 1.0f, "the mirror: a right drift's quick re-press steers right for that tick");
    // The hop arrow held: the new drift is on the same side already.
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 1.0f, -1.0f);
    SameSideSteer(s, 2, 1.0f, 0.0f, -1.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, -1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, -1.0f) == -1.0f, "re-press with the hop arrow held: left as it is");
    // A fresh counter press during the gap (a deliberate change of side).
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 1.0f, -1.0f);
    SameSideSteer(s, 2, 1.0f, 0.0f, 0.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 0.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == 1.0f, "counter arrow pressed only after the release: the side changes (left as it is)");
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 1.0f, 1.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 0.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == 1.0f, "counter arrow let go and pressed again in the gap: left as it is");
    // Too late: past the window.
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 0.0f, 1.0f);
    for (int i = 0; i < kSameSideWindow; ++i) SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == 1.0f, "re-press after the window: left as it is");
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 0.0f, 1.0f);
    for (int i = 0; i < kSameSideWindow - 1; ++i) SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == -1.0f, "re-press on the window's last tick: the old side");
    // No drift running: nothing changes; a drift that ended resets.
    s = SameSideState();
    Check(SameSideSteer(s, 0, 1.0f, 0.0f, 1.0f) == 1.0f && SameSideSteer(s, 0, 1.0f, 1.0f, 1.0f) == 1.0f, "no drift: a new drift starts as the game decides");
    SameSideSteer(s, 2, 1.0f, 0.0f, 1.0f);
    SameSideSteer(s, 4, 1.0f, 0.0f, 1.0f);
    SameSideSteer(s, 0, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == 1.0f, "a drift that ended (state 4 / 0) is forgotten");
    // A chain: re-press kept, the new drift let go again, re-pressed again.
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 0.0f, 1.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == -1.0f, "chain: first re-press kept");
    SameSideSteer(s, 1, 1.0f, 1.0f, 1.0f);
    SameSideSteer(s, 1, 1.0f, 0.0f, 1.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 1.0f) == -1.0f, "chain: released from the hop, re-pressed again: kept too");
    s = SameSideState();
    SameSideSteer(s, 2, 1.0f, 0.0f, 0.0f);
    SameSideSteer(s, 3, 1.0f, 0.0f, 0.0f);
    Check(SameSideSteer(s, 3, 1.0f, 1.0f, 0.0f) == 0.0f, "no arrow at the re-press: left as it is");
    s = SameSideState();
    SameSideSteer(s, 2, 0.0f, 0.0f, 1.0f);
    SameSideSteer(s, 3, 0.0f, 0.0f, 1.0f);
    Check(SameSideSteer(s, 3, 0.0f, 1.0f, 1.0f) == 1.0f, "no side known (latch 0): left as it is");
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail;
}
