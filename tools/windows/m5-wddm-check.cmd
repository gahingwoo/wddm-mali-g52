@echo off
rem m5-wddm-check: run AFTER a reboot that followed m5-wddm-test.cmd (a
rem restart-device keeps the old maliwddm image loaded; setupapi says
rem "restart required"). Runs m5test and saves the device state and the
rem driver's trace, including Build (the CI run number of the image that
rem is actually running). Output: m5-wddm-check.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-wddm-check.txt
set DEV=ACPI\RKCP7402\0
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-wddm-check %date% %time% > "%OUT%"
echo == device >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1
echo == m5test >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
echo m5test exit code %ERRORLEVEL% >> "%OUT%"
echo == driver trace >> "%OUT%"
reg query %KEY%\maliwddm >> "%OUT%" 2>&1
echo == UMD names: ours and Basic Render's >> "%OUT%"
powershell -NoProfile -Command "Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue | ForEach-Object { $p = Get-ItemProperty $_.PSPath; if ($p.UserModeDriverName -or $p.DriverDesc) { '{0}: {1} | UMD: {2}' -f $_.PSChildName, $p.DriverDesc, ($p.UserModeDriverName -join ',') } }" >> "%OUT%" 2>&1
echo == display adapters >> "%OUT%"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,Status,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution" >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
pause
