# cdp-media.ps1 - play one clip in an Edge tab over the DevTools protocol and
# print what edge://media-internals would show for it: the decoder chosen,
# its errors and the media log, then the playback numbers.
#
# Edge must run with --remote-debugging-port=9222 --remote-allow-origins=*
# and have a tab whose URL contains -Match (a file:// page next to the clips).
#
#   powershell -ExecutionPolicy Bypass -File cdp-media.ps1 -Match play.html -Src t-h264.mp4 [-Seconds 10]
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([string]$Match = 'play.html', [Parameter(Mandatory)][string]$Src, [int]$Seconds = 10)

$t = (Invoke-RestMethod http://127.0.0.1:9222/json/list) | ForEach-Object { $_ } |
     Where-Object { $_.type -eq 'page' -and $_.url -like "*$Match*" } | Select-Object -First 1
if (-not $t) { "no tab matching $Match"; exit 1 }
$ws = New-Object System.Net.WebSockets.ClientWebSocket
$ct = [Threading.CancellationToken]::None
$ws.ConnectAsync([Uri]$t.webSocketDebuggerUrl, $ct).Wait()
$script:id = 0
function Send($method, $params) {
  $script:id++
  $m = @{ id = $script:id; method = $method; params = $params } | ConvertTo-Json -Compress -Depth 6
  $b = [Text.Encoding]::UTF8.GetBytes($m)
  $ws.SendAsync([ArraySegment[byte]]$b, 'Text', $true, $ct).Wait()
}
Send 'Media.enable' @{}
# Play: a fresh <video>, so a new player (and decoder) is made for it.
$js = @"
(async () => { document.querySelectorAll('video').forEach(v => { v.pause(); v.remove(); });
  const v = document.createElement('video'); v.id = 'cdpv'; v.muted = true; v.loop = true;
  v.style.width = '100%'; v.src = '$Src'; document.body.prepend(v);
  try { await v.play(); } catch (e) { return 'play: ' + e; } return 'playing'; })()
"@
Send 'Runtime.evaluate' @{ expression = $js; awaitPromise = $true }

$buf = New-Object byte[] 1048576
$end = (Get-Date).AddSeconds($Seconds)
$task = $null
while ((Get-Date) -lt $end) {
  $sb = New-Object Text.StringBuilder
  # One receive outstanding at a time: a timed-out one is waited on again.
  if (-not $task) { $task = $ws.ReceiveAsync([ArraySegment[byte]]$buf, $ct) }
  if (-not $task.Wait([int][Math]::Max(1, ($end - (Get-Date)).TotalMilliseconds))) { break }
  [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $task.Result.Count))
  while (-not $task.Result.EndOfMessage) {
    $task = $ws.ReceiveAsync([ArraySegment[byte]]$buf, $ct); $task.Wait() | Out-Null
    [void]$sb.Append([Text.Encoding]::UTF8.GetString($buf, 0, $task.Result.Count))
  }
  $task = $null
  $j = $sb.ToString() | ConvertFrom-Json
  switch ($j.method) {
    'Media.playerPropertiesChanged' { $j.params.properties | ForEach-Object { "property $($_.name) = $($_.value)" } }
    'Media.playerErrorsRaised' { $j.params.errors | ForEach-Object { "ERROR $($_.errorType) $($_.code): $($_ | ConvertTo-Json -Compress -Depth 8)" } }
    'Media.playerMessagesLogged' { $j.params.messages | ForEach-Object { "$($_.level): $($_.message)" } }
    default { if ($j.id -and $j.result.result) { "evaluate: $($j.result.result.value)" } }
  }
}
Send 'Runtime.evaluate' @{ expression = "(() => { const v = document.getElementById('cdpv'); const q = v.getVideoPlaybackQuality(); return JSON.stringify({ t: v.currentTime, ready: v.readyState, err: v.error && (v.error.code + ' ' + v.error.message), frames: q.totalVideoFrames, dropped: q.droppedVideoFrames }); })()"; returnByValue = $true }
$end = (Get-Date).AddSeconds(3)
while ((Get-Date) -lt $end) {
  if (-not $task) { $task = $ws.ReceiveAsync([ArraySegment[byte]]$buf, $ct) }
  if (-not $task.Wait(3000)) { break }
  $j = [Text.Encoding]::UTF8.GetString($buf, 0, $task.Result.Count) | ConvertFrom-Json -ErrorAction SilentlyContinue
  $task = $null
  if ($j.id -eq $script:id) { "result: $($j.result.result.value)"; break }
}
