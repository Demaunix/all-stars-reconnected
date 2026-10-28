// Background running: see background.h and notes/BACKGROUND-RUNNING.md.
#include "background.h"
#include <stdint.h>
#include <string.h>
#include "bg_sites.h"
#include "log.h"

static const uintptr_t kImageBase = 0x400000;  // the exe has no relocations; every site is an absolute VA
static const uintptr_t kPlatformPtr = 0x7f9ff0;  // the platform object; byte +0x60 = settings.ini WINDOWED > 0 (re\fullscreen)
static bool g_installed = false;

// Bumped by every window message and input read on the game's main thread; the watchdog below
// logs when it stops moving (re\fullscreen\FINDINGS.md: a fullscreen game that went invisible).
static volatile LONG g_beat = 0;
static void Beat() { InterlockedIncrement(&g_beat); }

// True when the foreground window belongs to this process and is not minimized. While false, every
// input read below reports "nothing pressed", so keys pressed in another window do nothing and none
// stay held. Minimized counts as unfocused: in fullscreen, a taskbar click makes the still-minimized
// window the foreground one before the game has handled its activation, and reading the devices
// then could reach the game's Acquire retry loops (re\fullscreen\FINDINGS.md M1).
#ifdef BACKGROUND_TEST
static int g_testFocus = -1;  // tests: 0 / 1 overrides the foreground check
#endif
static bool GameHasFocus() {
#ifdef BACKGROUND_TEST
    if (g_testFocus >= 0) return g_testFocus != 0;
#endif
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(fg);
}

// ---------------------------------------------------------------- input wrappers
// DirectInput devices are called through their COM vtable (no dinput.h needed):
// slot 9 (+0x24) GetDeviceState(this, cb, buf), slot 25 (+0x64) Poll(this).
// slot 7 (+0x1c) Acquire(this).
typedef HRESULT(__stdcall* GetDeviceStateFn)(void* dev, DWORD cb, void* buf);
typedef HRESULT(__stdcall* PollFn)(void* dev);
typedef HRESULT(__stdcall* AcquireFn)(void* dev);
static GetDeviceStateFn RealGetDeviceState(void* dev) { return (GetDeviceStateFn)(*(void***)dev)[9]; }

// A focused read that fails: Acquire once and read again; if that fails too, report "nothing
// pressed" and success. The game's own answer to a failed read is an Acquire retry loop (keyboard
// 0x406090: no Sleep, no message pump; mouse 0x4065d2) that ends with the same zeroed buffer, but
// spins forever while DirectInput refuses the device, so it must never be entered.
static LONG g_readFailLogs = 0;
static HRESULT ReadOrIdle(void* dev, DWORD cb, void* buf, const wchar_t* what) {
    HRESULT r = RealGetDeviceState(dev)(dev, cb, buf);
    if (r >= 0) return r;
    HRESULT a = ((AcquireFn)(*(void***)dev)[7])(dev);
    if (a >= 0) {
        r = RealGetDeviceState(dev)(dev, cb, buf);
        if (r >= 0) return r;
    }
    memset(buf, 0, cb);
    if (InterlockedIncrement(&g_readFailLogs) <= 10)
        Log(false, L"background running: the %ls read failed while focused (0x%08lx, Acquire 0x%08lx): read as nothing pressed", what,
            (unsigned long)r, (unsigned long)a);
    return S_OK;
}

// Keyboard: unfocused -> all 256 keys released and success, so the game never enters its
// Acquire retry loop (which would spin while a foreground-only device cannot be acquired).
static HRESULT __stdcall KbdGetState(void* dev, DWORD cb, void* buf) {
    Beat();
    if (!GameHasFocus()) {
        memset(buf, 0, cb);
        return S_OK;
    }
    return ReadOrIdle(dev, cb, buf, L"keyboard");
}

// Mouse (DIMOUSESTATE): unfocused -> no movement, no buttons (its retry loop also spins otherwise).
static HRESULT __stdcall MouseGetState(void* dev, DWORD cb, void* buf) {
    Beat();
    if (!GameHasFocus()) {
        memset(buf, 0, cb);
        return S_OK;
    }
    return ReadOrIdle(dev, cb, buf, L"mouse");
}

// DirectInput joystick (foreground-exclusive): unfocused, Poll would fail and the game would skip the
// read, keeping the last buttons held. Report success instead so the neutral read below is used.
static HRESULT __stdcall JoyPoll(void* dev) {
    if (!GameHasFocus()) return S_OK;
    return ((PollFn)(*(void***)dev)[25])(dev);
}

// DIJOYSTATE2 (0x110 bytes): axes use the game's -1000..1000 range, so 0 is centred; the four POV
// hats at +0x20 read 0xFFFFFFFF when centred.
static HRESULT __stdcall JoyGetState(void* dev, DWORD cb, void* buf) {
    if (!GameHasFocus()) {
        memset(buf, 0, cb);
        if (cb >= 0x30) memset((uint8_t*)buf + 0x20, 0xff, 16);
        return S_OK;
    }
    return RealGetDeviceState(dev)(dev, cb, buf);
}

// XInput: keep the real result (so a missing pad still reads as missing); unfocused, a connected
// pad reads idle (XINPUT_STATE: dwPacketNumber, then the 12-byte XINPUT_GAMEPAD).
typedef DWORD(WINAPI* XInputGetStateFn)(DWORD index, void* state);
static DWORD WINAPI PadGetState(DWORD index, void* state) {
    DWORD r = ((XInputGetStateFn)kXInputGetStateStub)(index, state);
    if (r == ERROR_SUCCESS && !GameHasFocus()) memset((uint8_t*)state + 4, 0, 12);
    return r;
}

// ---------------------------------------------------------------- window drag
// Windows' own title-bar drag runs a modal loop that blocks the game thread until the mouse is
// released. Instead, a caption press on a normal (not maximized) window starts our drag: the game
// keeps pumping its own messages and moves the window on each mouse move. Button-up or losing the
// capture ends it; Escape puts the window back. Everything else goes to the game's WndProc.
static bool g_dragging = false;
static POINT g_dragGrab;   // cursor offset from the window's top-left corner
static RECT g_dragStart;   // window rectangle when the drag began (Escape restores it)

static void EndDrag(HWND h, bool restore) {
    if (!g_dragging) return;
    g_dragging = false;  // before ReleaseCapture, whose WM_CAPTURECHANGED re-enters EndDrag
    if (restore) SetWindowPos(h, nullptr, g_dragStart.left, g_dragStart.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (GetCapture() == h) ReleaseCapture();
}

// The first message arrives inside CreateWindowExA (0x4027c4), after the platform has stored WINDOWED.
static bool g_fullscreen = false;
static bool g_displayLogged = false;
static LONG g_focusLogs = 0;

static void LogDisplay() {
    g_displayLogged = true;
    const uint8_t* platform = *(const uint8_t* const*)kPlatformPtr;
    if (!platform) return;
    g_fullscreen = platform[0x60] == 0;
    Log(false, L"display: %ls (settings.ini WINDOWED)", g_fullscreen ? L"fullscreen" : L"windowed");
}

// ---------------------------------------------------------------- watchdog
// Logs when the main thread has handled no window message and read no input for 20 s (a long load,
// or stuck), with where it is (EIP, read while it is suspended for a moment; nothing else is done
// while it is suspended), and again when it moves on. Only logs; it never touches the game.
static HANDLE g_mainThread = nullptr;
static HANDLE g_watchStop = nullptr;
static HANDLE g_watchThread = nullptr;

static DWORD WINAPI Watchdog(void*) {
    LONG last = g_beat;
    DWORD still = 0, resetFor = 0;
    bool reported = false;
    while (WaitForSingleObject(g_watchStop, 1000) == WAIT_TIMEOUT) {
        // A device reset that keeps failing (platform +0x82 stays 1) stops the picture while messages
        // still flow, which the beat below can't see (re\fullscreen\FINDINGS.md M2).
        const uint8_t* plat = *(const uint8_t* const*)kPlatformPtr;
        if (plat && plat[0x82]) {
            if (++resetFor == 3) Log(false, L"watchdog: the game's display reset has not succeeded for 3 s (%ls)", g_fullscreen ? L"fullscreen" : L"windowed");
        } else {
            if (resetFor >= 3) Log(false, L"watchdog: the display reset succeeded (after %lu s)", resetFor);
            resetFor = 0;
        }
        LONG now = g_beat;
        if (now != last) {
            if (reported) Log(false, L"watchdog: the game's main thread is running again (after %lu s)", still);
            last = now;
            still = 0;
            reported = false;
            continue;
        }
        if (++still < 20 || reported) continue;
        reported = true;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_CONTROL;
        bool got = false;
        if (g_mainThread && SuspendThread(g_mainThread) != (DWORD)-1) {
            got = GetThreadContext(g_mainThread, &ctx) != 0;
            ResumeThread(g_mainThread);
        }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        const uint8_t* platform = *(const uint8_t* const*)kPlatformPtr;
        Log(false, L"watchdog: the game's main thread has handled no window message or input read for 20 s (a long load, or stuck): "
                   L"at 0x%08lx, %ls, foreground %ls%ls, device reset pending %d",
            got ? (unsigned long)ctx.Eip : 0ul, g_fullscreen ? L"fullscreen" : L"windowed", pid == GetCurrentProcessId() ? L"this game" : L"another window",
            fg && pid == GetCurrentProcessId() && IsIconic(fg) ? L" (minimized)" : L"", platform ? platform[0x82] : -1);
    }
    return 0;
}

static void WatchdogStart() {
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0))
        g_mainThread = nullptr;
    g_watchStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_watchStop) g_watchThread = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
}

static void WatchdogStop() {
    if (g_watchThread) {
        SetEvent(g_watchStop);
        WaitForSingleObject(g_watchThread, 3000);
        CloseHandle(g_watchThread);
        g_watchThread = nullptr;
    }
}

static LRESULT CALLBACK BgWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Beat();
    if (!g_displayLogged) LogDisplay();
    switch (msg) {
    case WM_NCLBUTTONDOWN:
        if (w == HTCAPTION && !IsZoomed(h) && !IsIconic(h)) {
            POINT p;
            if (GetCursorPos(&p) && GetWindowRect(h, &g_dragStart)) {
                g_dragGrab.x = p.x - g_dragStart.left;
                g_dragGrab.y = p.y - g_dragStart.top;
                g_dragging = true;
                SetCapture(h);
                return 0;
            }
        }
        break;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            POINT p;
            if (GetCursorPos(&p))
                SetWindowPos(h, nullptr, p.x - g_dragGrab.x, p.y - g_dragGrab.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (g_dragging) {
            EndDrag(h, false);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (g_dragging) {
            g_dragging = false;
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (g_dragging && w == VK_ESCAPE) {
            EndDrag(h, true);
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) EndDrag(h, false);
        if (InterlockedIncrement(&g_focusLogs) <= 40)
            Log(false, L"window: %ls%ls", LOWORD(w) == WA_INACTIVE ? L"lost focus" : L"has focus again", HIWORD(w) ? L" (minimized)" : L"");
        break;
    }
    return ((WNDPROC)kGameWndProc)(h, msg, w, l);
}

// ---------------------------------------------------------------- patching
static const void* const kHooks[kHookCount] = {
    (const void*)BgWndProc, (const void*)KbdGetState, (const void*)MouseGetState,
    (const void*)JoyPoll,   (const void*)JoyGetState, (const void*)PadGetState,
};

static void Replacement(const BgSite& s, uint8_t out[8]) {
    memcpy(out, s.orig, s.len);
    uint32_t hook = s.hook >= 0 ? (uint32_t)(uintptr_t)kHooks[s.hook] : 0;
    switch (s.kind) {
    case kBgBytes: memcpy(out, s.repl, s.len); break;
    case kBgMovEdxImm: out[0] = 0xba; memcpy(out + 1, &hook, 4); break;
    case kBgCallRel: {
        uint32_t rel = hook - (s.va + 5);
        out[0] = 0xe8;
        memcpy(out + 1, &rel, 4);
        break;
    }
    case kBgImm32At4: memcpy(out + 4, &hook, 4); break;
    }
}

static bool WriteCode(uint32_t va, const uint8_t* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtect((void*)(uintptr_t)va, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy((void*)(uintptr_t)va, bytes, n);
    DWORD ignored;
    VirtualProtect((void*)(uintptr_t)va, n, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)(uintptr_t)va, n);
    return true;
}

static void Restore(size_t count) {
    for (size_t i = count; i-- > 0;) WriteCode(kBgSites[i].va, kBgSites[i].orig, kBgSites[i].len);
}

void BackgroundInstall(HMODULE gameImage, const std::wstring& modDir) {
    std::wstring ini = modDir + L"\\config.ini";
    if (GetPrivateProfileIntW(L"Background", L"RunWhenUnfocused", 1, ini.c_str()) == 0) {
        Log(false, L"background running: off (config.ini RunWhenUnfocused=0)");
        return;
    }
    if ((uintptr_t)gameImage != kImageBase) {
        Log(true, L"background running: off (game image at %p, expected 0x400000)", gameImage);
        return;
    }
    // All or nothing: running unfocused without the input wrappers would let the keyboard and
    // mouse retry loops spin, so a single unexpected byte turns the whole feature off.
    for (const BgSite& s : kBgSites) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)(uintptr_t)s.va, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            memcmp((void*)(uintptr_t)s.va, s.orig, s.len) != 0) {
            Log(true, L"background running: off (code at 0x%06x, %hs, is not the expected original; another mod may patch it)", s.va,
                s.what);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(kBgSites) / sizeof(kBgSites[0]); ++i) {
        uint8_t bytes[8];
        Replacement(kBgSites[i], bytes);
        if (!WriteCode(kBgSites[i].va, bytes, kBgSites[i].len)) {
            Restore(i);
            Log(true, L"background running: off (could not write 0x%06x, error %lu)", kBgSites[i].va, GetLastError());
            return;
        }
    }
    g_installed = true;
    WatchdogStart();
    Log(true, L"background running: on (keeps running unfocused; input ignored while unfocused or minimized; window drag does not pause)");
}

void BackgroundRollback() {
    if (!g_installed) return;
    WatchdogStop();
    Restore(sizeof(kBgSites) / sizeof(kBgSites[0]));
    g_installed = false;
}
