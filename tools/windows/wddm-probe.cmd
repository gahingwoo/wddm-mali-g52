@echo off
rem wddm-probe: the WDDM version probe (docs/WDDM2.md, phase P1).
rem
rem   wddm-probe set 2000     report WDDM 2.0 from the next boot (2100 = 2.1,
rem                           ... 2900, 3000, 3100 = 3.1). Also arms a boot-time
rem                           DxgKrnl ETW capture to C:\mali\probe\boot.etl.
rem   wddm-probe keep 2000    the same, without the one-shot fallback (only
rem                           for a version already seen to start)
rem   wddm-probe check        after the reboot: stops the capture and writes
rem                           C:\mali\probe\probe-<time>.txt with everything
rem                           the probe is judged by
rem   wddm-probe off          back to WDDM 1.3 from the next boot; removes the
rem                           boot-time capture
rem
rem The driver reads the setting in DriverEntry, so each change needs a
rem reboot. A version dxgkrnl refuses falls back to 1.3 by itself on the boot
rem after the failed one (version.c); "check" then shows WddmProbeFellBack.
rem If the screen stays black, RDP still answers, or power off, boot again,
rem and the fallback takes it back to 1.3.
rem
rem Run from an ADMINISTRATOR prompt. etwrec.exe next to this script is used
rem to stop the capture if present, logman otherwise.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
setlocal enableextensions
set HERE=%~dp0
set PARAMS=HKLM\SYSTEM\CurrentControlSet\Services\maliwddm\Parameters
set DEVKEY="HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP7402\0\Device Parameters\maliwddm"
set DEV=ACPI\RKCP7402\0
set SESSION=wddm-mali-g52-dxgkrnl
set AUTO=HKLM\SYSTEM\CurrentControlSet\Control\WMI\Autologger\%SESSION%
set DIR=C:\mali\probe
if not exist "%DIR%" mkdir "%DIR%"

if /i "%~1"=="set" goto set
if /i "%~1"=="keep" goto set
if /i "%~1"=="check" goto check
if /i "%~1"=="off" goto off
echo usage: wddm-probe set ^<2000^|2100^|...^|3100^> ^| keep ^<v^> ^| check ^| off
exit /b 1

:set
if "%~2"=="" (echo which version? e.g. wddm-probe set 2000 & exit /b 1)
reg add %PARAMS% /v WddmProbe /t REG_DWORD /d 0x%~2 /f >nul || exit /b 1
for %%v in (WddmProbeArmed WddmProbeConfirmed WddmProbeFellBack WddmProbeUnknown WddmProbeKeep) do reg delete %PARAMS% /v %%v /f >nul 2>&1
if /i "%~1"=="keep" reg add %PARAMS% /v WddmProbeKeep /t REG_DWORD /d 1 /f >nul
rem The previous run's trace values, so that this run's are its own.
reg delete %DEVKEY% /f >nul 2>&1
rem Boot-time DxgKrnl capture: StartDevice's ETW event 494 ("Adapter
rem StartDevice has completed with status ...") and the caps events happen
rem long before anyone can log on.
reg add %AUTO% /v Start /t REG_DWORD /d 1 /f >nul
reg add %AUTO% /v Guid /t REG_SZ /d {6b0f6c52-9b0e-4c49-a7a1-9d3b2f9c5a10} /f >nul
reg add %AUTO% /v FileName /t REG_SZ /d %DIR%\boot.etl /f >nul
reg add %AUTO% /v BufferSize /t REG_DWORD /d 1024 /f >nul
reg add %AUTO% /v MinimumBuffers /t REG_DWORD /d 64 /f >nul
reg add %AUTO% /v MaximumBuffers /t REG_DWORD /d 128 /f >nul
reg add %AUTO% /v ClockType /t REG_DWORD /d 1 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v Enabled /t REG_DWORD /d 1 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v EnableLevel /t REG_DWORD /d 5 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v MatchAnyKeyword /t REG_QWORD /d 0xFFFFFFFFFFFFFFFF /f >nul
if exist "%DIR%\boot.etl" del /q "%DIR%\boot.etl"
reg query %PARAMS%
echo.
echo WddmProbe 0x%~2 is set. REBOOT now, then run: wddm-probe check
exit /b 0

:off
reg delete %PARAMS% /v WddmProbe /f >nul 2>&1
reg add %PARAMS% /v WddmProbeArmed /t REG_DWORD /d 0 /f >nul
reg delete %PARAMS% /v WddmProbeKeep /f >nul 2>&1
reg delete %AUTO% /f >nul 2>&1
echo WDDM 1.3 from the next boot. REBOOT to apply.
exit /b 0

:check
for /f %%s in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set STAMP=%%s
set OUT=%DIR%\probe-%STAMP%.txt
echo wddm-probe check %date% %time% > "%OUT%"
ver >> "%OUT%"

echo == stop the boot-time capture >> "%OUT%"
if exist "%HERE%etwrec.exe" (
  "%HERE%etwrec.exe" stop >> "%OUT%" 2>&1
) else (
  logman stop %SESSION% -ets >> "%OUT%" 2>&1
)
if exist "%DIR%\boot.etl" copy /y "%DIR%\boot.etl" "%DIR%\boot-%STAMP%.etl" >nul
dir "%DIR%\boot*.etl" >> "%OUT%" 2>&1

echo == 1. what the driver decided (service Parameters) >> "%OUT%"
reg query %PARAMS% >> "%OUT%" 2>&1
echo   WddmProbeConfirmed = this version reached 300 Render calls (DWM drawing) >> "%OUT%"
echo   WddmProbeFellBack  = this version was refused or hung; now on 1.3 >> "%OUT%"

echo == 2. what the adapter reported at StartDevice (Device Parameters\maliwddm) >> "%OUT%"
reg query %DEVKEY% | findstr /i "Build WddmVersion DdiInterface WddmProbe Started GpuUp LastFailedDdi Fail_ QAI_ Seen_CreateDevice Seen_Render" >> "%OUT%" 2>&1

echo == 3. device state (Problem code 43 = refused) >> "%OUT%"
pnputil /enum-devices /instanceid "%DEV%" >> "%OUT%" 2>&1

echo == 4. driver model and feature levels as DirectX sees them >> "%OUT%"
dxdiag /t "%DIR%\dxdiag-%STAMP%.txt"
:waitdx
if not exist "%DIR%\dxdiag-%STAMP%.txt" (ping -n 2 127.0.0.1 >nul & goto waitdx)
ping -n 3 127.0.0.1 >nul
findstr /i /c:"Card name" /c:"Driver Model" /c:"Feature Levels" /c:"DDI Version" /c:"Driver Version" /c:"Hardware Scheduling" "%DIR%\dxdiag-%STAMP%.txt" >> "%OUT%" 2>&1

echo == 5. is the UMD loaded in dwm.exe >> "%OUT%"
powershell -NoProfile -Command "(Get-Process dwm).Modules | Where-Object ModuleName -like 'maliumd*' | Select-Object FileName | Format-Table -HideTableHeaders" >> "%OUT%" 2>&1

echo == 6. the counters Task Manager's GPU page reads (WDDM 2.0 and later) >> "%OUT%"
typeperf -q "GPU Engine" > "%DIR%\gpu-engine-%STAMP%.txt" 2>&1
find /c "engtype" "%DIR%\gpu-engine-%STAMP%.txt" >> "%OUT%" 2>&1
findstr /i "engtype_3D" "%DIR%\gpu-engine-%STAMP%.txt" > "%DIR%\gpu-3d-%STAMP%.txt"
for /f %%n in ('type "%DIR%\gpu-3d-%STAMP%.txt" ^| find /c /v ""') do echo   3D engine instances: %%n >> "%OUT%"
typeperf "\GPU Engine(*engtype_3D)\Utilization Percentage" -sc 3 >> "%OUT%" 2>&1
typeperf "\GPU Adapter Memory(*)\Shared Usage" "\GPU Adapter Memory(*)\Dedicated Usage" -sc 1 >> "%OUT%" 2>&1

echo == 7. still to check by eye >> "%OUT%"
echo   - Task Manager, Performance tab: is there a GPU entry, named after the adapter, >> "%OUT%"
echo     with a 3D graph that moves when a window is dragged? Photo. >> "%OUT%"
echo   - Task Manager, Details tab, add the GPU and GPU engine columns: dwm.exe on "GPU 0 - 3D"? >> "%OUT%"
echo   - desktop: Start menu, File Explorer, a window drag >> "%OUT%"
echo   - Edge: opens, scrolls a page, edge://gpu shows Direct3D11 on the Mali (photo or save the page) >> "%OUT%"

type "%OUT%"
echo.
echo Saved to %OUT%
echo The boot-time capture stays armed for the next boot; "wddm-probe off" removes it.
echo Copy %DIR% off the board; decode boot-%STAMP%.etl with tools\etw\dxgk-decode.py.
exit /b 0
