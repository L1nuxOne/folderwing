<# Real-app bulk rename acceptance with private Windows/native WSL fixtures. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class RenameProbe {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\bulk_rename'
. "$root\tools\build-output.ps1"
New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0;$script:dialog=[IntPtr]::Zero
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command){$v=& wsl.exe --exec sh -c $command;if($LASTEXITCODE){throw 'Private WSL rename fixture failed'};($v -join "`n").TrimEnd()}
function Dialog {[FpUi]::FindWindowW('OpenPilotBulkRename','Bulk rename')}
function Field([int]$id){[RenameProbe]::GetDlgItem($script:dialog,$id)}
function Enabled([int]$id){[RenameProbe]::IsWindowEnabled((Field $id))}
function Prop([string]$name){[FpUi]::GetPropW($script:dialog,"BulkRename.$name").ToInt64()}
function Press([int]$id){Post $script:dialog 0x111 $id 0;Start-Sleep -Milliseconds 100}
function Ready {Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero -and (Prop 'ItemCount') -gt 0 -and (Prop 'Busy') -eq 0} 7000}
function Close-Rename {Press 2;Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000}
function Edit([int]$id,[string]$value){Chord (Field $id) 'Ctrl' 0x41 10;Post (Field $id) $WM_CHAR 8 0;Send-Text (Field $id) $value}
function Toggle([int]$id){Click (Field $id) 12 12;Post (Field $id) 0x202 0 0;Start-Sleep -Milliseconds 30}
function Row([int]$index){$s=[Text.StringBuilder]::new(3400);[FpUi]::SendMessageW((Field 109),0x189,[IntPtr]$index,$s)|Out-Null;$s.ToString()}
function Select-All {Click $pane0 ([int](65*$scale)) ([int](74*$scale));Post $pane0 0x202 0 0;Chord $pane0 'Ctrl' 0x41 20}
function Open-Rename([switch]$F2){Select-All;if($F2){Key $pane0 0x71 0}else{Chord $pane0 @('Ctrl','Shift') 0x71 0};if(-not(Ready)){throw 'Bulk rename did not load'}}
function Navigate([string]$path){
    Chord $pane0 'Ctrl' 0x4C 20;Send-Text (Pane-Edits $pane0)[0] $path;Key (Pane-Edits $pane0)[0] 0x0D 20
    Wait-Until {(Window-Text (Pane-Edits $pane0)[0]) -ceq $path -and [FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000
}
function Preview {Press 111;Ready}
function Apply {Press 112;Ready}
$scratch=Join-Path $env:TEMP "openpilot_bulk_rename_$PID";$linux="/tmp/openpilot-bulk-rename-$PID";$p=$null;$faultRuntime=$null
try{
    foreach($dir in @('main','collision','stale','invalid','folders','elsewhere')){New-Item -ItemType Directory -Force "$scratch\$dir"|Out-Null}
    foreach($dir in @('collision','stale')){[IO.File]::WriteAllText("$scratch\$dir\a.txt",'alpha');[IO.File]::WriteAllText("$scratch\$dir\b.txt",'beta')}
    [IO.File]::WriteAllText("$scratch\main\file01.txt",'first');[IO.File]::WriteAllText("$scratch\main\file02.txt",'second')
    [IO.File]::WriteAllText("$scratch\invalid\a.txt",'keep')
    New-Item -ItemType Directory -Force "$scratch\folders\folder.txt"|Out-Null;[IO.File]::WriteAllText("$scratch\folders\folder.txt\inside.txt",'keep')
    [IO.File]::WriteAllText("$scratch\folders\file.txt",'keep')
    $settings="$scratch\settings.ini";$baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n"
    Set-Content -LiteralPath $settings -Value $baseSettings
    $distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/main' '$linux/many'; printf alpha > '$linux/main/a:b.txt'; printf beta > '$linux/main/b.txt'"|Out-Null
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\rename-profile.csv","$scratch\main","$scratch\main")
    Check 'ordinary browsing has no bulk rename worker window' ((Dialog) -eq [IntPtr]::Zero)
    Check 'Windows fixture loads' (Wait-Until {[FpUi]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000)
    Select-All;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'multiple selection offers Bulk rename' ($menu -and $menu.Items -contains 'Bulk rename...')
    if(-not $menu -or -not(Pick-Menu $menu 'Bulk rename...')){throw 'Bulk rename context action missing'}
    Check 'editor reads the captured selection asynchronously' ((Ready) -and (Prop 'ItemCount') -eq 2)
    Check 'extensions are preserved by default and Apply requires a changed preview' ([FpUi]::SendMessageW((Field 105),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 1 -and -not(Enabled 112))
    Edit 101 'file';Edit 102 'photo';Edit 103 '2026_';Edit 104 '_edited';Toggle 106;Edit 107 '7';Edit 108 '3'
    Check 'editing rules does not touch the filesystem' ((Test-Path "$scratch\main\file01.txt") -and -not(Test-Path "$scratch\main\2026_photo01_edited_007.txt"))
    Check 'preview shows literal replacements and numbering in captured order' ((Preview) -and (Row 0).Contains('2026_photo01_edited_007.txt') -and (Row 1).Contains('2026_photo02_edited_008.txt') -and (Prop 'Reviewed') -eq 1 -and (Enabled 112)) (Row 0)
    Edit 103 'changed_'
    Check 'rule edits invalidate the previous preview before Apply' ((Prop 'Reviewed') -eq 0 -and -not(Enabled 112) -and (Window-Text (Field 110)) -match 'Preview again')
    Edit 103 '2026_';Check 'fresh preview enables Apply again' ((Preview) -and (Enabled 112))
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '01_windows_preview'}finally{$script:main=$savedMain}
    Check 'pane navigation works while the rename draft is open' (Navigate "$scratch\elsewhere")
    Check 'Apply renames both files in the captured source folder' ((Apply) -and (Prop 'Renamed') -eq 2 -and (Prop 'Errors') -eq 0 -and (Row 0) -match 'Renamed')
    Check 'contents and extensions survive the Windows batch' ([IO.File]::ReadAllText("$scratch\main\2026_photo01_edited_007.txt") -eq 'first' -and [IO.File]::ReadAllText("$scratch\main\2026_photo02_edited_008.txt") -eq 'second' -and -not(Test-Path "$scratch\main\file01.txt"))
    Check 'completion keeps the navigated pane and requires another preview' ((Window-Text (Pane-Edits $pane0)[0]) -eq "$scratch\elsewhere" -and -not(Enabled 112))
    Check 'completed editor closes cleanly' (Close-Rename)
    Check 'collision fixture opens' (Navigate "$scratch\collision");Open-Rename -F2
    Check 'F2 on a selection opens bulk rename without an inline editor' ((Prop 'ItemCount') -eq 2 -and (Pane-Edits $pane0).Count -eq 1)
    Edit 101 'a';Edit 102 'b'
    Check 'duplicate outputs and occupied selected names block Apply' ((Preview) -and (Prop 'Errors') -eq 2 -and -not(Enabled 112) -and (Row 0) -match 'collision')
    Check 'collision preview leaves both originals intact' ([IO.File]::ReadAllText("$scratch\collision\a.txt") -eq 'alpha' -and [IO.File]::ReadAllText("$scratch\collision\b.txt") -eq 'beta')
    Edit 101 '';Edit 102 '';Edit 103 'new_';Check 'a corrected draft is reviewable' ((Preview) -and (Enabled 112))
    [IO.File]::WriteAllText("$scratch\collision\new_b.txt",'existing')
    Check 'a new destination after preview blocks the whole batch' ((Apply) -and (Prop 'Renamed') -eq 0 -and (Prop 'Errors') -eq 1 -and -not(Test-Path "$scratch\collision\new_a.txt"))
    Check 'the occupied target and original files are never replaced' ([IO.File]::ReadAllText("$scratch\collision\new_b.txt") -eq 'existing' -and [IO.File]::ReadAllText("$scratch\collision\b.txt") -eq 'beta')
    Remove-Item -LiteralPath "$scratch\collision\new_b.txt";Check 'Preview then Apply works after resolving a collision' ((Preview) -and (Apply) -and (Prop 'Renamed') -eq 2)
    Close-Rename|Out-Null
    Check 'stale selection fixture opens' (Navigate "$scratch\stale");Open-Rename;Edit 103 'new_';Preview|Out-Null
    Move-Item -LiteralPath "$scratch\stale\b.txt" -Destination "$scratch\old-b.txt";[IO.File]::WriteAllText("$scratch\stale\b.txt",'replacement')
    Check 'a replaced source invalidates the review before any rename' ((Apply) -and (Prop 'Renamed') -eq 0 -and (Prop 'Errors') -eq 1 -and (Row 1) -match 'changed' -and -not(Test-Path "$scratch\stale\new_a.txt"))
    Check 'the replacement keeps its name and contents' ([IO.File]::ReadAllText("$scratch\stale\b.txt") -eq 'replacement')
    Close-Rename|Out-Null
    Check 'single-item bulk draft opens' (Navigate "$scratch\invalid");Open-Rename;Edit 101 'a';Edit 102 'CON'
    Check 'reserved Windows output names are rejected in preview' ((Preview) -and (Prop 'Errors') -eq 1 -and -not(Enabled 112) -and (Row 0) -match 'Invalid')
    Edit 101 '';Edit 102 '';Toggle 106;Edit 107 '999999999';Edit 108 '10';Press 111
    Check 'invalid numbering does not produce a truncated draft' (-not(Enabled 112) -and (Window-Text (Field 110)) -match '1–9 digits')
    Close-Rename|Out-Null
    Key $pane0 0x71 20
    Check 'F2 on a single item still opens the inline rename editor' (Wait-Until {(Pane-Edits $pane0).Count -eq 2} 3000)
    $edits=Pane-Edits $pane0;if($edits.Count -eq 2){Key $edits[1] 0x1B 20}
    Check 'file and folder extension fixture opens' (Navigate "$scratch\folders");Open-Rename;Edit 104 '_new'
    Check 'extension preservation treats folders as complete names' ((Preview) -and (Row 0).Contains('folder.txt_new') -and (Row 1).Contains('file_new.txt'))
    Check 'folder rename leaves all nested names and contents unchanged' ((Apply) -and (Test-Path "$scratch\folders\folder.txt_new\inside.txt") -and [IO.File]::ReadAllText("$scratch\folders\folder.txt_new\inside.txt") -eq 'keep')
    Close-Rename|Out-Null
    Check 'native WSL fixture opens' (Navigate "$unc\main");Open-Rename
    Check 'the explicitly captured WSL user is visible' ((Window-Text (Field 115)) -match 'WSL user: distro default')
    Edit 101 ':';Edit 102 '_';Edit 103 'linux_'
    Check 'Linux preview shows literal reserved names accurately' ((Preview) -and (Row 0).Contains('a:b.txt') -and (Row 0).Contains('linux_a_b.txt') -and (Enabled 112))
    Native "chmod 500 '$linux/main'"|Out-Null
    Check 'permission failures produce real per-file results without elevation' ((Apply) -and (Prop 'Renamed') -eq 0 -and (Prop 'Errors') -eq 2 -and (Row 0) -match 'Permission denied')
    Native "chmod 700 '$linux/main'"|Out-Null
    Check 'a fresh preview and Apply rename real native Linux entries' ((Preview) -and (Apply) -and (Prop 'Renamed') -eq 2 -and (Native "cat '$linux/main/linux_a_b.txt'") -eq 'alpha' -and (Native "cat '$linux/main/linux_b.txt'") -eq 'beta')
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '02_wsl_results'}finally{$script:main=$savedMain}
    Close-Rename|Out-Null
    $maker="from pathlib import Path; p=Path('$linux/many'); [(p/('item%03d.txt'%i)).write_text('keep') for i in range(256)]"
    & wsl.exe --exec python3 -c $maker;if($LASTEXITCODE){throw 'Could not create the bounded cancellation fixture'}
    Check 'maximum selection fixture opens' (Navigate "$unc\many");Open-Rename;Edit 103 'new_'
    Check '256 selected items have a complete reviewable preview' ((Preview) -and (Prop 'ItemCount') -eq 256 -and (Enabled 112))
    Post $script:dialog 0x111 112 0;Post $script:dialog 0x111 113 0;Start-Sleep -Milliseconds 100
    Check 'Cancel retains stopped per-file results and disables stale Apply' ((Ready) -and (Prop 'Stopped') -gt 0 -and -not(Enabled 112) -and (Window-Text (Field 110)) -match 'stopped')
    Check 'cancellation preserves files not attempted' ((Native "find '$linux/many' -maxdepth 1 -type f -name 'item*.txt' | wc -l") -ne '0')
    Press 111;Post $script:dialog 0x111 2 0
    Check 'closing during a preview stops the request and releases the window' (Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000)
    Check 'the private settings watcher is ready' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 3000)
    Set-Content -LiteralPath $settings -Value ($baseSettings.Replace('[functions]',"[functions]`nwsl.helper = off"));Start-Sleep -Milliseconds 500
    Open-Rename
    Check 'disabled native integration reports an unavailable helper instead of renaming through 9P' ((Prop 'Errors') -eq 256 -and (Row 0) -match 'helper unavailable' -and -not(Enabled 112))
    Close-Rename|Out-Null
    Set-Content -LiteralPath $settings -Value ($baseSettings.Replace('[functions]',"[functions]`nfile.bulk_rename = off"));Start-Sleep -Milliseconds 500
    Select-All;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'disabling the function removes it from the context menu' ($menu -and $menu.Items -notcontains 'Bulk rename...')
    if($menu){Key $menu.Window 0x1B 20}
    Key $pane0 0x71 20
    Check 'disabled bulk F2 does not rename one arbitrary selected item' ((Dialog) -eq [IntPtr]::Zero -and (Pane-Edits $pane0).Count -eq 1)
    Check 'the app closes cleanly after batch work' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\rename-profile.csv" -Raw
    Check 'bulk rename has no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'bulk rename keeps UI dispatch below 16 ms' ($match.Success -and [double]$match.Groups[1].Value -lt 16)
    # A private runtime copy injects reply loss without touching product helpers
    # or terminating any process. Never retry a rename with an unknown outcome.
    $faultRuntime="$out\fault-runtime-$PID";New-Item -ItemType Directory -Force $faultRuntime|Out-Null
    Copy-Item $exe "$faultRuntime\folderwing.exe"
    Publish-BuildAsset "$root\build\$Config\folderwing-wsl" $faultRuntime 'folderwing-wsl'
    $zig=$env:ZIG;if(-not $zig){$found=Get-Command zig -ErrorAction SilentlyContinue;$zig=if($found){$found.Source}else{Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}}
    & $zig cc -target x86_64-linux-musl -static -std=c11 -O2 -s "-I$root\src" "$root\tests\fixtures\rename_reply_loss.c" -o "$out\reply-loss-helper"
    if($LASTEXITCODE){throw 'Could not build the private reply-loss fixture'}
    Set-Content -LiteralPath $settings -Value $baseSettings
    $p=Start-Demo "$faultRuntime\folderwing.exe" @("--settings=$settings","--profile-out=$out\fault-profile.csv","$unc\main","$unc\main")
    Open-Rename;Edit 103 'fault_'
    Check 'reply-loss fixture starts with a real native preview' ((Preview) -and (Enabled 112))
    $nativeRuntime=Native "wslpath -u '$faultRuntime'";$nativeFault=Native "wslpath -u '$out\reply-loss-helper'"
    Native "cp '$nativeFault' '$nativeRuntime/folderwing-wsl.next'; chmod 755 '$nativeRuntime/folderwing-wsl.next'; mv -f '$nativeRuntime/folderwing-wsl.next' '$nativeRuntime/folderwing-wsl'"|Out-Null
    Check 'a missing apply reply is labelled unknown for every item' ((Apply) -and (Prop 'Uncertain') -eq 2 -and (Window-Text (Field 110)) -match 'could not be confirmed') ("uncertain=$(Prop 'Uncertain'); errors=$(Prop 'Errors'); row=$(Row 0); status=$(Window-Text (Field 110))")
    $trace=Get-Content "$faultRuntime\reply-loss-trace" -Raw
    Check 'the injected fault occurs after the rename request was consumed' ($trace -match 'request_consumed=1')
    Copy-Item "$faultRuntime\reply-loss-trace" "$out\reply-loss-trace.log" -Force
    Check 'unknown outcomes disable Apply and Preview rather than retrying' (-not(Enabled 112) -and -not(Enabled 111)) (Window-Text (Field 110))
    Check 'the fault fixture leaves the real files and contents intact' ((Native "cat '$linux/main/linux_a_b.txt'") -eq 'alpha' -and (Native "test ! -e '$linux/main/fault_linux_a_b.txt'; printf intact") -eq 'intact')
    Check 'closing an uncertain result releases the app cleanly' ((Close-Rename) -and (Stop-Demo $p));$p=$null
    $faultReport=Get-Content "$out\fault-profile.csv" -Raw
    Check 'reply loss has no function lifetime violations' ($faultReport -match 'metric,fn_violations,,,,0,')
    Remove-Item -Recurse -Force $faultRuntime;$faultRuntime=$null

}finally{
    if($p){Stop-Demo $p|Out-Null}
    if($faultRuntime){Remove-Item -Recurse -Force $faultRuntime -ErrorAction SilentlyContinue}
    Native "chmod -R u+rwX '$linux'; rm -rf '$linux'"|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
