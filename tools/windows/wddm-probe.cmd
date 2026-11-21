@echo off
rem wddm-probe: the WDDM version probe (docs/WDDM2.md, phase P1).
rem
rem   wddm-probe set 2000     report WDDM 2.0 from the next boot (2100 = 2.1,
rem                           ... 2900, 3000, 3100 = 3.1). Also arms a boot-time
rem                           DxgKrnl ETW capture to C:\mali\probe\boot.etl.
rem   wddm-probe set 2000 uart  the same, and the driver's watchdog also prints
rem                           one line a second on the debug UART (1500000 8n1)
rem   wddm-probe keep 2000    the same, without the one-shot fallback (only
rem                           for a version already seen to start)
rem   wddm-probe watch        on 1.3: run the driver's watchdog anyway (a
rem                           baseline for the Probe1300 key); "off" ends it
rem   wddm-probe check        after the reboot: stops the capture and writes
rem                           C:\mali\probe\probe-<time>.txt with everything
rem                           the probe is judged by
rem   wddm-probe off          back to WDDM 1.3 from the next boot; removes the
rem                           boot-time capture
rem
rem The driver reads the setting in DriverEntry, so each change needs a FULL
rem REBOOT. pnputil /restart-device keeps the loaded driver image and is not
rem a test of anything here. A version dxgkrnl refuses falls back to 1.3 by itself on the boot
rem after the failed one (version.c); "check" then shows WddmProbeFellBack.
rem If the screen stays black, RDP still answers, or power off, boot again,
rem and the fallback takes it back to 1.3.
rem
rem A hang leaves its breadcrumbs in Device Parameters\maliwddm\Probe<version>,
rem which the 1.3 fallback boot does not write (probe.c): per-DDI counts, the
rem last DDI entered, "still in <DDI> after N ms", and the paging operations
rem refused. "check" prints every Probe* key it finds.
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
if /i "%~1"=="watch" goto watch
if /i "%~1"=="off" goto off
echo usage: wddm-probe set ^<2000^|2100^|...^|3100^> [uart] ^| keep ^<v^> [uart] ^| watch ^| check ^| off
exit /b 1

:set
if "%~2"=="" (echo which version? e.g. wddm-probe set 2000 & exit /b 1)
reg add %PARAMS% /v WddmProbe /t REG_DWORD /d 0x%~2 /f >nul || exit /b 1
for %%v in (WddmProbeArmed WddmProbeConfirmed WddmProbeFellBack WddmProbeUnknown WddmProbeKeep ProbeUart ProbeWatchdog) do reg delete %PARAMS% /v %%v /f >nul 2>&1
if /i "%~1"=="keep" reg add %PARAMS% /v WddmProbeKeep /t REG_DWORD /d 1 /f >nul
if /i "%~3"=="uart" reg add %PARAMS% /v ProbeUart /t REG_DWORD /d 1 /f >nul
rem The previous run's trace values, so that this run's are its own. The
rem Probe* keys of earlier runs are saved to a file first.
reg query %DEVKEY% /s > "%DIR%\devkey-before-set.txt" 2>&1
reg delete %DEVKEY% /f >nul 2>&1
call :autologger
reg query %PARAMS%
echo.
echo WddmProbe 0x%~2 is set. Do a full REBOOT now (not pnputil /restart-device),
echo then run: wddm-probe check
exit /b 0

:watch
reg add %PARAMS% /v ProbeWatchdog /t REG_DWORD /d 1 /f >nul
if /i "%~2"=="uart" reg add %PARAMS% /v ProbeUart /t REG_DWORD /d 1 /f >nul
call :autologger
echo The watchdog runs on 1.3 from the next boot (key Probe1300). REBOOT, then: wddm-probe check
exit /b 0

rem Boot-time DxgKrnl capture: StartDevice's ETW event 494 ("Adapter
rem StartDevice has completed with status ...") and the caps events happen
rem long before anyone can log on. FileMax keeps one numbered file per boot
rem (boot.etl, boot.etl.001, ...) so the fallback boot does not overwrite the
rem failed one, and FlushTimer writes the buffers out every second, so a
rem power cut loses at most the last second.
:autologger
reg delete %AUTO% /f >nul 2>&1
reg add %AUTO% /v Start /t REG_DWORD /d 1 /f >nul
reg add %AUTO% /v Guid /t REG_SZ /d {6b0f6c52-9b0e-4c49-a7a1-9d3b2f9c5a10} /f >nul
reg add %AUTO% /v FileName /t REG_SZ /d %DIR%\boot.etl /f >nul
reg add %AUTO% /v FileMax /t REG_DWORD /d 8 /f >nul
reg add %AUTO% /v FlushTimer /t REG_DWORD /d 1 /f >nul
reg add %AUTO% /v BufferSize /t REG_DWORD /d 64 /f >nul
reg add %AUTO% /v MinimumBuffers /t REG_DWORD /d 16 /f >nul
reg add %AUTO% /v MaximumBuffers /t REG_DWORD /d 128 /f >nul
reg add %AUTO% /v ClockType /t REG_DWORD /d 1 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v Enabled /t REG_DWORD /d 1 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v EnableLevel /t REG_DWORD /d 5 /f >nul
reg add %AUTO%\{802EC45A-1E99-4B83-9920-87C98277BA9D} /v MatchAnyKeyword /t REG_QWORD /d 0xFFFFFFFFFFFFFFFF /f >nul
for %%f in ("%DIR%\boot.etl*") do move /y "%%f" "%DIR%\old-%%~nxf" >nul 2>&1
exit /b 0

:off
reg delete %PARAMS% /v WddmProbe /f >nul 2>&1
reg add %PARAMS% /v WddmProbeArmed /t REG_DWORD /d 0 /f >nul
reg delete %PARAMS% /v WddmProbeKeep /f >nul 2>&1
reg delete %PARAMS% /v ProbeWatchdog /f >nul 2>&1
reg delete %PARAMS% /v ProbeUart /f >nul 2>&1
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
rem One file per boot: the newest is this boot, the one before it the boot
rem that hung (if one did). Keep them all.
mkdir "%DIR%\etl-%STAMP%" >nul 2>&1
copy /y "%DIR%\boot.etl*" "%DIR%\etl-%STAMP%\" >nul 2>&1
dir /od "%DIR%\etl-%STAMP%" >> "%OUT%" 2>&1

echo == 1. what the driver decided (service Parameters) >> "%OUT%"
reg query %PARAMS% >> "%OUT%" 2>&1
echo   WddmProbeConfirmed = this version reached 300 Render calls (DWM drawing) >> "%OUT%"
echo   WddmProbeFellBack  = this version was refused or hung; now on 1.3 >> "%OUT%"

echo == 2. what the adapter reported at StartDevice, this boot (Device Parameters\maliwddm) >> "%OUT%"
reg query %DEVKEY% | findstr /i "Build WddmVersion DdiInterface WddmProbe Started GpuUp LastFailedDdi Fail_ QAI_ Seen_CreateDevice Seen_Render AdapterLuid" >> "%OUT%" 2>&1
reg query %DEVKEY% /s > "%DIR%\devkey-%STAMP%.txt" 2>&1

echo == 2b. the probe's breadcrumbs, every Probe* key (survive the fallback boot) >> "%OUT%"
echo   Wd_Stuck / Wd_WorstStuck = a DDI still in progress after 2 s >> "%OUT%"
echo   Wd_Last = the last DDI entered; Cnt_^<ddi^> = calls; PgRefused_^<op^> = paging ops refused >> "%OUT%"
for /f "delims=" %%k in ('reg query %DEVKEY% 2^>nul ^| findstr /i /r "\\Probe[0-9a-f]*$"') do reg query "%%k" >> "%OUT%" 2>&1

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

echo == 6. the counters Task Manager's GPU page reads >> "%OUT%"
echo   These exist on 1.3 too (seen 2026-11-22), so their presence is not the test: >> "%OUT%"
echo   which LUID they name, how many engines, and whether their 3D use moves. >> "%OUT%"
echo   AdapterLuidLow in section 2 is this adapter's LUID. >> "%OUT%"
powershell -NoProfile -Command "$p = (Get-Counter -ListSet 'GPU Engine').PathsWithInstances | Where-Object { $_ -like '*Utilization Percentage' }; 'GPU Engine instances: ' + $p.Count; $p | ForEach-Object { if ($_ -match 'luid_(0x[0-9A-Fa-f]+_0x[0-9A-Fa-f]+)_phys_\d+_eng_(\d+)_engtype_([A-Za-z0-9]+)') { $Matches[1] + ' eng_' + $Matches[2] + ' ' + $Matches[3] } } | Group-Object | Sort-Object Name | ForEach-Object { '  ' + $_.Name + '  x' + $_.Count + ' processes' }" >> "%OUT%" 2>&1
echo   Drag a window now: 5 samples of 3D use per adapter follow. >> "%OUT%"
echo DRAG A WINDOW AROUND FOR THE NEXT 10 SECONDS (3D use is being sampled)
powershell -NoProfile -Command "1..5 | ForEach-Object { $s = (Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage').CounterSamples; $s | Group-Object { if ($_.InstanceName -match 'luid_(0x[0-9a-f]+_0x[0-9a-f]+)') { $Matches[1] } } | ForEach-Object { '  ' + $_.Name + ' 3D ' + [math]::Round(($_.Group | Measure-Object CookedValue -Sum).Sum, 1) + ' %%' }; Start-Sleep 1 }" >> "%OUT%" 2>&1
powershell -NoProfile -Command "(Get-Counter '\GPU Adapter Memory(*)\Shared Usage','\GPU Adapter Memory(*)\Dedicated Usage').CounterSamples | ForEach-Object { '  ' + $_.Path + ' = ' + $_.CookedValue }" >> "%OUT%" 2>&1

echo == 7. still to check by eye >> "%OUT%"
echo   - Task Manager, Performance tab (also on the 1.3 baseline): a GPU entry named after the adapter? >> "%OUT%"
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
