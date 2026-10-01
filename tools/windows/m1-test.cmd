@echo off
rem m1-test: install m1probe from this stick and report whether the Mali-G52
rem ran its job. Run from an ADMINISTRATOR prompt. Output: m1-out.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
set OUT=%~dp0m1-out.txt
set INF=%~dp0m1probe\m1probe.inf
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m1-test %date% %time% > "%OUT%"

echo == GPU device from the firmware >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

echo == installing %INF% >> "%OUT%"
echo Installing the driver. If Windows asks about the publisher, choose Install anyway.
pnputil /add-driver "%INF%" /install >> "%OUT%" 2>&1
timeout /t 5 /nobreak >nul

echo == device after install >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

echo == results >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo Result 0x1 = the GPU ran the job. Results saved to %OUT%
