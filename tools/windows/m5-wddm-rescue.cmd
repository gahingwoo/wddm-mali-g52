@echo off
rem m5-wddm-rescue: run from WinPE (boot this stick) if Windows comes up
rem black after installing maliwddm. It disables the maliwddm service in the
rem installed Windows, so the next boot falls back to Basic Display.
rem Undo: set Start back to 3, or reinstall with m5-wddm-test.cmd.
set WIN=
rem The driver lives in the DriverStore (DIRID 13), not System32\drivers:
rem find each installed Windows by its SYSTEM hive and look for the service.
for %%d in (C D E F G H I J K L M) do if exist %%d:\Windows\System32\config\SYSTEM call :try %%d:
if "%WIN%"=="" (echo No installed Windows with a maliwddm service found on C: to M:. & pause & exit /b 1)
echo maliwddm disabled. Reboot into Windows; it will use Basic Display.
pause
exit /b 0

:try
reg load HKLM\OFFSYS %1\Windows\System32\config\SYSTEM >nul 2>&1 || exit /b 0
reg query HKLM\OFFSYS\ControlSet001\Services\maliwddm /v Start >nul 2>&1
if errorlevel 1 (reg unload HKLM\OFFSYS >nul & exit /b 0)
echo Windows with a maliwddm service found on %1
for %%c in (ControlSet001 ControlSet002) do reg add HKLM\OFFSYS\%%c\Services\maliwddm /v Start /t REG_DWORD /d 4 /f >nul 2>&1
reg query HKLM\OFFSYS\ControlSet001\Services\maliwddm /v Start
reg unload HKLM\OFFSYS
set WIN=%1
exit /b 0
