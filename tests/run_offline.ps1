# Offline checks: no Steam initialization happens in any of these cases.
# 0 parser unit tests  1 steam_api.dll missing  2 steam_api.dll tampered  3 null api pointer (in every run)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$vc = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /I`"$root\src\chaogarage`" /Fo`"$root\build\\`" /Fe`"$root\build\harness.exe`" `"$root\tests\harness.cpp`""
if ($LASTEXITCODE) { throw 'harness build failed' }
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /I`"$root\third_party\steamworks\sdk\public`" /Fo`"$root\build\\`" /Fe`"$root\build\test_parse.exe`" `"$root\tests\test_parse.cpp`""
if ($LASTEXITCODE) { throw 'parse test build failed' }
& (Join-Path $root 'build\test_parse.exe')
$fail = $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_bg_sites.exe`" `"$root\tests\test_bg_sites.cpp`""
if ($LASTEXITCODE) { throw 'bg sites test build failed' }
& (Join-Path $root 'build\test_bg_sites.exe') (Join-Path $root 're\bin\SSASR-unpacked.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_background.exe`" `"$root\tests\test_background.cpp`" user32.lib"
if ($LASTEXITCODE) { throw 'background test build failed' }
& (Join-Path $root 'build\test_background.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_steer.exe`" `"$root\tests\test_steer.cpp`""
if ($LASTEXITCODE) { throw 'steer test build failed' }
& (Join-Path $root 'build\test_steer.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_modset.exe`" `"$root\tests\test_modset.cpp`""
if ($LASTEXITCODE) { throw 'modset test build failed' }
$scratch = Join-Path $root 'build\modset-test'
if (Test-Path $scratch) { Remove-Item $scratch -Recurse -Force }
& (Join-Path $root 'build\test_modset.exe') $scratch 'A:\SteamLibrary\steamapps\common\Sonic and SEGA All Stars Racing\mods'
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /DSCRIPT_SUBST_TEST /I`"$root\src`" /I`"$root\build`" /Fo`"$root\build\\`" /Fe`"$root\build\test_script_subst.exe`" `"$root\tests\test_script_subst.cpp`" `"$root\src\script_subst.cpp`" bcrypt.lib"
if ($LASTEXITCODE) { throw 'script substitution test build failed' }
$game = 'A:\SteamLibrary\steamapps\common\Sonic and SEGA All Stars Racing'
# The Roster Pack's package: installed, or parked during a Metal Sonic 1.1.2 test (tools\metal112_setup.ps1).
$packSbn = "$game\mods\All-Stars Roster Pack\payloads\Scripts\FrontEndScripts.sbn"
if (-not (Test-Path $packSbn)) { $packSbn = "$game\mods-parked-for-metal112\All-Stars Roster Pack\payloads\Scripts\FrontEndScripts.sbn" }
& (Join-Path $root 'build\test_script_subst.exe') pack $packSbn
$fail += $LASTEXITCODE
& (Join-Path $root 'build\test_script_subst.exe') stock "$game\Scripts\FrontEndScripts.sbn"
$fail += $LASTEXITCODE
& (Join-Path $root 'build\test_script_subst.exe') metal (Join-Path $root 'reference\metal-sonic-1.1.2\FrontEndScripts.sbn')
$fail += $LASTEXITCODE
& (Join-Path $root 'build\test_script_subst.exe') other (Join-Path $root 'reference\metal-sonic-1.1.2\FrontEndScripts.sbn')
$fail += $LASTEXITCODE
& (Join-Path $root 'build\test_script_subst.exe') other "$game\Scripts\FrontEndScripts.sbn"
$fail += $LASTEXITCODE
& (Join-Path $root 'build\test_script_subst.exe') other $packSbn
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /DUNICODE /D_UNICODE /DNATIVES_TEST /I`"$root\src`" /I`"$root\build`" /Fo`"$root\build\\`" /Fe`"$root\build\test_natives.exe`" `"$root\tests\test_natives.cpp`" `"$root\src\natives.cpp`" user32.lib"
if ($LASTEXITCODE) { throw 'natives test build failed' }
& (Join-Path $root 'build\test_natives.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_race_wire.exe`" `"$root\tests\test_race_wire.cpp`""
if ($LASTEXITCODE) { throw 'race wire test build failed' }
& (Join-Path $root 'build\test_race_wire.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_search_rows.exe`" `"$root\tests\test_search_rows.cpp`""
if ($LASTEXITCODE) { throw 'search rows test build failed' }
& (Join-Path $root 'build\test_search_rows.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /I`"$root\src`" /Fo`"$root\build\\`" /Fe`"$root\build\test_voice.exe`" `"$root\tests\test_voice.cpp`""
if ($LASTEXITCODE) { throw 'voice test build failed' }
& (Join-Path $root 'build\test_voice.exe')
$fail += $LASTEXITCODE
cmd /c "`"$vc`" >nul 2>nul && cl /nologo /O2 /MT /EHsc /std:c++17 /DUNICODE /D_UNICODE /DRACE_TEST /I`"$root\src`" /I`"$root\build`" /Fo`"$root\build\\`" /Fe`"$root\build\test_race.exe`" `"$root\tests\test_race.cpp`" `"$root\src\race.cpp`""
if ($LASTEXITCODE) { throw 'race test build failed' }
& (Join-Path $root 'build\test_race.exe') (Join-Path $root 'menus\out\results.smosbin') | Select-String -NotMatch '^    log:'
$fail += $LASTEXITCODE
# The author's marks (src/fingerprint.h) are in the built DLL, and the finder sees none in a file without them.
# (The finder is the author's own tool: not in the public source tree.)
if (Test-Path (Join-Path $root 'tools\find_fingerprint.py')) {
    python -I (Join-Path $root 'tools\find_fingerprint.py') --expect-all (Join-Path $root 'out\Steam Multiplayer\runtime\SteamMultiplayer.dll')
    $fail += $LASTEXITCODE
    python -I (Join-Path $root 'tools\find_fingerprint.py') --expect-none (Join-Path $root 'out\Steam Multiplayer\runtime\steam_api.dll')
    $fail += $LASTEXITCODE
}
$work = Join-Path $root 'build\offline'
foreach ($case in 'missing', 'tampered') {
    $dir = Join-Path $work $case
    if (Test-Path $dir) { Remove-Item $dir -Recurse -Force }
    Copy-Item (Join-Path $root 'out\Steam Multiplayer') $dir -Recurse
    $api = Join-Path $dir 'runtime\steam_api.dll'
    if ($case -eq 'missing') { Remove-Item $api } else { $b = [IO.File]::ReadAllBytes($api); $b[1000] = $b[1000] -bxor 1; [IO.File]::WriteAllBytes($api, $b) }
    $out = & (Join-Path $root 'build\harness.exe') $dir 0 | Out-String
    $log = Get-Content (Join-Path $dir 'SteamMultiplayer.log') -Raw
    $ok = ($out -match 'init\(null api\) = 0x80070057') -and ($out -match 'init = 0x00000000') -and ($out -match 'steam_api.dll loaded: no') -and
          ($out -match 'rollback = 0x00000000') -and ($log -match 'online features are off') -and ($log -notmatch 'Steam ready')
    "$case : $(if ($ok) { 'PASS' } else { 'FAIL' })"
    if (-not $ok) { $fail++; $out; $log }
}
exit $fail
