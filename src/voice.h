#pragma once
// Voice chat (M7; re\m7-voice\FINDINGS.md): the console's online party voice, on Steam. Steam records and encodes the player's
// microphone (ISteamUser voice) on our Steam worker; each chunk goes unreliably on its own networking channel to the lobby's
// other members, whose workers decode it and play it through the mod's own XAudio2 engine (the game keeps its own sound
// system); the lobby bars show who is talking. config.ini [Voice]: Mode (off, ptt, open), Key (push-to-talk), Volume, Muted.
// This header holds the pure parts (tests\test_voice.cpp); the worker-side class is voice_chat.h / voice.cpp.
#include <stdint.h>
#include <string.h>

constexpr int kVoiceChannel = 2;           // ISteamNetworkingMessages channel (the race uses kRaceChannel = 1)
constexpr uint32_t kVoiceDataMax = 8192;   // compressed bytes per packet: one GetVoice chunk (a few hundred bytes), never split
constexpr uint32_t kVoiceHeader = 8;       // "ASV1", version, flags, u16 sequence (little-endian); the data follows
constexpr uint8_t kVoiceVersion = 1;
constexpr uint32_t kVoicePacketMax = kVoiceHeader + kVoiceDataMax;
constexpr uint32_t kVoiceTalkingMs = 300;  // a member "talks" while its packets keep coming within this

inline size_t VoiceEncode(uint16_t seq, const void* data, uint32_t len, uint8_t* out, size_t cap) {
    if (!data || !out || len == 0 || len > kVoiceDataMax || cap < kVoiceHeader + len) return 0;
    memcpy(out, "ASV1", 4);
    out[4] = kVoiceVersion;
    out[5] = 0;
    out[6] = (uint8_t)seq;
    out[7] = (uint8_t)(seq >> 8);
    memcpy(out + kVoiceHeader, data, len);
    return kVoiceHeader + len;
}

inline bool VoiceDecode(const uint8_t* in, size_t size, uint16_t& seq, const uint8_t*& data, uint32_t& len) {
    if (!in || size <= kVoiceHeader || size > kVoicePacketMax || memcmp(in, "ASV1", 4) != 0 || in[4] != kVoiceVersion) return false;
    seq = (uint16_t)(in[6] | (in[7] << 8));
    data = in + kVoiceHeader;
    len = (uint32_t)(size - kVoiceHeader);
    return true;
}

// Unreliable packets may come late or twice: only a newer sequence number is played (wrap-safe).
inline bool VoiceSeqNewer(uint16_t seq, uint16_t last) { return (int16_t)(seq - last) > 0; }
// Talking: a packet within kVoiceTalkingMs (ticks compared signed, as the lobby heartbeat; 0 = never).
inline bool VoiceTalking(uint32_t lastTick, uint32_t now) { return lastTick != 0 && (int32_t)(now - lastTick) < (int32_t)kVoiceTalkingMs; }
// The lobby popup's MUTE (a toggle, as an earlier console build's popup): adds or removes id in list[0..count) -> 1 muted now, 0 heard again,
// -1 the list is full (nothing changed) or id 0.
inline int VoiceMuteToggle(uint64_t* list, int& count, int cap, uint64_t id) {
    if (!id) return -1;
    for (int i = 0; i < count; ++i)
        if (list[i] == id) {
            for (int j = i + 1; j < count; ++j) list[j - 1] = list[j];
            list[--count] = 0;
            return 0;
        }
    if (count >= cap) return -1;
    list[count++] = id;
    return 1;
}

// The playout ring: kVoiceRingSlots fixed buffers per talker, used in turn. XAudio2 frees them in order, so the next slot is
// free again once fewer than kVoiceRingSlots buffers are queued; a full ring drops the packet (that keeps the delay short).
constexpr int kVoiceRingSlots = 16;
constexpr int kVoiceVolumeMax = 300;  // config.ini [Voice] Volume, percent (XAudio2 SetVolume amplifies above 1.0)
constexpr uint32_t kVoiceRingBytes = 24576;  // decoded 16-bit mono: 256 ms at 48 kHz (Steam suggests a 20 KB start for DecompressVoice)
struct VoiceRing {
    int next = 0;
    int Take(unsigned queued) {
        if (queued >= (unsigned)kVoiceRingSlots) return -1;
        int s = next;
        next = (next + 1) % kVoiceRingSlots;
        return s;
    }
    void Untake() { next = (next + kVoiceRingSlots - 1) % kVoiceRingSlots; }  // the taken slot wasn't queued after all
};

enum VoiceMode : int32_t { kVoiceOff = 0, kVoicePushToTalk = 1, kVoiceOpen = 2 };

inline bool VoiceWordIs(const wchar_t* s, const wchar_t* word) {
    for (;; ++s, ++word) {
        wchar_t a = *s, b = *word;
        if (a >= L'a' && a <= L'z') a = (wchar_t)(a - L'a' + L'A');
        if (b >= L'a' && b <= L'z') b = (wchar_t)(b - L'a' + L'A');
        if (a != b) return false;
        if (!a) return true;
    }
}

// config.ini [Voice] Mode: off (nothing sent or played), ptt (push-to-talk with Key), open (an open microphone while in a lobby).
inline int VoiceModeOf(const wchar_t* s) {
    if (!s) return kVoiceOff;
    while (*s == L' ' || *s == L'\t') ++s;
    if (VoiceWordIs(s, L"ptt") || VoiceWordIs(s, L"push") || VoiceWordIs(s, L"pushtotalk") || VoiceWordIs(s, L"1")) return kVoicePushToTalk;
    if (VoiceWordIs(s, L"open") || VoiceWordIs(s, L"mic") || VoiceWordIs(s, L"2")) return kVoiceOpen;
    return kVoiceOff;
}

// config.ini [Voice] Key: a letter or digit, F1..F24, SPACE, TAB, CTRL/LCTRL/RCTRL, SHIFT/LSHIFT/RSHIFT, ALT/LALT/RALT, CAPSLOCK,
// or a virtual-key number (decimal, or 0x..). 0 = none.
inline int VoiceKeyOf(const wchar_t* s) {
    if (!s) return 0;
    while (*s == L' ' || *s == L'\t') ++s;
    size_t n = 0;
    while (s[n] && s[n] != L' ' && s[n] != L'\t' && s[n] != L';') ++n;
    if (n == 0) return 0;
    if (n == 1) {
        wchar_t c = s[0];
        if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return (int)c;
        return 0;
    }
    wchar_t word[16] = {};
    if (n >= 16) return 0;
    for (size_t i = 0; i < n; ++i) word[i] = s[i];
    if ((word[0] == L'F' || word[0] == L'f') && word[1] >= L'0' && word[1] <= L'9') {
        int f = 0;
        for (size_t i = 1; i < n; ++i) {
            if (word[i] < L'0' || word[i] > L'9') return 0;
            f = f * 10 + (word[i] - L'0');
        }
        return f >= 1 && f <= 24 ? 0x70 + f - 1 : 0;
    }
    if (word[0] == L'0' && (word[1] == L'x' || word[1] == L'X')) {
        int v = 0;
        for (size_t i = 2; i < n; ++i) {
            wchar_t c = word[i];
            int d = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'a' && c <= L'f' ? c - L'a' + 10 : c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
            if (d < 0) return 0;
            v = v * 16 + d;
        }
        return v > 0 && v < 256 ? v : 0;
    }
    if (word[0] >= L'0' && word[0] <= L'9') {
        int v = 0;
        for (size_t i = 0; i < n; ++i) {
            if (word[i] < L'0' || word[i] > L'9') return 0;
            v = v * 10 + (word[i] - L'0');
        }
        return v > 0 && v < 256 ? v : 0;
    }
    struct Named { const wchar_t* name; int vk; };
    static const Named kNamed[] = {
        {L"SPACE", 0x20}, {L"TAB", 0x09}, {L"CAPSLOCK", 0x14}, {L"CTRL", 0x11}, {L"LCTRL", 0xA2}, {L"RCTRL", 0xA3},
        {L"SHIFT", 0x10}, {L"LSHIFT", 0xA0}, {L"RSHIFT", 0xA1}, {L"ALT", 0x12}, {L"LALT", 0xA4}, {L"RALT", 0xA5},
        {L"ENTER", 0x0D}, {L"BACKSPACE", 0x08}, {L"HOME", 0x24}, {L"END", 0x23}, {L"INSERT", 0x2D}, {L"DELETE", 0x2E},
        {L"PAGEUP", 0x21}, {L"PAGEDOWN", 0x22}, {L"TILDE", 0xC0}, {L"GRAVE", 0xC0},
    };
    for (const Named& k : kNamed)
        if (VoiceWordIs(word, k.name)) return k.vk;
    return 0;
}
