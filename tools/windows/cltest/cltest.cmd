@echo off
rem OpenCL on the Mali-G52 (docs/OPENCL.md, "Board test plan").
rem Run from the folder with cltest.exe and clinfo.exe, after the driver
rem package is installed. Writes cltest-<n>.txt beside this script.
rem   cltest.cmd          the whole plan, steps 1-6
rem   cltest.cmd quick    list and one small vecadd only
setlocal enabledelayedexpansion
set OUT=%~dp0cltest-%RANDOM%.txt
echo OpenCL board test > "%OUT%"
echo %DATE% %TIME% >> "%OUT%"
if not exist C:\mali\log mkdir C:\mali\log
reg query HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318} /s /f OpenCLDriverName >> "%OUT%" 2>&1
where OpenCL.dll >> "%OUT%" 2>&1

echo == 1. list >> "%OUT%"
"%~dp0cltest.exe" list >> "%OUT%" 2>&1
echo exit !ERRORLEVEL! >> "%OUT%"
if exist "%~dp0clinfo.exe" "%~dp0clinfo.exe" >> "%OUT%" 2>&1

echo == 2. vecadd 4096 >> "%OUT%"
"%~dp0cltest.exe" --expect Mali vecadd 4096 >> "%OUT%" 2>&1
echo exit !ERRORLEVEL! >> "%OUT%"
if /i "%1"=="quick" goto done

echo == 3. vecadd 16M >> "%OUT%"
"%~dp0cltest.exe" --expect Mali vecadd 16777216 >> "%OUT%" 2>&1
echo exit !ERRORLEVEL! >> "%OUT%"

echo == 4. sgemm 256, 512, 1024 >> "%OUT%"
for %%n in (256 512 1024) do (
  "%~dp0cltest.exe" --expect Mali sgemm %%n 5 >> "%OUT%" 2>&1
  echo exit !ERRORLEVEL! >> "%OUT%"
)

echo == 5. the same with PAN_MESA_DEBUG=sync >> "%OUT%"
set PAN_MESA_DEBUG=sync
"%~dp0cltest.exe" --expect Mali sgemm 256 2 >> "%OUT%" 2>&1
echo exit !ERRORLEVEL! >> "%OUT%"
set PAN_MESA_DEBUG=

echo == 6. off switch: MALIKM_OPENCL=0 must list no Mali >> "%OUT%"
set MALIKM_OPENCL=0
"%~dp0cltest.exe" list >> "%OUT%" 2>&1
set MALIKM_OPENCL=

:done
echo done: %OUT%
type "%OUT%"
