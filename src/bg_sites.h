// Code sites patched by the background-running feature (M2), with the exact original bytes.
// Verified on re\bin\SSASR-unpacked.exe (decrypted copy of the c110 Steam exe); see
// notes/BACKGROUND-RUNNING.md. tests/test_bg_sites.cpp re-checks every entry against that file.
// Header-only so the test can include it without the runtime.
#pragma once
#include <stdint.h>

enum BgSiteKind : uint8_t {
    kBgBytes,      // replace with `repl` as given
    kBgMovEdxImm,  // 5 bytes -> `mov edx, <hook>` (ba imm32)
    kBgCallRel,    // 5 bytes -> `call <hook>` (e8 rel32)
    kBgImm32At4,   // 8-byte `mov dword [esp+8], imm32`: keep the first 4 bytes, imm32 = <hook>
};

struct BgSite {
    uint32_t va;
    uint8_t len;
    uint8_t orig[8];
    BgSiteKind kind;
    uint8_t repl[8];  // kBgBytes only
    int hook;         // index into the hook table (other kinds)
    const char* what;
};

enum BgHook { kHookWndProc, kHookKbdState, kHookMouseState, kHookJoyPoll, kHookJoyState, kHookPadState, kHookCount };

static const BgSite kBgSites[] = {
    // Platform loop 0x402890: "inactive before and now -> Sleep(100), skip the frame". jne -> jmp.
    {0x4028fd, 2, {0x75, 0x29}, kBgBytes, {0xeb, 0x29}, -1, "platform loop keeps running unfocused"},
    // WndProc WM_ACTIVATE(deactivate): mov byte [0xc2a016], 1 (global "lost focus") -> 0.
    {0x403302, 7, {0xc6, 0x05, 0x16, 0xa0, 0xc2, 0x00, 0x01}, kBgBytes, {0xc6, 0x05, 0x16, 0xa0, 0xc2, 0x00, 0x00}, -1,
     "lost-focus flag stays clear"},
    // WndProc WM_ACTIVATE(deactivate): call 0x410900 (pause the startup movie) -> nop.
    {0x403315, 5, {0xe8, 0xe6, 0xd5, 0x00, 0x00}, kBgBytes, {0x90, 0x90, 0x90, 0x90, 0x90}, -1, "startup movie keeps playing"},
    // Window class registration: wc.lpfnWndProc = 0x4032d0 -> our WndProc (drag without freezing).
    {0x403662, 8, {0xc7, 0x44, 0x24, 0x08, 0xd0, 0x32, 0x40, 0x00}, kBgImm32At4, {}, kHookWndProc, "window procedure"},
    // Keyboard poll: mov edx,[eax]; mov edx,[edx+24h] (GetDeviceState) -> mov edx, hook; game's `call edx` follows.
    {0x40605c, 5, {0x8b, 0x10, 0x8b, 0x52, 0x24}, kBgMovEdxImm, {}, kHookKbdState, "keyboard read"},
    // Mouse poll: mov eax,[ecx+24h]; call eax (GetDeviceState, args pushed) -> call hook.
    {0x4065ac, 5, {0x8b, 0x41, 0x24, 0xff, 0xd0}, kBgCallRel, {}, kHookMouseState, "mouse read"},
    // DirectInput joystick poll: mov ecx,[eax]; mov edx,[ecx+64h] (Poll) -> mov edx, hook; `push eax; call edx` follows.
    {0x4057e4, 5, {0x8b, 0x08, 0x8b, 0x51, 0x64}, kBgMovEdxImm, {}, kHookJoyPoll, "joystick poll"},
    // DirectInput joystick read: mov eax,[ecx+24h]; call eax (GetDeviceState) -> call hook.
    {0x405828, 5, {0x8b, 0x41, 0x24, 0xff, 0xd0}, kBgCallRel, {}, kHookJoyState, "joystick read"},
    // XInput pad poll: call 0x4244a2 (XInputGetState stub) -> call hook.
    {0x405e2b, 5, {0xe8, 0x72, 0xe6, 0x01, 0x00}, kBgCallRel, {}, kHookPadState, "XInput pad read"},
};

static const uint32_t kXInputGetStateStub = 0x4244a2;  // jmp [0x67d24c] (XINPUT1_3 ordinal 2)
static const uint32_t kGameWndProc = 0x4032d0;
