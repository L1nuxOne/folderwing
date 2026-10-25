# tests/resize.ps1 -What split|tree|window|grip [-Panel]: how resizing performs (not a pass/fail test)
# Measures resizing: a pane splitter drag (posted mouse moves) or a window edge
# resize (SetWindowPos steps), optionally with the settings panel open.
param([ValidateSet('split', 'tree', 'window', 'grip')] [string] $What = 'split', [switch] $Panel, [int] $Steps = 200,
      [int] $IntervalMs = 4, [string] $Exe = "$PSScriptRoot\..\build\release\folderwing.exe", [string] $Tag = '')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\uilib.ps1"
if (-not ('FpDrag' -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices; using System.Diagnostics; using System.Threading;
public static class FpDrag {
  [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hh, uint f);
  [DllImport("user32.dll")] static extern IntPtr SendMessageTimeoutW(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint ms, out IntPtr r);
  public struct R { public int L, T, Rr, B; }
  static IntPtr XY(int x, int y) { return (IntPtr)(((y & 0xFFFF) << 16) | (x & 0xFFFF)); }
  static void Wait(Stopwatch sw, double until) { while (sw.Elapsed.TotalMilliseconds < until) Thread.SpinWait(50); }
  // a drag of (x0,y) by dx over steps, a move every ms; returns the worst answer time to WM_NULL while dragging
  public static double Drag(IntPtr h, int x0, int y, int steps, int dx, double ms) {
    PostMessageW(h, 0x201, (IntPtr)1, XY(x0, y));
    var sw = Stopwatch.StartNew(); double worst = 0;
    for (int i = 1; i <= steps; i++) {
      int x = x0 + (int)Math.Round(dx * Math.Sin(Math.PI * i / steps));  // out and back
      PostMessageW(h, 0x200, (IntPtr)1, XY(x, y));
      if (i % 10 == 0) { var t = Stopwatch.StartNew(); IntPtr r; SendMessageTimeoutW(h, 0, IntPtr.Zero, IntPtr.Zero, 0, 10000, out r); worst = Math.Max(worst, t.Elapsed.TotalMilliseconds); }
      Wait(sw, i * ms);
    }
    PostMessageW(h, 0x202, IntPtr.Zero, XY(x0, y));
    return worst;
  }
  // a drag in screen coordinates of a window that moves while dragged (the side panel's grip)
  public static double DragMoving(IntPtr h, IntPtr ping, int sx0, int sy, int steps, int dx, double ms) {
    R r; GetWindowRect(h, out r);
    PostMessageW(h, 0x201, (IntPtr)1, XY(sx0 - r.L, sy - r.T));
    var sw = Stopwatch.StartNew(); double worst = 0;
    for (int i = 1; i <= steps; i++) {
      int x = sx0 + (int)Math.Round(dx * Math.Sin(Math.PI * i / steps));
      GetWindowRect(h, out r);
      PostMessageW(h, 0x200, (IntPtr)1, XY(x - r.L, sy - r.T));
      if (i % 10 == 0) { var t = Stopwatch.StartNew(); IntPtr res; SendMessageTimeoutW(ping, 0, IntPtr.Zero, IntPtr.Zero, 0, 10000, out res); worst = Math.Max(worst, t.Elapsed.TotalMilliseconds); }
      Wait(sw, i * ms);
    }
    GetWindowRect(h, out r);
    PostMessageW(h, 0x202, IntPtr.Zero, XY(sx0 - r.L, sy - r.T));
    return worst;
  }
  // the window's right edge in and out: [worst, average] SetWindowPos time
  public static double[] Resize(IntPtr h, int steps, int dw, double ms) {
    R r; GetWindowRect(h, out r); int w0 = r.Rr - r.L, h0 = r.B - r.T;
    var sw = Stopwatch.StartNew(); double worst = 0, sum = 0;
    for (int i = 1; i <= steps; i++) {
      int w = w0 - (int)Math.Round(dw * Math.Sin(Math.PI * i / steps));
      var t = Stopwatch.StartNew();
      SetWindowPos(h, IntPtr.Zero, 0, 0, w, h0, 0x0002 | 0x0004 | 0x0010); // NOMOVE NOZORDER NOACTIVATE
      double e = t.Elapsed.TotalMilliseconds; sum += e; worst = Math.Max(worst, e);
      Wait(sw, i * ms);
    }
    return new double[] { worst, sum / steps };
  }
}
"@
}
$out = "$PSScriptRoot\..\build\resize"
New-Item -ItemType Directory -Force $out | Out-Null
$report = "$out\prof-$What$(if ($Panel) { '-panel' })$Tag.txt"
Remove-Item $report -ErrorAction SilentlyContinue
$p = Start-Demo $Exe @('--settings=', '--session=', "--profile-out=$report", 'C:\Windows\System32', 'C:\Windows\WinSxS')
try {
    Start-Sleep -Milliseconds 1500
    if ($Panel) { Chord $script:pane0 'Ctrl' 0xBC; Start-Sleep -Milliseconds 1500 }
    Chord $script:pane0 'Ctrl' 0x7B; Start-Sleep -Milliseconds 300 # Ctrl+F12: counters from here
    $r = New-Object FpUi+RECT
    $sw = [Diagnostics.Stopwatch]::StartNew()
    switch ($What) {
        'split' {
            [FpUi]::GetWindowRect($script:pane0, [ref]$r) | Out-Null; $pr = $r
            [FpUi]::GetWindowRect($script:main, [ref]$r) | Out-Null
            $cl = New-Object FpUi+RECT; [FpUi]::GetClientRect($script:main, [ref]$cl) | Out-Null
            $border = (($r.R - $r.L) - $cl.R) / 2
            $x = $pr.R - $r.L - $border + 1
            $res = "worst WM_NULL answer {0:N1} ms" -f [FpDrag]::Drag($script:main, $x, 300, $Steps, -150, $IntervalMs)
        }
        'tree' {
            [FpUi]::GetWindowRect($script:main, [ref]$r) | Out-Null; $mr = $r
            $tree = [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotTree', [IntPtr]::Zero)
            [FpUi]::GetWindowRect($tree, [ref]$r) | Out-Null
            $cl = New-Object FpUi+RECT; [FpUi]::GetClientRect($script:main, [ref]$cl) | Out-Null
            $border = (($mr.R - $mr.L) - $cl.R) / 2
            $res = "worst WM_NULL answer {0:N1} ms" -f [FpDrag]::Drag($script:main, $r.R - $mr.L - $border + 1, 300, $Steps, 150, $IntervalMs)
        }
        'grip' {
            $side = [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
            [FpUi]::GetWindowRect($side, [ref]$r) | Out-Null
            $res = "worst WM_NULL answer {0:N1} ms" -f [FpDrag]::DragMoving($side, $script:main, $r.L + 2, $r.T + 300, $Steps, -200, $IntervalMs)
        }
        'window' { $v = [FpDrag]::Resize($script:main, $Steps, 300, $IntervalMs); $res = "SetWindowPos worst {0:N1} ms, avg {1:N1} ms" -f $v[0], $v[1] }
    }
    $took = $sw.Elapsed.TotalMilliseconds
    Start-Sleep -Milliseconds 1500
    Shot $out "after-$What"
} finally {
    Stop-Demo $p | Out-Null
}
"$What$(if ($Panel) { ' + panel' }): $Steps steps in $([math]::Round($took)) ms; $res"
Get-Content $report | Select-String '^\s*(layout|pane\.paint|pane\.scrollbar|tree\.paint|status\.paint|ui\.dispatch|ui\.over|panel\.(resize|hung|input)|slowest)' | ForEach-Object { $_.Line }

