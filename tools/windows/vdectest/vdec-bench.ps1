# vdec-bench.ps1 - FFmpeg's D3D11 hardware decode on the Mali's VDPU383:
# speed, CPU and correctness, per codec (docs/VIDEO-DECODE.md, section 8).
#
#   powershell -ExecutionPolicy Bypass -File vdec-bench.ps1 -Clips D:\clips [-Ffmpeg D:\ffmpeg\bin\ffmpeg.exe] [-Runs 3]
#
# Clips: t-h264.mp4, t-hevc.mp4, t-vp9.webm, t-av1.mp4 (1080p30, 300 frames,
# the 2026-11-25 set; others are measured too, without a known reference).
# For each clip:
#
#   sw     software decode, -f null: the CPU baseline
#   gpu    -hwaccel d3d11va -hwaccel_output_format d3d11, -f null: frames
#          stay in the texture (zero copy: the decoder writes it, nothing
#          is copied). The number that has to compare with Linux (MPP).
#   dl     -hwaccel d3d11va: every frame downloaded to system memory (a
#          GPU copy to a staging texture and a CPU read of it)
#   md5    -hwaccel d3d11va -hwaccel_output_format nv12 -f md5, against the
#          software decode's MD5 (computed here, and against the 2026-11-25
#          reference where the clip is one of the set)
#
# Prints fps (frames / wall time), user and kernel CPU seconds and user CPU
# as a share of wall time, the best of -Runs runs; then the MD5 verdicts,
# vdpu383.sys's decode counter and import counters, and, through maliwddm,
# how many decodes went through its sessions (vdectest.exe kmd, any user;
# else vdpu383test.exe info, which needs an administrator or UserAccess),
# the UMD's zero-copy summary from C:\mali\log when the log is on, and the
# settings in force. Writes everything into -Out as well (bench.txt, and
# each FFmpeg run's stderr).
#
# Run as the user the player runs as (not elevated), with UmdVideo 2. The
# decoder goes through maliwddm (UmdVideoKmd, default 1), so vdpu383.sys
# needs no UserAccess. UmdVideoZeroCopy (GPU0 Device Parameters, default 1)
# 0 gives the copy path of V3a for comparison.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Clips,
    [string]$Ffmpeg = "ffmpeg.exe",
    [string]$Out = ".\vdec-bench",
    [int]$Runs = 3,
    [string]$Only = "",
    [string]$Vdectest = ""
)

$ErrorActionPreference = 'Stop'

# The 2026-11-25 set, software decode MD5 (-f md5 -pix_fmt yuv420p).
$known = @{
    't-h264.mp4' = '839ade1e73e39d528b4f80a5895ebf69'
    't-hevc.mp4' = 'a64f2eec5b3490b484367123e6b1b4bb'
    't-vp9.webm' = '65ff6e6c5308771fb7bb4cd3cf5fdc65'
    't-av1.mp4'  = '30f47ce96ff1f56a402f02373b39f7a5'
}

New-Item -ItemType Directory -Force $Out | Out-Null
$report = Join-Path $Out "bench.txt"
Set-Content -Path $report -Value ("vdec-bench {0}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"))
function Say([string]$s) { Write-Host $s; Add-Content -Path $report -Value $s }

$ff = (Get-Command $Ffmpeg -ErrorAction SilentlyContinue)
if (-not $ff) { Write-Host "no FFmpeg at $Ffmpeg (-Ffmpeg)"; exit 1 }
$ffPath = $ff.Source
Say ("ffmpeg: {0}" -f $ffPath)

# One FFmpeg run: wall time, the process's own CPU times, its stderr, the
# frame count it printed, the MD5 it printed (-f md5 -).
function Run-Ffmpeg([string[]]$a, [string]$tag) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ffPath
    $psi.Arguments = ($a | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    [void]$p.Start()
    $errTask = $p.StandardError.ReadToEndAsync()
    $outText = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    $sw.Stop()
    $errText = $errTask.Result
    $r = [ordered]@{
        Wall = $sw.Elapsed.TotalSeconds
        User = $p.UserProcessorTime.TotalSeconds
        Kernel = $p.PrivilegedProcessorTime.TotalSeconds
        Exit = $p.ExitCode
        Frames = 0
        Md5 = ''
        Rtime = 0.0
    }
    Set-Content -Path (Join-Path $Out "$tag.log") -Value ($psi.Arguments + "`r`n" + $errText + $outText)
    $m = [regex]::Matches($errText, 'frame=\s*(\d+)')
    if ($m.Count -gt 0) { $r.Frames = [int]$m[$m.Count - 1].Groups[1].Value }
    $m = [regex]::Match($outText + $errText, 'MD5=([0-9a-f]{32})')
    if ($m.Success) { $r.Md5 = $m.Groups[1].Value }
    $m = [regex]::Match($errText, 'rtime=([0-9.]+)s')
    if ($m.Success) { $r.Rtime = [double]$m.Groups[1].Value }
    return $r
}

function Best([string[]]$a, [string]$tag) {
    $best = $null
    for ($i = 1; $i -le $Runs; $i++) {
        $r = Run-Ffmpeg $a ("{0}-{1}" -f $tag, $i)
        if ($r.Exit -ne 0) { return $r }
        if (-not $best -or $r.Wall -lt $best.Wall) { $best = $r }
    }
    return $best
}

function Line([string]$clip, [string]$mode, $r) {
    if ($r.Exit -ne 0) { return ("{0,-12} {1,-4} FAILED (exit {2}), see {3}" -f $clip, $mode, $r.Exit, $Out) }
    $fps = if ($r.Wall -gt 0) { $r.Frames / $r.Wall } else { 0 }
    $share = if ($r.Wall -gt 0) { 100.0 * $r.User / $r.Wall } else { 0 }
    return ("{0,-12} {1,-4} {2,5} frames {3,7:N2} s {4,7:N1} fps  user {5,6:N2} s  kernel {6,5:N2} s  user/wall {7,5:N0}%" -f
            $clip, $mode, $r.Frames, $r.Wall, $fps, $r.User, $r.Kernel, $share)
}

# vdpu383.sys's counters, through vdpu383test info (optional).
$v383 = Join-Path $PSScriptRoot "vdpu383test.exe"
if (-not (Test-Path $v383)) {
    $c = Get-Command vdpu383test.exe -ErrorAction SilentlyContinue
    $v383 = if ($c) { $c.Source } else { $null }
}
# vdectest.exe kmd: the same counters through maliwddm, for any user.
if (-not $Vdectest) { $Vdectest = Join-Path $PSScriptRoot "vdectest.exe" }
if (-not (Test-Path $Vdectest)) { $Vdectest = $null }
function Decoder-Counters {
    $t = $null
    if ($Vdectest) {
        try { $t = (& $Vdectest kmd 2>&1 | Out-String) } catch { $t = $null }
        if ($t -notmatch 'runs \d+, timeouts') { $t = $null }
    }
    if (-not $t -and $v383) {
        try { $t = (& $v383 info 2>&1 | Out-String) } catch { $t = $null }
    }
    if (-not $t) { return $null }
    $h = @{}
    $m = [regex]::Match($t, 'sessions:?\s+(\d+) open, (\d+) made, (\d+) decodes through them')
    if ($m.Success) { $h.SessionRuns = [int]$m.Groups[3].Value; $h.SessionsMade = [int]$m.Groups[2].Value }
    $m = [regex]::Match($t, 'runs (\d+), timeouts (\d+)')
    if ($m.Success) { $h.Runs = [int]$m.Groups[1].Value; $h.Timeouts = [int]$m.Groups[2].Value }
    $m = [regex]::Match($t, 'imports\s+(\d+) made or refreshed, (\d+) taken back by the display driver, (\d+) failed')
    if ($m.Success) { $h.Imports = [int]$m.Groups[1].Value; $h.Revokes = [int]$m.Groups[2].Value; $h.ImportFailures = [int]$m.Groups[3].Value }
    return $h
}

# The settings in force (GPU0 and the decoder's Device Parameters).
try {
    foreach ($d in (Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -match 'RKCP0383|^ACPI\\RKCP' -or $_.Class -eq 'Display' })) {
        $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($d.InstanceId)\Device Parameters"
        if (Test-Path $k) {
            $p = Get-ItemProperty $k
            $names = 'UmdVideo', 'UmdVideoZeroCopy', 'UmdVideoCodecs', 'UmdAv1Synth', 'UmdVp9Mpp', 'UserAccess', 'Iommu'
            $vals = foreach ($n in $names) { if ($null -ne $p.$n) { "$n=$($p.$n)" } }
            Say ("{0}: {1}" -f $d.InstanceId, ($vals -join ' '))
        }
    }
} catch { Say "settings: not readable ($($_.Exception.Message))" }

$files = Get-ChildItem -Path $Clips -File | Where-Object { $_.Extension -in '.mp4', '.webm', '.mkv', '.ivf' } | Sort-Object Name
if ($Only) { $files = $files | Where-Object { $_.Name -like "*$Only*" } }
if (-not $files) { Write-Host "no clips in $Clips"; exit 1 }

$before = Decoder-Counters
$speed = @()
$verdicts = @()
$fail = 0
foreach ($f in $files) {
    $n = $f.Name
    $in = $f.FullName
    $base = @('-hide_banner', '-nostdin', '-benchmark', '-y')
    $sw = Best ($base + @('-i', $in, '-an', '-f', 'null', '-')) "$n-sw"
    $gpu = Best ($base + @('-hwaccel', 'd3d11va', '-hwaccel_output_format', 'd3d11', '-i', $in, '-an', '-f', 'null', '-')) "$n-gpu"
    $dl = Best ($base + @('-hwaccel', 'd3d11va', '-i', $in, '-an', '-f', 'null', '-')) "$n-dl"
    $speed += (Line $n 'sw' $sw)
    $speed += (Line $n 'gpu' $gpu)
    $speed += (Line $n 'dl' $dl)
    Write-Host $speed[-3]; Write-Host $speed[-2]; Write-Host $speed[-1]

    $md = @('-hide_banner', '-nostdin', '-y', '-i', $in, '-an', '-f', 'md5', '-pix_fmt', 'yuv420p', '-')
    $swm = Run-Ffmpeg $md "$n-md5-sw"
    $hwm = Run-Ffmpeg (@('-hide_banner', '-nostdin', '-y', '-hwaccel', 'd3d11va', '-hwaccel_output_format', 'nv12', '-i', $in,
                         '-an', '-f', 'md5', '-pix_fmt', 'yuv420p', '-')) "$n-md5-hw"
    $ref = $known[$n]
    $okSw = (-not $ref) -or ($swm.Md5 -eq $ref)
    $okHw = $hwm.Md5 -and ($hwm.Md5 -eq $swm.Md5)
    if (-not $okHw -or -not $okSw) { $fail++ }
    $verdicts += ("{0,-12} hw {1} sw {2}{3}  {4}" -f $n, $(if ($hwm.Md5) { $hwm.Md5 } else { 'none (exit ' + $hwm.Exit + ')' }), $swm.Md5,
                  $(if ($ref) { $(if ($okSw) { ' (= reference)' } else { " (reference ${ref}: software differs!)" }) } else { '' }),
                  $(if ($okHw) { 'MATCH' } else { 'MISMATCH' }))
    Write-Host $verdicts[-1]
}
$after = Decoder-Counters

Say ""
Say "speed (best of $Runs; fps = frames / wall; gpu = -hwaccel_output_format d3d11, dl = downloaded)"
foreach ($s in $speed) { Say $s }
Say ""
Say "correctness (-hwaccel d3d11va -hwaccel_output_format nv12 -f md5, against software)"
foreach ($v in $verdicts) { Say $v }
if ($before -and $after -and $null -ne $before.Runs) {
    Say ("vdpu383.sys: {0} decodes during the runs, {1} timeouts" -f ($after.Runs - $before.Runs), ($after.Timeouts - $before.Timeouts))
    if ($null -ne $after.SessionRuns) {
        Say ("through maliwddm (sessions): {0} decodes, {1} sessions made" -f
             ($after.SessionRuns - $before.SessionRuns), ($after.SessionsMade - $before.SessionsMade))
    }
    if ($null -ne $after.Imports) {
        Say ("vdpu383.sys imports: {0} made or refreshed, {1} taken back, {2} failed" -f
             ($after.Imports - $before.Imports), ($after.Revokes - $before.Revokes), ($after.ImportFailures - $before.ImportFailures))
    } else {
        Say "vdpu383.sys: no import counters (interface 2: zero copy needs the vdpu383-import driver)"
    }
} else {
    Say "vdpu383.sys counters: vdpu383test.exe not found or not readable"
}

# The UMD's own account, when C:\mali\log is on: the decoders' last lines.
if (Test-Path 'C:\mali\log') {
    $logs = Get-ChildItem 'C:\mali\log' -Filter 'ffmpeg*' -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 8
    $lines = foreach ($l in $logs) { Select-String -Path $l.FullName -Pattern 'decoder .* destroyed after|zero copy|not kept for the decoder|did not export' -ErrorAction SilentlyContinue | Select-Object -Last 3 }
    if ($lines) { Say ""; Say "UMD log (C:\mali\log):"; foreach ($l in $lines) { Say ("  " + $l.Line.Trim()) } }
}
Say ""
Say ("{0} of {1} clips match software decode" -f ($files.Count - $fail), $files.Count)
Say "written: $report"
exit $fail
