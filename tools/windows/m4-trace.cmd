@echo off
rem m4-trace: the triangle on Panfrost, with the whole image printed and saved
rem (triangle.ppm), then again with PAN_MESA_DEBUG=trace: pandecode writes
rem every job chain the Mali ran into trace\. Output: m4-out.txt here.
set HERE=%~dp0
set OUT=%HERE%m4-out.txt
echo m4-trace %date% %time% > "%OUT%"
set GALLIUM_DRIVER=panfrost
if not exist "%HERE%trace" mkdir "%HERE%trace"
cd /d "%HERE%trace"

echo == triangle, panfrost >> "%OUT%"
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"

copy /y triangle.ppm panfrost.ppm >nul

echo == triangle, panfrost, PAN_MESA_DEBUG=trace >> "%OUT%"
set PAN_MESA_DEBUG=trace
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
set PAN_MESA_DEBUG=
set GALLIUM_DRIVER=

echo == softpipe, for comparison >> "%OUT%"
"%HERE%mesa\triangle.exe" "%HERE%mesa\libgallium_d3d10.dll" >> "%OUT%" 2>&1
echo exit code %errorlevel% >> "%OUT%"
copy /y triangle.ppm softpipe.ppm >nul
dir "%HERE%trace" >> "%OUT%"
cd /d "%HERE%"
type "%OUT%"
echo Results saved to %OUT% and %HERE%trace\
pause
