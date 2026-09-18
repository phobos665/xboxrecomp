@echo off
rem Burnout 2 -- run the Release build with the switches it needs.
rem See titles\timesplitters2\run.bat for the build commands, the windows and
rem the keyboard mapping; they are the same here.
setlocal
cd /d "%~dp0build\Release" || exit /b 1
set RECOMP_VBLANK=1
set RECOMP_AC97_READY=1
set RECOMP_HLE_D3D8=shadow
set RECOMP_TRACE_BUDGET=0
burnout2_recomp.exe %*
