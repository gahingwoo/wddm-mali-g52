# codec-conformance.ps1 - a vdpu383 kit's streams (HEVC, VP9 or AV1)
# through the D3D11 decoder of the Mali (vdectest hw); MATCH when vdectest's
# output MD5 is the kit's FFmpeg reference.
#
#   powershell -ExecutionPolicy Bypass -File codec-conformance.ps1 -Kit D:\woa-vdpu383-vp9-2026-11-25 [-Only vp90-2-00] [-Limit 20]
#
# The kit is one of the vdpu383test kits (woa-vdpu383-hevc/-vp9/-av1-2026-11-25):
# streams\NAME.bit or streams\NAME.ivf, ref-md5.txt with one line per
# stream, "NAME FRAMES MD5 ..." (AV1: "NAME FRAMES MD5 MD5-WITHOUT-GRAIN ...";
# the decoder does not apply film grain, so the second is the one compared).
# 10-bit streams are skipped: the UMD offers 8 bit only.
#
# Writes NAME.txt (vdectest's output) and results.txt into -Out and prints
# one line per stream. Run as the user the player runs as (not elevated),
# with UmdVideo 2 (vdectest.exe is on the list) and vdpu383.sys UserAccess 1.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Kit,
    [string]$Exe = (Join-Path $PSScriptRoot "vdectest.exe"),
    [string]$Out = ".\codec-results",
    [string]$Only = "",
    [int]$Limit = 0
)

$list = Join-Path $Kit "ref-md5.txt"
if (-not (Test-Path $list)) { Write-Host "no $list"; exit 1 }
New-Item -ItemType Directory -Force $Out | Out-Null
$results = @()
$bad = 0
$done = 0
foreach ($line in Get-Content $list) {
    if (-not $line -or $line.StartsWith('#')) { continue }
    $f = $line -split '\s+'
    $name = $f[0]; $frames = [int]$f[1]
    if ($Only -and -not $name.StartsWith($Only)) { continue }
    if ($line -match '10le|p010') { $results += "{0,-40} SKIP     10 bit" -f $name; continue }
    # AV1 lines carry the MD5 without film grain as the fourth field.
    $md5 = if ($f.Count -ge 5 -and $f[3] -match '^[0-9a-f]{32}$') { $f[3] } else { $f[2] }
    $file = Get-ChildItem (Join-Path $Kit "streams") -Filter "$name.*" | Where-Object { $_.Extension -in '.bit', '.ivf', '.obu' } | Select-Object -First 1
    if (-not $file) { Write-Host "$name : no stream"; $bad++; continue }
    if ($Limit -and $done -ge $Limit) { break }
    $done++
    $log = Join-Path $Out "$name.txt"
    $t0 = Get-Date
    & $Exe hw $file.FullName > $log 2>&1
    $code = $LASTEXITCODE
    $ms = [int]((Get-Date) - $t0).TotalMilliseconds
    $text = Get-Content $log -Raw
    $gotMd5 = if ($text -match 'output md5 ([0-9a-f]{32})') { $Matches[1] } else { '' }
    $gotFrames = if ($text -match 'frames out (\d+)') { [int]$Matches[1] } else { -1 }
    $errs = if ($text -match 'decode errors (\d+)') { [int]$Matches[1] } else { -1 }
    $ok = ($gotMd5 -eq $md5) -and ($gotFrames -eq $frames) -and ($errs -eq 0)
    if (-not $ok) { $bad++ }
    $r = '{0,-40} {1,-8} frames {2,4}/{3,-4} decode errors {4,3} exit {5} {6,6} ms' -f `
        $name, ($(if ($ok) { 'MATCH' } else { 'MISMATCH' })), $gotFrames, $frames, $errs, $code, $ms
    Write-Host $r
    $results += $r
}
$results += "streams not matching: $bad"
$results | Set-Content (Join-Path $Out "results.txt")
Write-Host "streams not matching: $bad"
exit $bad
