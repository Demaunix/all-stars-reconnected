// Background running's input wrappers (src/background.cpp) against fake DirectInput devices:
// a read that fails while focused must never leave the game in its endless Acquire retry loop
// (keyboard 0x406090, mouse 0x4065d2: re\fullscreen\FINDINGS.md M1).
#define BACKGROUND_TEST
#include "../src/background.cpp"
#include <stdio.h>

void Log(bool, const wchar_t* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    wprintf(L"    log: ");
    vwprintf(fmt, a);
    wprintf(L"\n");
    va_end(a);
}

static const HRESULT kOtherAppHasPrio = (HRESULT)0x8007001e;
static const HRESULT kNotAcquired = (HRESULT)0x8007000c;

// A fake IDirectInputDevice8: only slots 7 (Acquire) and 9 (GetDeviceState) are used.
struct FakeDev {
    void** vtbl;
    int reads = 0, acquires = 0;
    HRESULT readResult[2] = {S_OK, S_OK};  // first read, later reads
    HRESULT acquireResult = S_OK;
    uint8_t fill = 0xab;
};
static HRESULT __stdcall FakeAcquire(FakeDev* d) {
    ++d->acquires;
    return d->acquireResult;
}
static HRESULT __stdcall FakeGetState(FakeDev* d, DWORD cb, void* buf) {
    HRESULT r = d->readResult[d->reads++ ? 1 : 0];
    if (r >= 0) memset(buf, d->fill, cb);
    return r;
}
static void* g_vtbl[26];

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
static bool AllBytes(const uint8_t* p, size_t n, uint8_t v) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] != v) return false;
    return true;
}

// The game's keyboard read (0x406075..0x4060a3) as C++: a failed read Acquires until the answer is not
// "another app has priority". Bounded here: more than 1000 Acquires means the real game would spin.
static bool GameKeyboardRead(FakeDev* d, uint8_t* buf) {
    if (KbdGetState(d, 256, buf) >= 0) return true;
    for (int i = 0; i < 1000; ++i)
        if (FakeAcquire(d) != kOtherAppHasPrio) return true;
    return false;
}

int main() {
    g_vtbl[7] = (void*)FakeAcquire;
    g_vtbl[9] = (void*)FakeGetState;
    uint8_t buf[256];

    {  // focused, read works: passed through, once
        g_testFocus = 1;
        FakeDev d;
        d.vtbl = g_vtbl;
        memset(buf, 0, sizeof buf);
        HRESULT r = KbdGetState(&d, 256, buf);
        Check(r == S_OK && d.reads == 1 && d.acquires == 0 && AllBytes(buf, 256, 0xab), "focused keyboard read passes through");
    }
    {  // focused, first read fails, Acquire works, second read works
        FakeDev d;
        d.vtbl = g_vtbl;
        d.readResult[0] = kNotAcquired;
        memset(buf, 0, sizeof buf);
        HRESULT r = KbdGetState(&d, 256, buf);
        Check(r == S_OK && d.reads == 2 && d.acquires == 1 && AllBytes(buf, 256, 0xab), "a lost keyboard is re-acquired once and read again");
    }
    {  // focused, the device can't be acquired (the fullscreen taskbar case): nothing pressed, no spin
        FakeDev d;
        d.vtbl = g_vtbl;
        d.readResult[0] = d.readResult[1] = kNotAcquired;
        d.acquireResult = kOtherAppHasPrio;
        memset(buf, 0x11, sizeof buf);
        bool done = GameKeyboardRead(&d, buf);
        Check(done && d.reads == 1 && d.acquires == 1 && AllBytes(buf, 256, 0), "keyboard Acquire refused: read as nothing pressed, the game's loop is never entered");
    }
    {  // focused, Acquire works but the second read still fails
        FakeDev d;
        d.vtbl = g_vtbl;
        d.readResult[0] = d.readResult[1] = kNotAcquired;
        memset(buf, 0x11, sizeof buf);
        HRESULT r = KbdGetState(&d, 256, buf);
        Check(r == S_OK && d.reads == 2 && d.acquires == 1 && AllBytes(buf, 256, 0), "keyboard read failing after Acquire: nothing pressed");
    }
    {  // mouse (DIMOUSESTATE, 0x14 bytes), Acquire refused
        FakeDev d;
        d.vtbl = g_vtbl;
        d.readResult[0] = d.readResult[1] = kNotAcquired;
        d.acquireResult = kOtherAppHasPrio;
        memset(buf, 0x11, sizeof buf);
        HRESULT r = MouseGetState(&d, 0x14, buf);
        Check(r == S_OK && d.acquires == 1 && AllBytes(buf, 0x14, 0) && buf[0x14] == 0x11, "mouse Acquire refused: no movement, no buttons, only its 20 bytes written");
    }
    {  // unfocused: the device is not touched
        g_testFocus = 0;
        FakeDev d;
        d.vtbl = g_vtbl;
        memset(buf, 0x11, sizeof buf);
        HRESULT r1 = KbdGetState(&d, 256, buf);
        HRESULT r2 = MouseGetState(&d, 0x14, buf);
        Check(r1 == S_OK && r2 == S_OK && d.reads == 0 && d.acquires == 0 && AllBytes(buf, 256, 0), "unfocused reads touch no device and read as idle");
    }
    {  // every wrapper call and window message moves the watchdog's beat
        LONG before = g_beat;
        FakeDev d;
        d.vtbl = g_vtbl;
        KbdGetState(&d, 256, buf);
        MouseGetState(&d, 0x14, buf);
        Check(g_beat == before + 2, "input reads move the watchdog's beat");
    }
    printf("%s\n", g_fail ? "SOME FAILED" : "ALL PASS");
    return g_fail;
}
