@echo off
setlocal DisableDelayedExpansion
"%~dp0mh2loc.exe" validate --project "%~dp0project.json"
set "result=%errorlevel%"
if not defined MH2LOC_NO_PAUSE pause
exit /b %result%
