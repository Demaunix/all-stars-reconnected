@echo off
rem Builds runtime\SteamMultiplayer.dll (x86) and assembles out\Steam Multiplayer\ (the mod folder).
setlocal
set ROOT=%~dp0..
set SDK=%ROOT%\third_party\steamworks\sdk
if not exist "%SDK%\public\steam\steam_api.h" (echo Steamworks SDK 1.65 not extracted to third_party\steamworks\sdk & exit /b 1)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>nul || exit /b 1
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
python -I "%ROOT%\tools\build_menus.py" "%ROOT%" || exit /b 1
cl /nologo /c /O2 /MT /GS /EHsc /W3 /std:c++17 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN ^
   /I"%ROOT%\src" /I"%ROOT%\src\chaogarage" /I"%ROOT%\build" /I"%SDK%\public" ^
   /Fo"%ROOT%\build\\" "%ROOT%\src\steam_mp.cpp" "%ROOT%\src\background.cpp" "%ROOT%\src\script_subst.cpp" "%ROOT%\src\natives.cpp" "%ROOT%\src\race.cpp" "%ROOT%\src\voice.cpp" "%ROOT%\src\update.cpp" || exit /b 1
link /nologo /DLL /MACHINE:X86 /DYNAMICBASE /NXCOMPAT /SAFESEH /OPT:REF /DEF:"%ROOT%\src\exports.def" ^
   /OUT:"%ROOT%\build\SteamMultiplayer.dll" "%ROOT%\build\steam_mp.obj" "%ROOT%\build\background.obj" "%ROOT%\build\script_subst.obj" "%ROOT%\build\natives.obj" "%ROOT%\build\race.obj" "%ROOT%\build\voice.obj" "%ROOT%\build\update.obj" ^
   "%SDK%\redistributable_bin\steam_api.lib" delayimp.lib bcrypt.lib shell32.lib user32.lib ole32.lib winhttp.lib /DELAYLOAD:steam_api.dll /DELAYLOAD:winhttp.dll || exit /b 1
python -I "%ROOT%\tools\package.py" "%ROOT%" || exit /b 1
