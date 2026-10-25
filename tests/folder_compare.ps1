<# Real-app current-level comparison, reviewed copy staging and native WSL. #>
param([string]$Config='release',[string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class CompareProbe {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access,bool inherit,uint pid);
 [DllImport("kernel32.dll")] static extern IntPtr VirtualAllocEx(IntPtr p,IntPtr a,UIntPtr n,uint type,uint protect);
 [DllImport("kernel32.dll")] static extern bool WriteProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr n,out UIntPtr written);
 [DllImport("kernel32.dll")] static extern bool VirtualFreeEx(IntPtr p,IntPtr a,UIntPtr n,uint type);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static bool Select(IntPtr list,uint pid,int index) {
  IntPtr p=OpenProcess(0x28,false,pid);if(p==IntPtr.Zero)return false;
  IntPtr a=VirtualAllocEx(p,IntPtr.Zero,(UIntPtr)128,0x3000,4);
  try {
   if(a==IntPtr.Zero)return false;byte[] b=new byte[128];UIntPtr written;
   Array.Copy(BitConverter.GetBytes((uint)3),0,b,16,4);
   if(!WriteProcessMemory(p,a,b,(UIntPtr)b.Length,out written))return false;
   SendMessageW(list,0x102b,(IntPtr)(-1),a);
   Array.Copy(BitConverter.GetBytes((uint)3),0,b,12,4);
   if(!WriteProcessMemory(p,a,b,(UIntPtr)b.Length,out written))return false;
   return SendMessageW(list,0x102b,(IntPtr)index,a)!=IntPtr.Zero;
  } finally {if(a!=IntPtr.Zero)VirtualFreeEx(p,a,UIntPtr.Zero,0x8000);CloseHandle(p);}
 }
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=if($EvidenceRoot){$EvidenceRoot}else{Join-Path $root 'build\folder_compare'};New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero
$script:readinessEvidence=Join-Path $out "readiness-$PID.jsonl"
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{Write-UiReadinessFailure $name;"  FAIL  $name $detail";$script:failed++}}
function Native([string]$cmd){$v=& wsl.exe --exec sh -c $cmd;if($LASTEXITCODE){throw 'Private native comparison fixture failed'};($v -join "`n").TrimEnd()}
function Dialog {Find-OwnedWindow 'OpenPilotFolderCompare' 'Compare folders'}
function Field([int]$id){[CompareProbe]::GetDlgItem($script:dialog,$id)}
function Enabled([int]$id){[CompareProbe]::IsWindowEnabled((Field $id))}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"FolderCompare.$name").ToInt64()}
function Press([int]$id){[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]$id,[IntPtr]::Zero)|Out-Null}
function Ready {Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and (Prop 'Done') -eq 1 -and (Prop 'Busy') -eq 0} 7000}
function Open-Compare([IntPtr]$pane=$pane0){
 $clock=[Diagnostics.Stopwatch]::StartNew()
 try{
  Chord-Acknowledged $pane @('Ctrl','Shift') 0x76 7000
  $remaining=7000-[int]$clock.ElapsedMilliseconds
  if($remaining -le 0 -or -not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and (Prop 'Done') -eq 1 -and (Prop 'Busy') -eq 0} $remaining)){throw 'Comparison did not finish within 7000ms'}
 }catch{Write-UiReadinessFailure 'comparison open/input deadline';throw}
}
function Close-Compare {Press 2;Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000}
function Scan {Press 104;Ready}
function Hashes([bool]$on){[FpUi]::SendMessageW((Field 103),0xF1,[IntPtr][int]$on,[IntPtr]::Zero)|Out-Null}
function Select-Name([string]$text){for($i=0;$i -lt (Prop 'Count');$i++){if(-not[CompareProbe]::Select((Field 106),$p.Id,$i)){throw 'Owned virtual list selection failed'};if((Window-Text (Field 108)).Contains("Left: $text     ") -or (Window-Text (Field 108)).Contains("Right: $text`r`n")){return $true}};$false}
function Navigate([IntPtr]$pane,[string]$path){Navigate-Ready $path $pane 7000;$true}
function Slot {$side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero);[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)}
function Snapshot {Chord (Slot) 'Ctrl' 0x43 30;Get-Clipboard -Raw}
function Pending {Wait-Until {(Snapshot) -match 'Ready to start'} 3000}
function Finish {Key (Slot) 0x0D 0;Wait-Until {(Snapshot) -match 'Completed|Finished with errors'} 7000}
$scratch=Join-Path $env:TEMP "openpilot_compare_$PID";$left="$scratch\left";$right="$scratch\right";$linux="/tmp/openpilot-compare-$PID";$p=$null
try{
    New-Item -ItemType Directory -Force $left,$right,"$scratch\elsewhere","$left\folder","$right\folder","$left\type"|Out-Null
    $fixed=[datetime]'2024-01-01T00:00:00Z'
    foreach($side in @($left,$right)){
        foreach($name in @('same.txt','different.txt','date.txt')){[IO.File]::WriteAllText("$side\$name",'xxx');[IO.File]::SetLastWriteTimeUtc("$side\$name",$fixed)}
    }
    [IO.File]::WriteAllText("$right\different.txt",'abc');[IO.File]::SetLastWriteTimeUtc("$right\different.txt",$fixed)
    [IO.File]::SetLastWriteTimeUtc("$right\date.txt",$fixed.AddDays(1))
    [IO.File]::WriteAllText("$left\left.txt",'left');[IO.File]::WriteAllText("$right\right.txt",'right')
    [IO.File]::WriteAllText("$left\size.txt",'x');[IO.File]::WriteAllText("$right\size.txt",'xx');[IO.File]::WriteAllText("$right\type",'file')
    [IO.File]::WriteAllText("$left\.hidden",'hidden');[IO.File]::SetAttributes("$left\.hidden",[IO.FileAttributes]::Hidden)
    [IO.File]::WriteAllText("$left\Case.txt",'case');[IO.File]::WriteAllText("$right\case.txt",'case');[IO.File]::SetLastWriteTimeUtc("$left\Case.txt",$fixed);[IO.File]::SetLastWriteTimeUtc("$right\case.txt",$fixed)
    $settings="$scratch\settings.ini";$baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n";Set-Content $settings $baseSettings
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux'; printf xxx > '$linux/same.txt'; printf linux > '$linux/native.txt'; printf literal > '$linux/a:b'; printf case > '$linux/case.txt'; ln -s same.txt '$linux/link'; mkfifo '$linux/fifo'"|Out-Null
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\compare-profile.csv",$left,$right)
    Check 'browsing has no comparison window' ((Dialog) -eq [IntPtr]::Zero)
    Check 'both pane snapshots settle' (Wait-Until {[FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0 -and [FpUi]::GetPropW($pane1,'Address.Loading').ToInt64() -eq 0} 7000)
    Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'Compare folders is available in the pane context menu' ($menu -and $menu.Items -contains 'Compare folders...')
    if(-not $menu -or -not(Pick-Menu $menu 'Compare folders...')){throw 'Compare command missing'}
    Check 'comparison completes on its worker' ((Ready) -and (Prop 'Count') -eq 10 -and (Prop 'Error') -eq 0)
    Check 'left and right-only files include hidden items' ((Prop 'OnlyLeft') -eq 2 -and (Prop 'OnlyRight') -eq 1)
    Check 'metadata flags size and date differences separately' ((Prop 'Size') -eq 1 -and (Prop 'Date') -eq 1)
    Check 'matching metadata never claims matching contents' ((Prop 'Metadata') -eq 3 -and (Prop 'Equal') -eq 0)
    Check 'subfolders and file-folder conflicts stay explicit' ((Prop 'Folders') -eq 1 -and (Prop 'Types') -eq 1)
    Check 'Windows names differing only in case are paired' (Select-Name 'Case.txt')
    Check 'virtual rows expose a readable selected result and UTC dates' ((Window-Text (Field 108)) -match 'Metadata matches.*unchecked' -and (Window-Text (Field 108)).Contains('2024-01-01 00:00:00.000 UTC'))
    Hashes $true
    Check 'SHA-256 finds equal-sized differing contents' ((Scan) -and (Prop 'Different') -eq 1 -and (Prop 'Equal') -eq 3 -and (Prop 'Metadata') -eq 0)
    Check 'verified differences can be selected' ((Select-Name 'different.txt') -and (Prop 'SelectedState') -eq 6)
    $saved=$script:main;try{$script:main=$script:dialog;Shot $out '01_verified'}finally{$script:main=$saved}
    Check 'pane navigation stays available during review' (Navigate $pane0 "$scratch\elsewhere")
    Check 'scan paths remain the captured physical left and right folders' ((Window-Text (Field 101)).Contains($left) -and (Window-Text (Field 102)).Contains($right) -and (Scan) -and (Prop 'Different') -eq 1)
    Check 'left-only source can be selected' (Select-Name 'left.txt');Press 109
    Check 'copy is staged for explicit Start in Operations' ((Pending) -and -not(Test-Path "$right\left.txt"))
    Check 'staged copy uses captured paths after pane navigation' ((Finish) -and [IO.File]::ReadAllText("$right\left.txt") -eq 'left' -and -not(Test-Path "$scratch\elsewhere\left.txt"))
    Check 'rescan sees the completed transfer' ((Scan) -and (Prop 'OnlyLeft') -eq 1)
    Check 'right-only source can be selected' (Select-Name 'right.txt');Press 110
    Check 'opposite direction also awaits Start' ((Pending) -and -not(Test-Path "$left\right.txt"))
    Check 'right to left copies the chosen source' ((Finish) -and [IO.File]::ReadAllText("$left\right.txt") -eq 'right')
    Select-Name 'folder'|Out-Null;Press 109
    Check 'folder rows cannot silently copy unexamined contents' ((Window-Text (Field 108)) -match 'differing regular files')
    Select-Name '.hidden'|Out-Null;Press 109
    Check 'current-level selected file also awaits reviewed Start' ((Pending) -and -not(Test-Path "$right\.hidden"))
    Move-Item -LiteralPath "$left\.hidden" -Destination "$left\.hidden-original" -Force
    New-Item -ItemType Directory -Path "$left\.hidden"|Out-Null
    [IO.File]::WriteAllText("$left\.hidden\unselected.txt",'never copy')
    Check 'a selected file replaced by a folder cannot copy unselected children' ((Finish) -and (Snapshot) -match 'Finished with errors' -and -not(Test-Path "$right\.hidden"))
    Remove-Item -LiteralPath "$left\.hidden" -Recurse -Force
    Move-Item -LiteralPath "$left\.hidden-original" -Destination "$left\.hidden" -Force
    Check 'comparison closes cleanly' (Close-Compare)
    Check 'Windows source pane returns' (Navigate $pane0 $left)
    Check 'native WSL destination loads' (Navigate $pane1 $unc)
    Open-Compare $pane1
    Check 'invoking from the right pane preserves physical left/right meaning' ((Window-Text (Field 101)).Contains($left) -and (Window-Text (Field 102)).Contains($unc))
    Hashes $true
    Check 'mixed Windows and WSL SHA-256 verifies real bytes' ((Scan) -and (Prop 'Equal') -eq 1 -and (Prop 'Errors') -eq 0)
    Check 'Linux links and FIFOs are excluded without opening their targets' ((Prop 'Excluded') -eq 2)
    Check 'mixed filesystems match names exactly' ((Select-Name 'Case.txt') -and (Prop 'SelectedState') -eq 1)
    Check 'literal Linux reserved characters remain readable' ((Select-Name 'a:b') -and (Prop 'SelectedState') -eq 2)
    Select-Name 'native.txt'|Out-Null;Press 110
    Check 'native WSL source copy is staged' ((Pending) -and -not(Test-Path "$left\native.txt"))
    Check 'native WSL source arrives through Operations' ((Finish) -and [IO.File]::ReadAllText("$left\native.txt") -eq 'linux')
    Select-Name 'left.txt'|Out-Null;Press 109
    Check 'Windows source into WSL awaits Start' ((Pending) -and (Native "test ! -e '$linux/left.txt' && echo absent") -eq 'absent')
    Check 'Windows to native WSL completes through Operations' ((Finish) -and (Native "cat '$linux/left.txt'") -eq 'left')
    Check 'mixed comparison closes' (Close-Compare)
    Set-UiSettings $settings ($baseSettings.Replace('[functions]',"[functions]`noperations.show = off"));Wait-UiSettings 500
    Open-Compare;Select-Name '.hidden'|Out-Null;Press 109
    Check 'disabled Operations cannot bypass explicit staging' ((Window-Text (Field 108)) -match 'could not be staged' -and (Native "test ! -e '$linux/.hidden' && echo absent") -eq 'absent')
    Close-Compare|Out-Null
    Set-UiSettings $settings $baseSettings;Wait-UiSettings 500
    # A matching-size large file guarantees a real cancellable hashing interval.
    $large="$left\large.bin";$stream=[IO.File]::Create($large);$stream.SetLength(128MB);$stream.Dispose()
    Native "truncate -s 134217728 '$linux/large.bin'"|Out-Null
    Open-Compare;Hashes $true;Press 104;Press 105
    Check 'Cancel retains an explicitly incomplete result' ((Ready) -and (Prop 'Cancelled') -eq 1 -and -not(Enabled 109) -and -not(Enabled 110))
    Check 'cancelled comparison leaves source bytes untouched' ((Get-Item $large).Length -eq 128MB -and (Native "stat -c %s '$linux/large.bin'") -eq '134217728')
    Press 104;Press 2
    Check 'closing a running comparison cancels and releases its window' (Wait-Until {(Dialog) -eq [IntPtr]::Zero} 7000)
    Check 'private settings watcher is active' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 3000)
    Set-UiSettings $settings ($baseSettings.Replace('[functions]',"[functions]`nwsl.helper = off"));Wait-UiSettings 500
    Open-Compare
    Check 'disabled WSL integration fails explicitly without a 9P hash fallback' ((Prop 'Error') -eq 50 -and -not(Enabled 109) -and -not(Enabled 110))
    Close-Compare|Out-Null
    Set-UiSettings $settings ($baseSettings.Replace('[functions]',"[functions]`nfile.compare_folders = off"));Wait-UiSettings 500
    Chord $pane0 @('Ctrl','Shift') 0x76 0
    Check 'disabled comparison does not open a worker window' ((Dialog) -eq [IntPtr]::Zero)
    Check 'app shuts down cleanly' (Stop-Demo $p);$p=$null
    $profile=Get-Content "$out\compare-profile.csv" -Raw
    Check 'comparison respects cold function lifetimes' ($profile -match 'metric,fn_violations,,,,0,')
    $m=[regex]::Match($profile,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'comparison keeps UI dispatch below 16 ms' ($m.Success -and [double]$m.Groups[1].Value -lt 16) ($m.Groups[1].Value)
}finally{
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'"|Out-Null
    if(Test-Path $scratch){Remove-Item -LiteralPath $scratch -Recurse -Force}
}
"`n$script:passed passed, $script:failed failed"
exit ([int]($script:failed -gt 0))
