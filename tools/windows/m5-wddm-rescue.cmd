@echo off
rem m5-wddm-rescue: run from WinPE (boot this stick) if Windows comes up
rem black after installing maliwddm. It disables the maliwddm service in the
rem installed Windows, so the next boot falls back to Basic Display.
rem Undo: set Start back to 3, or reinstall with m5-wddm-test.cmd.
set WIN=
for %%d in (C D E F G H I J K) do if exist %%d:\Windows\System32\config\SYSTEM if exist %%d:\Windows\System32\drivers\maliwddm.sys set WIN=%%d:
if "%WIN%"=="" (echo No Windows with maliwddm.sys found on C: to K:. & pause & exit /b 1)
echo Windows with maliwddm found on %WIN%
reg load HKLM\OFFSYS %WIN%\Windows\System32\config\SYSTEM || (pause & exit /b 1)
for %%c in (ControlSet001 ControlSet002) do reg add HKLM\OFFSYS\%%c\Services\maliwddm /v Start /t REG_DWORD /d 4 /f
reg unload HKLM\OFFSYS
echo maliwddm disabled. Reboot into Windows; it will use Basic Display.
pause
