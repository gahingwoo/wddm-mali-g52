@echo off
rem m5-dod-rescue: run from WinPE (boot this stick) if Windows comes up
rem black after installing malidod. It disables the malidod service in the
rem installed Windows, so the next boot falls back to Basic Display.
rem Undo: set Start back to 3 (or reinstall with m5-dod-test.cmd).
set WIN=
for %%d in (C D E F G H I J K) do if exist %%d:\Windows\System32\config\SYSTEM if exist %%d:\Windows\System32\drivers\malidod.sys set WIN=%%d:
if "%WIN%"=="" (echo No Windows with malidod.sys found on C: to K:. & exit /b 1)
echo Windows with malidod found on %WIN%
reg load HKLM\OFFSYS %WIN%\Windows\System32\config\SYSTEM || exit /b 1
for %%c in (ControlSet001 ControlSet002) do reg add HKLM\OFFSYS\%%c\Services\malidod /v Start /t REG_DWORD /d 4 /f
reg unload HKLM\OFFSYS
echo malidod disabled. Reboot into Windows; it will use Basic Display.
