@echo off
rem m5-dod-update: with malidod already installed, install the newer build
rem from this stick, restart the device, and show what it reports.
rem Run from an ADMINISTRATOR prompt. Output: m5-dod-out.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-dod-out.txt
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-dod-update %date% %time% > "%OUT%"

echo == installing the new malidod >> "%OUT%"
pnputil /add-driver "%HERE%malidod\malidod.inf" /install >> "%OUT%" 2>&1
echo == restarting the device (the screen may flash) >> "%OUT%"
pnputil /restart-device "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
timeout /t 10 /nobreak >nul

echo == device >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
echo == driver registry values >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
pause
