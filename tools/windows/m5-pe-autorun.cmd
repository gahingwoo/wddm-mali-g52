@echo off
rem m5-pe-autorun: present on the stick = WinPE runs the M5.2 test by itself.
rem The stick's woa-debug\collect.cmd calls this first (one hook line); delete
rem this file to get a plain WinPE back.
echo.
echo ===== wddm-mali-g52: running mali\m5-pe-test.cmd =====
call "%~dp0m5-pe-test.cmd" auto
echo ===== m5-pe-test done: output in %~dp0m5-pe-out.txt =====
rem Shut down cleanly so the stick is flushed: the board turning itself
rem off means the results are on the stick. (woa-debug's collection is
rem skipped in this mode.)
echo Shutting down in 5 s...
ping -n 6 127.0.0.1 >nul
wpeutil shutdown
