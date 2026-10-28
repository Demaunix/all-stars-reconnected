#pragma once
// Voice chat on Steam: the worker-side class (voice.cpp; the pure parts are voice.h). Steam worker thread only.
#include <stdint.h>
#include "voice.h"

struct VoiceAudio;   // the mod's XAudio2 engine (voice.cpp)
struct VoiceTalker;  // one member's playout voice and ring

class VoiceChat {
public:
    void Configure(const wchar_t* configIni);  // config.ini [Voice]: Mode, Key, Volume, Muted
    // Every worker loop: records and sends while talking (Mode open, or the push-to-talk key down with the game in the
    // foreground and no Steam overlay), receives and plays the peers' voice. peers = the lobby's other real members (the only
    // senders accepted, the only recipients); active = this game is in a lobby (the race keeps the lobby).
    void Pump(const uint64_t* peers, int peerCount, bool active, bool overlayOpen);
    bool Talking(uint64_t id, uint32_t now) const;  // that member's voice arrived within kVoiceTalkingMs
    bool Transmitting(uint32_t now) const;          // this player's voice went out within kVoiceTalkingMs
    void Shutdown();                                // the mod unloads / Steam stops
    int Mode() const { return m_mode; }
    bool Muted(uint64_t id) const;                  // config.ini [Voice] Muted, or the lobby popup's MUTE
    int ToggleMuted(uint64_t id);                   // the lobby popup's MUTE: 1 muted now, 0 heard again, -1 the list is full

private:
    bool EnsureAudio();
    VoiceTalker* TalkerFor(uint64_t id, const uint64_t* peers, int peerCount, uint32_t now);
    void Record(const uint64_t* peers, int peerCount, bool active, bool overlayOpen, uint32_t now);
    void Receive(const uint64_t* peers, int peerCount, uint32_t now);
    void Play(VoiceTalker& t, const uint8_t* data, uint32_t len);

    int m_mode = kVoiceOff, m_key = 0, m_volume = 100;
    uint64_t m_muted[16] = {};
    int m_mutedCount = 0;
    bool m_recording = false, m_draining = false, m_audioFailed = false;
    uint16_t m_seq = 0;
    uint32_t m_lastSent = 0, m_rate = 48000;
    int m_logged = 0;
    VoiceAudio* m_audio = nullptr;
    VoiceTalker* m_talkers[8] = {};
    int m_talkerCount = 0;
    uint32_t m_sent = 0, m_received = 0, m_badPackets = 0;
};
