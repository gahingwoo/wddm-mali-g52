@echo off
rem m5-wddm-logs: read-only. Collects what Windows itself says about the
rem maliwddm adapter: the DxgKrnl event logs, display/PnP events from the
rem System log, the device's problem status and setupapi's last entries.
rem Run from an ADMINISTRATOR prompt. Output: m5-wddm-logs.txt here.
net session >nul 2>&1 || (echo Run this from an ADMINISTRATOR command prompt. & pause & exit /b 1)
set HERE=%~dp0
set OUT=%HERE%m5-wddm-logs.txt
echo m5-wddm-logs %date% %time% > "%OUT%"
powershell -NoProfile -Command ^
 "$since=(Get-Date).AddHours(-3);" ^
 "Get-WinEvent -ListLog *DxgKrnl* -ErrorAction SilentlyContinue | Format-Table LogName,RecordCount -AutoSize | Out-String -Width 200;" ^
 "foreach($l in (Get-WinEvent -ListLog *DxgKrnl* -ErrorAction SilentlyContinue)){ if($l.RecordCount){ '==== '+$l.LogName; Get-WinEvent -LogName $l.LogName -MaxEvents 60 -ErrorAction SilentlyContinue | Where-Object {$_.TimeCreated -gt $since} | Format-List TimeCreated,Id,LevelDisplayName,Message | Out-String -Width 300 } };" ^
 "'==== System (display, PnP, kernel-pnp)';" ^
 "Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$since} -ErrorAction SilentlyContinue | Where-Object { $_.ProviderName -match 'Display|dxg|Kernel-PnP|UserPnp|Video' -or $_.Message -match 'RKCP7402|maliwddm' } | Format-List TimeCreated,ProviderName,Id,LevelDisplayName,Message | Out-String -Width 300;" ^
 "'==== device';" ^
 "Get-PnpDevice -InstanceId 'ACPI\RKCP7402\0' | Get-PnpDeviceProperty | Where-Object { $_.KeyName -match 'ProblemCode|ProblemStatus|DriverVersion|DriverInfPath|LastArrival' } | Format-Table KeyName,Data -AutoSize | Out-String -Width 200" >> "%OUT%" 2>&1
echo ==== setupapi.dev.log, last 80 lines >> "%OUT%"
powershell -NoProfile -Command "Get-Content $env:windir\INF\setupapi.dev.log -Tail 80" >> "%OUT%" 2>&1
type "%OUT%"
echo.
echo Results saved to %OUT%
pause
