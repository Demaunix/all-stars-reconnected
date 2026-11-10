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
// Before the game's first parse: offer the stock 21 racers online, whatever the installed network module lists (modset.h ModLocalOnly).
void ScriptSubstUseStockRacers();
// Before the game's first parse: On-Foot Sonic and his add-on are installed; a Metal Sonic package then gets the 23-racer module.
void ScriptSubstUseOnFootRacers();
// Whether the lobby's racer list is the 23-racer one with On-Foot Sonic (after the first parse; false before it or without Metal).
bool ScriptSubstOnFootListed();
