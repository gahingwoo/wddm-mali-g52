@echo off
rem m5-pe-test: M5.2 in WinPE, booted from this stick. Nothing persists, so
rem a driver that misbehaves costs a reboot, not an installed Windows.
rem Loads malidod (display-only, DSP0 = ACPI\RKCP7403) and then maliwddm
rem (render-only, GPU0 = ACPI\RKCP7402), runs m5test, and saves the device
rem state and both drivers' traces. Needs firmware with DSP0 and testsigning
rem on in this stick's BCD. Output: m5-pe-out.txt here.
setlocal
set HERE=%~dp0
rem Everything is written to the RAM disk first and copied to the stick at
rem the end: PE run 3 was reset while the stick was being written, and FAT
rem cross-linked the output file with woa-debug's (its text was replaced by
rem a registry dump, and the .etl never appeared).
set OUT=X:\m5-pe-out.txt
rem A copy goes to the stick after each step too: PE run 7 bugchecked
rem mid-test (WHEA_INTERNAL_ERROR) and the RAM disk took everything with it.
set SAVE=copy /y X:\m5-pe-out.txt "%HERE%m5-pe-out.txt"
set KEY=HKLM\SYSTEM\CurrentControlSet\Enum\ACPI
echo m5-pe-test %date% %time% > "%OUT%"
echo == what this WinPE has >> "%OUT%"
rem WinPE has no findstr, timeout or choice: a pipe into a missing command
rem ended the whole batch chain on the first run. Waits use ping.
for %%f in (drivers\dxgkrnl.sys drivers\dxgmms2.sys d3d10warp.dll d3d11.dll dxgi.dll gdi32.dll) do if exist %SystemRoot%\System32\%%f (echo   have %%f >> "%OUT%") else (echo   MISSING %%f >> "%OUT%")
dir /b /s %SystemRoot%\System32\DriverStore\FileRepository\BasicDisplay.sys %SystemRoot%\System32\DriverStore\FileRepository\BasicRender.sys >> "%OUT%" 2>&1
bcdedit /enum {current} >> "%OUT%" 2>&1
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

rem dxgkrnl's own ETW events, around the drvloads. WPR fails to start in
rem this WinPE (0x80070002), so etwrec (tools/windows/etwrec.c) runs it.
echo == etwrec start (dxgkrnl ETW) >> "%OUT%"
"%HERE%etwrec.exe" start X:\dxgkrnl.etl >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"

%SAVE% >nul
rem malidod only with a render-only maliwddm: a full maliwddm owns the screen
rem itself, and the two would compete for the POST framebuffer. The stick
rem says which: mali\with-malidod present = load it.
if exist "%HERE%with-malidod" (
  echo == drvload malidod >> "%OUT%"
  drvload "%HERE%malidod\malidod.inf" >> "%OUT%" 2>&1
  echo exit %ERRORLEVEL% >> "%OUT%"
) else (
  echo == malidod not loaded: maliwddm is a full adapter >> "%OUT%"
)
ping -n 6 127.0.0.1 >nul
%SAVE% >nul
echo == drvload maliwddm >> "%OUT%"
drvload "%HERE%maliwddm\maliwddm.inf" >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"
ping -n 11 127.0.0.1 >nul

echo == devices after >> "%OUT%"
pnputil /enum-devices /class Display >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7403\0" >> "%OUT%" 2>&1
pnputil /enum-devices /instanceid "ACPI\RKCP7402\0" >> "%OUT%" 2>&1

%SAVE% >nul
echo == m5test >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
echo m5test exit code %ERRORLEVEL% >> "%OUT%"
rem ETW keeps recording through m5test: its render and paging matter most.
%SAVE% >nul
echo == etwrec stop >> "%OUT%"
"%HERE%etwrec.exe" stop >> "%OUT%" 2>&1
echo exit %ERRORLEVEL% >> "%OUT%"
dir X:\dxgkrnl.etl >> "%OUT%" 2>&1


echo == maliwddm trace >> "%OUT%"
reg query "%KEY%\RKCP7402\0\Device Parameters\maliwddm" >> "%OUT%" 2>&1
echo == malidod trace >> "%OUT%"
reg query "%KEY%\RKCP7403\0\Device Parameters" >> "%OUT%" 2>&1
copy /y X:\m5-pe-out.txt "%HERE%m5-pe-out.txt" >nul
if exist X:\dxgkrnl.etl copy /y X:\dxgkrnl.etl "%HERE%dxgkrnl.etl" >nul
type "%OUT%"
echo.
echo Results saved to %HERE%m5-pe-out.txt
if /i not "%~1"=="auto" pause
