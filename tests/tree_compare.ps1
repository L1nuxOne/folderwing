<# Real-app recursive comparison, reviewed copy staging and native WSL. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class TreeProbe {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr p,IntPtr a,UIntPtr n,uint type,uint protect);
 [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr written);
 [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr p,IntPtr a,UIntPtr n,uint type);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,UIntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
 public static double MaxNavigationMs; public static int NavigationSamples;
 public static void NavigationSample(double elapsed) {
  MaxNavigationMs=Math.Max(MaxNavigationMs,elapsed);NavigationSamples++;
 }
 public static bool Select(IntPtr list,uint pid,int index,bool keep=false) {
  IntPtr p=OpenProcess(0x28,false,pid);if(p==IntPtr.Zero)return false;
  IntPtr a=VirtualAllocEx(p,IntPtr.Zero,(UIntPtr)128,0x3000,4);
  try {
   if(a==IntPtr.Zero)return false;byte[] b=new byte[128];UIntPtr written;
   Array.Copy(BitConverter.GetBytes((uint)3),0,b,16,4);
   if(!WriteProcessMemory(p,a,b,(UIntPtr)b.Length,out written))return false;
   if(!keep)SendMessageW(list,0x102b,(IntPtr)(-1),a);
   Array.Copy(BitConverter.GetBytes((uint)3),0,b,12,4);
   if(!WriteProcessMemory(p,a,b,(UIntPtr)b.Length,out written))return false;
   return SendMessageW(list,0x102b,(IntPtr)index,a)!=IntPtr.Zero;
  } finally {if(a!=IntPtr.Zero)VirtualFreeEx(p,a,UIntPtr.Zero,0x8000);CloseHandle(p);}
 }
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\tree_compare';New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$cmd){$v=& wsl.exe --exec sh -c $cmd;if($LASTEXITCODE){throw 'Private native comparison fixture failed'};($v -join "`n").TrimEnd()}
function Dialog {
    $after=[IntPtr]::Zero
    while(($after=[FpUi]::FindWindowExW([IntPtr]::Zero,$after,'OpenPilotFolderCompare',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        [uint32]$ownerPid=0;[FpUi]::GetWindowThreadProcessId($after,[ref]$ownerPid)|Out-Null
        if($ownerPid -eq $p.Id){return $after}
    }
    [IntPtr]::Zero
}
function Field([int]$id){[TreeProbe]::GetDlgItem($script:dialog,$id)}
function Enabled([int]$id){[TreeProbe]::IsWindowEnabled((Field $id))}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"FolderCompare.$name").ToInt64()}
function Press([int]$id){[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]$id,[IntPtr]::Zero)|Out-Null}
function Ready {Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and (Prop 'Done') -eq 1 -and (Prop 'Busy') -eq 0} 7000}
function Open-Compare([IntPtr]$pane=$pane0){Chord $pane @('Ctrl','Shift') 0x76 0;if(-not(Ready)){throw 'Comparison did not finish'}}
function Close-Compare {Press 2;Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000}
function Scan {Press 104;Ready}
function Hashes([bool]$on){[FpUi]::SendMessageW((Field 103),0xF1,[IntPtr][int]$on,[IntPtr]::Zero)|Out-Null}
function Select-Name([string]$text){for($i=0;$i -lt (Prop 'Count');$i++){if(-not[TreeProbe]::Select((Field 106),$p.Id,$i)){throw 'Owned virtual list selection failed'};if((Window-Text (Field 108)).Contains("Left: $text     ") -or (Window-Text (Field 108)).Contains("Right: $text`r`n")){return $true}};$false}
function Navigate([IntPtr]$pane,[string]$path){
    $before=[FpInput]::KeySamples
    $ready=Navigate-Verified $path $pane 7000
    if([FpInput]::KeySamples -ne $before+1){throw 'Navigation did not retain exactly one acknowledged Enter sample'}
    [TreeProbe]::NavigationSample([FpInput]::LastKeyMs)
    $ready
}
function Slot {$side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero);[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)}
function Snapshot {Chord (Slot) 'Ctrl' 0x43 30;Get-Clipboard -Raw}
function Pending {Wait-Until {(Snapshot) -match 'Ready to start'} 3000}
function Finish {Key (Slot) 0x0D 0;Wait-Until {(Snapshot) -match 'Completed|Finished with errors'} 7000}

function Recursive([bool]$on){[FpUi]::SendMessageW((Field 111),0xF1,[IntPtr][int]$on,[IntPtr]::Zero)|Out-Null}
function Tree-Scan {Recursive $true;Scan}
function Select-Many([string[]]$names){
    $indices=@();foreach($name in $names){
        $found=$false
        for($i=0;$i -lt (Prop 'Count');$i++){
            [TreeProbe]::Select((Field 106),$p.Id,$i)|Out-Null
            if((Window-Text (Field 108)).Contains("Left: $name     ")){$indices+=,$i;$found=$true;break}
        };if(-not $found){return $false}
    }
    [TreeProbe]::Select((Field 106),$p.Id,$indices[0])|Out-Null
    foreach($i in $indices[1..($indices.Count-1)]){[TreeProbe]::Select((Field 106),$p.Id,$i,$true)|Out-Null};$true
}
$scratch=Join-Path $env:TEMP "openpilot_tree_$PID";$left="$scratch\left";$right="$scratch\right";$linux="/tmp/openpilot-tree-$PID";$p=$null
$owned=Get-Content "$out\test_distro.json" -Raw|ConvertFrom-Json
if(-not $owned.created -or $owned.name -notmatch '^OpenPilotCrossTest-[0-9a-f]{12}$'){throw 'Owned cross-distro fixture is required'}
function Other([string]$cmd){$v=& wsl.exe -d $owned.name --exec /bin/busybox sh -c $cmd;if($LASTEXITCODE){throw 'Owned second-distro tree fixture failed'};($v -join "`n").TrimEnd()}
try{
    New-Item -ItemType Directory -Force "$left\a\deep","$left\b","$left\conflict","$right\a\deep","$right\rightonly","$scratch\elsewhere"|Out-Null
    [IO.File]::WriteAllText("$left\a\deep\pick.txt",'first');[IO.File]::WriteAllText("$left\b\pick.txt",'second')
    [IO.File]::WriteAllText("$left\b\unselected.txt",'leave');[IO.File]::WriteAllText("$left\conflict\secret",'leave');[IO.File]::WriteAllText("$right\conflict",'type')
    [IO.File]::WriteAllText("$right\rightonly\return.txt",'return')
    foreach($side in @($left,$right)){[IO.File]::WriteAllText("$side\a\deep\same.txt",'xxx');[IO.File]::WriteAllText("$side\a\deep\changed.txt",'xxx');[IO.File]::SetLastWriteTimeUtc("$side\a\deep\same.txt",[datetime]'2024-01-01Z')}
    [IO.File]::WriteAllText("$right\a\deep\changed.txt",'abc')
    $long=(@('segment1234567890')*18)-join '\';New-Item -ItemType Directory -Force "$left\$long"|Out-Null;[IO.File]::WriteAllText("$left\$long\long.txt",'long')
    $settings="$scratch\settings.ini";$baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n";Set-Content $settings $baseSettings
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))";$otherUnc="\\wsl.localhost\$($owned.name)$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/native/deep'; printf linux > '$linux/native/deep/from.txt'; printf literal > '$linux/native/deep/a:b'; ln -s / '$linux/link'; mkfifo '$linux/fifo'"|Out-Null
    Other "mkdir -p '$linux'"|Out-Null
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\tree-profile.csv",$left,$right)
    Check 'initial pane snapshots settle' (Wait-Until {[FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0 -and [FpUi]::GetPropW($pane1,'Address.Loading').ToInt64() -eq 0} 7000)
    Open-Compare
    Check 'current-level option is preserved' ((Prop 'Recursive') -eq 0 -and -not(Select-Name 'a\deep\pick.txt'))
    Hashes $true
    Check 'recursive scan finishes with relative descendants' ((Tree-Scan) -and (Prop 'Recursive') -eq 1 -and (Prop 'Error') -eq 0 -and (Select-Name 'a\deep\pick.txt'))
    Check 'nested same-size differences use SHA-256' ((Select-Name 'a\deep\changed.txt') -and (Prop 'SelectedState') -eq 6 -and (Prop 'Equal') -eq 1)
    Check 'folder groups describe scanned descendants' ((Select-Name 'a') -and (Window-Text (Field 108)) -match 'Folder group.*descendants')
    Check 'type conflicts exclude their entire subtree' ((Prop 'Types') -eq 1 -and -not(Select-Name 'conflict\secret'))
    Check 'paths beyond one filename remain visible' (Select-Name "$long\long.txt")
    Check 'two same-name files in different parents can be selected' (Select-Many @('a\deep\pick.txt','b\pick.txt'));Press 109
    Check 'nested copy awaits Start and creates no parents during review' ((Pending) -and -not(Test-Path "$right\b") -and -not(Test-Path "$right\a\deep\pick.txt"))
    Check 'review describes one job with both selected files' ((Snapshot) -match '0/2 items processed')
    Check 'one copy job preserves both relative destinations' ((Finish) -and [IO.File]::ReadAllText("$right\a\deep\pick.txt") -eq 'first' -and [IO.File]::ReadAllText("$right\b\pick.txt") -eq 'second')
    Check 'unselected siblings remain absent' (-not(Test-Path "$right\b\unselected.txt") -and -not(Test-Path "$right\pick.txt"))
    Check 'rescan verifies copied nested bytes' ((Scan) -and (Select-Name 'b\pick.txt') -and (Prop 'SelectedState') -eq 7)
    Check 'reverse nested copy can be selected' (Select-Name 'rightonly\return.txt');Press 110
    Check 'reverse copy awaits Start' ((Pending) -and -not(Test-Path "$left\rightonly"))
    Check 'reverse copy creates missing parents under the left root' ((Finish) -and [IO.File]::ReadAllText("$left\rightonly\return.txt") -eq 'return')
    $saved=$script:main;try{$script:main=$script:dialog;Shot $out '01_recursive'}finally{$script:main=$saved}
    Check 'recursive window closes' (Close-Compare)
    Check 'WSL pane loads for nested transfers' (Navigate $pane1 $unc)
    Open-Compare;Hashes $true
    Check 'mixed tree scan excludes links and special items' ((Tree-Scan) -and (Prop 'Excluded') -eq 2 -and (Prop 'Error') -eq 0)
    Select-Name 'native\deep\from.txt'|Out-Null;Press 110
    Check 'nested WSL to Windows transfer is reviewed' ((Pending) -and -not(Test-Path "$left\native"))
    Check 'nested WSL bytes arrive under their original parents' ((Finish) -and [IO.File]::ReadAllText("$left\native\deep\from.txt") -eq 'linux')
    Select-Name 'native\deep\a:b'|Out-Null;Press 110
    Check 'Linux names unsafe on Windows fail before any file copy' ((Pending) -and (& {Key (Slot) 0x0D 0;Wait-Until {(Snapshot) -match 'Could not start'} 3000}) -and -not(Test-Path "$left\native\deep\a"))
    Select-Name 'b\pick.txt'|Out-Null;Press 109
    Check 'nested Windows to WSL copy waits for Start' ((Pending) -and (Native "test ! -e '$linux/b' && echo absent") -eq 'absent')
    Check 'Windows file reaches its native relative parent' ((Finish) -and (Native "cat '$linux/b/pick.txt'") -eq 'second')
    Close-Compare|Out-Null
    Check 'native source pane loads' (Navigate $pane0 $unc)
    Check 'owned other distro loads' (Navigate $pane1 $otherUnc)
    Open-Compare;Hashes $true
    Check 'cross-distro recursive comparison finishes' ((Tree-Scan) -and (Prop 'Error') -eq 0 -and (Select-Name 'native\deep\a:b'))
    Press 109
    Check 'cross-distro nested copy awaits Start' ((Pending) -and (Other "test ! -e '$linux/native' && echo absent") -eq 'absent')
    Check 'cross-distro byte stream preserves literal relative filename' ((Finish) -and (Other "cat '$linux/native/deep/a:b'") -eq 'literal')
    Check 'cross-distro copied content verifies on rescan' ((Scan) -and (Select-Name 'native\deep\a:b') -and (Prop 'SelectedState') -eq 7)
    Recursive $false
    Check 'switching back restores current-level results' ((Scan) -and (Prop 'Recursive') -eq 0 -and -not(Select-Name 'native\deep\a:b'))
    Recursive $true;Press 104;Press 105
    Check 'recursive cancellation disables copy of partial results' ((Ready) -and (Prop 'Cancelled') -eq 1 -and -not(Enabled 109) -and -not(Enabled 110))
    Press 104;Press 2
    Check 'closing a recursive worker releases its window' (Wait-Until {(Dialog) -eq [IntPtr]::Zero} 7000)
    Check 'app shuts down cleanly' (Stop-Demo $p);$p=$null
    $profile=Get-Content "$out\tree-profile.csv" -Raw
    Check 'recursive comparison respects function lifetimes' ($profile -match 'metric,fn_violations,,,,0,')
    $m=[regex]::Match($profile,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'recursive comparison stays below 16 ms dispatch' ($m.Success -and [double]$m.Groups[1].Value -lt 16 -and [TreeProbe]::NavigationSamples -gt 0 -and [TreeProbe]::MaxNavigationMs -lt 16) ($m.Groups[1].Value+" ms queued; max acknowledged navigation round trip: $([TreeProbe]::MaxNavigationMs) ms across $([TreeProbe]::NavigationSamples) submissions")
}finally{
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'"|Out-Null;Other "rm -rf '$linux'"|Out-Null
    if(Test-Path $scratch){Remove-Item -LiteralPath $scratch -Recurse -Force}
}
"`n$script:passed passed, $script:failed failed"
exit ([int]($script:failed -gt 0))
