@echo off
rem m5-pair-test: the display-only + render-only pair. Needs firmware with
rem DSP0 (ACPI\RKCP7403, edk2-rk3576 cm5io-mali-m1). Installs malidod on
rem DSP0 (the screen) and maliwddm on GPU0 (the Mali, render-only), then
rem asks for a reboot; after it, run m5-wddm-check.cmd.
rem Output: m5-pair-out.txt here. Run from an ADMINISTRATOR prompt.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-pair-out.txt
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-pair-test %date% %time% > "%OUT%"
echo == devices before >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7403\0" >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
echo == installing malidod (display) >> "%OUT%"
pnputil /add-driver "%HERE%malidod\malidod.inf" /install >> "%OUT%" 2>&1
echo == installing maliwddm (render-only) >> "%OUT%"
reg delete %KEY%\maliwddm /f >nul 2>&1
pnputil /add-driver "%HERE%maliwddm\maliwddm.inf" /install >> "%OUT%" 2>&1
echo == devices after >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7403\0" >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
echo REBOOT now, then run m5-wddm-check.cmd.
pause
