<# Linux permissions acceptance: native controls and private Linux fixtures. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class PermissionProbe {
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr h,string name);
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\permissions'
New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0;$dialog=[IntPtr]::Zero
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command,[switch]$AsRoot){
    $value=if($AsRoot){& wsl.exe --user root --exec sh -c $command}else{& wsl.exe --exec sh -c $command}
    if($LASTEXITCODE){throw "Private WSL fixture failed: $command"};($value -join "`n").TrimEnd()
}
function Dialog { [FpUi]::FindWindowW('OpenPilotPermissions','Linux permissions') }
function Field([int]$id){[FpUi]::FindControl($script:dialog,$id)}
function Enabled([int]$id){[FpUi]::ControlEnabled((Field $id))}
function Press([int]$id){Post $script:dialog 0x111 $id 0;Start-Sleep -Milliseconds 100}
function Done {Wait-Until {[PermissionProbe]::GetPropW($script:dialog,'Permissions.Busy').ToInt64() -eq 0} 7000}
function Loaded {Wait-Until {[PermissionProbe]::GetPropW($script:dialog,'Permissions.Loaded').ToInt64() -eq 1 -and [PermissionProbe]::GetPropW($script:dialog,'Permissions.Busy').ToInt64() -eq 0} 7000}
function Octal([string]$value){Chord (Field 101) 'Ctrl' 0x41 20;Send-Text (Field 101) $value}
function Mode([string]$path){Native "stat -c %a '$path'"}
function Navigate([string]$path){
    Chord $pane0 'Ctrl' 0x4C 20;Send-Text (Pane-Edits $pane0)[0] $path;Key (Pane-Edits $pane0)[0] 0x0D 20
    Wait-Until {(Window-Text (Pane-Edits $pane0)[0]) -ceq $path -and [PermissionProbe]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000
}
function Select-First {Click $pane0 ([int](65*$scale)) ([int](74*$scale));Post $pane0 0x202 0 0}
function Open-Editor {
    Select-First;Chord $pane0 @('Ctrl','Alt') 0x50 20
    if(-not(Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)){throw 'Permissions dialog did not open'}
}
function Close-Editor {Press 2;Wait-Until {(Dialog) -eq [IntPtr]::Zero} 3000}
$linux="/tmp/openpilot-permissions-$PID";$scratch=Join-Path $env:TEMP "openpilot_permissions_$PID";$p=$null
try {
    $uid=Native 'id -u';$gid=Native 'id -g';$distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux/file' '$linux/folder/tree' '$linux/denied' '$linux/link' '$linux/multi'; printf content > '$linux/file/run β'; chmod 640 '$linux/file/run β'; printf keep > '$linux/folder/tree/nested'; chmod 640 '$linux/folder/tree/nested'; chmod 750 '$linux/folder/tree'; ln -s '../file/run β' '$linux/link/link'; touch '$linux/multi/a' '$linux/multi/b'"|Out-Null
    Native "printf keep > '$linux/denied/root-owned'; chown root:root '$linux/denied/root-owned'; chmod 644 '$linux/denied/root-owned'" -AsRoot|Out-Null
    New-Item -ItemType Directory -Force $scratch|Out-Null;Set-Content -LiteralPath "$scratch\local.txt" -Value 'local'
    $settings="$scratch\settings.ini"
    $baseSettings="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n[keys]`nfile.permissions = Ctrl+Alt+P`n"
    Set-Content -LiteralPath $settings -Value $baseSettings
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\permissions-profile.csv","$unc\file","$unc\file")
    Check 'Linux fixture finishes loading' (Wait-Until {(Title) -eq 'file - Folderwing' -and [PermissionProbe]::GetPropW($pane0,'Address.Loading').ToInt64() -eq 0} 7000)
    Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1
    $menu=Open-Menu
    Check 'single Linux item offers Permissions in its context menu' ($menu -and $menu.Items -contains 'Permissions...')
    if(-not $menu -or -not(Pick-Menu $menu 'Permissions...')){throw 'Permissions context action unavailable'}
    Check 'native editor reads fresh metadata asynchronously' (Wait-Until {($script:dialog=Dialog) -ne [IntPtr]::Zero} 3000)
    Check 'initial read succeeds' (Loaded)
    Check 'owner and group names and numeric ids are visible' ((Window-Text (Field 102)) -match "uid $uid" -and (Window-Text (Field 103)) -match "gid $gid")
    Check 'initial octal and checkboxes agree' ((Window-Text (Field 101)) -eq '0640' -and [FpUi]::SendMessageW((Field 120),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 1 -and [FpUi]::SendMessageW((Field 122),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 0)
    Check 'Apply is disabled until the mode changes' (-not(Enabled 106))
    Octal '751'
    Check 'octal editing updates checkboxes and a before/after preview' ((Window-Text (Field 104)) -match 'Current: 0640' -and (Window-Text (Field 104)) -match 'After Apply: 0751' -and [FpUi]::SendMessageW((Field 122),0xF0,[IntPtr]::Zero,[IntPtr]::Zero).ToInt64() -eq 1) (Window-Text (Field 101))
    $savedMain=$script:main;try{$script:main=$script:dialog;Shot $out '01_editor'}finally{$script:main=$savedMain}
    Check 'editing does not modify the file' ((Mode "$linux/file/run β") -eq '640')
    Octal '789'
    Check 'non-octal input disables Apply' (-not(Enabled 106) -and (Window-Text (Field 104)) -match 'octal digits')
    Octal '10000'
    Check 'a pasted fifth digit is rejected rather than truncated into a different mode' (-not(Enabled 106))
    Octal '0751';Press 106
    Check 'Apply returns verified success' ((Done) -and (Window-Text (Field 105)) -match 'applied and verified') (Window-Text (Field 105))
    Check 'Apply changed the real Linux mode and retained content' ((Mode "$linux/file/run β") -eq '751' -and (Native "cat '$linux/file/run β'") -eq 'content')
    Check 'the verified mode becomes the new current mode' ((Window-Text (Field 104)) -match 'Current: 0751' -and -not(Enabled 106))
    # Actual checkbox click (the helper's synthetic click needs its matching release).
    Click (Field 131) 12 12;Post (Field 131) 0x202 0 0
    Check 'special-bit checkbox updates octal without changing the file' ((Window-Text (Field 101)) -eq '1751' -and (Mode "$linux/file/run β") -eq '751')
    Check 'the pane can navigate independently while the editor is open' (Navigate $scratch)
    Check 'Close discards unapplied changes' ((Close-Editor) -and (Mode "$linux/file/run β") -eq '751')
    Check 'returning to the file fixture works' (Navigate "$unc\file")
    Open-Editor;Check 'reopening reads the actual current mode' ((Loaded) -and (Window-Text (Field 101)) -eq '0751')
    Octal '0777';Native "chmod 600 '$linux/file/run β'"|Out-Null;Press 106
    Check 'an external mode change is rejected before Apply' ((Done) -and (Window-Text (Field 105)) -match 'changed or was replaced' -and -not(Enabled 106) -and (Mode "$linux/file/run β") -eq '600')
    Press 107;Check 'Reload adopts fresh permissions after a stale edit' ((Loaded) -and (Window-Text (Field 101)) -eq '0600')
    Close-Editor|Out-Null
    Check 'folder fixture opens' (Navigate "$unc\folder");Open-Editor;Check 'folder permissions load' (Loaded)
    Octal '1700';Press 106
    Check 'folder Apply changes only the folder' ((Done) -and (Mode "$linux/folder/tree") -eq '1700' -and (Mode "$linux/folder/tree/nested") -eq '640')
    Close-Editor|Out-Null
    Check 'denied fixture opens' (Navigate "$unc\denied");Open-Editor;Check 'root-owned item is readable in the editor' (Loaded)
    Check 'the editor states the captured default WSL user choice' ((Window-Text (Field 108)) -eq 'WSL user: distro default')
    Check 'settings watching is ready for a user-role change' (Wait-Until {[PermissionProbe]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Set-Content -LiteralPath $settings -Value ($baseSettings+"[wsl.helper]`nuser = root`n");Start-Sleep -Milliseconds 500
    Octal '0777';Press 106
    Check 'permission denied is explicit and preserves the original mode' ((Done) -and (Window-Text (Field 105)) -match 'Permission denied' -and (Mode "$linux/denied/root-owned") -eq '644') (Window-Text (Field 105))
    Check 'an open editor keeps its original user choice after settings change' ((Window-Text (Field 108)) -eq 'WSL user: distro default')
    Close-Editor|Out-Null;Open-Editor
    Check 'a newly opened editor captures the explicitly configured root user' ((Loaded) -and (Window-Text (Field 108)) -eq 'WSL user: root')
    Octal '0600';Press 106
    Check 'the explicitly chosen root user can apply the protected fixture mode' ((Done) -and (Window-Text (Field 105)) -match 'applied and verified' -and (Native "stat -c %a '$linux/denied/root-owned'" -AsRoot) -eq '600')
    Close-Editor|Out-Null
    Set-Content -LiteralPath $settings -Value $baseSettings;Start-Sleep -Milliseconds 500
    Check 'symlink fixture opens'  (Navigate "$unc\link");Open-Editor
    Check 'symlink edits are refused without changing the target' ((Done) -and (Window-Text (Field 105)) -match 'Symlinks and special files are excluded' -and -not(Enabled 106) -and (Mode "$linux/file/run β") -eq '600')
    Close-Editor|Out-Null
    Check 'multiple selection fixture opens' (Navigate "$unc\multi");Select-First;Chord $pane0 'Ctrl' 0x41 20
    Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'Permissions is offered for multiple selected Linux items' ($menu -and $menu.Items -contains 'Permissions...');if($menu){Key $menu.Window 0x1B 20}
    Check 'Windows fixture opens' (Navigate $scratch);Select-First;Post $pane0 $WM_CONTEXTMENU $pane0.ToInt64() -1;$menu=Open-Menu
    Check 'Windows items keep their existing Properties action and do not offer Linux permissions' ($menu -and $menu.Items -notcontains 'Permissions...' -and $menu.Items -contains 'Properties');if($menu){Key $menu.Window 0x1B 20}
    Check 'a pending-query shutdown fixture opens' (Navigate "$unc\file")
    Select-First;Chord $pane0 @('Ctrl','Alt') 0x50 0
    Check 'closing the app releases the permissions function cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\permissions-profile.csv" -Raw
    Check 'permissions work has no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'permission editing keeps UI dispatch below 16 ms' ($match.Success -and [double]$match.Groups[1].Value -lt 16)
}finally{
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'" -AsRoot|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
