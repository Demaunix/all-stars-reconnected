#pragma once
// Writes a line to mods\Steam Multiplayer\SteamMultiplayer.log; `host` also mirrors it to SsasrMods.log.
void Log(bool host, const wchar_t* fmt, ...);
