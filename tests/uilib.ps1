<#
.SYNOPSIS
  Helpers for driving folderwing.exe with posted messages. Dot-source it:
  . "$PSScriptRoot\uilib.ps1"
  Posted input does not update keyboard state, so Ctrl/Shift shortcuts are
  reached through their WM_CHAR codes or mouse clicks, or with Chord, which
  holds the modifiers down in the demo's own thread while it sends the key.
#>
Add-Type -AssemblyName System.Drawing
if (-not ('FpUi' -as [type])) {
    Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public static class FpUi {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  // title as a pointer: PowerShell turns $null into "" for string parameters,
  // which would only match windows with an empty caption
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, IntPtr title);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,UIntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,UIntPtr w,StringBuilder l,uint flags,uint timeout,out UIntPtr result);
  public static void OwnedWindow(IntPtr h,uint owner) {
    uint actual;
    if(h==IntPtr.Zero || GetWindowThreadProcessId(h,out actual)==0 || actual!=owner)
      throw new InvalidOperationException("Acknowledged input target is not owned by the current demo");
  }
  public static ulong Input(IntPtr h,uint message,uint key,IntPtr data,uint timeout) {
    UIntPtr result;
    if(timeout==0 || h==IntPtr.Zero || SendMessageTimeoutW(h,message,(UIntPtr)key,data,0x23,timeout,out result)==IntPtr.Zero)
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Input was not acknowledged within its deadline");
    return result.ToUInt64();
  }
  public static string ReadText(IntPtr h,uint timeout) {
    UIntPtr result;var text=new StringBuilder(8192);
    if(timeout==0 || h==IntPtr.Zero || SendMessageTimeoutW(h,0xD,(UIntPtr)text.Capacity,text,0x23,timeout,out result)==IntPtr.Zero)
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Text read was not acknowledged within its deadline");
    return text.ToString();
  }
  public static void Type(IntPtr h,string text,uint owner,int timeout) {
    var watch=System.Diagnostics.Stopwatch.StartNew();
    foreach(char c in text) {
      OwnedWindow(h,owner);long remaining=timeout-watch.ElapsedMilliseconds;
      if(remaining<=0)throw new TimeoutException("Character input exceeded its deadline");
      Input(h,0x102,c,IntPtr.Zero,(uint)remaining);
    }
  }
  public sealed class SettingsState {
    public ulong Revision, Have, Snapshot, Keys, Chars, Clicks, Request, Completed, Status, Pending;
    public ulong FieldOpen, FieldSaving, FocusKind, FocusId, FocusSub, QueryHash, QueryLength, Controls;
    public ulong Recording, Conflict, Builder, BuilderSaving, BaseRevision;
    public override string ToString() {
      return String.Format("state={0} snapshot={1} input={2}/{3}/{4} request={5} completed={6} status={7} pending={8} field={9}/{10} focus={11}/{12:X}/{13} query={14:X}/{15} controls={16} recording={17} conflict={18} builder={19}/{20} base={21}",
        Revision,Snapshot,Keys,Chars,Clicks,Request,Completed,Status,Pending,FieldOpen,FieldSaving,FocusKind,FocusId,FocusSub,QueryHash,QueryLength,Controls,Recording,Conflict,Builder,BuilderSaving,BaseRevision);
    }
  }
  static ulong SettingsProp(IntPtr h,string name) {return unchecked((ulong)GetPropW(h,"Settings."+name).ToInt64());}
  public static ulong SettingsHash(string text) {
    unchecked {ulong h=0;foreach(char c in text)h=(h^c)*1099511628211UL;return (h^0xffffUL)*1099511628211UL;}
  }
  public static SettingsState ReadSettingsState(IntPtr h) {
    ulong revision=SettingsProp(h,"State");if(revision==0 || (revision&1)!=0)return null;
    var s=new SettingsState {Revision=revision,
      Have=SettingsProp(h,"Have"),Snapshot=SettingsProp(h,"Snapshot"),Keys=SettingsProp(h,"Keys"),Chars=SettingsProp(h,"Chars"),Clicks=SettingsProp(h,"Clicks"),
      Request=SettingsProp(h,"Request"),Completed=SettingsProp(h,"Completed"),Status=SettingsProp(h,"Status"),Pending=SettingsProp(h,"Pending"),
      FieldOpen=SettingsProp(h,"FieldOpen"),FieldSaving=SettingsProp(h,"FieldSaving"),FocusKind=SettingsProp(h,"FocusKind"),FocusId=SettingsProp(h,"FocusId"),FocusSub=SettingsProp(h,"FocusSub"),
      QueryHash=SettingsProp(h,"QueryHash"),QueryLength=SettingsProp(h,"QueryLength"),Controls=SettingsProp(h,"Controls"),Recording=SettingsProp(h,"Recording"),Conflict=SettingsProp(h,"Conflict"),
      Builder=SettingsProp(h,"Builder"),BuilderSaving=SettingsProp(h,"BuilderSaving"),BaseRevision=SettingsProp(h,"BaseRevision")};
    return revision==SettingsProp(h,"State") ? s : null;
  }
  [DllImport("user32.dll")] public static extern int GetMenuItemCount(IntPtr m);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr m, uint id, StringBuilder s, int n, uint flags);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr h, string name);
  [DllImport("user32.dll")] public static extern uint GetMenuState(IntPtr m, uint id, uint flags);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public struct RECT { public int L, T, R, B; }

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO { public int cb; public string r, desk, title; public int x, y, w, h, cx, cy, fill, flags;
                       public short show, r2; public IntPtr r3, i, o, e; }
  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION { public IntPtr process, thread; public int pid, tid; }
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CreateProcessW(string app, StringBuilder cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                    IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] public static extern bool GetExitCodeProcess(IntPtr process, out uint code);

  // Starts a GUI process whose window shows without taking the foreground,
  // so the user's typing never lands in a test window.
  public static int StartNoActivate(string exe, string args) {
    var si = new STARTUPINFO(); si.cb = Marshal.SizeOf(si);
    si.flags = 1;   // STARTF_USESHOWWINDOW
    si.show = 4;    // SW_SHOWNOACTIVATE
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe, new StringBuilder("\"" + exe + "\" " + args), IntPtr.Zero, IntPtr.Zero, false, 0,
                        IntPtr.Zero, null, ref si, out pi)) throw new System.ComponentModel.Win32Exception();
    CloseHandle(pi.thread); CloseHandle(pi.process);
    return pi.pid;
  }

  [DllImport("user32.dll", SetLastError = true)] static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] static extern bool GetKeyboardState(byte[] s);
  [DllImport("user32.dll")] static extern bool SetKeyboardState(byte[] s);
  [StructLayout(LayoutKind.Sequential)] struct MSG { public IntPtr h; public uint m; public IntPtr w,l; public uint time; public int x,y; public uint p; }
  [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m, IntPtr h, uint first, uint last, uint remove);
  [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
  [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h, uint flags);
  delegate bool ChildVisitor(IntPtr h, IntPtr data);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h, ChildVisitor visit, IntPtr data);
  // Control IDs remain useful when a form groups its fields under a container.
  public static IntPtr FindControl(IntPtr parent, int id) {
    if(parent==IntPtr.Zero) return IntPtr.Zero;
    var found = GetDlgItem(parent,id);
    if (found != IntPtr.Zero) return found;
    EnumChildWindows(parent,(h,data)=> { if(GetDlgCtrlID(h)!=id) return true; found=h; return false; },IntPtr.Zero);
    return found;
  }
  public static bool ControlEnabled(IntPtr h) {
    if (h==IntPtr.Zero) return false;
    for(;h!=IntPtr.Zero;h=GetAncestor(h,1)) if(!IsWindowEnabled(h)) return false;
    return true;
  }
  // Sends a key to h while mods (virtual keys) are down. Sharing the demo
  // thread's input state is the only way its GetKeyState sees them; the
  // user's real keyboard state is untouched.
  public static void Chord(IntPtr h, byte[] mods, int vk, IntPtr lparam) {
    Chord(h,mods,vk,lparam,0);
  }
  public static void Chord(IntPtr h, byte[] mods, int vk, IntPtr lparam, uint timeout) {
    uint pid, me = GetCurrentThreadId(), them = GetWindowThreadProcessId(h, out pid);
    if(h==IntPtr.Zero || them==0) throw new InvalidOperationException("Chord target is not a live window");
    MSG pending; PeekMessageW(out pending,IntPtr.Zero,0,0,0); // create this caller's input queue
    bool attached=false; byte[] original=null;
    try {
      if(me!=them) {
        if(!AttachThreadInput(me,them,true)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Could not attach test input");
        attached=true;
      }
      var state=new byte[256];
      if(!GetKeyboardState(state)) throw new InvalidOperationException("Could not read test keyboard state");
      original=(byte[])state.Clone();
      // A held/unrelated modifier must not turn Ctrl+Alt+T into Ctrl+Shift+Alt+T.
      foreach(var key in new[]{0x10,0x11,0x12,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5}) state[key]&=0x7f;
      foreach(var key in mods) state[key]|=0x80;
      if(!SetKeyboardState(state)) throw new InvalidOperationException("Could not set test keyboard state");
      if(timeout==0)SendMessageW(h,0x100,(IntPtr)vk,lparam);
      else Input(h,0x100,(uint)vk,lparam,timeout);
    } finally {
      try {
        if(original!=null && !SetKeyboardState(original)) throw new InvalidOperationException("Could not restore test keyboard state");
      } finally {
        if(attached && !AttachThreadInput(me,them,false)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Could not detach test input");
      }
    }
  }
}
"@
}

# Like Start-Process, but the window never steals focus from the user.
function Start-Quiet([string] $exe, [string] $arguments) {
    Get-Process -Id ([FpUi]::StartNoActivate((Resolve-Path $exe).Path, $arguments))
}

$WM_KEYDOWN = 0x100; $WM_CHAR = 0x102; $WM_SYSKEYDOWN = 0x104; $WM_COMMAND = 0x111
$WM_LBUTTONDOWN = 0x201; $WM_RBUTTONUP = 0x205; $WM_MOUSEWHEEL = 0x20A; $WM_CONTEXTMENU = 0x7B

# Starts the demo and fills $script:main / tree / pane0 / pane1 / dbg / scale.
# Defaults only, unless the test passes --settings=FILE / --session=FILE: a
# personal openpilot.ini must not change what is tested, and a test must
# never overwrite the user's session.
function Start-Demo([string] $exe, [string[]] $arguments) {
    if (-not ($arguments -like '--settings=*')) { $arguments = @('--settings=') + $arguments }
    if (-not ($arguments -like '--session=*')) { $arguments = @('--session=') + $arguments }
    $quoted = ($arguments | ForEach-Object { "`"$_`"" }) -join ' '
    $p = Start-Quiet $exe $quoted
    $sw = [Diagnostics.Stopwatch]::StartNew()
    do {
        $p.Refresh()
        $script:main = $p.MainWindowHandle
        $ready = $false
        if ($script:main -ne [IntPtr]::Zero) {
            $script:tree = [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotTree', [IntPtr]::Zero)
            $script:pane0 = [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotPane', [IntPtr]::Zero)
            $script:pane1 = [FpUi]::FindWindowExW($script:main, $script:pane0, 'OpenPilotPane', [IntPtr]::Zero)
            $script:dbg = [FpUi]::FindWindowExW($script:main, [IntPtr]::Zero, 'OpenPilotDebug', [IntPtr]::Zero)
            $ready = $script:main -ne [IntPtr]::Zero -and $script:tree -ne [IntPtr]::Zero -and
                     $script:pane0 -ne [IntPtr]::Zero -and $script:pane1 -ne [IntPtr]::Zero -and $script:dbg -ne [IntPtr]::Zero
        }
        if ($ready) { break }
        Start-Sleep -Milliseconds 5
    } while (-not $p.HasExited -and $sw.ElapsedMilliseconds -lt 5000)
    # Window discovery can race startup; wait for the owning UI thread to finish
    # initialization before injecting modifier state into its input queue.
    if ($ready) { $ready = $p.WaitForInputIdle([Math]::Max(0, 5000 - [int]$sw.ElapsedMilliseconds)) }
    if (-not $ready) {
        if (-not $p.HasExited) { Stop-Demo $p | Out-Null }
        throw 'OpenPilot main window, pane controls and UI thread were not ready within 5000ms'
    }
    $script:scale = [FpUi]::GetDpiForWindow($script:main) / 96.0
    $script:demoPid = [uint32]$p.Id
    if ($env:OPENPILOT_CHILD_UI_LEASE) {
        try {
            . "$PSScriptRoot\child_session_context.ps1"
            Set-ChildUiPlacement $p $script:main
        } catch { Stop-Demo $p | Out-Null; throw }
    }
    $p
}

# A visible popup menu owned by the demo (other apps keep hidden menu windows around).
# $class: 'OpenPilotMenu' (drawn by the app), '#32768' (Windows'), or either.
function Find-DemoMenu([string] $class = '') {
    $classes = if ($class) { @($class) } else { @('OpenPilotMenu', '#32768') }
    foreach ($c in $classes) {
        $w = [IntPtr]::Zero
        while (($w = [FpUi]::FindWindowExW([IntPtr]::Zero, $w, $c, [IntPtr]::Zero)) -ne [IntPtr]::Zero) {
            $owner = [uint32]0
            [FpUi]::GetWindowThreadProcessId($w, [ref]$owner) | Out-Null
            if ($owner -eq $script:demoPid -and [FpUi]::IsWindowVisible($w)) { return $w }
        }
    }
    [IntPtr]::Zero
}

function Stop-Demo($p) {
    $p.CloseMainWindow() | Out-Null
    if (-not $p.WaitForExit(3000)) { $p.Kill(); return $false }
    $true
}

function Shot([string] $dir, [string] $name) {
    $r = New-Object FpUi+RECT
    [FpUi]::GetWindowRect($script:main, [ref]$r) | Out-Null
    $bmp = New-Object Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [FpUi]::PrintWindow($script:main, $hdc, 2) | Out-Null
    $g.ReleaseHdc($hdc)
    $bmp.Save("$dir\$name.png", [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

function Title { $sb = New-Object Text.StringBuilder 512; [FpUi]::GetWindowTextW($script:main, $sb, 512) | Out-Null; $sb.ToString() }
function Post($h, $msg, $w, $l = 0) { [FpUi]::PostMessageW($h, $msg, [IntPtr]$w, [IntPtr]$l) | Out-Null }
# A real scan code (and the extended bit for navigation keys) keeps
# TranslateMessage from inventing a WM_CHAR for keys like PageUp.
function Key-LParam([int] $vk) {
    $scan = [FpUi]::MapVirtualKeyW([uint32]$vk, 0)
    $ext = if (($vk -ge 0x21 -and $vk -le 0x2E) -or $vk -eq 0x5D) { 1 -shl 24 } else { 0 }
    1 -bor ($scan -shl 16) -bor $ext
}
function Key($h, [int] $vk, [int] $ms = 200) { Post $h $WM_KEYDOWN $vk (Key-LParam $vk); Start-Sleep -Milliseconds $ms }
# Ctrl/Shift + key, e.g. Chord $h 'Ctrl','Shift' 0x43
function Chord($h, [string[]] $mods, [int] $vk, [int] $ms = 200) {
    $v = foreach ($m in $mods) { switch ($m) { 'Ctrl' { 0x11; 0xA2 } 'Shift' { 0x10; 0xA0 } 'Alt' { 0x12; 0xA4 } } }
    [FpUi]::Chord($h, [byte[]]$v, $vk, [IntPtr](Key-LParam $vk))
    Start-Sleep -Milliseconds $ms
}
# Opt-in synchronous native input, with one deadline for the whole operation.
function Input-Remaining($clock,[int]$limit) {
    $remaining=$limit-[int]$clock.ElapsedMilliseconds
    if($remaining -le 0){throw "Input deadline of${limit}ms expired"};$remaining
}
function Send-AcknowledgedText($h,[string]$text,[int]$ms=3000) {
    [FpUi]::Type($h,$text,$script:demoPid,$ms)
}
function Send-AcknowledgedKey($h,[int]$vk,[int]$ms=3000) {
    [FpUi]::OwnedWindow($h,$script:demoPid)
    [void][FpUi]::Input($h,$WM_KEYDOWN,$vk,[IntPtr](Key-LParam $vk),[uint32]$ms)
}
function Chord-Acknowledged($h,[string[]]$mods,[int]$vk,[int]$ms=3000) {
    [FpUi]::OwnedWindow($h,$script:demoPid)
    $keys=foreach($m in $mods){switch($m){'Ctrl'{0x11;0xA2}'Shift'{0x10;0xA0}'Alt'{0x12;0xA4}}}
    [FpUi]::Chord($h,[byte[]]$keys,$vk,[IntPtr](Key-LParam $vk),[uint32]$ms)
}
# A native send acknowledges the host, not its out-of-process Settings helper.
# Bind this target once per helper generation, then observe coherent read-only
# state after each input. Existing generic input helpers keep their semantics.
function New-SettingsTarget {
    $side=[FpUi]::FindWindowExW($script:main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
    $slot=[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)
    $h=[IntPtr]::Zero
    while(($h=[FpUi]::FindWindowExW([IntPtr]::Zero,$h,'FpSettingsHidden',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        $owner=[uint32]0;[FpUi]::GetWindowThreadProcessId($h,[ref]$owner)|Out-Null
        $p=Get-Process -Id $owner -ErrorAction SilentlyContinue
        if($p -and $p.Parent.Id -eq $script:demoPid){
            $target=[pscustomobject]@{Helper=$h;Pid=$owner;Started=$p.StartTime;Slot=$slot;Host=$script:main;Owner=$script:demoPid}
            Assert-SettingsTarget $target
            return $target
        }
    }
    throw 'No owned Settings helper is available'
}
function Assert-SettingsTarget($target) {
    if($target.Owner -ne $script:demoPid -or $target.Host -ne $script:main){throw 'Settings target belongs to a different demo'}
    [FpUi]::OwnedWindow($target.Helper,$target.Pid);[FpUi]::OwnedWindow($target.Slot,$target.Owner)
    $p=Get-Process -Id $target.Pid -ErrorAction SilentlyContinue
    if(-not $p -or $p.StartTime -ne $target.Started -or $p.Parent.Id -ne $target.Owner -or
       -not [FpUi]::IsWindowVisible($target.Slot) -or [FpUi]::GetPropW($target.Helper,'Settings.Ready') -eq [IntPtr]::Zero){
        throw 'Settings helper generation or visible initialized input target changed'
    }
}
function Wait-SettingsState($target,[scriptblock]$predicate,$clock,[int]$ms,[string]$phase) {
    $state=$null
    do {
        Assert-SettingsTarget $target
        $next=[FpUi]::ReadSettingsState($target.Helper)
        if($next){$state=$next;if((& $predicate $state) -and $clock.ElapsedMilliseconds -lt $ms){return $state}}
        $remaining=$ms-[int]$clock.ElapsedMilliseconds
        if($remaining -gt 0){Start-Sleep -Milliseconds ([Math]::Min(5,$remaining))}
    }while($clock.ElapsedMilliseconds -lt $ms)
    $caption=[Text.StringBuilder]::new(1200);[FpUi]::GetWindowTextW($target.Helper,$caption,1200)|Out-Null
    throw "Settings $phase failed within${ms}ms; elapsed=$($clock.ElapsedMilliseconds)ms; helper=$($target.Helper)/$($target.Pid); slot=$($target.Slot); $state; caption=$caption"
}
function Settings-State($target,[int]$ms=3000) {
    Wait-SettingsState $target {param($s) $s.Have -eq 1} ([Diagnostics.Stopwatch]::StartNew()) $ms 'snapshot'
}
function Send-SettingsText($target,[string]$text,[int]$ms=3000) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $target (Input-Remaining $clock $ms)
    Send-AcknowledgedText $target.Slot $text (Input-Remaining $clock $ms)
    Wait-SettingsState $target {param($s) $s.Chars -eq $before.Chars+$text.Length} $clock $ms 'character acknowledgment'
}
function Send-SettingsKey($target,[int]$vk,[string[]]$mods=@(),[int]$ms=3000) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $target (Input-Remaining $clock $ms)
    if($mods.Count){Chord-Acknowledged $target.Slot $mods $vk (Input-Remaining $clock $ms)}
    else{Send-AcknowledgedKey $target.Slot $vk (Input-Remaining $clock $ms)}
    Wait-SettingsState $target {param($s) $s.Keys -eq $before.Keys+1} $clock $ms 'key acknowledgment'
}
function Send-SettingsClick($target,[int]$x,[int]$y,[int]$ms=3000) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $target (Input-Remaining $clock $ms)
    $xy=($x -band 0xffff) -bor (($y -band 0xffff) -shl 16)
    [void][FpUi]::Input($target.Slot,$WM_LBUTTONDOWN,1,[IntPtr]$xy,[uint32](Input-Remaining $clock $ms))
    [void][FpUi]::Input($target.Slot,0x202,0,[IntPtr]$xy,[uint32](Input-Remaining $clock $ms))
    Wait-SettingsState $target {param($s) $s.Clicks -eq $before.Clicks+1} $clock $ms 'click acknowledgment'
}
function Wait-SettingsSave($target,$before,[int]$status,$clock,[int]$ms) {
    Wait-SettingsState $target {param($s)
        if($s.Request -gt $before.Request -and $s.Completed -eq $s.Request -and $s.Pending -eq 0 -and $s.Status -ne $status){
            throw "Settings save returned status=$($s.Status), expected=$status; $s"
        }
        $s.Request -gt $before.Request -and $s.Completed -eq $s.Request -and $s.Status -eq $status -and
        $s.Pending -eq 0 -and $s.FieldSaving -eq 0 -and $s.BuilderSaving -eq 0
    } $clock $ms "save acknowledgment (status=$status, after=$($before.Request))"
}
function Invoke-SettingsSave($target,[int]$vk=0x0D,[string[]]$mods=@(),[int]$status=1,[int]$ms=2000) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $target (Input-Remaining $clock $ms)
    if($before.Pending){throw "Settings save requires the previous request to complete; $before"}
    $null=Send-SettingsKey $target $vk $mods (Input-Remaining $clock $ms)
    Wait-SettingsSave $target $before $status $clock $ms
}
function Assert-SettingsFocus($target,[string]$query,[int]$kind,[string]$id='',[int]$ms=2000) {
    $hash=[FpUi]::SettingsHash($query);$record=[FpUi]::SettingsHash($id)
    Wait-SettingsState $target {param($s)
        $s.QueryHash -eq $hash -and $s.QueryLength -eq $query.Length -and $s.FocusKind -eq $kind -and
        $s.FocusId -eq $record -and $s.FieldOpen -eq 0 -and $s.Pending -eq 0 -and $s.Builder -eq 0
    } ([Diagnostics.Stopwatch]::StartNew()) $ms "focus (query=$query, kind=$kind, id=$id)"
}
function Address-State($pane,$edit) {
    $text='<unavailable>';$selection='<unavailable>'
    try{$text=[FpUi]::ReadText($edit,50);$selection=[FpUi]::Input($edit,0xB0,0,[IntPtr]::Zero,50)}catch{}
    "actual=$text; selection=$selection; edit=$edit; visible=$([FpUi]::IsWindowVisible($edit)); enabled=$([FpUi]::ControlEnabled($edit)); request=$([FpUi]::GetPropW($pane,'Address.Request')); loading=$([FpUi]::GetPropW($pane,'Address.Loading')); resolving=$([FpUi]::GetPropW($pane,'Address.Resolving')); error=$([FpUi]::GetPropW($pane,'Address.Error')); settings=$([FpUi]::GetPropW($script:main,'Settings.Revision'))"
}
function Navigate-Verified([string]$path,[IntPtr]$pane=$script:pane0,[int]$ms=7000) {
    $limitMs=$ms;$clock=[Diagnostics.Stopwatch]::StartNew();$phase='address shortcut';$edit=[IntPtr]::Zero
    try {
        Chord-Acknowledged $pane 'Ctrl' 0x4C (Input-Remaining $clock $limitMs)
        $edit=(Pane-Edits $pane)[0];[FpUi]::OwnedWindow($edit,$script:demoPid)
        if(-not [FpUi]::IsWindowVisible($edit) -or -not [FpUi]::ControlEnabled($edit)){throw 'Address input is not visible and enabled'}
        $phase='address characters'
        Send-AcknowledgedText $edit $path (Input-Remaining $clock $limitMs)
        $actual=[FpUi]::ReadText($edit,[uint32](Input-Remaining $clock $limitMs))
        if($actual -cne $path){throw 'Acknowledged address text differs from the requested path'}
        $phase='navigation Enter'
        $request=[FpUi]::GetPropW($pane,'Address.Request').ToInt64()
        Send-AcknowledgedKey $edit 13 (Input-Remaining $clock $limitMs)
        if([FpUi]::GetPropW($pane,'Address.Request').ToInt64() -ne $request+1){throw 'The address handler did not acknowledge this navigation request'}
        $phase='navigation completion'
        $ready=Wait-Until {
            $remaining=Input-Remaining $clock $limitMs
            if([FpUi]::GetPropW($pane,'Address.Error') -ne [IntPtr]::Zero){throw 'The address handler reported a navigation error'}
            [FpUi]::ReadText($edit,[uint32]$remaining) -ceq $path -and
            [FpUi]::GetPropW($pane,'Address.Request').ToInt64() -eq $request+1 -and
            [FpUi]::GetPropW($pane,'Address.Loading') -eq [IntPtr]::Zero -and
            [FpUi]::GetPropW($pane,'Address.Resolving') -eq [IntPtr]::Zero -and
            [FpUi]::GetPropW($pane,'Address.Error') -eq [IntPtr]::Zero
        } (Input-Remaining $clock $limitMs)
        if(-not $ready){throw 'Navigation did not commit the requested folder'}
        $true
    }catch{throw "Navigation failed in $phase after$($clock.ElapsedMilliseconds)ms; requested=$path; $(Address-State $pane $edit); cause=$($_.Exception.Message)"}
}
function Send-Text($h, [string] $s) {
    foreach ($c in $s.ToCharArray()) { Post $h $WM_CHAR ([int][char]$c) }
    Start-Sleep -Milliseconds 200
}
function Click($h, [int] $x, [int] $y, [int] $msg = $WM_LBUTTONDOWN) {
    Post $h $msg 1 ((($y -band 0xFFFF) -shl 16) -bor ($x -band 0xFFFF))
    Start-Sleep -Milliseconds 200
}
function Client-Width($h) { $r = New-Object FpUi+RECT; [FpUi]::GetClientRect($h, [ref]$r) | Out-Null; $r.R }

# Clicks a pane's column header. Mirrors p_columns() in app/fpdemo.c.
function Click-Header($h, [string] $column) {
    $s = $script:scale; $w = Client-Width $h
    $x = $w - 6 * $s
    $dateX = if ($w -ge 430 * $s) { $x - 140 * $s } else { $w }
    if ($w -ge 430 * $s) { $x = $dateX - 10 * $s }
    $sizeX = $x - 80 * $s
    $typeX = if ($w -ge 580 * $s) { $sizeX - 80 * $s } else { -1 }
    $cx = switch ($column) {
        'Name' { 52 * $s } 'Type' { $typeX + 20 * $s } 'Size' { $sizeX + 40 * $s } 'Modified' { $dateX + 20 * $s }
    }
    if ($cx -lt 0) { throw "column $column is not shown at width $w" }
    Click $h ([int]$cx) ([int](50 * $s))
}

# WM_GETTEXT is marshalled across processes, unlike GetWindowText on controls.
function Window-Text($h) { $sb = New-Object Text.StringBuilder 1024; [FpUi]::SendMessageW($h, 0x0D, [IntPtr]1024, $sb) | Out-Null; $sb.ToString() }

# Edit children of a pane: [0] is the address bar, [1] the rename box when open.
function Pane-Edits($pane) {
    $list = @(); $e = [IntPtr]::Zero
    while (($e = [FpUi]::FindWindowExW($pane, $e, 'Edit', [IntPtr]::Zero)) -ne [IntPtr]::Zero) { $list += $e }
    # don't rely on z-order: the address bar is the topmost on screen
    , @($list | Sort-Object { $r = New-Object FpUi+RECT; [FpUi]::GetWindowRect($_, [ref]$r) | Out-Null; $r.T })
}

# The open popup menu: its window and the visible item texts ('-' for separators).
function Open-Menu([int] $ms = 3000, [string] $class = '') {
    if (-not (Wait-Until { ($script:menuWnd = Find-DemoMenu $class) -ne [IntPtr]::Zero } $ms)) { return $null }
    $hm = [FpUi]::SendMessageW($script:menuWnd, 0x01E1, [IntPtr]::Zero, [IntPtr]::Zero)   # MN_GETHMENU
    $items = for ($i = 0; $i -lt [FpUi]::GetMenuItemCount($hm); $i++) {
        $sb = New-Object Text.StringBuilder 256
        [FpUi]::GetMenuStringW($hm, [uint32]$i, $sb, 256, 0x400) | Out-Null       # MF_BYPOSITION
        $state = [FpUi]::GetMenuState($hm, [uint32]$i, 0x400)
        if ($state -band 0x800) { '-' } else { ($sb.ToString() -split "`t")[0] -replace '&', '' }
    }
    $sb = New-Object Text.StringBuilder 64
    [FpUi]::GetClassNameW($script:menuWnd, $sb, 64) | Out-Null
    [pscustomobject]@{ Window = $script:menuWnd; Items = @($items); Class = $sb.ToString() }
}

# Picks a menu item by its position among selectable items, using the keyboard.
function Pick-Menu($menu, [string] $text) {
    $selectable = @($menu.Items | Where-Object { $_ -ne '-' })
    $at = [array]::IndexOf($selectable, $text)
    if ($at -lt 0 -or -not [FpUi]::IsWindowVisible($menu.Window)) { return $false }
    # A real pointer move can already have highlighted a row. Start from a
    # known item and queue the key sequence together, before mouse input.
    Post $menu.Window $WM_KEYDOWN 0x24 (Key-LParam 0x24) # Home: first item
    for ($i = 0; $i -lt $at; $i++) { Post $menu.Window $WM_KEYDOWN 0x28 (Key-LParam 0x28) }
    Post $menu.Window $WM_KEYDOWN 0x0D (Key-LParam 0x0D)
    Start-Sleep -Milliseconds 300
    $true
}

function Confirm-Dialog([string] $title) {
    for ($i = 0; $i -lt 50; $i++) {
        $d = [FpUi]::FindWindowW('#32770', $title)
        if ($d -ne [IntPtr]::Zero) { Post $d $WM_COMMAND 1; return $true }   # IDOK
        Start-Sleep -Milliseconds 50
    }
    return $false
}

function Wait-Until([scriptblock] $cond, [int] $ms = 3000) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.ElapsedMilliseconds -lt $ms) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 25 }
    return [bool](& $cond)
}
