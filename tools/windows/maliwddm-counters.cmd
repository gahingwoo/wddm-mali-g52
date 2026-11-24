@echo off
rem maliwddm-counters: the "RK3576 GPU" performance counters (docs/PERFDATA.md,
rem section 6). maliwddm.sys provides them from the moment it loads, at WDDM
rem 1.3 and 2.x alike; Windows shows them only once the manifest is
rem installed, and no INF directive can do that, so this script runs lodctr
rem on the copy the INF put next to maliwddm.sys in the driver store.
rem
rem   maliwddm-counters install    lodctr /m on the installed driver's manifest
rem                                (again after every driver package update:
rem                                the driver store folder changes)
rem   maliwddm-counters uninstall  unlodctr by the provider GUID
rem   maliwddm-counters check      list the set and read each counter once
rem
rem Counter paths (PDH, English names):
rem   \RK3576 GPU(gpu0)\Temperature (C)
rem   \RK3576 GPU(gpu0)\Clock (MHz)
rem   \RK3576 GPU(gpu0)\%% busy
rem   \RK3576 GPU(gpu0)\Throttled
rem   \RK3576 GPU(gpu0)\Throttled reason   (0 none, 1 thermal, 2 faults)
rem
rem Run from an ADMINISTRATOR prompt.
setlocal enableextensions
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & exit /b 1)
set GUID={f5aa14ca-df31-4bf4-b28d-74063728197c}
set DIR=
for /f "usebackq delims=" %%d in (`powershell -NoProfile -Command "$p = (Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\maliwddm' -ErrorAction SilentlyContinue).ImagePath; if ($p) { $p = $p -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''; Split-Path $p }"`) do set DIR=%%d

if /i "%~1"=="install" goto install
if /i "%~1"=="uninstall" goto uninstall
if /i "%~1"=="check" goto check
echo usage: maliwddm-counters install ^| uninstall ^| check
exit /b 1

:install
if "%DIR%"=="" (echo maliwddm is not installed: no Services\maliwddm ImagePath. & exit /b 2)
if not exist "%DIR%\maliwddm.man" (echo no maliwddm.man in %DIR%: a package from before the counters? & exit /b 2)
echo driver store: %DIR%
unlodctr /g:%GUID% >nul 2>&1
lodctr /m:"%DIR%\maliwddm.man"
if errorlevel 1 (echo lodctr failed: %errorlevel% & exit /b 3)
echo installed. "maliwddm-counters check" reads them.
exit /b 0

:uninstall
unlodctr /g:%GUID%
exit /b %errorlevel%

:check
echo driver store: %DIR%
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Perflib\_V2Providers\%GUID%" /s 2>nul | findstr /i "ApplicationIdentity ProviderName"
typeperf -qx "RK3576 GPU"
typeperf "\RK3576 GPU(gpu0)\Temperature (C)" "\RK3576 GPU(gpu0)\Clock (MHz)" "\RK3576 GPU(gpu0)\%% busy" "\RK3576 GPU(gpu0)\Throttled" "\RK3576 GPU(gpu0)\Throttled reason" -sc 3
exit /b %errorlevel%
