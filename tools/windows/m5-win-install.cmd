@echo off
rem m5-win-install: M5.2 steps (d)/(e) on the board's installed Windows (not
rem WinPE). Installs maliwddm as the display adapter for ACPI\RKCP7402, with
rem Mesa's d3d10umd (Panfrost) as its UMD, maliumd.dll. Then reboot and run
rem m5-win-check.cmd.
rem
rem RDP is turned on first: a full adapter that comes up wrong can leave the
rem screen black (2026-10-02, RDP still answered). If Windows does not come
rem back at all, boot WinPE from this stick and run m5-wddm-rescue.cmd.
rem
rem Only maliumd.dll goes into System32, plus the VC runtime if Windows has
rem none; d3d11, dxgi, DXCore and the HLSL compiler are Windows' own here,
rem and mali\tri\sys (copies taken for WinPE) is NOT copied over them.
rem
rem Run from an ADMINISTRATOR prompt. Output: m5-win-install-out.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
setlocal
set HERE=%~dp0
set OUT=%HERE%m5-win-install-out.txt
set DEV=ACPI\RKCP7402\0
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-win-install %date% %time% > "%OUT%"
ver >> "%OUT%"

echo == RDP on >> "%OUT%"
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f >> "%OUT%" 2>&1
netsh advfirewall firewall set rule group="remote desktop" new enable=Yes >> "%OUT%" 2>&1
ipconfig | find "IPv4" >> "%OUT%"

echo == testsigning >> "%OUT%"
bcdedit /enum {current} | find /i "testsigning" >> "%OUT%" 2>&1

echo == UMD >> "%OUT%"
copy /y "%HERE%tri\libgallium_d3d10.dll" %SystemRoot%\System32\maliumd.dll >> "%OUT%" 2>&1
if not exist %SystemRoot%\System32\vcruntime140.dll copy /y "%HERE%tri\sys\vcruntime140.dll" %SystemRoot%\System32\ >> "%OUT%" 2>&1
if not exist %SystemRoot%\System32\vcruntime140_1.dll copy /y "%HERE%tri\sys\vcruntime140_1.dll" %SystemRoot%\System32\ >> "%OUT%" 2>&1
dir %SystemRoot%\System32\maliumd.dll %SystemRoot%\System32\vcruntime140*.dll >> "%OUT%" 2>&1

echo == device before >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1

echo == installing maliwddm >> "%OUT%"
reg delete %KEY%\maliwddm /f >nul 2>&1
pnputil /add-driver "%HERE%maliwddm\maliwddm.inf" /install >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"
ping -n 11 127.0.0.1 >nul

echo == device after >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1
reg query %KEY%\maliwddm /v Build >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo Saved to %OUT%
echo Now REBOOT, then run m5-win-check.cmd from an administrator prompt.
pause
