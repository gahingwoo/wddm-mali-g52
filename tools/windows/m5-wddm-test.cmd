@echo off
rem m5-wddm-test: M5.2 steps (a)/(b) on the board. Replaces malidod (or
rem whatever drives ACPI\RKCP7402) with maliwddm from this stick, restarts
rem the device, runs m5test (one WRITE_VALUE job through D3DKMT) and saves
rem the driver's trace. Output: m5-wddm-out.txt here.
rem
rem When an older maliwddm is already loaded, restart-device keeps the OLD
rem image running: reboot afterwards and run m5-wddm-check.cmd.
rem
rem Run from an ADMINISTRATOR prompt, ideally over RDP: maliwddm has no
rem user-mode driver yet, and what the desktop does without one is one of
rem the things this run measures. If Windows comes up black and RDP does
rem not work either, boot WinPE from this stick and run m5-wddm-rescue.cmd.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-wddm-out.txt
set DEV=ACPI\RKCP7402\0
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-wddm-test %date% %time% > "%OUT%"

echo == removing malidod from the driver store >> "%OUT%"
powershell -NoProfile -Command "Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -like '*\malidod.inf' } | ForEach-Object { pnputil /delete-driver $_.Driver /uninstall /force }" >> "%OUT%" 2>&1

echo == installing maliwddm >> "%OUT%"
reg delete %KEY%\maliwddm /f >nul 2>&1
pnputil /add-driver "%HERE%maliwddm\maliwddm.inf" /install >> "%OUT%" 2>&1
pnputil /enable-device "%DEV%" >> "%OUT%" 2>&1
pnputil /restart-device "%DEV%" >> "%OUT%" 2>&1
timeout /t 10 /nobreak >nul

echo == device >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1

echo == m5test >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
echo m5test exit code %ERRORLEVEL% >> "%OUT%"

echo == driver trace >> "%OUT%"
reg query %KEY%\maliwddm >> "%OUT%" 2>&1
echo == display adapters >> "%OUT%"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,Status,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution" >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
echo.
echo If maliwddm was installed before, REBOOT now and run m5-wddm-check.cmd.
pause
