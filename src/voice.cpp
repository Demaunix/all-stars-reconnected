// Voice chat on Steam (voice.h, voice_chat.h; re\m7-voice\FINDINGS.md). Every Steam call here runs on the Steam worker thread
// (steam_mp.cpp's loop calls Pump each time round). Playback: the mod's own XAudio2 engine (xaudio2_9.dll, loaded at the first
// packet; without it the others' voice can't be played here and the log says so), one mono 16-bit source voice per talker at
// Steam's optimal decode rate, a ring of fixed buffers per talker (a full ring drops the packet: the delay stays short).
#include <windows.h>
#include <xaudio2.h>
#include <stdio.h>
#include "steam/steam_api.h"
#include "voice_chat.h"
#include "log.h"

struct VoiceAudio {
    HMODULE lib = nullptr;
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    bool com = false;  // this thread's CoInitializeEx is ours to undo
};

struct VoiceTalker {
    uint64_t id = 0;
    IXAudio2SourceVoice* voice = nullptr;
    uint8_t* ring = nullptr;  // kVoiceRingSlots * kVoiceRingBytes
    VoiceRing slots;
    uint16_t lastSeq = 0;
    bool seqSeen = false;
    uint32_t lastTick = 0, packets = 0, dropped = 0, late = 0;
};

typedef HRESULT(__stdcall* XAudio2CreateFn)(IXAudio2** engine, UINT32 flags, XAUDIO2_PROCESSOR processor);

static uint8_t g_pcm[kVoiceRingBytes];  // the decode scratch (worker thread only)

void VoiceChat::Configure(const wchar_t* ini) {
    wchar_t mode[64] = {}, key[64] = {}, muted[512] = {};
    GetPrivateProfileStringW(L"Voice", L"Mode", L"ptt", mode, 64, ini);
    GetPrivateProfileStringW(L"Voice", L"Key", L"V", key, 64, ini);
    GetPrivateProfileStringW(L"Voice", L"Muted", L"", muted, 512, ini);
    int volume = (int)GetPrivateProfileIntW(L"Voice", L"Volume", 100, ini);
    m_volume = volume < 0 ? 0 : volume > kVoiceVolumeMax ? kVoiceVolumeMax : volume;  // (above 100 amplifies: quiet microphones)
    m_mode = VoiceModeOf(mode);
    m_key = VoiceKeyOf(key);
    m_mutedCount = 0;
    for (const wchar_t* s = muted; *s && m_mutedCount < 16;) {
        wchar_t* end = nullptr;
        unsigned long long id = wcstoull(s, &end, 10);
        if (end == s) break;
        if (id) m_muted[m_mutedCount++] = id;
        s = *end == L',' ? end + 1 : end;
    }
    if (m_mode == kVoicePushToTalk && !m_key) {
        Log(true, L"voice chat: the push-to-talk key \"%s\" is not a key name I know (config.ini [Voice] Key): voice chat is off", key);
        m_mode = kVoiceOff;
    }
    if (m_mode == kVoiceOff) {
        Log(false, L"voice chat: off (config.ini [Voice] Mode)");
        return;
    }
    m_rate = SteamUser()->GetVoiceOptimalSampleRate();
    if (m_rate < 11025 || m_rate > 48000) m_rate = 48000;
    Log(true, L"voice chat: %s%s%s, volume %d, %u Hz, %d player(s) muted (config.ini [Voice])", m_mode == kVoiceOpen ? L"open microphone while in a lobby" : L"push-to-talk, hold ",
        m_mode == kVoiceOpen ? L"" : key, m_mode == kVoiceOpen ? L"" : L" (keyboard)", m_volume, m_rate, m_mutedCount);
}

bool VoiceChat::Muted(uint64_t id) const {
    for (int i = 0; i < m_mutedCount; ++i)
        if (m_muted[i] == id) return true;
    return false;
}

int VoiceChat::ToggleMuted(uint64_t id) { return VoiceMuteToggle(m_muted, m_mutedCount, 16, id); }

bool VoiceChat::Talking(uint64_t id, uint32_t now) const {
    if (Muted(id)) return false;  // the bubble lights only for a voice this player hears
    for (int i = 0; i < m_talkerCount; ++i)
        if (m_talkers[i]->id == id) return VoiceTalking(m_talkers[i]->lastTick, now);
    return false;
}

// This player's bubble: push-to-talk while the key is held (as the console's talk button), open mic while voice actually goes out.
bool VoiceChat::Transmitting(uint32_t now) const { return (m_mode == kVoicePushToTalk && m_recording) || VoiceTalking(m_lastSent, now); }

void VoiceChat::Pump(const uint64_t* peers, int peerCount, bool active, bool overlayOpen) {
    uint32_t now = GetTickCount() | 1;
    // The channel is always drained: the others send their voice whatever this player's setting, and Steam's connections hold
    // one receive buffer for every channel to a peer (steamnetworkingtypes.h RecvBufferSize): unread voice would make the
    // race's own messages (channel 1) from that player drop.
    if (m_mode != kVoiceOff) Record(peers, peerCount, active, overlayOpen, now);
    Receive(peers, peerCount, now);
}

// Push-to-talk: the key is read with GetAsyncKeyState, only while this game's window is in the foreground (two games on one
// PC share the keyboard) and the Steam overlay isn't up (typing there).
void VoiceChat::Record(const uint64_t* peers, int peerCount, bool active, bool overlayOpen, uint32_t now) {
    bool keyDown = false;
    if (m_mode == kVoicePushToTalk && m_key) {
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg) GetWindowThreadProcessId(fg, &pid);
        keyDown = pid == GetCurrentProcessId() && (GetAsyncKeyState(m_key) & 0x8000) != 0;
    }
    bool want = active && peerCount > 0 && !overlayOpen && (m_mode == kVoiceOpen || keyDown);
    if (want != m_recording) {
        m_recording = want;
        if (want) {
            SteamUser()->StartVoiceRecording();
            m_draining = false;
        } else {
            SteamUser()->StopVoiceRecording();  // (Steam keeps a little more: drained below until NotRecording)
            m_draining = true;
        }
        SteamFriends()->SetInGameVoiceSpeaking(SteamUser()->GetSteamID(), want);
        if (m_logged++ < 8) {
            if (want) Log(false, L"voice: recording (%s)", m_mode == kVoiceOpen ? L"open microphone" : L"push-to-talk");
            else Log(false, L"voice: recording stopped (%u packets sent so far)", m_sent);
        }
    }
    if (!m_recording && !m_draining) return;
    for (int chunks = 0; chunks < 4; ++chunks) {  // a few chunks per pump (the loop runs every 30 ms in the menus, 2 ms in a race)
        uint32_t avail = 0;
        EVoiceResult r = SteamUser()->GetAvailableVoice(&avail);
        if (r == k_EVoiceResultNotRecording) {
            m_draining = false;
            break;
        }
        if (r != k_EVoiceResultOK || avail == 0) break;
        uint8_t chunk[kVoiceDataMax];
        uint32_t written = 0;
        r = SteamUser()->GetVoice(true, chunk, sizeof(chunk), &written);
        if (r != k_EVoiceResultOK || written == 0) {
            if (r != k_EVoiceResultNoData && m_logged++ < 12) Log(false, L"voice: GetVoice answered %d (%u bytes waiting)", (int)r, avail);
            break;
        }
        uint8_t pkt[kVoicePacketMax];
        size_t size = VoiceEncode(++m_seq, chunk, written, pkt, sizeof(pkt));
        if (!size) continue;
        for (int i = 0; i < peerCount; ++i) {
            SteamNetworkingIdentity to;
            to.SetSteamID64(peers[i]);
            SteamNetworkingMessages()->SendMessageToUser(to, pkt, (uint32)size, k_nSteamNetworkingSend_UnreliableNoDelay | k_nSteamNetworkingSend_AutoRestartBrokenSession, kVoiceChannel);
        }
        ++m_sent;
        m_lastSent = now;
        if (m_sent <= 3 || m_sent % 500 == 0) Log(false, L"voice: packet %u sent to %d player(s) (%u bytes)", m_sent, peerCount, written);
    }
}

void VoiceChat::Receive(const uint64_t* peers, int peerCount, uint32_t now) {
    SteamNetworkingMessage_t* in[16];
    int n;
    while ((n = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kVoiceChannel, in, 16)) > 0) {
        for (int i = 0; i < n; ++i) {
            uint64_t from = in[i]->m_identityPeer.GetSteamID64();
            bool member = false;
            for (int j = 0; j < peerCount; ++j) member = member || peers[j] == from;
            uint16_t seq = 0;
            const uint8_t* data = nullptr;
            uint32_t len = 0;
            bool packet = member && m_mode != kVoiceOff && VoiceDecode((const uint8_t*)in[i]->m_pData, (size_t)in[i]->m_cbSize, seq, data, len);
            VoiceTalker* t = packet ? TalkerFor(from, peers, peerCount, now) : nullptr;
            if (m_mode == kVoiceOff) {
                // (drained and dropped: voice chat is off here)
            } else if (!t) {
                if (++m_badPackets <= 3) Log(false, L"voice: a packet from %llu ignored (%s)", from, !member ? L"not in our lobby" : !packet ? L"not a voice packet" : L"no room for another talker");
            } else if (t->seqSeen && !VoiceSeqNewer(seq, t->lastSeq)) {
                ++t->late;  // (came late or twice: played would be out of order)
            } else {
                t->lastSeq = seq;
                t->seqSeen = true;
                t->lastTick = now;
                ++t->packets;
                ++m_received;
                if (t->packets <= 3 || t->packets % 500 == 0) Log(false, L"voice: packet %u from %llu (%u bytes%s)", t->packets, from, len, Muted(from) ? L", muted" : L"");
                if (!Muted(from) && m_volume > 0) Play(*t, data, len);
            }
            in[i]->Release();
        }
    }
}

// A talker's voice and ring are kept for the whole session (one per player heard); with all 8 in use, the one that is no longer in
// the lobby, or else the longest quiet, is given to the new player (its voice flushed, its sequence forgotten).
VoiceTalker* VoiceChat::TalkerFor(uint64_t id, const uint64_t* peers, int peerCount, uint32_t now) {
    for (int i = 0; i < m_talkerCount; ++i)
        if (m_talkers[i]->id == id) return m_talkers[i];
    VoiceTalker* t = nullptr;
    if (m_talkerCount < 8) {
        t = new VoiceTalker;
        m_talkers[m_talkerCount++] = t;
    } else {
        int pick = -1;
        bool pickMember = true;
        for (int i = 0; i < m_talkerCount; ++i) {  // a player no longer in the lobby first; else the longest quiet
            bool member = false;
            for (int j = 0; j < peerCount; ++j) member = member || peers[j] == m_talkers[i]->id;
            bool older = pick < 0 || (int32_t)(m_talkers[i]->lastTick - m_talkers[pick]->lastTick) < 0;
            if (pick < 0 || (!member && pickMember) || (member == pickMember && older)) {
                pick = i;
                pickMember = member;
            }
        }
        (void)now;
        if (pick < 0) return nullptr;
        t = m_talkers[pick];
        if (t->voice) {
            t->voice->Stop(0);
            t->voice->FlushSourceBuffers();
            t->voice->Start(0);
        }
        t->slots = VoiceRing();
        t->seqSeen = false;
        t->lastSeq = 0;
        t->lastTick = 0;
        t->packets = t->dropped = t->late = 0;
        if (m_logged++ < 12) Log(false, L"voice: %llu's voice slot goes to %llu", t->id, id);
    }
    t->id = id;
    return t;
}

bool VoiceChat::EnsureAudio() {
    if (m_audio) return true;
    if (m_audioFailed) return false;
    VoiceAudio* a = new VoiceAudio;
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // (the audio device is reached through COM; this is our own thread)
    a->com = SUCCEEDED(co);
    a->lib = LoadLibraryW(L"xaudio2_9.dll");
    XAudio2CreateFn create = a->lib ? (XAudio2CreateFn)GetProcAddress(a->lib, "XAudio2Create") : nullptr;
    HRESULT hr = create ? create(&a->engine, 0, XAUDIO2_DEFAULT_PROCESSOR) : E_FAIL;
    if (SUCCEEDED(hr)) hr = a->engine->CreateMasteringVoice(&a->master);
    if (FAILED(hr)) {
        Log(true, L"voice: the other players' voice can't be played here: XAudio2 %s (0x%08x)", a->lib ? L"failed to start" : L"(xaudio2_9.dll) isn't on this PC", (unsigned)hr);
        if (a->engine) a->engine->Release();
        if (a->lib) FreeLibrary(a->lib);
        if (a->com) CoUninitialize();
        delete a;
        m_audioFailed = true;
        return false;
    }
    m_audio = a;
    Log(false, L"voice: XAudio2 ready; the others' voice plays at %u Hz, volume %d", m_rate, m_volume);
    return true;
}

void VoiceChat::Play(VoiceTalker& t, const uint8_t* data, uint32_t len) {
    if (!EnsureAudio()) return;
    if (!t.voice) {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = m_rate;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 2;
        fmt.nAvgBytesPerSec = m_rate * 2;
        HRESULT hr = m_audio->engine->CreateSourceVoice(&t.voice, &fmt);
        if (FAILED(hr) || !t.voice) {
            t.voice = nullptr;
            if (m_logged++ < 12) Log(false, L"voice: no source voice for %llu (0x%08x)", t.id, (unsigned)hr);
            return;
        }
        t.voice->SetVolume(m_volume / 100.0f);
        t.voice->Start(0);
        t.ring = new uint8_t[kVoiceRingSlots * kVoiceRingBytes];
    }
    XAUDIO2_VOICE_STATE st = {};
    t.voice->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (st.BuffersQueued >= (unsigned)kVoiceRingSlots) {
        ++t.dropped;  // (the ring is full: this PC is behind; the newest packet is dropped, undecoded)
        return;
    }
    uint32_t written = 0;
    EVoiceResult r = SteamUser()->DecompressVoice(data, len, g_pcm, sizeof(g_pcm), &written, m_rate);
    if (r != k_EVoiceResultOK || written == 0) {
        ++t.dropped;
        if (m_logged++ < 12) Log(false, L"voice: DecompressVoice answered %d for %u bytes from %llu", (int)r, len, t.id);
        return;
    }
    int slot = t.slots.Take(st.BuffersQueued);
    if (slot < 0) return;
    uint8_t* dst = t.ring + (size_t)slot * kVoiceRingBytes;
    memcpy(dst, g_pcm, written);
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = written;
    b.pAudioData = dst;
    if (FAILED(t.voice->SubmitSourceBuffer(&b))) {
        ++t.dropped;
        t.slots.Untake();  // (the slot wasn't queued: the next packet takes it again)
    }
}

void VoiceChat::Shutdown() {
    if (m_recording) {
        SteamUser()->StopVoiceRecording();
        SteamFriends()->SetInGameVoiceSpeaking(SteamUser()->GetSteamID(), false);
        m_recording = false;
    }
    for (int i = 0; i < m_talkerCount; ++i) {
        VoiceTalker* t = m_talkers[i];
        if (t->voice) {
            t->voice->Stop(0);
            t->voice->FlushSourceBuffers();
            t->voice->DestroyVoice();
        }
        delete[] t->ring;
        delete t;
        m_talkers[i] = nullptr;
    }
    m_talkerCount = 0;
    if (m_audio) {
        if (m_audio->master) m_audio->master->DestroyVoice();
        if (m_audio->engine) m_audio->engine->Release();
        if (m_audio->lib) FreeLibrary(m_audio->lib);
        if (m_audio->com) CoUninitialize();
        delete m_audio;
        m_audio = nullptr;
    }
    if (m_mode != kVoiceOff && (m_sent || m_received)) Log(false, L"voice: %u packets sent, %u received, %u ignored", m_sent, m_received, m_badPackets);
}
