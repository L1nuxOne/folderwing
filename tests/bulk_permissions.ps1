<# Batch permission acceptance: real controls, guarded Linux changes, bounded cancellation. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class BulkProbe {
 public static double MaxDispatchMs {get;private set;}
 public static string MaxDispatchDetails {get;private set;}
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetNextDlgTabItem(IntPtr dialog,IntPtr current,bool previous);
 [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,UIntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
 public static void Dispatch(IntPtr h,uint message,UIntPtr w,IntPtr l,uint timeout) {
  var clock=System.Diagnostics.Stopwatch.StartNew();
  try {
   UIntPtr result;
   if(SendMessageTimeoutW(h,message,w,l,3,timeout,out result)==IntPtr.Zero)
    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Test input was not acknowledged");
  } finally {
   double elapsed=clock.Elapsed.TotalMilliseconds;
   if(elapsed>MaxDispatchMs){MaxDispatchMs=elapsed;MaxDispatchDetails=String.Format("hwnd={0:X} message={1:X} wParam={2}",h.ToInt64(),message,w.ToUInt64());}
  }
 }
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\bulk_permissions'
New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero;$p=$null
$script:requestClock=$null
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command,[switch]$AsRoot){
    $value=if($AsRoot){& wsl.exe --user root --exec sh -c $command}else{& wsl.exe --exec sh -c $command}
    $code=$LASTEXITCODE
    if($code){throw "Private fixture failed (exit $code): $command | $($value -join ' ')"};($value -join "`n").TrimEnd()
}
function Dialog {
    $h=[IntPtr]::Zero
    while(($h=[FpUi]::FindWindowExW([IntPtr]::Zero,$h,'OpenPilotPermissions',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        $owner=[uint32]0;[FpUi]::GetWindowThreadProcessId($h,[ref]$owner)|Out-Null
        if($owner -eq $script:demoPid -and (Window-Text $h) -eq 'Linux permissions'){return $h}
    }
    [IntPtr]::Zero
}
function Field([int]$id){[FpUi]::FindControl($script:dialog,$id)}
function Enabled([int]$id){[FpUi]::ControlEnabled((Field $id))}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"Permissions.$name").ToInt64()}
function Press([int]$id){
    if($script:dialog -eq [IntPtr]::Zero -or (Dialog) -ne $script:dialog){throw 'The owned batch editor is no longer available'}
    if($id -in 106,107){$script:requestClock=[Diagnostics.Stopwatch]::StartNew()}
    $remaining=if($script:requestClock){7000-[int]$script:requestClock.ElapsedMilliseconds}else{3000}
    if($remaining -le 0){throw 'Batch request exceeded its 7000ms deadline before command acknowledgment'}
    # A handled WM_COMMAND has started the worker (or completed the action).
    # A posted command followed by an idle poll can observe the previous request.
    [BulkProbe]::Dispatch($script:dialog,0x111,[UIntPtr][uint32]$id,[IntPtr]::Zero,[uint32]$remaining)
}
function Done {
    $clock=if($script:requestClock){$script:requestClock}else{[Diagnostics.Stopwatch]::StartNew()}
    $remaining=[Math]::Max(0,7000-[int]$clock.ElapsedMilliseconds)
    $ready=Wait-Until {
        (Dialog) -eq $script:dialog -and $script:dialog -ne [IntPtr]::Zero -and
        (Prop 'Busy') -eq 0 -and (Enabled 107) -and (Enabled 2)
    } $remaining
    if(-not $ready){throw "Batch request did not finish within 7000ms: $(Window-Text (Field 105)); completed=$(Prop 'Completed'), busy=$(Prop 'Busy')"}
    $script:requestClock=$null
    $true
}
function Loaded([int]$n){(Done) -and (Prop 'ReadyCount') -eq $n}
function Bit([int]$id){[FpUi]::SendMessageW((Field $id),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()}
function Toggle([int]$id){[FpUi]::SendMessageW((Field $id),0xF5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null}
function Octal([string]$value){Chord (Field 101) 'Ctrl' 0x41 20;if($value){Send-Text (Field 101) $value}else{Key (Field 101) 8 20}}
function Rows {
    $list=Field 110;$n=[FpUi]::SendMessageW($list,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()
    for($i=0;$i -lt $n;$i++){$sb=New-Object Text.StringBuilder 512;[FpUi]::SendMessageW($list,0x189,[IntPtr]$i,$sb)|Out-Null;$sb.ToString()}
}
function Row([string]$name){@(Rows|Where-Object {$_ -match ('^'+[regex]::Escape($name)+'\s+\|')}) -join ''}
function Mode([string]$path){Native "stat -c %a '$path'"}
function Navigate([string]$path){
    $clock=[Diagnostics.Stopwatch]::StartNew()
    Chord $pane0 'Ctrl' 0x4C 20;$edit=(Pane-Edits $pane0)[0];Send-Text $edit $path
    if(-not(Wait-Until {(Window-Text $edit) -ceq $path} ([Math]::Max(0,7000-[int]$clock.ElapsedMilliseconds)))){throw 'Address input did not arrive within 7000ms'}
    $remaining=7000-[int]$clock.ElapsedMilliseconds
    if($remaining -le 0){throw 'Navigation exceeded its 7000ms deadline before submission'}
    [BulkProbe]::Dispatch($edit,$WM_KEYDOWN,[UIntPtr]13,[IntPtr](Key-LParam 13),[uint32]$remaining)
    if(-not(Wait-Until {
        (Window-Text $edit) -ceq $path -and [FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0 -and
        [FpUi]::GetPropW($pane0,'Address.Resolving').ToInt64() -eq 0 -and [FpUi]::GetPropW($pane0,'Address.Error').ToInt64() -eq 0
    } ([Math]::Max(0,7000-[int]$clock.ElapsedMilliseconds)))){throw "Navigation did not finish within 7000ms: $path"}
    $true
}
function Select-All {
    $point=([int](65*$scale) -band 0xffff) -bor (([int](74*$scale) -band 0xffff) -shl 16)
    [BulkProbe]::Dispatch($pane0,$WM_LBUTTONDOWN,[UIntPtr]1,[IntPtr]$point,3000)
    [BulkProbe]::Dispatch($pane0,0x202,[UIntPtr]::Zero,[IntPtr]$point,3000)
    Chord $pane0 'Ctrl' 0x41 20
}
function Open-Editor([switch]$AlreadySelected){
    if((Dialog) -ne [IntPtr]::Zero){throw 'The previous batch editor did not close'}
    if(-not $AlreadySelected){Select-All};Chord $pane0 @('Ctrl','Alt') 0x50 0
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($script:dialog) -and (Field 101) -ne [IntPtr]::Zero} 3000)){throw 'Batch permissions editor did not open'}
    $script:requestClock=$null
}
function Close-Editor {
    Done|Out-Null;$clock=[Diagnostics.Stopwatch]::StartNew();Press 2
    if(-not(Wait-Until {(Dialog) -eq [IntPtr]::Zero} ([Math]::Max(0,3000-[int]$clock.ElapsedMilliseconds)))){throw 'Batch editor did not close within 3000ms'}
    $script:dialog=[IntPtr]::Zero
    $true
}
$linux="/tmp/openpilot-bulk-permissions-$PID";$scratch=Join-Path $env:TEMP "openpilot_bulk_permissions_$PID"
try {
    $uid=Native 'id -u';$gid=Native 'id -g';$groups=Native 'id -G'
    if($uid -eq '0' -or @($groups -split ' ') -contains '0'){throw 'Requires the unprivileged default WSL user without root group membership'}
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/mixed/c-folder' '$linux/discard' '$linux/failures' '$linux/replacements' '$linux/verification' '$linux/stop' '$linux/bounds'; printf content > '$linux/mixed/a β'; printf second > '$linux/mixed/b:star*'; printf nested > '$linux/mixed/c-folder/nested'; chmod 4750 '$linux/mixed/a β'; chmod 640 '$linux/mixed/b:star*' '$linux/mixed/c-folder/nested'; chmod 1770 '$linux/mixed/c-folder'; touch '$linux/discard/a' '$linux/discard/b'; chmod 640 '$linux/discard/'*; touch '$linux/failures/a-owned' '$linux/failures/c-stale' '$linux/failures/e-replaced'; chmod 640 '$linux/failures/'*; ln -s a-owned '$linux/failures/d-link'; touch '$linux/verification/a-setgid' '$linux/verification/b-control'; chmod 640 '$linux/verification/b-control'; i=0; while [ `$i -lt 16 ]; do touch '$linux/stop/item-'`$i; chmod 640 '$linux/stop/item-'`$i; i=`$((i+1)); done; i=0; while [ `$i -lt 257 ]; do touch '$linux/bounds/item-'`$i; chmod 640 '$linux/bounds/item-'`$i; i=`$((i+1)); done"|Out-Null
    Native "printf protected > '$linux/failures/b-root'; chown root:root '$linux/failures/b-root'; chmod 640 '$linux/failures/b-root'; chown ${uid}:0 '$linux/verification/a-setgid'; chmod 2750 '$linux/verification/a-setgid'" -AsRoot|Out-Null
    New-Item -ItemType Directory -Force $scratch|Out-Null;$settings="$scratch\settings.ini"
    $baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n[keys]`nfile.permissions = Ctrl+Alt+P`n"
    Set-Content -LiteralPath $settings -Value $baseSettings
    $p=Start-Demo (Join-Path $root "build\$Config\folderwing.exe") @("--settings=$settings","--profile-out=$out\bulk-profile.csv","$unc\mixed","$unc\mixed")
    Check 'mixed fixture loads' (Wait-Until {(Title) -eq 'mixed - Folderwing' -and [FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000)
    Open-Editor;Check 'all three selected files and folders have fresh metadata' ((Loaded 3) -and (Prop 'ItemCount') -eq 3)
    $tabOrder=@(120..131)+@(101,110);$tabsWork=$true
    for($i=0;$i -lt $tabOrder.Count-1;$i++){
        $tabsWork=$tabsWork -and [BulkProbe]::GetNextDlgTabItem($script:dialog,(Field $tabOrder[$i]),$false) -eq (Field $tabOrder[$i+1])
    }
    Check 'Tab traverses every permission choice and continues into the result list' $tabsWork
    Check 'mixed ordinary and special bits are marked explicitly with no change requested' ((Bit 124) -eq 2 -and (Window-Text (Field 124)) -eq 'Mixed' -and (Window-Text (Field 129)) -eq 'Setuid (Mixed)' -and (Bit 122) -eq 2 -and (Bit 129) -eq 2 -and (Bit 131) -eq 2 -and (Window-Text (Field 101)) -eq '' -and -not(Enabled 106))
    Check 'each row previews its original mode' ((Row 'a β') -match '\| 4750 \| 4750 \|' -and (Row 'b:star*') -match '\| 0640 \| 0640 \|' -and (Row 'c-folder') -match '\| 1770 \| 1770 \|') ((Rows) -join '; ')
    Toggle 124
    Check 'setting one mixed bit preserves all other bits and file types' ((Bit 124) -eq 1 -and (Window-Text (Field 124)) -eq 'Set' -and (Bit 122) -eq 2 -and (Row 'a β') -match '\| 4750 \| 4770 \|' -and (Row 'b:star*') -match '\| 0640 \| 0660 \|' -and (Row 'c-folder') -match '\| 1770 \| 1770 \|')
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '01_mixed_preview'}finally{$script:main=$savedMain}
    Toggle 124;Check 'clearing a mixed bit previews only its removal' ((Bit 124) -eq 0 -and (Window-Text (Field 124)) -eq 'Clear' -and (Row 'c-folder') -match '\| 1770 \| 1750 \|')
    Toggle 124;Check 'cycling back to Mixed keeps each original value' ((Bit 124) -eq 2 -and -not(Enabled 106))
    Toggle 124;Check 'preview leaves actual modes unchanged' ((Mode "$linux/mixed/a β") -eq '4750' -and (Mode "$linux/mixed/b:star*") -eq '640')
    Press 106
    Check 'Apply reports two verified changes and one unchanged folder' ((Done) -and (Prop 'Applied') -eq 2 -and (Prop 'Failed') -eq 0 -and (Window-Text (Field 105)) -match '1 unchanged')
    Check 'real modes retain special bits and folder contents' ((Mode "$linux/mixed/a β") -eq '4770' -and (Mode "$linux/mixed/b:star*") -eq '660' -and (Mode "$linux/mixed/c-folder") -eq '1770' -and (Mode "$linux/mixed/c-folder/nested") -eq '640')
    Check 'individual verified results become the new current modes' ((Row 'a β') -match '\| 4770 \| 4770 \| Applied and verified' -and -not(Enabled 106))
    Octal '888';Check 'invalid octal disables batch Apply' (-not(Enabled 106))
    Octal '10000';Check 'a fifth pasted digit cannot become a valid batch mode' (-not(Enabled 106))
    Octal '0755';Check 'exact octal deliberately replaces all bits for every available item' ((Row 'a β') -match '\| 4770 \| 0755 \|' -and (Row 'c-folder') -match '\| 1770 \| 0755 \|' -and (Bit 129) -eq 0 -and (Bit 131) -eq 0)
    Toggle 124;Check 'checkbox editing after exact octal retains the other explicit choices' ((Window-Text (Field 101)) -eq '' -and (Row 'a β') -match '\| 4770 \| 0775 \|' -and (Row 'c-folder') -match '\| 1770 \| 0775 \|')
    Press 106;Check 'exact-plus-checkbox batch is verified for all three items' ((Done) -and (Prop 'Applied') -eq 3 -and (Prop 'Failed') -eq 0 -and (Mode "$linux/mixed/a β") -eq '775' -and (Mode "$linux/mixed/b:star*") -eq '775' -and (Mode "$linux/mixed/c-folder") -eq '775')
    Check 'ownership, content and nested modes remain intact' ((Native "stat -c '%u:%g' '$linux/mixed/a β'") -eq "${uid}:$gid" -and (Native "cat '$linux/mixed/a β'") -eq 'content' -and (Mode "$linux/mixed/c-folder/nested") -eq '640')
    Close-Editor|Out-Null
    Check 'discard fixture opens' (Navigate "$unc\discard");Open-Editor;Check 'discard metadata loads' (Loaded 2)
    Toggle 122;Press 111;Check 'Reset discards the complete pending patch' (-not(Enabled 106) -and (Bit 122) -eq 0)
    Toggle 122;Check 'Close discards pending changes for every selected item' ((Close-Editor) -and (Mode "$linux/discard/a") -eq '640' -and (Mode "$linux/discard/b") -eq '640')
    Check 'partial failure fixture opens' (Navigate "$unc\failures");Open-Editor
    Check 'unsupported symlink is excluded while four regular files load' ((Loaded 4) -and (Prop 'ItemCount') -eq 5 -and (Row 'd-link') -match 'Unsupported item/helper')
    $all=@(Rows);$rootIndex=0;for($i=0;$i -lt $all.Count;$i++){if($all[$i] -match '^b-root\s'){$rootIndex=$i}}
    [FpUi]::SendMessageW((Field 110),0x186,[IntPtr]$rootIndex,[IntPtr]::Zero)|Out-Null;Press ((1 -shl 16) -bor 110)
    Check 'selected row shows its own owner, group and complete path' ((Window-Text (Field 102)) -match 'uid 0' -and (Window-Text (Field 103)) -match 'gid 0' -and (Window-Text (Field 112)) -eq "$unc\failures\b-root")
    Toggle 122;Native "chmod 600 '$linux/failures/c-stale'; mv '$linux/failures/e-replaced' '$linux/replacements/old'; printf replacement > '$linux/failures/e-replaced'; chmod 644 '$linux/failures/e-replaced'"|Out-Null
    Press 106;Check 'one success and four failures are reported individually' ((Done) -and (Prop 'Applied') -eq 1 -and (Prop 'Failed') -eq 4) (Window-Text (Field 105))
    Check 'denied and stale items retain their actual modes' ((Mode "$linux/failures/a-owned") -eq '740' -and (Mode "$linux/failures/b-root") -eq '640' -and (Mode "$linux/failures/c-stale") -eq '600' -and (Mode "$linux/failures/e-replaced") -eq '644')
    Check 'rows distinguish denial, replacement and unsupported links' ((Row 'a-owned') -match 'Applied and verified' -and (Row 'b-root') -match 'Permission denied' -and (Row 'c-stale') -match 'Changed/replaced; Reload' -and (Row 'e-replaced') -match 'Changed/replaced; Reload' -and (Row 'd-link') -match 'Unsupported item/helper') ((Rows) -join '; ')
    Check 'stale rows cannot be edited again until Reload' ((Prop 'ReadyCount') -eq 2 -and (Row 'c-stale') -match '\| ---- \| ---- \|')
    Press 107;Check 'Reload adopts each fresh mode and clears the patch' ((Loaded 4) -and (Row 'c-stale') -match '\| 0600 \| 0600 \|' -and (Row 'e-replaced') -match '\| 0644 \| 0644 \|' -and -not(Enabled 106))
    Check 'settings watcher is ready' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Set-Content -LiteralPath $settings -Value ($baseSettings+"[wsl.helper]`nuser = root`n");Start-Sleep -Milliseconds 500
    Toggle 124;Press 106;Check 'open batch retains the captured default user after settings change' ((Done) -and (Prop 'Applied') -eq 3 -and (Prop 'Failed') -eq 2 -and (Window-Text (Field 108)) -eq 'WSL user: distro default' -and (Mode "$linux/failures/b-root") -eq '640')
    Close-Editor|Out-Null;Open-Editor;Check 'new batch captures explicitly configured root' ((Loaded 4) -and (Window-Text (Field 108)) -eq 'WSL user: root')
    Toggle 124;Press 106;Check 'explicit root changes the remaining protected file without following the link' ((Done) -and (Prop 'Applied') -eq 1 -and (Prop 'Failed') -eq 1 -and (Mode "$linux/failures/b-root") -eq '660' -and (Native "readlink '$linux/failures/d-link'") -eq 'a-owned')
    Close-Editor|Out-Null;Set-Content -LiteralPath $settings -Value $baseSettings;Start-Sleep -Milliseconds 500
    Check 'verification-loss fixture opens' (Navigate "$unc\verification");Open-Editor;Check 'special group-bit fixture loads' (Loaded 2)
    Toggle 124;Press 106
    Check 'kernel-cleared setgid is reported as a changed but unverified item' ((Done) -and (Prop 'Applied') -eq 1 -and (Prop 'Failed') -eq 1 -and (Prop 'Unverified') -eq 1 -and (Row 'a-setgid') -match 'Changed; mode not retained; Reload') (Window-Text (Field 105))
    Check 'actual changed mode and untouched ownership are visible through Linux' ((Mode "$linux/verification/a-setgid") -eq '770' -and (Mode "$linux/verification/b-control") -eq '660' -and (Native "stat -c '%u:%g' '$linux/verification/a-setgid'") -eq "${uid}:0")
    Press 107;Check 'Reload adopts the actual mode after failed retention' ((Loaded 2) -and (Row 'a-setgid') -match '\| 0770 \| 0770 \|' -and -not(Enabled 106))
    Close-Editor|Out-Null
    Check 'stop fixture opens' (Navigate "$unc\stop");Open-Editor;Check 'sixteen items load for cancellation' (Loaded 16)
    Octal '0600';Press 106;Press 109
    Check 'Stop completes the current request and marks remaining items unattempted' ((Done) -and (Prop 'Stopped') -gt 0 -and (Prop 'Applied')+(Prop 'Stopped') -eq 16 -and (Prop 'Failed') -eq 0) (Window-Text (Field 105))
    $changed=[int](Native "find '$linux/stop' -type f -perm 600 | wc -l");$kept=[int](Native "find '$linux/stop' -type f -perm 640 | wc -l")
    Check 'real filesystem changes exactly match the stopped batch report' ($changed -eq (Prop 'Applied') -and $kept -eq (Prop 'Stopped') -and $changed+$kept -eq 16)
    Press 107;Check 'Reload after Stop reads every item and creates no pending changes' ((Loaded 16) -and -not(Enabled 106))
    Close-Editor|Out-Null
    Check 'bounded selection fixture opens' (Navigate "$unc\bounds");Select-All;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check '257 selections exceed the editor limit and hide its menu action' ($menu -and $menu.Items -notcontains 'Permissions...');if($menu){Key $menu.Window 0x1B 20}
    Chord $pane0 @('Ctrl','Alt') 0x50 20;Check 'the keyboard action also refuses an oversized selection' ((Dialog) -eq [IntPtr]::Zero)
    Chord $pane0 'Ctrl' 0x20 20;Open-Editor -AlreadySelected;Press 109
    Check '256 items are accepted and a read can be stopped' ((Done) -and (Prop 'ItemCount') -eq 256 -and (Prop 'Stopped') -gt 0 -and (Prop 'Completed')+(Prop 'Stopped') -eq 256) (Window-Text (Field 105))
    Check 'stopping reads never changes modes' ((Native "find '$linux/bounds' -type f -perm 640 | wc -l") -eq '257')
    Press 107;$fullReadClock=[Diagnostics.Stopwatch]::StartNew()
    Check 'all 256 selected items reload within the existing batch deadline' ((Loaded 256) -and (Prop 'Failed') -eq 0 -and -not(Enabled 106))
    Write-Host ("BULK_METADATA_256_MS {0:F3}" -f $fullReadClock.Elapsed.TotalMilliseconds)
    Close-Editor|Out-Null;Open-Editor -AlreadySelected
    $nativeIdentity="worstNativeId=$([FpUi]::GetPropW($main,'Permissions.Command.MaxId')); worstNativeUs=$([FpUi]::GetPropW($main,'Permissions.Command.MaxUs'))"
    Check 'app shutdown releases an in-flight batch cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\bulk-profile.csv" -Raw
    Check 'batch workers have no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    # Sent messages bypass dispatch_timed. Retain the queued-message guard and
    # conservatively bound the acknowledged input round trip as well.
    $native=@(Import-Csv "$out\bulk-profile.csv"|Where-Object {$_.kind -eq 'zone' -and $_.name -like 'permissions.command.*'})
    $nativeMax=($native|ForEach-Object {[double]$_.max_ms}|Measure-Object -Maximum).Maximum
    $nativeDetail=($native|ForEach-Object {"$($_.name)=$($_.max_ms)ms"}) -join '; '
    $delivery=[BulkProbe]::MaxDispatchMs
    Check 'batch editing keeps UI dispatch below 16 ms' ($match.Success -and [double]$match.Groups[1].Value -lt 16 -and $delivery -lt 16 -and $native.Count -gt 0 -and $nativeMax -lt 16) "$($match.Value) max acknowledged input round trip: $delivery ms $([BulkProbe]::MaxDispatchDetails); native handlers: $nativeDetail; $nativeIdentity"
}finally{
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'" -AsRoot|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
