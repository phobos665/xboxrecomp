@echo off
rem TimeSplitters 2 -- run the Release build with the switches it needs.
rem
rem Build first:
rem   cmake -S titles\timesplitters2 -B titles\timesplitters2\build -G "Visual Studio 16 2019" -A x64
rem   cmake --build titles\timesplitters2\build --config Release
rem
rem The picture is in the second window, titled as the shadow renderer; the
rem first window is the title's own frame buffer, which nothing draws into.
rem Keyboard, unless it has been rebound: arrows = D-pad, Enter = START,
rem Backspace = BACK, Z = A, X = B, A = X, S = Y, Q = White, W = Black,
rem E = left trigger, R = right trigger. An XInput controller on port 0 works
rem as itself, and pads in slots 1-3 become controllers 2-4.
rem
rem To rebind, or to pick which device drives which controller:
rem   py -3 -m tools.input_ui
rem It writes %APPDATA%\xboxrecomp\input_bindings.json, which every title
rem reads; see docs\technical\input-binding.md.
rem
rem The game saves under games\Time Splitters 2\UDATA\4553000a\. A profile
rem saved by an earlier run changes the menu path ("save exists, overwrite?"),
rem which is the console's behaviour too.
rem
rem Extra arguments are passed to the executable. Useful environment:
rem   RECOMP_FPS=5                      frame-rate line every 5 s on stderr
rem   RECOMP_INPUT_SEQ=12000:start,...  scripted presses (see input_host.c)
setlocal
cd /d "%~dp0build\Release" || exit /b 1
set RECOMP_VBLANK=1
set RECOMP_AC97_READY=1
set RECOMP_HLE_D3D8=shadow
set RECOMP_TRACE_BUDGET=0
timesplitters2_recomp.exe %*
