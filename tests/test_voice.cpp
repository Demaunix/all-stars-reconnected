// Offline test of the voice chat's pure parts (src/voice.h): the packet format, the sequence and talking rules, the playout
// ring, the config words (Mode, Key).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "voice.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

int main() {
    // Packets: "ASV1", version, flags, the sequence, then the compressed data as it is.
    uint8_t data[300], pkt[kVoicePacketMax];
    for (int i = 0; i < 300; ++i) data[i] = (uint8_t)(i * 7);
    size_t n = VoiceEncode(0x1234, data, 300, pkt, sizeof(pkt));
    CHECK(n == kVoiceHeader + 300 && memcmp(pkt, "ASV1", 4) == 0 && pkt[4] == kVoiceVersion && pkt[6] == 0x34 && pkt[7] == 0x12);
    uint16_t seq = 0;
    const uint8_t* body = nullptr;
    uint32_t len = 0;
    CHECK(VoiceDecode(pkt, n, seq, body, len) && seq == 0x1234 && len == 300 && body == pkt + kVoiceHeader && memcmp(body, data, 300) == 0);
    {   // the lobby popup's MUTE: a toggle per player, a full list refuses, id 0 never
        uint64_t list[3] = {};
        int count = 0;
        CHECK(VoiceMuteToggle(list, count, 3, 11) == 1 && count == 1 && list[0] == 11);
        CHECK(VoiceMuteToggle(list, count, 3, 22) == 1 && VoiceMuteToggle(list, count, 3, 33) == 1 && count == 3);
        CHECK(VoiceMuteToggle(list, count, 3, 44) == -1 && count == 3);              // full: nothing changed
        CHECK(VoiceMuteToggle(list, count, 3, 22) == 0 && count == 2 && list[0] == 11 && list[1] == 33 && list[2] == 0);
        CHECK(VoiceMuteToggle(list, count, 3, 44) == 1 && count == 3 && list[2] == 44);
        CHECK(VoiceMuteToggle(list, count, 3, 0) == -1 && count == 3);
    }
    CHECK(VoiceEncode(1, data, 0, pkt, sizeof(pkt)) == 0);                         // nothing to send
    CHECK(VoiceEncode(1, data, kVoiceDataMax + 1, pkt, sizeof(pkt)) == 0);         // too big for one packet
    CHECK(VoiceEncode(1, data, 300, pkt, kVoiceHeader + 299) == 0);                // no room
    CHECK(VoiceEncode(1, nullptr, 300, pkt, sizeof(pkt)) == 0);
    CHECK(!VoiceDecode(pkt, kVoiceHeader, seq, body, len));                        // a header alone
    CHECK(!VoiceDecode(pkt, kVoicePacketMax + 1, seq, body, len));
    uint8_t bad[64];
    memcpy(bad, pkt, 64);
    bad[0] = 'B';
    CHECK(!VoiceDecode(bad, 64, seq, body, len));
    memcpy(bad, pkt, 64);
    bad[4] = kVoiceVersion + 1;
    CHECK(!VoiceDecode(bad, 64, seq, body, len));
    CHECK(!VoiceDecode(nullptr, 64, seq, body, len));
    // Only a newer sequence plays; the counter wraps.
    CHECK(VoiceSeqNewer(7, 5) && !VoiceSeqNewer(5, 7) && !VoiceSeqNewer(5, 5) && VoiceSeqNewer(0, 0xffff) && !VoiceSeqNewer(0xffff, 0) && VoiceSeqNewer(3, 0xfff0));
    // Talking: within 300 ms, ticks compared signed; never talked = 0.
    CHECK(VoiceTalking(1000, 1200) && !VoiceTalking(1000, 1300) && !VoiceTalking(0, 100) && VoiceTalking(1001, 1000) && VoiceTalking(0xfffffff1u, 10u) &&
          VoiceTalking(10u, 0xfffffff1u) && !VoiceTalking(10u, 0x7ffffffbu));  // (a tick slightly ahead counts; one far behind doesn't)
    // The ring: 16 slots in turn; a full queue drops.
    VoiceRing ring;
    for (int i = 0; i < 16; ++i) CHECK(ring.Take((unsigned)i) == i);
    CHECK(ring.Take(0) == 0 && ring.Take(15) == 1 && ring.Take(16) == -1 && ring.Take(99) == -1 && ring.Take(3) == 2);
    ring.Untake();  // a submit that failed: the slot is taken again next
    CHECK(ring.Take(3) == 2 && ring.Take(3) == 3);
    VoiceRing fresh;
    fresh.Untake();
    CHECK(fresh.Take(0) == 15 && fresh.Take(0) == 0);
    // Config words.
    CHECK(VoiceModeOf(L"off") == kVoiceOff && VoiceModeOf(L"OFF") == kVoiceOff && VoiceModeOf(L"") == kVoiceOff && VoiceModeOf(nullptr) == kVoiceOff &&
          VoiceModeOf(L"bogus") == kVoiceOff && VoiceModeOf(L"0") == kVoiceOff);
    CHECK(VoiceModeOf(L"ptt") == kVoicePushToTalk && VoiceModeOf(L" PTT") == kVoicePushToTalk && VoiceModeOf(L"push") == kVoicePushToTalk && VoiceModeOf(L"1") == kVoicePushToTalk);
    CHECK(VoiceModeOf(L"open") == kVoiceOpen && VoiceModeOf(L"Open") == kVoiceOpen && VoiceModeOf(L"mic") == kVoiceOpen && VoiceModeOf(L"2") == kVoiceOpen);
    CHECK(VoiceKeyOf(L"V") == 'V' && VoiceKeyOf(L"v") == 'V' && VoiceKeyOf(L" b") == 'B' && VoiceKeyOf(L"7") == '7' && VoiceKeyOf(L"F3") == 0x72 && VoiceKeyOf(L"f12") == 0x7b &&
          VoiceKeyOf(L"F24") == 0x87 && VoiceKeyOf(L"F25") == 0 && VoiceKeyOf(L"F0") == 0);
    CHECK(VoiceKeyOf(L"SPACE") == 0x20 && VoiceKeyOf(L"space") == 0x20 && VoiceKeyOf(L"LCTRL") == 0xa2 && VoiceKeyOf(L"Ctrl") == 0x11 && VoiceKeyOf(L"RShift") == 0xa1 &&
          VoiceKeyOf(L"ALT") == 0x12 && VoiceKeyOf(L"CAPSLOCK") == 0x14 && VoiceKeyOf(L"TAB") == 0x09 && VoiceKeyOf(L"tilde") == 0xc0);
    CHECK(VoiceKeyOf(L"0x5A") == 0x5a && VoiceKeyOf(L"0x5a") == 0x5a && VoiceKeyOf(L"93") == 93 && VoiceKeyOf(L"0") == '0' && VoiceKeyOf(L"999") == 0 && VoiceKeyOf(L"0x100") == 0);
    CHECK(VoiceKeyOf(L"") == 0 && VoiceKeyOf(nullptr) == 0 && VoiceKeyOf(L"bogus") == 0 && VoiceKeyOf(L"?") == 0 && VoiceKeyOf(L"V ; the key") == 'V' && VoiceKeyOf(L"thisnameistoolongtobeakey") == 0);
    printf("voice: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
