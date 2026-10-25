<# On-demand Windows / native WSL folder sizing through the real app. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class SizeProbe {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern IntPtr CreateFileW(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\folder_sizes'
New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command){$value=& wsl.exe --exec sh -c $command;if($LASTEXITCODE){throw 'Private WSL fixture failed'};($value -join "`n").TrimEnd()}
function Dialog {[FpUi]::FindWindowW('OpenPilotFolderSize','Folder size')}
function Field([int]$id){[SizeProbe]::GetDlgItem($script:dialog,$id)}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"FolderSize.$name").ToInt64()}
# Wait for command acceptance before Ready can observe its Busy/result state.
function Press([int]$id){[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]$id,[IntPtr]::Zero)|Out-Null}
function Ready {Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and (Prop 'Busy') -eq 0 -and (Prop 'State') -gt 1} 7000}
function Close-Size {Press 2;Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000}
function Select-First {Click $pane0 ([int](65*$scale)) ([int](74*$scale));Post $pane0 0x202 0 0}
function Navigate([string]$path){
    Chord $pane0 'Ctrl' 0x4C 20;Send-Text (Pane-Edits $pane0)[0] $path;Key (Pane-Edits $pane0)[0] 0x0D 20
    Wait-Until {(Window-Text (Pane-Edits $pane0)[0]) -ceq $path -and [FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000
}
function Open-Size {
    Select-First;Chord $pane0 @('Ctrl','Alt') 0x5A 0
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)){throw 'Folder size window did not open'}
}
function Folder-Menu {
    $r=[FpUi+RECT]::new();[FpUi]::GetWindowRect($pane0,[ref]$r)|Out-Null
    Post $pane0 $WM_CONTEXTMENU $pane0 ((($r.B-45) -shl 16) -bor ($r.L+40));Open-Menu
}
$scratch=Join-Path $env:TEMP "openpilot_sizes_$PID";$linux="/tmp/openpilot-sizes-$PID";$p=$null;$locked=[IntPtr]::Zero
try{
    $view="$scratch\view";$target="$view\target"
    New-Item -ItemType Directory -Force "$target\empty","$target\nested","$scratch\largeview\large"|Out-Null
    [IO.File]::WriteAllBytes("$target\plain",[byte[]](1,2,3));[IO.File]::WriteAllBytes("$target\nested\inside",[byte[]](1,2,3,4,5))
    [IO.File]::WriteAllBytes("$target\.hidden",[byte[]](1,2));[IO.File]::SetAttributes("$target\.hidden",[IO.FileAttributes]::Hidden)
    $settings="$scratch\settings.ini";$baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n[keys]`nfile.folder_size = Ctrl+Alt+Z`n"
    Set-Content -LiteralPath $settings -Value $baseSettings
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/view/target/nested' '$linux/view/target/empty' '$linux/view/target/denied'; printf abc > '$linux/view/target/plain'; printf 12345 > '$linux/view/target/nested/inside'; printf xx > '$linux/view/target/.hidden'; printf private > '$linux/view/target/denied/secret'; ln -s . '$linux/view/target/loop'; mkfifo '$linux/view/target/fifo'"|Out-Null
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\sizes-profile.csv",$view,$view)
    Check 'ordinary browsing starts without a size window' ((Dialog) -eq [IntPtr]::Zero)
    Check 'Windows fixture finishes loading' (Wait-Until {[FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000)
    Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'selected folder offers Calculate folder size' ($menu -and $menu.Items -contains 'Calculate folder size')
    if(-not $menu -or -not(Pick-Menu $menu 'Calculate folder size')){throw 'Folder size action missing'}
    Check 'Windows scan completes in the background' (Ready)
    Check 'Windows totals include hidden files and the root folder' ((Prop 'State') -eq 2 -and (Prop 'Bytes') -eq 10 -and (Prop 'Files') -eq 3 -and (Prop 'Folders') -eq 3 -and (Prop 'Errors') -eq 0)
    Check 'logical size and live-metadata rules are visible' ((Window-Text (Field 103)) -match '10 bytes' -and (Window-Text (Field 106)) -match 'Recalculate')
    Check 'completed controls offer Recalculate and disable Cancel' ([SizeProbe]::IsWindowEnabled((Field 109)) -and -not[SizeProbe]::IsWindowEnabled((Field 108)))
    Check 'main pane can navigate with the result open' (Navigate $scratch)
    Check 'navigation does not change the measured path' ((Window-Text (Field 101)) -eq $target)
    [IO.File]::WriteAllBytes("$target\new",[byte[]](1,2,3,4));Press 109
    Check 'Recalculate reads new metadata without a cached total' ((Ready) -and (Prop 'Bytes') -eq 14 -and (Prop 'Files') -eq 4)
    $locked=[SizeProbe]::CreateFileW("$target\nested",[uint32]2147483648,0,[IntPtr]::Zero,3,0x02000000,[IntPtr]::Zero)
    if($locked -eq [IntPtr](-1)){throw 'Could not lock the private nested folder'}
    Press 109;Check 'unreadable Windows subtree retains explicitly partial totals' ((Ready) -and (Prop 'State') -eq 3 -and (Prop 'Bytes') -eq 9 -and (Prop 'Errors') -eq 1 -and (Window-Text (Field 102)) -match 'Partial')
    [SizeProbe]::CloseHandle($locked)|Out-Null;$locked=[IntPtr]::Zero
    Press 109;Check 'recalculating after the error restores a complete total' ((Ready) -and (Prop 'State') -eq 2 -and (Prop 'Bytes') -eq 14)
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '01_windows_complete'}finally{$script:main=$savedMain}
    Check 'size window closes cleanly' (Close-Size)
    Key $tree 0x24 20;Post $tree $WM_CONTEXTMENU $tree.ToInt64() -1;$menu=Open-Menu
    Check 'tree folders offer the size action' ($menu -and $menu.Items -contains 'Calculate folder size')
    if($menu){Key $menu.Window 0x1B 20}
    Check 'current-folder context fixture opens' (Navigate $target)
    $menu=Folder-Menu;Check 'empty-space menu measures the current folder' ($menu -and $menu.Items -contains 'Calculate folder size')
    if($menu){Pick-Menu $menu 'Calculate folder size'|Out-Null}
    Check 'empty-space calculation uses the current folder' ((Ready) -and (Window-Text (Field 101)) -eq $target -and (Prop 'Bytes') -eq 14)
    Close-Size|Out-Null
    Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'folder action remains available alongside existing actions' ($menu -and $menu.Items -contains 'Calculate folder size' -and $menu.Items -contains 'Properties')
    if($menu){Key $menu.Window 0x1B 20}
    Chord $pane0 'Ctrl' 0x41 20;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'multiple selection does not measure an unintended current folder' ($menu -and $menu.Items -notcontains 'Calculate folder size')
    if($menu){Key $menu.Window 0x1B 20}
    if(-not(Navigate "$target\nested")){throw 'Single file fixture did not open'}
    Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'regular files do not offer the folder-only action' ($menu -and $menu.Items -notcontains 'Calculate folder size')
    if($menu){Key $menu.Window 0x1B 20}
    Check 'WSL fixture opens' (Navigate "$unc\view");Open-Size
    Check 'native WSL scan completes with links and special files excluded' ((Ready) -and (Prop 'State') -eq 2 -and (Prop 'Bytes') -eq 17 -and (Prop 'Files') -eq 4 -and (Prop 'Folders') -eq 4 -and (Prop 'Skipped') -eq 2)
    Check 'native WSL user choice is visible' ((Window-Text (Field 107)) -eq 'WSL user: distro default')
    Native "chmod 000 '$linux/view/target/denied'"|Out-Null;Press 109
    Check 'unreadable Linux subtree keeps measurable bytes and reports partial' ((Ready) -and (Prop 'State') -eq 3 -and (Prop 'Bytes') -eq 10 -and (Prop 'Errors') -eq 1 -and (Window-Text (Field 106)) -match 'First error')
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '02_wsl_partial'}finally{$script:main=$savedMain}
    Native "chmod 700 '$linux/view/target/denied'"|Out-Null
    Close-Size|Out-Null
    # One owned large tree makes Cancel deterministic without any product delay.
    $maker=@"
import os
p='$linux/largeview/large'
os.makedirs(p)
for i in range(600):
    d=p+'/d'+str(i)
    os.mkdir(d)
    for j in range(30):
        open(d+'/f'+str(j),'wb').close()
"@
    & wsl.exe --exec python3 -c $maker
    if($LASTEXITCODE){throw 'Cancellation fixture creation failed'}
    Check 'cancellation fixture opens' (Navigate "$unc\largeview");Open-Size;Press 108
    Check 'Cancel keeps labelled incomplete totals and enables Recalculate' ((Ready) -and (Prop 'State') -eq 4 -and (Window-Text (Field 102)) -match 'Cancelled' -and [SizeProbe]::IsWindowEnabled((Field 109)))
    Press 109;Press 2
    Check 'closing a running scan cancels and releases the window' (Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000)
    Check 'private settings watcher is ready before disabling the function' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 3000)
    Set-Content -LiteralPath $settings -Value ($baseSettings.Replace('[functions]',"[functions]`nfile.folder_size = off"));Start-Sleep -Milliseconds 500
    Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'disabled function disappears from the folder context menu' ($menu -and $menu.Items -notcontains 'Calculate folder size')
    if($menu){Key $menu.Window 0x1B 20}
    Check 'app closes without waiting on a background scan' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\sizes-profile.csv" -Raw
    Check 'folder size work has no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'folder size work keeps UI dispatch below 16 ms' ($match.Success -and [double]$match.Groups[1].Value -lt 16)
}finally{
    if($locked -ne [IntPtr]::Zero -and $locked -ne [IntPtr](-1)){[SizeProbe]::CloseHandle($locked)|Out-Null}
    if($p){Stop-Demo $p|Out-Null}
    Native "chmod -R u+rwX '$linux'; rm -rf '$linux'"|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
