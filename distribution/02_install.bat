@echo off
setlocal DisableDelayedExpansion
echo Close Manhunt 2 before installing.
"%~dp0mh2loc.exe" install --project "%~dp0project.json"
set "result=%errorlevel%"
if not defined MH2LOC_NO_PAUSE pause
exit /b %result%
