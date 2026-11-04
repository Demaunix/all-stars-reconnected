// The controller pictures' font-record swap (src\pad_glyphs.cpp, built with PADGLYPHS_TEST) on fake elements and fonts laid out as
// the game's (re\pad-glyphs\FINDINGS2.md Q4): element +0xe8 its scene, +0x104 its text object; text +0x14 the font row {+0 hash,
// +4 u16 count, +8 the sorted 0x18-byte glyph records}.
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "pad_glyphs.h"

static int g_logs = 0;
void Log(bool, const wchar_t* fmt, ...) {
    ++g_logs;
    va_list ap;
    va_start(ap, fmt);
    wprintf(L"  log: ");
    vwprintf(fmt, ap);
    wprintf(L"\n");
    va_end(ap);
}
const wchar_t* LobbyConfigPath() { return L""; }

static int g_checks = 0, g_failed = 0;
static void Check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        printf("FAIL: %s\n", what);
    }
}

#pragma pack(push, 1)
struct Rec {
    uint32_t sprite;
    uint16_t ch, u, v, w, h;
    int16_t xoff, adv, yoff;
    uint16_t f14, f16;
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 0x18, "glyph record");

const uint32_t kKeysSprite = 0x11111111, kXboxSprite = 0x22222222, kPsSprite = 0x33333333;

// A font as tools\pad_glyphs.py leaves it: letters, F000.. (the keys), F010.. Xbox, F018.. PlayStation, F020.. the keys' copy, FFFF.
// Every field of every set differs, so a record copied from the wrong set (or the char copied) shows.
static std::vector<Rec> MakeFont(int n, bool withSets = true) {
    std::vector<Rec> g;
    for (uint16_t c = 'A'; c <= 'Z'; ++c) g.push_back({0x44444444, c, (uint16_t)(c * 3), 7, 20, 25, 1, 22, 18, 1, 0});
    auto add = [&](uint16_t first, uint32_t sprite, int base) {
        for (int i = 0; i < n; ++i)
            g.push_back({sprite, (uint16_t)(first + i), (uint16_t)(base + 73 * i), (uint16_t)(base + 1), (uint16_t)(base + 2 + i),
                         (uint16_t)(base + 3), (int16_t)(base + 4), (int16_t)(base + 5), (int16_t)(base + 6), (uint16_t)(base + 7), (uint16_t)(base + 8)});
    };
    add(0xF000, kKeysSprite, 100);
    if (withSets) {
        add(0xF010, kXboxSprite, 200);
        add(0xF018, kPsSprite, 300);
        add(0xF020, kKeysSprite, 100);  // (the copy: the keys' records with their own chars)
    }
    g.push_back({kKeysSprite, 0xFFFF, 0, 0, 0, 0, 0, 30, 0, 1, 0});
    return g;
}

struct Element {
    uint8_t elem[0x110] = {};
    uint8_t text[0x20] = {};
    uint8_t row[0x20] = {};
    uint8_t scene[0x10] = {};
    std::vector<Rec> glyphs;
    void Build(uint32_t font, std::vector<Rec> g) {
        glyphs = g;
        *(uint8_t**)(elem + 0xe8) = scene;
        *(uint8_t**)(elem + 0x104) = text;
        *(uint8_t**)(text + 0x14) = row;
        *(uint32_t*)row = font;
        *(uint16_t*)(row + 4) = (uint16_t)glyphs.size();
        *(uint8_t**)(row + 8) = (uint8_t*)glyphs.data();
    }
    const Rec* Find(uint16_t ch) const {
        for (const Rec& r : glyphs)
            if (r.ch == ch) return &r;
        return nullptr;
    }
};

static Element g_footer, g_buttons, g_buttons2;
static bool g_footerUp = true, g_resultsUp = true;
static uint8_t* FakeFind(uint32_t element) {
    if (element == 0xe0c0001b) return g_footerUp ? g_footer.elem : nullptr;
    if (element == 0x56ff27d8) return g_resultsUp ? g_buttons.elem : nullptr;
    if (element == 0xa8541e22) return g_resultsUp ? g_buttons2.elem : nullptr;
    return nullptr;
}
static bool g_buttonsLackXbox = false;
static bool FakeProbe(uint8_t* scene, uint32_t sprite) {
    if (sprite != kXboxSprite && sprite != kPsSprite) return false;
    return !(g_buttonsLackXbox && sprite == kXboxSprite && (scene == g_buttons.scene || scene == g_buttons2.scene));
}
static uint8_t g_pad[0x400];
static bool g_hasPrimary = true;
static uint8_t* FakePrimary() { return g_hasPrimary ? g_pad : nullptr; }
static int g_layer = 0;
static int FakeLayer() { return g_layer; }
static bool g_online = true;
static bool FakeOnline() { return g_online; }
static void SetDevice(uint32_t vtable, int slot) {
    memset(g_pad, 0, sizeof(g_pad));
    *(uint32_t*)g_pad = vtable;
    *(int32_t*)(g_pad + 0x1d8) = slot;
}
const uint32_t kKeyboard = 0x67e444, kXInput = 0x67e3f8, kDirectInput = 0x67e39c;

// The element's F000.. equal set `first`'s records in every byte but the char; every other record as built.
static bool Shows(const Element& e, const std::vector<Rec>& built, uint16_t first, int n) {
    if (e.glyphs.size() != built.size()) return false;
    for (size_t k = 0; k < built.size(); ++k) {
        const Rec& now = e.glyphs[k];
        Rec want = built[k];
        if (want.ch >= 0xF000 && want.ch < 0xF000 + n) {
            const Rec* src = nullptr;
            for (const Rec& r : built)
                if (r.ch == first + (want.ch - 0xF000)) src = &r;
            if (!src) return false;
            want = *src;
            want.ch = built[k].ch;
        }
        if (memcmp(&now, &want, sizeof(Rec)) != 0) return false;
    }
    return true;
}

int main() {
    std::vector<Rec> footerFont = MakeFont(8), buttonsFont = MakeFont(7);
    g_footer.Build(0xff7f8ec5, footerFont);
    g_buttons.Build(0x7ea1f827, buttonsFont);
    g_buttons2.Build(0x30f8365e, buttonsFont);
    PadGlyphsTestSetup(&FakeFind, &FakeProbe, &FakePrimary, &FakeLayer, &FakeOnline);
    PadGlyphsTestSetMode(0);
    auto all = [&](uint16_t first) {
        return Shows(g_footer, footerFont, first, 8) && Shows(g_buttons, buttonsFont, first, 7) && Shows(g_buttons2, buttonsFont, first, 7);
    };

    // An Xbox-style pad (XInput, Steam Input says nothing about its slot) in the online menus: Xbox buttons everywhere.
    SetDevice(kXInput, 0);
    PadGlyphsFrame();
    Check(all(0xF010) && PadGlyphsTestShown() == 1, "XInput pad online: Xbox buttons in the footer and both button texts");
    Check(g_footer.Find(0xF007) && g_footer.Find(0xF007)->sprite == kXboxSprite && g_buttons.Find(0xF007) == nullptr,
          "the footer's 8th picture (INVITE) swapped; the results font has 7");
    // Steam Input says that slot is a PlayStation pad.
    PadGlyphsTestSetSlot(0, 2);
    PadGlyphsFrame();
    Check(all(0xF018) && PadGlyphsTestShown() == 2, "Steam Input: a PlayStation pad behind slot 0: PlayStation buttons");
    // Another slot's PlayStation pad doesn't count.
    SetDevice(kXInput, 1);
    PadGlyphsFrame();
    Check(all(0xF010), "the primary pad is slot 1 (not a PlayStation pad): Xbox buttons");
    PadGlyphsTestSetSlot(1, 1);
    PadGlyphsFrame();
    Check(all(0xF010), "slot 1 a non-PlayStation Steam Input pad: Xbox buttons");
    SetDevice(kXInput, 7);
    PadGlyphsFrame();
    Check(all(0xF010), "a slot index out of range: Xbox buttons");
    // Leaving the online menus: the keys come back, byte for byte.
    SetDevice(kXInput, 0);
    g_online = false;
    PadGlyphsFrame();
    Check(all(0xF020) && memcmp(g_footer.glyphs.data(), footerFont.data(), footerFont.size() * sizeof(Rec)) == 0 &&
              memcmp(g_buttons.glyphs.data(), buttonsFont.data(), buttonsFont.size() * sizeof(Rec)) == 0 && PadGlyphsTestShown() == 0,
          "not online: the keys, every record as built");
    g_online = true;
    // The keyboard (or no primary device yet): the keys.
    SetDevice(kKeyboard, 0);
    PadGlyphsFrame();
    Check(all(0xF020) && PadGlyphsTestShown() == 0, "the keyboard: the keys");
    g_hasPrimary = false;
    PadGlyphsFrame();
    Check(all(0xF020), "no primary device chosen yet: the keys");
    g_hasPrimary = true;
    SetDevice(0x12345678, 0);
    PadGlyphsFrame();
    Check(all(0xF020), "an unknown device class: the keys");
    // A DirectInput pad (the game binds only the PS2 adapter): PlayStation buttons.
    SetDevice(kDirectInput, 0);
    PadGlyphsFrame();
    Check(all(0xF018), "a DirectInput pad: PlayStation buttons");
    // config.ini [Menus] PadPictures.
    SetDevice(kXInput, 0);
    PadGlyphsTestSetSlot(0, 2);
    PadGlyphsTestSetMode(1);
    PadGlyphsFrame();
    Check(all(0xF010), "PadPictures=xbox over a PlayStation pad: Xbox buttons");
    PadGlyphsTestSetSlot(0, 0);
    PadGlyphsTestSetMode(2);
    PadGlyphsFrame();
    Check(all(0xF018), "PadPictures=playstation over an Xbox pad: PlayStation buttons");
    PadGlyphsTestSetMode(3);
    PadGlyphsFrame();
    Check(all(0xF020), "PadPictures=keys: the keys");
    PadGlyphsTestSetMode(1);
    SetDevice(kKeyboard, 0);
    PadGlyphsFrame();
    Check(all(0xF020), "PadPictures=xbox with the keyboard: the keys");
    PadGlyphsTestSetMode(0);
    // A scene whose sprite table lacks the set's sprite: that element keeps the keys (a missing sprite would end the string).
    SetDevice(kXInput, 0);
    g_buttonsLackXbox = true;
    PadGlyphsFrame();
    Check(Shows(g_footer, footerFont, 0xF010, 8) && Shows(g_buttons, buttonsFont, 0xF020, 7) && Shows(g_buttons2, buttonsFont, 0xF020, 7),
          "the results scene lacks the Xbox sprite: its buttons keep the keys, the footer shows Xbox");
    PadGlyphsTestSetSlot(0, 2);
    PadGlyphsFrame();
    Check(all(0xF018), "...and takes the PlayStation set, whose sprite is there");
    PadGlyphsTestSetSlot(0, 0);
    g_buttonsLackXbox = false;
    // Another mod's scene: a font without the sets is left exactly as it is.
    std::vector<Rec> plain = MakeFont(8, false);
    g_footer.Build(0xff7f8ec5, plain);
    PadGlyphsFrame();
    Check(memcmp(g_footer.glyphs.data(), plain.data(), plain.size() * sizeof(Rec)) == 0, "a font without the sets: untouched");
    // A font with another hash under the footer element: untouched.
    g_footer.Build(0x12345678, footerFont);
    PadGlyphsFrame();
    Check(memcmp(g_footer.glyphs.data(), footerFont.data(), footerFont.size() * sizeof(Rec)) == 0, "another font: untouched");
    // Only part of a set (a record missing): untouched.
    std::vector<Rec> part = footerFont;
    for (size_t k = 0; k < part.size(); ++k)
        if (part[k].ch == 0xF01D) part.erase(part.begin() + k);
    g_footer.Build(0xff7f8ec5, part);
    PadGlyphsFrame();
    Check(memcmp(g_footer.glyphs.data(), part.data(), part.size() * sizeof(Rec)) == 0, "a set with a record missing: untouched");
    // Scenes not loaded (no results screen): nothing to do, no crash.
    g_footer.Build(0xff7f8ec5, footerFont);
    g_resultsUp = false;
    PadGlyphsFrame();
    Check(Shows(g_footer, footerFont, 0xF010, 8), "no results scene: the footer still swapped");
    g_resultsUp = true;
    // The 4:3 / 16:9 fonts of the other button text are accepted too.
    g_buttons.Build(0x5e0a0003, buttonsFont);
    g_buttons2.Build(0x3c005ba2, buttonsFont);
    PadGlyphsFrame();
    Check(all(0xF010), "the other aspect's button fonts");
    // The chars never change and the arrays stay sorted.
    bool sorted = true;
    for (const Element* e : {&g_footer, &g_buttons, &g_buttons2})
        for (size_t k = 1; k < e->glyphs.size(); ++k) sorted = sorted && e->glyphs[k - 1].ch < e->glyphs[k].ch;
    Check(sorted, "every array still sorted by char");

    // INVITE FRIENDS on a pad: the primary pad's Back (action 34) held, in the active input layer.
    SetDevice(kXInput, 0);
    Check(!PadGlyphsInviteHeld(), "invite: nothing held");
    g_pad[0xd4 + 34] = 0x01;
    g_layer = 0;
    Check(PadGlyphsInviteHeld(), "invite: Back held in the menus' layer");
    g_pad[0xd4 + 34] = 0x10;
    Check(!PadGlyphsInviteHeld(), "invite: only the pressed bit, not held: no");
    g_pad[0xd4 + 34] = 0x01;
    g_layer = 1;
    Check(!PadGlyphsInviteHeld(), "invite: the dialog layer reads its own byte");
    g_pad[0xd4 + 0x2e + 34] = 0x01;
    Check(PadGlyphsInviteHeld(), "invite: Back held in the dialog layer");
    g_layer = 3;
    Check(!PadGlyphsInviteHeld(), "invite: no layer active: no");
    g_layer = 0;
    *(uint32_t*)g_pad = kKeyboard;
    Check(!PadGlyphsInviteHeld(), "invite: the keyboard's action 34 is Esc: never");
    *(uint32_t*)g_pad = kDirectInput;
    Check(PadGlyphsInviteHeld(), "invite: a DirectInput pad's Back");
    PadGlyphsTestSetFocused(false);
    Check(!PadGlyphsInviteHeld(), "invite: not while another window is in front");
    PadGlyphsTestSetFocused(true);
    g_hasPrimary = false;
    Check(!PadGlyphsInviteHeld(), "invite: no primary device: no");
    g_hasPrimary = true;

    printf("pad glyphs: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
