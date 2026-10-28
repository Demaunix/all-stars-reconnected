// The author's marks: a rights notice and fingerprint markers kept inside
// SteamMultiplayer.dll, so that a copy of this DLL or of its code can be recognised. Nothing shows
// them in the game or the log. The author's finder tool reads the values from this file: keep them
// the same in every build. Include from steam_mp.cpp only.
#pragma once
#include <stdint.h>

static const char kRightsNotice[] =
    "Steam Multiplayer for Sonic & SEGA All-Stars Racing. Copyright (C) 2026 Demaunix. All rights reserved. "
    "No copying, reuploading or reuse of this mod, its code or any part of it without the author's written permission.";
static const char kFingerprintId[] = "SMP-7617399cff25e342f9abe44076edb13a";
static const char kFingerprintTag[] = "dema-chao-lantern-7CE05FB9";
static const uint32_t kFingerprintMark = 0x68B2DC97;

// Volatile stores, so the compiler and the linker (/OPT:REF) keep the strings, and the init code
// carries the mark as an immediate.
static const char* volatile g_fingerprintText[3];
static volatile uint32_t g_fingerprintMark;

static void FingerprintKeep() {
    g_fingerprintText[0] = kRightsNotice;
    g_fingerprintText[1] = kFingerprintId;
    g_fingerprintText[2] = kFingerprintTag;
    g_fingerprintMark = kFingerprintMark;
}
