#pragma once
#include <windows.h>
#include <string>

// Multiplayer menus (M2): substitutes our front-end script modules when the game parses
// Scripts/FrontEndScripts.sbn (see script_subst.cpp, notes/SCRIPT-LOADER.md). Install runs once in
// SsasrModInitialize, before the game's CRT; on any unexpected byte or module it stays off and the
// game keeps its own menus.
// Config: <mod>\config.ini [Menus] Enabled=1 (0 keeps the game's own menus).
void ScriptSubstInstall(HMODULE gameImage, const std::wstring& modDir);
void ScriptSubstRollback();
