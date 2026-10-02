@echo off
rem m5-pe-test: M5.2 in WinPE, booted from this stick. Nothing persists, so
rem a driver that misbehaves costs a reboot, not an installed Windows.
rem Loads malidod (display-only, DSP0 = ACPI\RKCP7403) and then maliwddm
rem (render-only, GPU0 = ACPI\RKCP7402), runs m5test, and saves the device
rem state and both drivers' traces. Needs firmware with DSP0 and testsigning
rem on in this stick's BCD. Output: m5-pe-out.txt here.
setlocal
set HERE=%~dp0
set OUT=%HERE%m5-pe-out.txt
set KEY=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI
echo m5-pe-test %date% %time% > "%OUT%"
echo == what this WinPE has >> "%OUT%"
for %%f in (drivers\dxgkrnl.sys drivers\dxgmms2.sys drivers\BasicDisplay.sys drivers\BasicRender.sys d3d10warp.dll d3d11.dll dxgi.dll gdi32.dll) do if exist %SystemRoot%\System32\%%f (echo   have %%f >> "%OUT%") else (echo   MISSING %%f >> "%OUT%")
bcdedit /enum {current} 2>nul | findstr /i "testsigning" >> "%OUT%"
echo == devices before >> "%OUT%"
pnputil /enum-devices /class Display >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7403\0" >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

rem This WinPE has no WARP; maliwddm names d3d10warp.dll as its UMD, and an
rem adapter with no loadable UMD fails with code 43. The stick carries the
rem DLL from the installed Windows (same build, 22621); X: is writable.
if exist "%HERE%d3d10warp.dll" (
  copy /y "%HERE%d3d10warp.dll" %SystemRoot%\System32\ >> "%OUT%" 2>&1
) else (
  echo   no d3d10warp.dll on the stick: maliwddm will likely fail with code 43 >> "%OUT%"
)

echo == drvload malidod >> "%OUT%"
drvload "%HERE%malidod\malidod.inf" >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"
timeout /t 5 /nobreak >nul
echo == drvload maliwddm >> "%OUT%"
drvload "%HERE%maliwddm\maliwddm.inf" >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"
timeout /t 10 /nobreak >nul

echo == devices after >> "%OUT%"
pnputil /enum-devices /class Display >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7403\0" >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

echo == m5test >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
echo m5test exit code %ERRORLEVEL% >> "%OUT%"

echo == maliwddm trace >> "%OUT%"
reg query "%KEY%\RKCP7402\0\Device Parameters\maliwddm" >> "%OUT%" 2>&1
echo == malidod trace >> "%OUT%"
reg query "%KEY%\RKCP7403\0\Device Parameters" >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
if /i not "%~1"=="auto" pause
