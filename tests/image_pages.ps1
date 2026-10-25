<# TIFF page acceptance through real viewer input and the restricted decoder. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class PageProbe {
 [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr process);
 [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr process);
 [DllImport("user32.dll")] public static extern bool IsClipboardFormatAvailable(uint format);
 [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")] static extern bool CloseClipboard();
 [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint format);
 [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
 [DllImport("kernel32.dll")] static extern UIntPtr GlobalSize(IntPtr h);
 // Dimensions and pixels must describe one clipboard read. A later open can
 // fail transiently or observe a different copy, even after pixels succeeded.
 public static int[] Snapshot(int x,int y) {
  if(!OpenClipboard(IntPtr.Zero))return new int[0];
  try {
   var memory=GetClipboardData(17);if(memory==IntPtr.Zero)return new int[0];
   ulong bytes=GlobalSize(memory).ToUInt64();if(bytes<124)return new int[0];
   var p=GlobalLock(memory);if(p==IntPtr.Zero)return new int[0];
   try {
    int header=Marshal.ReadInt32(p,0),w=Marshal.ReadInt32(p,4),signedHeight=Marshal.ReadInt32(p,8);
    if(header!=124||w<=0||signedHeight==0||signedHeight==int.MinValue||
       Marshal.ReadInt16(p,12)!=1||Marshal.ReadInt16(p,14)!=32||Marshal.ReadInt32(p,16)!=3||
       Marshal.ReadInt32(p,40)!=0x00ff0000||Marshal.ReadInt32(p,44)!=0x0000ff00||
       Marshal.ReadInt32(p,48)!=0x000000ff||Marshal.ReadInt32(p,52)!=unchecked((int)0xff000000))return new int[0];
    int h=Math.Abs(signedHeight);
    if(x<0||x>=w||y<0||y>=h||124+(ulong)w*(ulong)h*4>bytes)return new int[0];
    int row=signedHeight<0?y:h-1-y;
    ulong offset=124+((ulong)row*(ulong)w+(ulong)x)*4;
    if(offset>int.MaxValue-3)return new int[0];
    int off=(int)offset;
    return new[]{w,h,(int)Marshal.ReadByte(p,off),(int)Marshal.ReadByte(p,off+1),(int)Marshal.ReadByte(p,off+2),(int)Marshal.ReadByte(p,off+3)};
   }finally{GlobalUnlock(memory);}
  }finally{CloseClipboard();}
 }
}
"@
. "$PSScriptRoot\uilib.ps1"
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\image_pages';New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0;$p=$null;$linux=$null;$pausedHelper=$null
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Image {[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)}
function Title {Window-Text (Image)}
function Prop([string]$name){[FpUi]::GetPropW((Image),"Image.$name").ToInt64()}
function Ready([int]$page,[string]$pattern='pages.tiff') {Wait-Until {[FpUi]::IsWindowVisible((Image)) -and (Prop 'Page') -eq $page -and (Prop 'PageState') -eq 0 -and (Title) -match $pattern -and (Title) -notmatch 'Loading|unavailable'} 7000}
function Navigate([string]$path,[IntPtr]$pane=$script:pane0){Chord $pane 'Ctrl' 0x4C 20;$edit=(Pane-Edits $pane)[0];Send-Text $edit $path;Key $edit 0x0D 20;Wait-Until {(Window-Text $edit) -eq $path -and [FpUi]::GetPropW($pane,'Address.Loading').ToInt64() -eq 0} 7000}
function Next {Chord (Image) @('Ctrl','Shift') 0x22 0}
function Previous {Chord (Image) @('Ctrl','Shift') 0x21 0}
function Helpers {@(Get-Process folderwing-image -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid})}
function Copy-Pixels {
    $script:copied=@();$script:pixel=@()
    Set-Clipboard 'waiting for page';Chord (Image) @('Ctrl','Shift') 0x43 20
    $ready=Wait-Until {($script:copied=[PageProbe]::Snapshot(0,0)).Count -eq 6} 3000
    if($ready){$script:pixel=$script:copied[2..5]}
    $ready
}
function Native([string]$command){$v=& wsl.exe --exec sh -c $command;if($LASTEXITCODE){throw "Private WSL fixture failed: $command"};($v -join "`n").TrimEnd()}
$scratch=Join-Path $env:TEMP "openpilot_image_pages_$PID"
try {
    foreach($dir in @('main','oversize','corrupt','empty','single')){New-Item -ItemType Directory -Force "$scratch\$dir"|Out-Null}
    $fixtures="$root\tests\fixtures\images"
    Copy-Item "$fixtures\pages.tiff" "$scratch\main\01-pages.tiff";Copy-Item "$fixtures\alpha.png" "$scratch\main\02-next.png"
    Copy-Item "$fixtures\page_oversize.tiff" "$scratch\oversize\page_oversize.tiff";Copy-Item "$fixtures\page_corrupt.tiff" "$scratch\corrupt\page_corrupt.tiff"
    Copy-Item "$fixtures\oriented.tiff" "$scratch\single\single.tiff"
    $settings="$scratch\settings.ini";$base="[functions]`npreview.text = off`npreview.pdf = off`n[preview.image]`nlinger = 0`n"
    Set-Content -LiteralPath $settings -Value $base
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","--profile-out=$out\pages-profile.csv","$scratch\main","$scratch\empty")
    Check 'multipage TIFF starts on its first decoded page' (Ready 1 '01-pages.tiff.*16 x 8.*Page 1 / 3') (Title)
    Check 'page count and folder count are independent' ((Prop 'PageCount') -eq 3 -and (Prop 'Count') -eq 2 -and (Prop 'Position') -eq 1)
    Check 'first page disables Previous page and enables Next page' ((Prop 'PreviousPage') -eq 0 -and (Prop 'NextPage') -eq 1)
    $helper=(Helpers)[0];$viewer=Image;$hash=(Get-FileHash "$scratch\main\01-pages.tiff").Hash
    Next;Check 'page shortcut loads the oriented second page with different dimensions' (Ready 2 '6 x 10.*Page 2 / 3') (Title)
    Check 'page changes keep the same viewer, helper, file and pane selection' ((Image) -eq $viewer -and (Helpers).Count -eq 1 -and (Helpers)[0].Id -eq $helper.Id -and (Prop 'Position') -eq 1 -and (Prop 'Count') -eq 2)
    Check 'second page pixels copy successfully' (Copy-Pixels)
    Check 'copied dimensions belong to the selected oriented page' (($script:copied[0..1] -join ',') -eq '6,10')
    Check 'copied pixels are the green second page' ($script:pixel[1] -gt 150 -and $script:pixel[2] -lt 80 -and $script:pixel[3] -eq 255) ($script:pixel -join ',')
    Chord (Image) 'Ctrl' 0x31 20;Next
    Check 'exact zoom stays at 100 percent on the third page' (Ready 3 '8 x 12.*100%.*Zoom.*Page 3 / 3')
    Check 'last page disables Next and retains Previous' ((Prop 'NextPage') -eq 0 -and (Prop 'PreviousPage') -eq 1)
    $last=Title;Next;Start-Sleep -Milliseconds 120;Check 'TIFF page navigation does not wrap' ((Title) -eq $last)
    Check 'third page pixels copy successfully' (Copy-Pixels)
    Check 'third page resets its colour profile and copies blue pixels' ($script:pixel[0] -eq 240 -and $script:pixel[2] -eq 20 -and ($script:copied[0..1] -join ',') -eq '8,12') ("BGRA="+($script:pixel -join ',')+" dimensions="+($script:copied[0..1] -join ','))
    Chord (Image) 'Ctrl' 0x32 20;Previous;Check 'Fit width persists when page dimensions change' (Ready 2 'Fit width')
    Chord (Image) 'Ctrl' 0x30 20;Previous;Check 'Fit persists when returning to page one' (Ready 1 'Fit')
    Click (Image) ([int](170*$scale)) ([int](93*$scale));Post (Image) 0x202 0 0
    Check 'Next page header button selects the second page' (Ready 2)
    Click (Image) ([int](60*$scale)) ([int](93*$scale));Post (Image) 0x202 0 0
    Check 'Previous page header button selects the first page' (Ready 1)
    Next;Post (Image) $WM_CONTEXTMENU (Image).ToInt64() -1;$menu=Open-Menu
    Check 'context menu exposes TIFF page commands separately from folder commands' ($menu -and $menu.Items -contains 'Previous image page' -and $menu.Items -contains 'Next image page' -and $menu.Items -contains 'Next image' -and $menu.Items -notcontains 'Play / pause animation');if($menu){Key $menu.Window 0x1B 20}
    Chord (Image) @('Ctrl','Shift') 0x27 20;Start-Sleep -Milliseconds 100
    Check 'animation shortcut does not change a TIFF page' (Ready 2)
    $savedMain=$script:main;try{$script:main=Image;Shot $out '01_tiff_page_controls'}finally{$script:main=$savedMain}
    Chord (Image) 'Ctrl' 0x22 20
    Check 'folder shortcut moves to the next file instead of the next TIFF page' (Wait-Until {(Title) -match '02-next.png.*16 x 8' -and (Prop 'PageCount') -eq 0 -and (Prop 'Position') -eq 2} 7000)
    Check 'single image disables both page directions' ((Prop 'PreviousPage') -eq 0 -and (Prop 'NextPage') -eq 0)
    Chord (Image) 'Ctrl' 0x21 20;Check 'returning from folder browsing opens TIFF at page one' (Ready 1)
    $pausedHelper=(Helpers)[0];Check 'owned helper suspends for a page/copy overlap fixture' ([PageProbe]::NtSuspendProcess($pausedHelper.Handle) -eq 0)
    Set-Clipboard 'cancelled page copy';Chord (Image) @('Ctrl','Shift') 0x43 0;Next;Previous;Next;Next;Previous;Next
    Check 'pending page choice is truthful before decoder completion' ((Prop 'PageRequested') -eq 3 -and (Prop 'Page') -eq 0 -and (Prop 'PageState') -eq 1 -and (Title) -match 'Loading page') (Title)
    [PageProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null;$pausedHelper=$null
    Check 'rapid navigation displays only the latest requested page' (Ready 3)
    Check 'cancelled copy cannot publish pixels from an obsolete page' (-not [PageProbe]::IsClipboardFormatAvailable(17))
    Previous;Check 'second page remains navigable after overlap' (Ready 2)
    $pausedHelper=(Helpers)[0];[PageProbe]::NtSuspendProcess($pausedHelper.Handle)|Out-Null;Next
    Chord (Image) 'Ctrl' 0x22 0
    [PageProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null;$pausedHelper=$null
    Check 'late TIFF page response cannot replace a newly selected PNG' (Wait-Until {(Title) -match '02-next.png' -and (Prop 'PageCount') -eq 0 -and (Prop 'Position') -eq 2} 7000)
    Chord (Image) 'Ctrl' 0x21 20;Check 'TIFF reopens after stale response rejection' (Ready 1)
    Check 'settings watcher is ready' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Set-Content -LiteralPath $settings -Value ($base+"[functions]`nimage.next_page = off`n")
    Check 'disabling page navigation dims its native control' (Wait-Until {(Prop 'NextPage') -eq 0} 5000)
    Next;Click (Image) ([int](170*$scale)) ([int](93*$scale));Post (Image) 0x202 0 0
    Check 'disabled page function is inert through keys and header button' (Ready 1)
    Set-Content -LiteralPath $settings -Value ($base+"[keys]`nimage.next_page = Ctrl+Alt+P`n")
    Check 'reenabling the page function updates its control' (Wait-Until {(Prop 'NextPage') -eq 1} 5000)
    Next;Start-Sleep -Milliseconds 120;Check 'custom binding replaces the default page shortcut' (Ready 1)
    Chord (Image) @('Ctrl','Alt') 0x50 20;Check 'custom shortcut navigates through normal function dispatch' (Ready 2)
    Set-Content -LiteralPath $settings -Value ($base+"[functions]`nimage.next_page = off`n")
    Check 'custom page binding is disabled before restoring defaults' (Wait-Until {(Prop 'NextPage') -eq 0} 5000)
    Set-Content -LiteralPath $settings -Value $base
    Check 'default page binding reload completes before error fixtures' (Wait-Until {(Prop 'NextPage') -eq 1} 5000)
    Check 'oversized-page fixture opens'  (Navigate "$scratch\oversize");Check 'first page of oversized fixture decodes' (Ready 1 'page_oversize.tiff')
    Next;Check 'an oversized page displays its own error and keeps navigation available' (Wait-Until {(Title) -match 'Page unavailable.*Page 2 / 3' -and (Prop 'PageRequested') -eq 2 -and (Prop 'PageState') -eq -1 -and (Prop 'NextPage') -eq 1} 7000) (Title)
    Check 'a bad later page does not remove the TIFF from folder browsing' ((Prop 'Count') -eq 1 -and (Prop 'Position') -eq 1)
    Next;Check 'navigation recovers on the page after an oversized page' (Ready 3 'page_oversize.tiff.*8 x 12')
    Previous;Check 'oversized page error can be revisited' (Wait-Until {(Prop 'PageRequested') -eq 2 -and (Prop 'PageState') -eq -1} 7000)
    Previous;Check 'navigation recovers backwards from an oversized page' (Ready 1 'page_oversize.tiff')
    Check 'corrupt-page fixture opens' (Navigate "$scratch\corrupt");Check 'first page of corrupt fixture decodes' (Ready 1 'page_corrupt.tiff')
    Next;Check 'a corrupt pixel page reports an error for that page only' (Wait-Until {(Title) -match 'Page unavailable.*Page 2 / 3' -and (Prop 'PageState') -eq -1 -and (Prop 'Count') -eq 1} 7000) (Title)
    Next;Check 'navigation can decode the good page after corrupt pixels' (Ready 3 'page_corrupt.tiff')
    Check 'single-page TIFF folder opens' (Navigate "$scratch\single");Check 'single-page TIFF shows its oriented dimensions' (Ready 1 'single.tiff.*8 x 16')
    Check 'single-page TIFF has no page navigation controls' ((Prop 'PageCount') -eq 1 -and (Prop 'NextPage') -eq 0 -and (Prop 'PreviousPage') -eq 0)
    Check 'page navigation leaves the source TIFF unchanged' ((Get-FileHash "$scratch\main\01-pages.tiff").Hash -eq $hash)
    Check 'main page test app closes cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\pages-profile.csv" -Raw
    Check 'page navigation has no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $m=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,');Check 'page navigation keeps UI dispatch below 16 ms' ($m.Success -and [double]$m.Groups[1].Value -lt 16) $m.Value
    $m=[regex]::Match($report,'zone,image.paint,\d+,[\d.]+,([\d.]+),,');Check 'TIFF page painting stays below 16 ms' ($m.Success -and [double]$m.Groups[1].Value -lt 16) $m.Value
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","$scratch\empty","$scratch\main")
    Post $pane1 7 0;Key $pane1 0x24 0;Check 'right-pane TIFF source opens' (Ready 1 '01-pages.tiff')
    Next;Check 'right-pane TIFF supports page navigation' (Ready 2)
    Check 'page navigation retains the right source pane' ([FpUi]::IsWindowVisible($pane1) -and -not [FpUi]::IsWindowVisible($pane0))
    $pausedHelper=(Helpers)[0];[PageProbe]::NtSuspendProcess($pausedHelper.Handle)|Out-Null;Next
    Check 'a pending-page shutdown fixture has an outstanding request' ((Prop 'PageState') -eq 1)
    Check 'right-pane app closes cleanly with a suspended pending page' (Stop-Demo $p);$p=$null
    [PageProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null;$pausedHelper=$null
    $linux="/tmp/openpilot-image-pages-$PID";$distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux'; cp /mnt/c/Work/openpilot/tests/fixtures/images/pages.tiff '$linux/pages:case.tiff'"|Out-Null
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","$unc","$scratch\empty")
    Check 'Linux TIFF with reserved punctuation opens' (Ready 1 'pages:case.tiff')
    Next;Check 'Linux TIFF page changes retain literal filename and orientation' (Ready 2 'pages:case.tiff.*6 x 10')
    Key (Image) 0x1B 20;Check 'Escape closes the page viewer and restores the panes' (Wait-Until {-not [FpUi]::IsWindowVisible((Image)) -and [FpUi]::IsWindowVisible($pane1)} 3000)
    Check 'Linux page app closes cleanly' (Stop-Demo $p);$p=$null
} finally {
    if($pausedHelper){[PageProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null}
    if($p){Stop-Demo $p|Out-Null}
    if($linux){Native "rm -rf '$linux'"|Out-Null}
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
