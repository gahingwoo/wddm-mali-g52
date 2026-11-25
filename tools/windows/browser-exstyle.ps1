# browser-exstyle.ps1 - do the browser's windows have a redirection bitmap?
#
# Chromium's browser process creates its top-level windows with
# WS_EX_NOREDIRECTIONBITMAP on Windows 11 22H2 and later, unless it was
# started with --disable-direct-composition (components/viz/common/features.cc,
# ShouldRemoveRedirectionBitmap). Without DirectComposition the GPU process
# presents through an ANGLE window swap chain in the blt model, whose
# presents are copies into the window's redirection surface. On a window
# with no redirection bitmap such a present shows nothing.
#
# So whether the GPU process alone can turn DirectComposition off (and the
# page still be seen) depends on this bit of the browser's windows
# (docs/EDGE-PRESENT.md, "DirectComposition off without a flag").
#
# Run in the user's session (schtasks /it), with Edge started normally (no flags):
#   powershell -ExecutionPolicy Bypass -File C:\mali\browser-exstyle.ps1 > C:\mali\exstyle.txt
#
# Prints each visible top-level Chrome_WidgetWin_* window of msedge.exe,
# chrome.exe and msedgewebview2.exe with its extended style. Exit code: 0
# every such window keeps its redirection bitmap, 2 at least one has
# WS_EX_NOREDIRECTIONBITMAP, 1 no browser window was found.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class ExStyle {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h, int i);
  public static List<IntPtr> All() {
    var l = new List<IntPtr>();
    EnumWindows((h, p) => { l.Add(h); return true; }, IntPtr.Zero);
    return l;
  }
}
'@

$names = @{}
Get-Process msedge, chrome, msedgewebview2 -ErrorAction SilentlyContinue |
  ForEach-Object { $names[[uint32]$_.Id] = $_.ProcessName }

$found = 0
$noredir = 0
foreach ($h in [ExStyle]::All()) {
  if (-not [ExStyle]::IsWindowVisible($h)) { continue }
  $wpid = [uint32]0
  [void][ExStyle]::GetWindowThreadProcessId($h, [ref]$wpid)
  if (-not $names.ContainsKey($wpid)) { continue }
  $cls = New-Object Text.StringBuilder 256
  [void][ExStyle]::GetClassName($h, $cls, 256)
  if ($cls.ToString() -notlike 'Chrome_WidgetWin_*') { continue }
  $title = New-Object Text.StringBuilder 256
  [void][ExStyle]::GetWindowText($h, $title, 256)
  $ex = [int64][ExStyle]::GetWindowLongPtr($h, -20)
  $bit = ($ex -band 0x00200000) -ne 0
  $found++
  if ($bit) { $noredir++ }
  '{0}.exe pid {1} hwnd 0x{2:x} {3} exstyle 0x{4:x8} NOREDIRECTIONBITMAP {5} "{6}"' -f `
    $names[$wpid], $wpid, [int64]$h, $cls, $ex, ($(if ($bit) { 'yes' } else { 'no' })), $title
}

if ($found -eq 0) { 'no visible browser window found'; exit 1 }
"$found window(s), $noredir without a redirection bitmap"
if ($noredir) { exit 2 }
exit 0
