@echo off
rem m3-test: install malikm from this stick, run m3test (the driver alone),
rem then the D3D11 triangle through Mesa with GALLIUM_DRIVER=panfrost.
rem Run from an ADMINISTRATOR prompt. Output: m3-out.txt here.
rem
rem Expected layout next to this script:
rem   malikm\malikm.sys .inf .cat     (CI artifact malikm-arm64)
rem   m3test.exe                      (CI artifact m3test-arm64)
rem   mesa\libgallium_d3d10.dll, triangle.exe, present.exe  (artifact mesa-d3d10umd-panfrost-arm64)
rem   vc_redist.arm64.exe             (https://aka.ms/vs/17/release/vc_redist.arm64.exe)
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m3-out.txt
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m3-test %date% %time% > "%OUT%"

rem triangle.exe, present.exe and the Mesa DLL need the VC++ runtime.
if not exist "%SystemRoot%\System32\vcruntime140.dll" (
  echo Installing the VC++ runtime ^(vc_redist.arm64.exe^)...
  "%HERE%vc_redist.arm64.exe" /install /quiet /norestart
  echo vc_redist exit code %errorlevel% >> "%OUT%"
)

echo == GPU device before >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

rem m1probe binds the same device; take it off first.
echo == removing m1probe, if installed >> "%OUT%"
powershell -NoProfile -Command ^
  "$d = pnputil /enum-drivers | Out-String; " ^
  "foreach ($b in ($d -split '\r?\n\r?\n')) { if ($b -match 'Original Name:\s+m1probe\.inf' -and $b -match 'Published Name:\s+(oem\d+\.inf)') { pnputil /delete-driver $Matches[1] /uninstall /force } }" >> "%OUT%" 2>&1

echo == installing malikm >> "%OUT%"
echo Installing the driver. If Windows asks about the publisher, choose Install anyway.
pnputil /add-driver "%HERE%malikm\malikm.inf" /install >> "%OUT%" 2>&1
timeout /t 5 /nobreak >nul

echo == device after install >> "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1
echo == driver registry values >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1

echo == m3test >> "%OUT%"
"%HERE%m3test.exe" >> "%OUT%" 2>&1
echo m3test exit code %errorlevel% >> "%OUT%"

echo == triangle, softpipe >> "%OUT%"
set GALLIUM_DRIVER=
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"

echo == triangle, panfrost >> "%OUT%"
set GALLIUM_DRIVER=panfrost
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"

rem sync: wait for every job and report GPU faults.
echo == triangle, panfrost, PAN_MESA_DEBUG=sync >> "%OUT%"
set PAN_MESA_DEBUG=sync
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
set PAN_MESA_DEBUG=

rem A window and a swap chain: three cleared frames (red, green, blue) should
rem flash in a small window, through the GDI copy in mesa-patches/0014.
echo == present, panfrost >> "%OUT%"
"%HERE%mesa\present.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
set GALLIUM_DRIVER=

echo == driver registry values after >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1

type "%OUT%"
echo.
echo Results saved to %OUT%
