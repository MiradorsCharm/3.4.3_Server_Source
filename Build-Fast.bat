@echo off
rem Fast local build (Ninja + compiler cache + optional unity build).
rem
rem Passes every argument straight through to Build-Fast.ps1, e.g.
rem   Build-Fast.bat
rem   Build-Fast.bat -Unity -Cores 6
rem   Build-Fast.bat -Clean
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-Fast.ps1" %*
exit /b %ERRORLEVEL%
