// Drift+ off (src/steer.h): a controller steers with its stick OR its d-pad; the first one used keeps control until let go.
#include <stdio.h>
#include "steer.h"

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
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail;
}
