@echo off
setlocal DisableDelayedExpansion
echo Capture clean originals once. Close the game and remove other localization patches first.
"%~dp0mh2loc.exe" capture-originals --project "%~dp0project.json"
set "result=%errorlevel%"
if not defined MH2LOC_NO_PAUSE pause
exit /b %result%
