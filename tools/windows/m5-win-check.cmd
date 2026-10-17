@echo off
rem m5-win-check: after m5-win-install and a reboot. Is maliwddm the display
rem adapter, is DWM presenting through it (the KMD's Presents counter in
rem m5test's stats), and do the D3D11 triangle and a swap chain work on the
rem desktop. Output: m5-win-check-out.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
setlocal
set HERE=%~dp0
set OUT=%HERE%m5-win-check-out.txt
set DEV=ACPI\RKCP7402\0
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m5-win-check %date% %time% > "%OUT%"

echo == device >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1
reg query %KEY%\maliwddm /v Build >> "%OUT%" 2>&1
echo == display adapters >> "%OUT%"
powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,Status,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate" >> "%OUT%" 2>&1

echo == m5test (stats: Presents counts what DWM handed the KMD) >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
echo m5test exit code %ERRORLEVEL% >> "%OUT%"
ping -n 6 127.0.0.1 >nul
echo == m5test again, 5 s later: did Presents move on its own? >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1

echo == triangle hw >> "%OUT%"
pushd "%TEMP%"
set GALLIUM_DRIVER=panfrost
"%HERE%tri\triangle.exe" hw >> "%OUT%" 2>&1
echo triangle hw exit code %ERRORLEVEL% >> "%OUT%"
echo == present hw >> "%OUT%"
"%HERE%tri\present.exe" hw >> "%OUT%" 2>&1
echo present hw exit code %ERRORLEVEL% >> "%OUT%"
set GALLIUM_DRIVER=
popd
echo == stats after >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1

echo == driver trace >> "%OUT%"
reg query %KEY%\maliwddm >> "%OUT%" 2>&1
echo == recent display-related system events >> "%OUT%"
powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName='System'; StartTime=(Get-Date).AddHours(-1)} -ErrorAction SilentlyContinue | Where-Object { $_.ProviderName -match 'Display|dxg|Kernel-PnP|WHEA' } | Select-Object -First 30 TimeCreated,ProviderName,Id,Message | Format-List" >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo Saved to %OUT%
pause
