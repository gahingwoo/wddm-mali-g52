# c1-edge.ps1 - Edge with a debug port for the C1 board test (docs/CHROMIUM-VIDEO.md
# section 6): what the GPU process got (edge://gpu) and which video decoder a
# <video> ended up with (the DevTools Media domain), read over CDP.
#
#   Start Edge in the user's session (schtasks /it, see the doc), extra flags quoted:
#     powershell -File C:\mali\c1-edge.ps1 -Start [-Flags "--disable-direct-composition"]
#   Then, from SSH (the debug port is localhost only):
#     powershell -File C:\mali\c1-edge.ps1 -Read [-Video file:///C:/mali/c1/bbb.mp4] [-Tag a]
#   And to close that Edge (only the one with this profile):
#     powershell -File C:\mali\c1-edge.ps1 -Stop
#
# Writes C:\mali\c1\<tag>-gpu.txt (edge://gpu as text), <tag>-media.txt (the
# player's properties and messages: kVideoDecoderName, kIsPlatformVideoDecoder,
# errors) and <tag>-caps.txt (MediaCapabilities.decodingInfo for H.264 1080p:
# powerEfficient is true only with a hardware decoder).
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([switch]$Start, [switch]$Read, [switch]$Stop, [string]$Flags = '',
      [string]$Video = 'file:///C:/mali/c1/bbb.mp4', [string]$Tag = 'run', [int]$Port = 9333)

$edge = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
$prof = 'C:\mali\edgeprof-c1'
$out = 'C:\mali\c1'
New-Item -ItemType Directory -Force $out | Out-Null

if ($Start) {
  $a = @("--remote-debugging-port=$Port", "--user-data-dir=$prof", '--no-first-run',
         '--enable-logging', '--v=1', '--autoplay-policy=no-user-gesture-required')
  if ($Flags) { $a += $Flags -split ' ' | Where-Object { $_ } }
  $a += 'about:blank'
  Start-Process $edge -ArgumentList $a
  "started $(Get-Date -Format o): $($a -join ' ')" | Out-File "$out\start.txt"
  exit 0
}

if ($Stop) {
  Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'msedge.exe' -and $_.CommandLine -match 'edgeprof-c1' } |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
  exit 0
}

if (-not $Read) { Write-Host 'one of -Start, -Read, -Stop'; exit 1 }

$ct = [Threading.CancellationToken]::None
function Connect($url) {
  $ws = New-Object System.Net.WebSockets.ClientWebSocket
  $ws.ConnectAsync([Uri]$url, $ct).Wait()
  return $ws
}
function SendMsg($ws, $obj) {
  $j = $obj | ConvertTo-Json -Depth 8 -Compress
  $b = [Text.Encoding]::UTF8.GetBytes($j)
  $ws.SendAsync([ArraySegment[byte]]$b, 'Text', $true, $ct).Wait()
}
# Every message until one with this id, or until the deadline; all kept.
$script:buf = New-Object byte[] 8388608
$script:pending = $null
function Pump($ws, $id, $seconds, $log) {
  $until = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $until) {
    $sb = New-Object Text.StringBuilder
    # A receive that timed out is still outstanding: wait on it, not a new one.
    if (-not $script:pending) { $script:pending = $ws.ReceiveAsync([ArraySegment[byte]]$script:buf, $ct) }
    if (-not $script:pending.Wait([int][Math]::Max(100, ($until - (Get-Date)).TotalMilliseconds))) { break }
    $r = $script:pending.Result
    $script:pending = $null
    $buf = $script:buf
    [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
    while (-not $r.EndOfMessage) {
      $r = $ws.ReceiveAsync([ArraySegment[byte]]$buf, $ct).Result
      [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $r.Count))
    }
    $s = $sb.ToString()
    if ($log) { $log.Add($s) | Out-Null }
    if ($id -and $s -match "`"id`":$id[,}]") { return $s }
  }
  return $null
}
$walk = '(function(){function walk(n){let s="";if(n.shadowRoot)s+=walk(n.shadowRoot);for(const c of n.childNodes){if(c.nodeType===3)s+=c.textContent+"\n";else s+=walk(c);}return s;}return walk(document.body);})()'

try { $tabs = Invoke-RestMethod "http://127.0.0.1:$Port/json" } catch { Write-Host "no debug port on $Port (is Edge running with -Start?)"; exit 1 }
$t = $tabs | Where-Object type -eq 'page' | Select-Object -First 1
$ws = Connect $t.webSocketDebuggerUrl

# 1. edge://gpu
SendMsg $ws @{ id = 1; method = 'Page.navigate'; params = @{ url = 'edge://gpu' } }
Pump $ws 1 10 $null | Out-Null
Start-Sleep 6
SendMsg $ws @{ id = 2; method = 'Runtime.evaluate'; params = @{ expression = $walk; returnByValue = $true } }
$g = Pump $ws 2 20 $null
try { ($g | ConvertFrom-Json).result.result.value | Out-File "$out\$Tag-gpu.txt" -Encoding utf8 } catch { $g | Out-File "$out\$Tag-gpu.txt" }

# 2. The video, with the Media domain on first.
$log = New-Object System.Collections.ArrayList
SendMsg $ws @{ id = 3; method = 'Media.enable' }
Pump $ws 3 5 $log | Out-Null
SendMsg $ws @{ id = 4; method = 'Page.navigate'; params = @{ url = $Video } }
Pump $ws 0 25 $log | Out-Null
$media = foreach ($m in $log) {
  if ($m -match '"method":"Media\.') {
    try {
      $o = $m | ConvertFrom-Json
      $p = $o.params
      if ($p.properties) { $p.properties | ForEach-Object { "property $($_.name) = $($_.value)" } }
      if ($p.messages) { $p.messages | ForEach-Object { "message [$($_.level)] $($_.message)" } }
      if ($p.errors) { $p.errors | ForEach-Object { "error $($_.errorType) $($_.code) $($_ | ConvertTo-Json -Compress -Depth 6)" } }
      if ($p.events) { $p.events | ForEach-Object { "event $($_.type) $($_.value)" } }
    } catch { $m }
  }
}
$media | Out-File "$out\$Tag-media.txt" -Encoding utf8

# 3. MediaCapabilities: powerEfficient means a hardware decoder for this config.
$caps = 'navigator.mediaCapabilities.decodingInfo({type:"file",video:{contentType:"video/mp4; codecs=\"avc1.640028\"",width:1920,height:1080,bitrate:5000000,framerate:30}}).then(r=>JSON.stringify(r))'
SendMsg $ws @{ id = 5; method = 'Runtime.evaluate'; params = @{ expression = $caps; awaitPromise = $true; returnByValue = $true } }
$c = Pump $ws 5 15 $null
try { ($c | ConvertFrom-Json).result.result.value | Out-File "$out\$Tag-caps.txt" -Encoding utf8 } catch { $c | Out-File "$out\$Tag-caps.txt" }

Write-Host "---- $Tag-gpu.txt (selected)"
Get-Content "$out\$Tag-gpu.txt" -ErrorAction SilentlyContinue |
  Select-String -Pattern 'Video Decode|Compositing|Canvas|Rasterization|WebGL|GL_RENDERER|D3D11 Feature Level|Feature Level|Direct Composition|DirectComposition|GPU process crash|Software Rendering|Hardware accelerated|D3D11VideoDecoder|Decode H264|Display type|gpuDevice|VENDOR' |
  ForEach-Object { $_.Line }
Write-Host "---- $Tag-media.txt (selected)"
Get-Content "$out\$Tag-media.txt" -ErrorAction SilentlyContinue |
  Select-String -Pattern 'DecoderName|PlatformVideoDecoder|error|D3D|fallback|kVideoTracks|kFrameUrl' | ForEach-Object { $_.Line }
Write-Host "---- $Tag-caps.txt"
Get-Content "$out\$Tag-caps.txt" -ErrorAction SilentlyContinue
