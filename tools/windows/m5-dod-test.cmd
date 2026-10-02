@echo off
rem m5-dod-test: replace malikm (or m1probe) with malidod, the display-only
rem driver, and record what it reports. Run from an ADMINISTRATOR prompt.
rem Output: m5-dod-out.txt here. Run AFTER m3-test.cmd is done.
rem
rem If the screen goes black: wait 30 s, then reboot; the driver is demand
rem start, so remove it from WinPE or safe mode (pnputil /delete-driver).
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-dod-out.txt
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-dod-test %date% %time% > "%OUT%"

echo == display adapters before >> "%OUT%"
pnputil /enum-devices /class Display >> "%OUT%" 2>&1

echo == removing malikm and m1probe, if installed >> "%OUT%"
powershell -NoProfile -Command ^
  "$d = pnputil /enum-drivers | Out-String; " ^
  "foreach ($b in ($d -split '\r?\n\r?\n')) { if ($b -match 'Original Name:\s+(m1probe|malikm)\.inf' -and $b -match 'Published Name:\s+(oem\d+\.inf)') { pnputil /delete-driver $Matches[1] /uninstall /force } }" >> "%OUT%" 2>&1

echo == installing malidod >> "%OUT%"
echo Installing the display driver. If Windows asks about the publisher, choose Install anyway.
pnputil /add-driver "%HERE%malidod\malidod.inf" /install >> "%OUT%" 2>&1
timeout /t 10 /nobreak >nul

echo == device after install >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
echo == display adapters after >> "%OUT%"
pnputil /enum-devices /class Display >> "%OUT%" 2>&1
echo == driver registry values >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo Move a window around for a few seconds, then run:
echo   reg query %KEY%
echo Presents should have grown. Results saved to %OUT%
pause
