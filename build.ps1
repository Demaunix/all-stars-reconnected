$ErrorActionPreference = "Stop"
& cmd /c "`"$PSScriptRoot\tools\build.cmd`""
exit $LASTEXITCODE
