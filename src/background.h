#pragma once
#include <windows.h>
#include <string>

// Background running (M2): the game keeps running while unfocused or while its window is dragged,
// and ignores game input while unfocused. Config: <mod>\config.ini [Background] RunWhenUnfocused=1.
// Install runs once in SsasrModInitialize (main thread, before the game's CRT). If any original
// byte differs from the expected build, nothing is patched and the feature stays off.
void BackgroundInstall(HMODULE gameImage, const std::wstring& modDir);
void BackgroundRollback();
