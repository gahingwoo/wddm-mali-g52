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

rem XHC0 (USB-C, ACPI\PNP0D10\0) is disabled before anything else when
rem mali\no-xhc0 is on the stick. Every run so far ended in WHEA_INTERNAL_ERROR
rem (0x122: 9, 0x11 = a synchronous external abort): usbxhci read a port-2
rem PORTSC (+0x430) while UsbHub3 suspended a root port. XHC0's port 2 is
rem the SS port whose USBDP PHY the firmware never brings up; this run
rem tells it apart from XHC1's. The stick and keyboard are on XHC1.
if exist "%HERE%no-xhc0" (
  echo == disable XHC0 ^(USB-C^) >> "%OUT%"
  pnputil /disable-device "ACPI\PNP0D10\0" >> "%OUT%" 2>&1
  pnputil /enum-devices /instanceid "ACPI\PNP0D10\0" >> "%OUT%" 2>&1
  pnputil /enum-devices /instanceid "ACPI\PNP0D10\1" >> "%OUT%" 2>&1
)
ping -n 3 127.0.0.1 >nul
%SAVE% >nul

rem maliwddm names maliumd.dll (Mesa's d3d10umd with Panfrost) as its UMD,
rem and an adapter with no loadable UMD fails with code 43, so it goes into
rem System32 before the driver loads, with the D3D11 runtime, HLSL compiler
rem and VC runtime WinPE lacks (mali\tri\sys, from the installed Windows).
if exist "%HERE%tri\libgallium_d3d10.dll" copy /y "%HERE%tri\libgallium_d3d10.dll" %SystemRoot%\System32\maliumd.dll >> "%OUT%" 2>&1
if exist "%HERE%tri\sys" copy /y "%HERE%tri\sys\*.dll" %SystemRoot%\System32\ >> "%OUT%" 2>&1

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
rem Step (c), first half: M4's triangle through Panfrost and the shim's
rem maliwddm backend. WinPE has no D3D11 runtime, HLSL compiler or VC
rem runtime; mali\tri\sys carries them from the installed Windows.
rem (goto, not a block: %ERRORLEVEL% in a block is expanded too early.)
if not exist "%HERE%tri\triangle.exe" goto :no_triangle
%SAVE% >nul
echo == triangle on the Mali through maliwddm >> "%OUT%"
pushd X:\
set GALLIUM_DRIVER=panfrost
set MALIKM_TRACE=1
"%HERE%tri\triangle.exe" "%HERE%tri\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo triangle exit code %ERRORLEVEL% >> "%OUT%"
set MALIKM_TRACE=
set GALLIUM_DRIVER=
if exist X:\triangle.ppm copy /y X:\triangle.ppm "%HERE%triangle-wddm.ppm" >nul
if exist X:\triangle.ppm del X:\triangle.ppm
%SAVE% >nul
echo == triangle as a HARDWARE device: the runtime loads maliumd.dll >> "%OUT%"
set GALLIUM_DRIVER=panfrost
set MALIKM_TRACE=1
"%HERE%tri\triangle.exe" hw >> "%OUT%" 2>&1
echo triangle hw exit code %ERRORLEVEL% >> "%OUT%"
set MALIKM_TRACE=
set GALLIUM_DRIVER=
if exist X:\triangle.ppm copy /y X:\triangle.ppm "%HERE%triangle-hw.ppm" >nul
%SAVE% >nul
if not exist "%HERE%tri\present.exe" goto :no_present
echo == present.exe as a hardware device (DXGI present through maliwddm) >> "%OUT%"
set GALLIUM_DRIVER=panfrost
set MALIKM_TRACE=1
"%HERE%tri\present.exe" hw >> "%OUT%" 2>&1
echo present hw exit code %ERRORLEVEL% >> "%OUT%"
set MALIKM_TRACE=
set GALLIUM_DRIVER=
:no_present
popd
echo == the driver's counters after the triangle (m5test again) >> "%OUT%"
"%HERE%m5test.exe" >> "%OUT%" 2>&1
:no_triangle
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
