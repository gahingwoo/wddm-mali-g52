@echo off
rem m3-mesa: with malikm already installed, run the Mesa tests again with
rem every driver call logged (MALIKM_TRACE=1). Output: m3-mesa-out.txt here.
set HERE=%~dp0
set OUT=%HERE%m3-mesa-out.txt
set KEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters"
echo m3-mesa %date% %time% > "%OUT%"
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

set GALLIUM_DRIVER=panfrost
set MALIKM_TRACE=1
echo == triangle, panfrost, traced >> "%OUT%"
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"

set MALIKM_TRACE=
set PAN_MESA_DEBUG=sync
echo == triangle, panfrost, PAN_MESA_DEBUG=sync >> "%OUT%"
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
set PAN_MESA_DEBUG=

echo == present, panfrost >> "%OUT%"
"%HERE%mesa\present.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
set GALLIUM_DRIVER=

echo == driver registry values >> "%OUT%"
reg query %KEY% >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
pause
