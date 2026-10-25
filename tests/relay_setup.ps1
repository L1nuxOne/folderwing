<# Real Settings command creation, preview, atomic save, import and invocation. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
$root=Split-Path $PSScriptRoot -Parent;$exe=Join-Path $root "build\$Config\folderwing.exe"
$out=Join-Path $root 'build\relays';New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Helper {
    $h=[IntPtr]::Zero
    while(($h=[FpUi]::FindWindowExW([IntPtr]::Zero,$h,'FpSettingsHidden',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        $owner=[uint32]0;[FpUi]::GetWindowThreadProcessId($h,[ref]$owner)|Out-Null
        $proc=Get-Process -Id $owner -ErrorAction SilentlyContinue;if($proc -and $proc.Parent.Id -eq $script:demoPid){return $h}
    };[IntPtr]::Zero
}
function Caption { $s=[Text.StringBuilder]::new(1200);[FpUi]::GetWindowTextW((Helper),$s,1200)|Out-Null;$s.ToString() }
function Slot { $side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero);if($side -eq [IntPtr]::Zero){return [IntPtr]::Zero};[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero) }
function Settings-Ready {
    $h=Helper;$slot=Slot
    $h -ne [IntPtr]::Zero -and $slot -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($slot) -and
    [FpUi]::GetPropW($h,'Settings.Ready') -ne [IntPtr]::Zero -and
    ($script:settingsHelper -eq [IntPtr]::Zero -or $h -eq $script:settingsHelper)
}
function Open-Settings {
    if((Helper) -ne [IntPtr]::Zero -or ((Slot) -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible((Slot)))){throw 'Previous Settings helper or input target has not closed'}
    $script:settingsHelper=[IntPtr]::Zero
    Chord $pane0 'Ctrl' 0xBC 0
    if(-not(Wait-Until {Settings-Ready} 3000)){throw 'A new visible initialized Settings helper did not become ready within3000ms'}
    $script:settingsHelper=Helper
    $script:settingsTarget=New-SettingsTarget
    $script:settingsPid=[uint32]0;[FpUi]::GetWindowThreadProcessId($script:settingsHelper,[ref]$script:settingsPid)|Out-Null
    $true
}
function Point([string]$name){$n=[FpUi]::GetPropW((Helper),$name).ToInt64();if(-not $n){return $null};$x=$n -band 65535;$y=($n -shr 16) -band 65535;if($y -ge 32768){$y-=65536};@([int]$x,[int]$y)}
# A query is admitted only after the exact current helper has consumed it.
function Select-CommandQuery([string]$query){
    $clock=[Diagnostics.Stopwatch]::StartNew();$before=Settings-State $script:settingsTarget (Input-Remaining $clock 3000)
    $null=Send-SettingsClick $script:settingsTarget ([int](40*$scale)) ([int](26*$scale)) (Input-Remaining $clock 3000)
    $null=Send-SettingsKey $script:settingsTarget 0x41 @('Ctrl') (Input-Remaining $clock 3000)
    $null=Send-SettingsText $script:settingsTarget $query (Input-Remaining $clock 3000)
    $hash=[FpUi]::SettingsHash($query)
    $state=Wait-SettingsState $script:settingsTarget {param($v)$v.Have -eq 1 -and $v.Snapshot -eq $before.Snapshot -and $v.Pending -eq 0 -and $v.Builder -eq 0 -and $v.FocusKind -eq 0 -and $v.QueryHash -eq $hash -and $v.QueryLength -eq $query.Length} $clock 3000 'exact relay search query before Reach'
    $script:relayQueryContract=@{hash=$hash;length=$query.Length;snapshot=$state.Snapshot;helper=$script:settingsTarget.Helper.ToInt64();pid=$script:settingsTarget.Pid;started=$script:settingsTarget.Started}
    @{phase='query-admitted';state=$state;helper=$script:relayQueryContract;elapsed_ms=$clock.Elapsed.TotalMilliseconds}|ConvertTo-Json -Depth 5 -Compress|Add-Content "$out\query-$PID.jsonl"
}
function Assert-RelayQuery {
    if(-not $script:relayQueryContract){return}
    Assert-SettingsTarget $script:settingsTarget
    $c=$script:relayQueryContract;$v=[FpUi]::ReadSettingsState($script:settingsTarget.Helper)
    if(-not $v){throw 'Coherent relay query state unavailable before Reach; no edit click submitted'}
    if($v.Snapshot -ne $c.snapshot -or $v.QueryHash -ne $c.hash -or $v.QueryLength -ne $c.length -or $v.FocusKind -ne 0 -or $v.Builder -ne 0 -or $v.Pending){
        @{phase='query-invalid-before-Reach';expected=$c;state=$v}|ConvertTo-Json -Depth 5 -Compress|Add-Content "$out\query-$PID.jsonl"
        throw 'Relay query/helper snapshot changed before Reach; no edit click submitted'
    }
}
function Reach([string]$name){
    if(-not(Settings-Ready)){throw 'Settings input requires its owned visible initialized helper'}
    Assert-RelayQuery
    for($i=0;$i -lt 80;$i++){$xy=Point $name;$r=[FpUi+RECT]::new();[FpUi]::GetClientRect((Slot),[ref]$r)|Out-Null
        if($xy -and $xy[1] -gt [int](90*$scale) -and $xy[1] -lt ($r.B-15)){Assert-RelayQuery;return $xy}
        if(-not $xy){Start-Sleep -Milliseconds 25;continue}
        $delta=if($xy[1] -lt 90*$scale){120}else{-120};Post (Slot) $WM_MOUSEWHEEL (($delta -band 65535) -shl 16);Start-Sleep -Milliseconds 45
    };throw "Control unreachable: $name | $(Caption)"
}
function Press([int]$action,[int]$sub=0){$xy=Reach "Command.Action.$action.$sub";$null=Send-SettingsClick $script:settingsTarget $xy[0] $xy[1]}
function Field([string]$name,[string]$value){
    $xy=Reach "Command.Field.$name";$clock=[Diagnostics.Stopwatch]::StartNew()
    $null=Send-SettingsClick $script:settingsTarget $xy[0] $xy[1] (Input-Remaining $clock 3000)
    $null=Send-SettingsKey $script:settingsTarget 0x41 @('Ctrl') (Input-Remaining $clock 3000)
    $null=Send-SettingsText $script:settingsTarget $value (Input-Remaining $clock 3000)
    if($name -eq 'name'){
        $identity='\| ID '+[regex]::Escape($value)+' \|'
        if(-not(Wait-Until {(Settings-Ready) -and (Caption) -cmatch $identity} (Input-Remaining $clock 3000))){throw "Command ID input was not acknowledged: expected=$value actual=$(Caption)"}
    }
}
function New-Command {
    if(-not(Settings-Ready)){throw 'Settings is not ready to open a command draft'}
    Click (Slot) ([int]((Client-Width (Slot))-65*$scale)) ([int](26*$scale))
    if(-not(Wait-Until {(Settings-Ready) -and (Caption) -match 'Custom commands.*New'} 3000)){throw 'New command draft did not open within3000ms'}
    $true
}
function Save {
    if(-not(Settings-Ready)){throw 'Settings is not ready to save'}
    $null=Invoke-SettingsSave $script:settingsTarget 0x53 @('Ctrl') 1 4000
    $true
}
function Close-Settings {
    if(-not(Settings-Ready)){throw 'Settings is not ready to close'}
    $oldPid=$script:settingsPid
    Chord (Slot) 'Ctrl' 0xBC 0
    if(-not(Wait-Until {
        $slot=Slot
        -not(Get-Process -Id $oldPid -ErrorAction SilentlyContinue) -and (Helper) -eq [IntPtr]::Zero -and
        ($slot -eq [IntPtr]::Zero -or -not [FpUi]::IsWindowVisible($slot))
    } 3000)){throw 'Settings helper and input target did not close within3000ms'}
    $script:settingsHelper=[IntPtr]::Zero;$script:settingsPid=0;$true
}
$scratch=Join-Path $env:TEMP "openpilot_relays_$PID";$p=$null
try {
    $folder="$scratch\files with spaces";$dest="$scratch\other";New-Item -ItemType Directory -Force $folder,$dest|Out-Null
    Set-Content "$folder\a b.txt" 'text';Set-Content "$folder\c.dat" 'data'
    $settings="$scratch\settings.ini";Set-Content $settings "; keep this comment`n[functions]`npreview.text = off`n[panel.side]`nslide = off"
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\relay-profile.csv",$folder,$dest)
    Check 'no Settings helper is created at startup' ((Helper) -eq [IntPtr]::Zero)
    $null=Open-Settings
    $ready=Wait-Until {
        $helper=Helper
        Settings-Ready
    } 3000
    Check 'Settings is ready for command setup' $ready
    if(-not $ready){
        $helper=Helper
        Write-Host "Settings startup state: children=$((Get-Process folderwing-settings -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}|ForEach-Object Id)-join ','); window=$helper; snapshot=$([FpUi]::GetPropW($helper,'Settings.Ready'))"
        if($env:OPENPILOT_PANEL_STARTUP_LOG -and (Test-Path $env:OPENPILOT_PANEL_STARTUP_LOG)){
            Get-Content $env:OPENPILOT_PANEL_STARTUP_LOG -Tail 16|Write-Host
        }
        throw 'Settings did not apply its first snapshot within3000ms'
    }
    Check 'Commands opens the form inside Settings' (New-Command) (Caption)
    Field 'name' 'write_list';Field 'title' 'Write a file list';Field 'program' 'cmd';Field 'arguments' '/d /c echo %* > "%D\result.txt"';Field 'types' 'txt'
    Press 14 4 # Hidden window
    Press 8 # record shortcut
    Chord (Slot) @('Ctrl','Alt') 0x77 20 # F8
    Press 16 # preview
    Check 'preview checks the program on a worker' (Wait-Until {(Caption) -match 'Program found'} 5000) (Caption)
    Set-Clipboard 'waiting';Press 17 # copy preview
    Check 'preview resolves selected files and current folder' (Wait-Until {
        $script:preview=Get-Clipboard -Raw -ErrorAction SilentlyContinue
        $script:preview -match [regex]::Escape("`"$folder\a b.txt`"") -and $script:preview.Contains("$folder\result.txt")
    } 3000) $script:preview
    Check 'preview never runs the command' (-not (Test-Path "$folder\result.txt"))
    Shot $out '01_preview'
    Check 'the form saves a complete new command' (Save) (Caption)
    $saved=Get-Content $settings -Raw
    if($saved -notmatch '(?m)^\[relay\.write_list\]\r?$'){throw 'Saved command identity was not write_list; dependent invocations are unsafe'}
    Check 'title, arguments and file filters are saved together' ($saved -match '\[relay.write_list\]' -and $saved -match 'title = Write a file list' -and $saved -match 'types = txt' -and $saved -match 'window = hidden')
    Check 'recorded shortcut is saved with the command' ($saved -match 'relay.write_list = Ctrl\+Alt\+F8') $saved
    Check 'saving preserves existing comments and settings' ($saved -match '; keep this comment' -and $saved -match 'preview.text = off')
    Check 'saving does not execute the command' (-not (Test-Path "$folder\result.txt"))
    Check 'Settings closes without losing the saved command' (Close-Settings)
    Post $pane0 $WM_CONTEXTMENU 0 -1;$menu=Open-Menu
    Check 'the new command appears in the file menu' ($menu -and $menu.Items -contains 'Write a file list')
    if($menu){Check 'the new menu command runs with resolved arguments' ((Pick-Menu $menu 'Write a file list') -and (Wait-Until {Test-Path "$folder\result.txt"} 4000))}
    Check 'the invoked command receives the selected filename' ((Get-Content "$folder\result.txt" -Raw) -match [regex]::Escape('a b.txt'))
    Remove-Item "$folder\result.txt"
    Chord $pane0 @('Ctrl','Alt') 0x77 20
    Check 'the saved shortcut invokes the command' (Wait-Until {Test-Path "$folder\result.txt"} 4000)
    Check 'Settings can reopen after command creation' (Open-Settings)
    New-Command|Out-Null
    Field 'name' 'bad_placeholder';Field 'title' 'Bad placeholder';Field 'program' 'cmd';Field 'arguments' '%Q'
    $null=Send-SettingsKey $script:settingsTarget 0x53 @('Ctrl') 2000
    Check 'unsupported placeholders are explained without saving' (Wait-Until {(Caption) -match 'Unknown placeholder'} 2000)
    Check 'invalid drafts leave the file unchanged' ((Get-Content $settings -Raw) -notmatch '\[relay.bad_placeholder\]')
    Field 'arguments' ''
    Set-Clipboard ('x'*1100);Chord (Slot) 'Ctrl' 0x56 20
    Check 'oversized pasted arguments are rejected without truncation' (Wait-Until {(Caption) -match 'too long.*not inserted'} 2000)
    Field 'arguments' '%*';Field 'keys' 'Ctrl+L'
    $null=Send-SettingsKey $script:settingsTarget 0x53 @('Ctrl') 2000
    Check 'shortcut conflicts require an explicit decision' (Wait-Until {(Caption) -match 'Shortcut also belongs'} 2000) (Caption)
    Field 'keys' 'none'
    Field 'name' 'write_list'
    $null=Send-SettingsKey $script:settingsTarget 0x53 @('Ctrl') 2000
    Check 'a duplicate command cannot silently overwrite another' (Wait-Until {(Caption) -match 'already exists'} 3000)
    Field 'name' 'conflict_test';Field 'title' 'Draft survives an external edit'
    Add-Content $settings "`n; external edit stays"
    Start-Sleep -Milliseconds 500
    try {
        $null=Invoke-SettingsSave $script:settingsTarget 0x53 @('Ctrl') 2 3000
        Check 'an external edit causes a conflict and keeps the draft' ((Caption) -match 'changed meanwhile|changed elsewhere') (Caption)
    } catch {
        Check 'an external edit causes a conflict and keeps the draft' $false $_.Exception.Message
        throw # no dependent second Save until this conflict has been acknowledged
    }
    Check 'a conflicted save adds no partial command' ((Get-Content $settings -Raw) -notmatch '\[relay.conflict_test\]')
    Check 'saving again applies the retained draft to current settings' (Save) (Caption)
    Check 'the external edit is retained' ((Get-Content $settings -Raw) -match '; external edit stays')
    Shot $out '02_saved'
    Press 1 # cancel back to Functions
    Check 'cancel returns to the function list' (Wait-Until {(Caption) -eq 'Settings'} 2000)
    # Editing uses the current function snapshot and keeps the stable identity.
    Select-CommandQuery 'relay.write_list'
    $xy=Reach 'Command.Edit.relay.write_list';$null=Send-SettingsClick $script:settingsTarget $xy[0] $xy[1];$script:relayQueryContract=$null
    Check 'a saved command opens in the same form for editing' (Wait-Until {(Caption) -match 'Custom commands.*Edit.*ID write_list'} 3000)
    Field 'title' 'Write a reviewed list';Press 11 # hide from context menus
    Check 'edits save without changing the command identity' (Save) (Caption)
    Check 'menu placement is saved independently of its shortcut' ((Get-Content $settings -Raw) -match '(?s)\[menu\].*relay.write_list = off')
    Press 1
    # Import a private Explorer command for the selected file association.
    Close-Settings|Out-Null
    $extension=".oprelay_$PID";$prog="OpenPilot.Relay.UI.$PID"
    New-Item -Path "HKCU:\Software\Classes\$extension" -Force|Out-Null
    Set-Item -Path "HKCU:\Software\Classes\$extension" -Value $prog
    $commandKey="HKCU:\Software\Classes\$prog\shell\proof\command"
    New-Item -Path $commandKey -Force|Out-Null
    Set-Item -Path $commandKey -Value ("cmd /d /c echo %1 > `"$folder\imported.txt`"")
    New-ItemProperty -Path "HKCU:\Software\Classes\$prog\shell\proof" -Name MUIVerb -Value 'OpenPilot import proof' -PropertyType String -Force|Out-Null
    Set-Content "$folder\proof$extension" 'proof'
    # Filter the source pane so the association used by discovery is explicit.
    Key $pane0 0x1B 20;Send-Text $pane0 "proof$extension"
    Check 'the import fixture is selected in the source pane' (Wait-Until {(Title) -match 'files with spaces'} 3000)
    Check 'Settings is available for imports' (Open-Settings)
    New-Command|Out-Null;Press 2
    Check 'Explorer import opens a read-only metadata catalogue' (Wait-Until {(Caption) -match 'Custom commands.*Import'} 3000)
    Field 'filter' 'OpenPilot import proof'
    $xy=Reach 'Command.Pick.OpenPilot import proof';Click (Slot) $xy[0] $xy[1];Post (Slot) 0x202 0 0
    Check 'a registered Explorer command becomes an editable draft' (Wait-Until {(Caption) -match 'Custom commands.*New.*ID openpilot_import_proof'} 3000) (Caption)
    Press 14 4
    Press 16;Check 'an imported command previews without invoking it' (Wait-Until {(Caption) -match 'Program found'} 4000)
    Check 'metadata discovery and preview do not launch the registered command' (-not (Test-Path "$folder\imported.txt"))
    Check 'an imported command saves through the same transaction' (Save) (Caption)
    $imported=Get-Content $settings -Raw
    Check 'import retains the selected-file placeholder and file-type scope' ($imported -match '\[relay.openpilot_import_proof\]' -and $imported -match 'types = oprelay_' -and $imported.Contains('echo %1 >'))
    Shot $out '03_imported_command'
    Close-Settings|Out-Null
    Post $pane0 $WM_CONTEXTMENU 0 -1;$menu=Open-Menu
    Check 'imported action is exposed for its file type' ($menu -and $menu.Items -contains 'OpenPilot import proof')
    if($menu){Check 'the imported Explorer command runs from OpenPilot menu' ((Pick-Menu $menu 'OpenPilot import proof') -and (Wait-Until {Test-Path "$folder\imported.txt"} 4000))}
    Check 'the imported action receives its exact selected path' ((Get-Content "$folder\imported.txt" -Raw) -match [regex]::Escape("proof$extension"))
    # Send to uses its existing dedicated helper, and handler drafts require menu text.
    Open-Settings|Out-Null;New-Command|Out-Null;Press 2
    Field 'filter' 'Compressed'
    $xy=Reach 'Command.Pick.Compressed (zipped) folder';Click (Slot) $xy[0] $xy[1];Post (Slot) 0x202 0 0
    Check 'Send to imports without requiring a raw command line' (Wait-Until {(Caption) -match 'Custom commands.*New'} 3000)
    Check 'Send to import saves as a shell relay' (Save) (Caption)
    Check 'Send to metadata is preserved in the saved command' ((Get-Content $settings -Raw) -match 'sendto = Compressed \(zipped\) folder')
    # Handler discovery reads registration only; an exact menu item is required.
    $handlerKey="HKCU:\Software\Classes\$prog\shellex\ContextMenuHandlers\OpenPilotHandlerProof"
    New-Item -Path $handlerKey -Force|Out-Null;Set-Item -Path $handlerKey -Value '{00000000-0000-0000-0000-000000000000}'
    Press 1;New-Command|Out-Null;Press 2;Field 'filter' 'OpenPilotHandlerProof'
    $xy=Reach 'Command.Pick.OpenPilotHandlerProof';Click (Slot) $xy[0] $xy[1];Post (Slot) 0x202 0 0
    Field 'name' 'pinned_handler'
    $null=Send-SettingsKey $script:settingsTarget 0x53 @('Ctrl') 2000
    Check 'an extension import requires exact menu-item text before saving' (Wait-Until {(Caption) -match 'exact Explorer menu text'} 3000) (Caption)
    Field 'action' 'Proof submenu > Inspect'
    Check 'a selected handler can be saved without loading the extension' (Save) (Caption)
    $pinned=Get-Content $settings -Raw
    Check 'imported handlers default to no full-menu fallback' ($pinned -match '(?s)\[relay.pinned_handler\].*handler = \{00000000-0000-0000-0000-000000000000\}.*fallback = off')
    Press 16;Set-Clipboard 'waiting';Press 17
    Check 'handler preview clearly requests only the selected extension' (Wait-Until {(Get-Clipboard -Raw -ErrorAction SilentlyContinue) -match 'Only the selected extension'} 3000)
    Press 1
    # Optional IDs are generated from titles and do not replace existing commands.
    New-Command|Out-Null;Field 'title' 'Automatic command ID';Field 'program' 'program_missing_for_preview.exe';Field 'arguments' '%D'
    Press 16
    # A preview still needs an identity for full draft validation; saving generates it.
    Check 'saving generates a usable ID from its title' (Save) (Caption)
    Check 'the generated ID is stable in settings' ((Get-Content $settings -Raw) -match '\[relay.automatic_command_id\]')
    Press 16;Check 'a missing program is reported without blocking the UI' (Wait-Until {(Caption) -match 'Program not found'} 5000) (Caption)
    Press 1
    Check 'test explorer closes cleanly' (Stop-Demo $p);$p=$null
    $profile=Import-Csv "$out\relay-profile.csv"
    Check 'setup has no function lifetime violations' (($profile|Where-Object name -eq 'fn_violations').value -eq '0')
    $ui=$profile|Where-Object {$_.kind -eq 'zone' -and $_.name -eq 'ui.dispatch'}
    Check 'command setup keeps UI dispatch under 16 ms' ([double]$ui.max_ms -lt 16) $ui.max_ms
}finally{if($p -and -not $p.HasExited){Stop-Demo $p|Out-Null};
    if($prog){Remove-Item "HKCU:\Software\Classes\$prog" -Recurse -Force -ErrorAction SilentlyContinue}
    if($extension){Remove-Item "HKCU:\Software\Classes\$extension" -Recurse -Force -ErrorAction SilentlyContinue}
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
"`n$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
