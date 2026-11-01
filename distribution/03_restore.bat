@echo off
setlocal DisableDelayedExpansion
echo Close Manhunt 2 before restoring.
"%~dp0mh2loc.exe" restore --project "%~dp0project.json"
set "result=%errorlevel%"
if not defined MH2LOC_NO_PAUSE pause
exit /b %result%
