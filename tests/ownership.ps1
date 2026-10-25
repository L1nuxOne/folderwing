<# Ownership acceptance: real controls, guarded Linux changes, bounded cancellation. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\ownership'
New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero;$p=$null
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command,[switch]$AsRoot){
    $value=if($AsRoot){& wsl.exe --user root --exec sh -c $command}else{& wsl.exe --exec sh -c $command}
    if($LASTEXITCODE){throw "Private fixture failed: $command"};($value -join "`n").TrimEnd()
}
function Dialog {
    $title=if($script:ownerView){'Linux owner/group'}else{'Linux permissions'};$w=[IntPtr]::Zero
    while(($w=[FpUi]::FindWindowExW([IntPtr]::Zero,$w,'OpenPilotPermissions',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        $ownerPid=[uint32]0;[FpUi]::GetWindowThreadProcessId($w,[ref]$ownerPid)|Out-Null
        if($ownerPid -eq $script:demoPid -and (Window-Text $w) -eq $title){return $w}
    }
    [IntPtr]::Zero
}
function Field([int]$id){[FpUi]::FindControl($script:dialog,$id)}
function Enabled([int]$id){[FpUi]::ControlEnabled((Field $id))}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"Permissions.$name").ToInt64()}
function Press([int]$id){[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]$id,[IntPtr]::Zero)|Out-Null}
function Done {
    $ok=Wait-Until {$script:dialog -ne [IntPtr]::Zero -and (Dialog) -eq $script:dialog -and (Prop 'Busy') -eq 0 -and (Enabled 2)} 7000
    if(-not $ok){throw "Ownership request did not finish within7000ms: $(Window-Text (Field 105))"};$true
}
function Loaded([int]$n){
    $ok=Wait-Until {$script:dialog -ne [IntPtr]::Zero -and (Dialog) -eq $script:dialog -and (Prop 'Busy') -eq 0 -and (Enabled 2) -and (Prop 'ReadyCount') -eq $n} 7000
    if(-not $ok){throw "Ownership metadata expected $n ready items; got $(Prop 'ReadyCount'): $(Window-Text (Field 105))"};$true
}
function Bit([int]$id){[FpUi]::SendMessageW((Field $id),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()}
function Toggle([int]$id){[FpUi]::SendMessageW((Field $id),0xF5,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null}
function Edit([int]$id,[string]$value){Chord (Field $id) 'Ctrl' 0x41 20;if($value){Send-Text (Field $id) $value}else{Key (Field $id) 8 20}}
function Rows {
    $list=Field 110;$n=[FpUi]::SendMessageW($list,0x18B,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64()
    for($i=0;$i -lt $n;$i++){$sb=New-Object Text.StringBuilder 512;[FpUi]::SendMessageW($list,0x189,[IntPtr]$i,$sb)|Out-Null;$sb.ToString()}
}
function Row([string]$name){@(Rows|Where-Object {$_ -match ('^'+[regex]::Escape($name)+'\s+\|')}) -join ''}
function Mode([string]$path,[switch]$AsRoot){Native "stat -c %a '$path'" -AsRoot:$AsRoot}
function Navigate([string]$path){Navigate-Verified $path $pane0 7000}
function Settings-User([bool]$rootUser){
    $revision=[FpUi]::GetPropW($main,'Settings.Revision').ToInt64()
    $text=$baseSettings+$(if($rootUser){"[wsl.helper]`nuser = root`n"}else{''})
    Set-Content -LiteralPath $settings -Value $text
    if(-not(Wait-Until {
        [FpUi]::GetPropW($main,'Settings.Revision').ToInt64() -gt $revision -and
        [FpUi]::GetPropW($main,'Settings.WslRoot').ToInt64() -eq [int]$rootUser
    } 500)){throw "Ownership settings did not apply the requested user within the existing500ms bound"}
}
function Select-All {Click $pane0 ([int](65*$scale)) ([int](74*$scale));Post $pane0 0x202 0 0;Chord $pane0 'Ctrl' 0x41 20}
function Open-Editor([switch]$AlreadySelected){
    $script:ownerView=$false;
    if(-not $AlreadySelected){Select-All};Chord $pane0 @('Ctrl','Alt') 0x50 0
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)){throw 'Batch permissions editor did not open'}
}
function Close-Editor {Press 2;if(-not(Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000)){throw 'Ownership editor did not close within3000ms'};$script:dialog=[IntPtr]::Zero;$true}
function Owner-View([switch]$NoWait){
    Press 132;$script:ownerView=$true
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)){throw 'Ownership view did not open'}
    if(-not $NoWait){if(-not(Done)){throw 'Ownership metadata did not return'}}
}
function Back-View {
    Press 143;$script:ownerView=$false
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)){throw 'Permissions view did not reopen within3000ms'}
    $true
}
function Review {Press 142;Done}
function IDs([string]$path,[switch]$AsRoot){Native "stat -c '%u:%g' '$path'" -AsRoot:$AsRoot}
$linux="/tmp/openpilot-ownership-$PID";$scratch=Join-Path $env:TEMP "openpilot_ownership_$PID";$script:ownerView=$false
try {
    $uid=Native 'id -u';$gid=Native 'id -g';$user=Native 'id -un';$group=Native 'id -gn';$groups=Native 'id -G'
    if($uid -eq '0' -or @($groups -split ' ') -contains '0'){throw 'Requires the unprivileged default WSL user without root group membership'}
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/single' '$linux/mixed/c-folder' '$linux/replaced' '$linux/stop'; printf single > '$linux/single/file β:star*'; chmod 640 '$linux/single/'*; printf content > '$linux/mixed/a-own'; printf nested > '$linux/mixed/c-folder/nested'; touch '$linux/mixed/e-stale' '$linux/mixed/f-replace'; chmod 640 '$linux/mixed/c-folder/nested' '$linux/mixed/e-stale' '$linux/mixed/f-replace'; chmod 6755 '$linux/mixed/a-own'; ln -s a-own '$linux/mixed/d-link'; i=0; while [ `$i -lt 16 ]; do touch '$linux/stop/item-'`$i; chmod 640 '$linux/stop/item-'`$i; i=`$((i+1)); done"|Out-Null
    Native "printf protected > '$linux/mixed/b-root'; chown root:root '$linux/mixed/b-root'; chmod 640 '$linux/mixed/b-root'; chown ${uid}:0 '$linux/mixed/c-folder' '$linux/mixed/e-stale'; chmod 2750 '$linux/mixed/c-folder'" -AsRoot|Out-Null
    New-Item -ItemType Directory -Force $scratch|Out-Null;$settings="$scratch\settings.ini"
    $baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n[keys]`nfile.permissions = Ctrl+Alt+P`n"
    Set-Content -LiteralPath $settings -Value $baseSettings
    $p=Start-Demo (Join-Path $root "build\$Config\folderwing.exe") @("--settings=$settings","--profile-out=$out\ownership-profile.csv","$unc\single","$unc\single")
    Check 'single fixture loads' (Wait-Until {(Title) -eq 'single - Folderwing' -and [FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000)
    Open-Editor;Check 'permissions view offers ownership editing after fresh metadata' ((Loaded 1) -and (Enabled 132))
    Edit 101 '0750';Check 'pending chmod changes cannot be mixed into an ownership draft' (-not(Enabled 132))
    Press 132;Check 'a direct ownership action cannot discard pending chmod changes' ((Window-Text (Field 101)) -eq '0750' -and (Window-Text (Field 105)) -match 'Apply or reset')
    Edit 101 '0640';Check 'restoring the current mode enables ownership editing' (Enabled 132)
    Check 'settings watcher is ready' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Settings-User $true
    Owner-View
    Check 'view transition retains the already captured default WSL user' ((Loaded 1) -and (Prop 'Ownership') -eq 1 -and (Window-Text (Field 108)) -eq 'WSL user: distro default')
    Check 'blank owner and group keep current values and cannot Apply' ((Window-Text (Field 140)) -eq '' -and (Window-Text (Field 141)) -eq '' -and -not(Enabled 106) -and -not(Enabled 142))
    Edit 140 'root';Check 'an unresolved name cannot be applied' (-not(Enabled 106) -and (Prop 'Reviewed') -eq 0)
    Check 'Review resolves the Linux owner name and numeric ID' ((Review) -and (Prop 'Reviewed') -eq 1 -and (Window-Text (Field 144)) -match 'root \(uid 0\)' -and (Window-Text (Field 144)) -match "Keep each item's group")
    Check 'single-item row previews changed owner and unchanged group' ((Row 'file β:star*') -match "$uid -> 0" -and (Row 'file β:star*') -match "$gid -> $gid" -and (Row 'file β:star*') -match 'Will change') (Row 'file β:star*')
    Check 'Review is read-only' ((IDs "$linux/single/file β:star*") -eq "${uid}:$gid" -and (Mode "$linux/single/file β:star*") -eq '640')
    Edit 140 '4294967295';Check 'changing a reviewed target invalidates the Apply draft' ((Prop 'Reviewed') -eq 0 -and -not(Enabled 106))
    Check 'the reserved numeric sentinel is refused' ((Review) -and (Prop 'Reviewed') -eq 0 -and (Window-Text (Field 105)) -match 'Owner could not be resolved')
    Edit 140 ('0'*128);Check 'oversized pasted input is kept visible and cannot be truncated into an ID' (Wait-Until {(Window-Text (Field 140)).Length -eq 128 -and -not(Enabled 142)} 3000)
    Press 142;Check 'a direct Review action also refuses oversized input' ((Window-Text (Field 105)) -match 'too long' -and -not(Enabled 106))
    Edit 140 $uid;Edit 141 'not_a_group_openpilot';Check 'an unknown group is identified separately' ((Review) -and (Window-Text (Field 105)) -match 'Group could not be resolved' -and -not(Enabled 106))
    Edit 141 $gid;$before=Native "stat -c %Z '$linux/single/file β:star*'"
    Check 'an unchanged numeric-ID draft disables Apply' ((Review) -and (Prop 'Reviewed') -eq 1 -and -not(Enabled 106) -and (Native "stat -c %Z '$linux/single/file β:star*'") -eq $before)
    Press 111;Check 'Reset clears both identity fields and their resolved draft' ((Window-Text (Field 140)) -eq '' -and (Window-Text (Field 141)) -eq '' -and (Prop 'Reviewed') -eq 0)
    Edit 140 'root';Review|Out-Null;Press 106
    Check 'default-user denial is reported without automatic elevation' ((Done) -and (Prop 'Applied') -eq 0 -and (Prop 'Failed') -eq 1 -and (Row 'file β:star*') -match 'Permission denied' -and (IDs "$linux/single/file β:star*") -eq "${uid}:$gid")
    Close-Editor|Out-Null;Open-Editor;Check 'newly opened editor captures explicitly configured root' ((Loaded 1) -and (Window-Text (Field 108)) -eq 'WSL user: root')
    Owner-View;Edit 141 'root';Review|Out-Null;Press 106
    Check 'group-only Apply changes group and retains owner and mode' ((Done) -and (Prop 'Applied') -eq 1 -and (Prop 'Failed') -eq 0 -and (IDs "$linux/single/file β:star*") -eq "${uid}:0" -and (Mode "$linux/single/file β:star*") -eq '640')
    Check 'returning to permissions reads the verified current metadata' ((Back-View) -and (Loaded 1) -and (Window-Text (Field 101)) -eq '0640' -and (Window-Text (Field 103)) -match 'gid 0' -and (Window-Text (Field 108)) -eq 'WSL user: root')
    Close-Editor|Out-Null
    Check 'mixed fixture opens' (Navigate "$unc\mixed");Open-Editor;Check 'six selected items include five supported nodes' ((Loaded 5) -and (Prop 'ItemCount') -eq 6)
    Owner-View;Edit 140 '0';Check 'numeric owner Review succeeds for the selection' ((Review) -and (Prop 'Reviewed') -eq 1)
    Check 'blank group preserves each distinct group and no-op owner is visible' ((Row 'a-own') -match "$gid -> $gid" -and (Row 'b-root') -match '0 -> 0' -and (Row 'b-root') -match 'No change requested' -and (Row 'c-folder') -match '0 -> 0')
    Check 'special-bit side effects are called out in the per-item preview' ((Row 'a-own') -match '6755; may clear setid' -and (Enabled 106))
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '01_ownership_preview'}finally{$script:main=$savedMain}
    Press 143;Check 'a reviewed ownership change cannot be silently discarded by switching views' ((Window-Text (Field 105)) -match 'Apply or reset ownership')
    Native "chmod 600 '$linux/mixed/e-stale'; mv '$linux/mixed/f-replace' '$linux/replaced/original'; printf replacement > '$linux/mixed/f-replace'; chmod 664 '$linux/mixed/f-replace'"|Out-Null
    Press 106;Check 'partial Apply reports two verified changes, three failures and one no-op' ((Done) -and (Prop 'Applied') -eq 2 -and (Prop 'Failed') -eq 3 -and (Window-Text (Field 105)) -match '1 unchanged') (Window-Text (Field 105))
    Check 'actual owner changes retain each original group' ((IDs "$linux/mixed/a-own") -eq "0:$gid" -and (IDs "$linux/mixed/c-folder") -eq '0:0' -and (IDs "$linux/mixed/b-root") -eq '0:0')
    Check 'verified regular-file setid loss is reported as the actual mode' ((Mode "$linux/mixed/a-own") -eq '755' -and (Row 'a-own') -match '0755' -and (Row 'a-own') -match 'Applied and verified' -and (Prop 'Unverified') -eq 0)
    Check 'directory ownership is nonrecursive and retains its mode' ((Mode "$linux/mixed/c-folder") -eq '2750' -and (IDs "$linux/mixed/c-folder/nested" -AsRoot) -eq "${uid}:$gid" -and (Mode "$linux/mixed/c-folder/nested" -AsRoot) -eq '640')
    Check 'stale and replaced files retain their actual owner and content' ((IDs "$linux/mixed/e-stale") -eq "${uid}:0" -and (IDs "$linux/mixed/f-replace") -eq "${uid}:$gid" -and (Native "cat '$linux/mixed/f-replace'") -eq 'replacement')
    Check 'rows distinguish stale files and excluded symlinks' ((Row 'e-stale') -match 'Changed/replaced; Reload' -and (Row 'f-replace') -match 'Changed/replaced; Reload' -and (Row 'd-link') -match 'Unsupported item/helper' -and (Native "readlink '$linux/mixed/d-link'") -eq 'a-own')
    Check 'stale metadata is unavailable until Reload' ((Prop 'ReadyCount') -eq 3 -and (Row 'e-stale') -match '----')
    Press 107;Check 'Reload refreshes all supported items and discards the resolved draft' ((Loaded 5) -and (Prop 'Reviewed') -eq 0 -and -not(Enabled 106))
    Edit 140 $user;Edit 141 $group;Check 'both account names resolve to numeric targets' ((Review) -and (Window-Text (Field 144)) -match "uid $uid" -and (Window-Text (Field 144)) -match "gid $gid")
    Press 111;Check 'Reset after name resolution clears all proposed changes' ((Prop 'Reviewed') -eq 0 -and -not(Enabled 106))
    Edit 140 $user;Edit 141 $group;Review|Out-Null;Press 106
    Check 'both owner and group apply independently per item with no-op skipping' ((Done) -and (Prop 'Applied') -eq 4 -and (Prop 'Failed') -eq 1 -and (IDs "$linux/mixed/a-own") -eq "${uid}:$gid" -and (IDs "$linux/mixed/b-root") -eq "${uid}:$gid" -and (IDs "$linux/mixed/c-folder") -eq "${uid}:$gid" -and (IDs "$linux/mixed/e-stale") -eq "${uid}:$gid") (Window-Text (Field 105))
    Close-Editor|Out-Null;Settings-User $false
    Check 'single default-user group-change fixture opens' (Navigate "$unc\single");Open-Editor;Check 'single metadata loads again' (Loaded 1);Owner-View
    Edit 141 $gid;Review|Out-Null;Press 106
    Check 'an owner can select its permitted group without root' ((Done) -and (Prop 'Applied') -eq 1 -and (IDs "$linux/single/file β:star*") -eq "${uid}:$gid" -and (Window-Text (Field 108)) -eq 'WSL user: distro default')
    Edit 140 '4294967294';Check 'an unmapped numeric owner is explicit in the preview' ((Review) -and (Window-Text (Field 144)) -match 'Unmapped \(uid 4294967294\)')
    Check 'Close discards a resolved ownership draft without changing the file' ((Close-Editor) -and (IDs "$linux/single/file β:star*") -eq "${uid}:$gid")
    Settings-User $true
    Check 'cancellation fixture opens' (Navigate "$unc\stop");Open-Editor;Check 'sixteen permission snapshots load' (Loaded 16);Owner-View
    Edit 140 'root';Review|Out-Null;[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]106,[IntPtr]::Zero)|Out-Null;Post $script:dialog 0x111 109 0
    Check 'Stop finishes the current ownership request and skips remaining items' ((Done) -and (Prop 'Stopped') -gt 0 -and (Prop 'Applied')+(Prop 'Stopped') -eq 16 -and (Prop 'Failed') -eq 0)
    $changed=[int](Native "find '$linux/stop' -type f -uid 0 | wc -l");$kept=[int](Native "find '$linux/stop' -type f -uid $uid | wc -l")
    Check 'real ownership exactly matches the stopped batch counts' ($changed -eq (Prop 'Applied') -and $kept -eq (Prop 'Stopped') -and $changed+$kept -eq 16)
    Check 'ownership cancellation never changes ordinary modes' ((Native "find '$linux/stop' -type f -perm 640 | wc -l") -eq '16')
    Press 107;Check 'Reload after cancellation restores all fresh snapshots' ((Loaded 16) -and -not(Enabled 106))
    Back-View|Out-Null;Check 'permissions view reloads after ownership cancellation' (Loaded 16)
    Owner-View -NoWait;Post $script:dialog 0x111 109 0
    Check 'ownership metadata reads can also be stopped' ((Done) -and (Prop 'Stopped') -gt 0 -and (Prop 'Completed')+(Prop 'Stopped') -eq 16)
    Check 'returning from a stopped read remains possible' ((Back-View) -and (Loaded 16))
    Owner-View;Edit 140 'root';[FpUi]::SendMessageW($script:dialog,0x111,[IntPtr]142,[IntPtr]::Zero)|Out-Null
    Check 'app shutdown releases an in-flight identity review cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\ownership-profile.csv" -Raw
    Check 'ownership transitions and workers have no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'ownership editing keeps UI dispatch below 16 ms' ($match.Success -and [double]$match.Groups[1].Value -lt 16) $match.Value
}finally{
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'" -AsRoot|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
