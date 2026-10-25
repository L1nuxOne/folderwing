<# Native WSL acceptance: UI transfers, mutations and Bash/pane following. Scratch only. #>
param([string] $Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class WslRenameProbe {
 [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint m,UIntPtr w,IntPtr l,uint flags,uint timeout,out UIntPtr result);
 public static void Input(IntPtr h,uint m,uint w,IntPtr l,uint timeout) {
  UIntPtr result;
  if(SendMessageTimeoutW(h,m,(UIntPtr)w,l,3,timeout,out result)==IntPtr.Zero)
   throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Rename input was not acknowledged");
 }
 public static uint Selection(IntPtr h) {
  UIntPtr result;
  if(SendMessageTimeoutW(h,0xB0,UIntPtr.Zero,IntPtr.Zero,3,3000,out result)==IntPtr.Zero)
   throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Rename selection was not available");
  return unchecked((uint)result.ToUInt64());
 }
}
"@
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root "build\$Config\folderwing.exe"
$out=Join-Path $root 'build\wsl'
New-Item -ItemType Directory -Force $out | Out-Null
$script:failed=0; $script:passed=0
function Check([string] $name,[bool] $ok,[string] $detail='') {
    if($ok){Write-Host "  ok    $name"; $script:passed++}else{Write-Host "  FAIL  $name $detail"; $script:failed++}
}
function Native([string] $command) { $value=& wsl.exe --exec sh -c $command; if($LASTEXITCODE){throw "Linux fixture command failed: $command"}; ($value -join "`n") }
function Native-ExactFile([string]$root,[string]$nameHex,[string]$marker,[string]$otherHex) {
    $rootHex=[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($root))
    $code="import os; b=bytes.fromhex('$rootHex'); a=bytes.fromhex('$nameHex'); z=bytes.fromhex('$otherHex'); m=os.fsencode('$marker'); print(int(os.path.isfile(b+b'/'+a+b'/'+m) and not os.path.exists(b+b'/'+z+b'/'+m)))"
    $codeHex=[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($code))
    (Native "python3 -c 'import sys;exec(bytes.fromhex(sys.argv[1]))' $codeHex") -eq '1'
}
function Slot {
    if($main -eq [IntPtr]::Zero){return [IntPtr]::Zero}
    $side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
    if($side -eq [IntPtr]::Zero){return [IntPtr]::Zero}
    [FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)
}
function Apps([string] $name) { @(Get-Process $name -ErrorAction SilentlyContinue | Where-Object { $_.Parent.Id -eq $script:demoPid }) }
function Terminal-Window {
    $h=[IntPtr]::Zero
    $children=@(Apps 'folderwing-terminal'|ForEach-Object Id)
    if($children.Count -ne 1){return [IntPtr]::Zero}
    while(($h=[FpUi]::FindWindowExW([IntPtr]::Zero,$h,'FpTermHidden',[IntPtr]::Zero)) -ne [IntPtr]::Zero){
        $owner=[uint32]0;[FpUi]::GetWindowThreadProcessId($h,[ref]$owner)|Out-Null
        if($owner -in $children){return $h}
    };[IntPtr]::Zero
}
function Terminal-Snapshot {
    $h=Terminal-Window
    if($h -eq [IntPtr]::Zero){return $null}
    $revision=[FpUi]::GetPropW($h,'Terminal.Revision').ToInt64()
    if($revision -le 0 -or ($revision -band 1)){return $null}
    $text=[Text.StringBuilder]::new(512);[FpUi]::GetWindowTextW($h,$text,512)|Out-Null
    $value=[pscustomobject]@{Window=$h;Slot=(Slot);Revision=$revision;Cwd=$text.ToString();
        Session=[FpUi]::GetPropW($h,'Terminal.Session').ToInt64();Kind=[FpUi]::GetPropW($h,'Terminal.Kind').ToInt64();
        Ready=[FpUi]::GetPropW($h,'Terminal.Ready').ToInt64();Prompt=[FpUi]::GetPropW($h,'Terminal.AtPrompt').ToInt64();
        Chars=[FpUi]::GetPropW($h,'Terminal.Chars').ToInt64();Enters=[FpUi]::GetPropW($h,'Terminal.Enters').ToInt64();
        Error=[FpUi]::GetPropW($h,'Terminal.InputError').ToInt64()}
    if([FpUi]::GetPropW($h,'Terminal.Revision').ToInt64() -ne $revision){return $null};$value
}
function Terminal-Prompt([string]$cwd) {
    $s=Terminal-Snapshot
    $ready=$null -ne $s -and $s.Ready -eq 1 -and $s.Prompt -eq 1 -and $s.Session -gt 0 -and $s.Kind -eq 3 -and $s.Error -eq 0 -and
    $s.Slot -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($s.Slot) -and $s.Cwd -ceq $cwd
    if($ready){$script:terminalReadySnapshot=$s};$ready
}
function Terminal-State {
    $s=Terminal-Snapshot
    if($null -eq $s){return "No coherent owned terminal snapshot; children=$((Apps 'folderwing-terminal'|ForEach-Object Id)-join ',')"}
    $s|ConvertTo-Json -Compress
}
function Terminal-SameSession($s,$before) {
    $null -ne $s -and $s.Window -eq $before.Window -and $s.Slot -eq $before.Slot -and
    $s.Session -eq $before.Session -and $s.Kind -eq 3 -and $s.Ready -eq 1 -and $s.Error -eq 0 -and [FpUi]::IsWindowVisible($s.Slot)
}
function Terminal-Command([string]$command,[string]$cwd,[scriptblock]$completed) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$phase='session/cwd readiness'
    try {
        if(-not(Wait-Until {Terminal-Prompt $cwd} (Input-Remaining $clock 5000))){throw 'The expected WSL session and working folder were not ready'}
        $before=$script:terminalReadySnapshot
        $phase='character delivery'
        Send-AcknowledgedText $before.Slot $command (Input-Remaining $clock 5000)
        if(-not(Wait-Until {
            $now=Terminal-Snapshot
            if($null -eq $now){return $false} # A concurrent snapshot publication is not a session change.
            if(-not(Terminal-SameSession $now $before)){throw 'Terminal session or input target changed during typing'}
            if($now.Chars -gt $before.Chars+$command.Length -or $now.Enters -ne $before.Enters){throw 'Unexpected terminal input interleaved with the command'}
            $now.Chars -eq $before.Chars+$command.Length
        } (Input-Remaining $clock 5000))){throw 'The terminal did not acknowledge all command characters'}
        $phase='Enter submission'
        Send-AcknowledgedKey $before.Slot 13 (Input-Remaining $clock 5000)
        if(-not(Wait-Until {
            $now=Terminal-Snapshot
            if($null -eq $now){return $false}
            if(-not(Terminal-SameSession $now $before)){throw 'Terminal session changed during Enter submission'}
            if($now.Chars -ne $before.Chars+$command.Length -or $now.Enters -gt $before.Enters+1){throw 'Unexpected terminal input interleaved with Enter'}
            $now.Enters -eq $before.Enters+1
        } (Input-Remaining $clock 5000))){throw 'The terminal did not acknowledge Enter'}
        $phase='command result'
        if(-not(Wait-Until {
            $now=Terminal-Snapshot
            if($null -eq $now){return $false}
            if(-not(Terminal-SameSession $now $before)){throw 'Terminal session changed before its result'}
            & $completed
        } (Input-Remaining $clock 5000))){throw 'The command result was not verified'}
        $true
    }catch{throw "Terminal failed in $phase after$($clock.ElapsedMilliseconds)ms; expected cwd=$cwd; command=$command; state=$(Terminal-State); cause=$($_.Exception.Message)"}
}
function Result {
    $slot=Slot
    if($slot -eq [IntPtr]::Zero -or -not [FpUi]::IsWindowVisible($slot) -or (Apps 'folderwing-operations').Count -ne 1){return ''}
    Chord $slot 'Ctrl' 0x43 80
    Get-Clipboard -Raw
}
function Transfer([int] $policy) {
    # Operation tokens belong to a host, and start over in a new host process.
    if($script:operationHost -ne $script:demoPid){$script:operationHost=$script:demoPid;$script:lastToken=[uint64]0}
    Set-Clipboard 'waiting'
    Key $pane0 0x74 0
    $ready=Wait-Until {
        $snapshot=Result
        $identity=[regex]::Match([string]$snapshot,'(?m)^Operation #(\d+)\r?$')
        if($snapshot -match 'Ready to start' -and $identity.Success -and [uint64]$identity.Groups[1].Value -gt $script:lastToken){
            $script:currentToken=[uint64]$identity.Groups[1].Value;return $true
        };$false
    } 5000
    Check 'WSL transfer waits for Start' $ready
    if(-not $ready){throw 'A new owned Operations request did not become Ready within5000ms'}
    $script:lastToken=$script:currentToken
    for($i=0;$i -lt $policy;$i++){Key (Slot) 0x27 20}
    Key (Slot) 0x0D 0
    $complete=Wait-Until {
        $snapshot=Result
        if($snapshot -cmatch "(?m)^Operation #$script:currentToken`r?$" -and $snapshot -match 'Completed|Finished with|Cancelled|Could not start'){$script:snapshot=$snapshot;return $true};$false
    } 10000
    Check 'WSL transfer completes in Operations' $complete
    if(-not $complete){throw "Operation #$script:currentToken did not complete within10000ms"}
    Chord (Slot) @('Ctrl','Shift') 0x4F
    $closed=Wait-Until { $slot=Slot; (Apps 'folderwing-operations').Count -eq 0 -and ($slot -eq [IntPtr]::Zero -or -not [FpUi]::IsWindowVisible($slot)) }
    Check 'WSL transfer closes its Operations process and input target' $closed
    if(-not $closed){throw 'Operations did not close within3000ms'}
}
function Navigate([string]$path) {$null=Navigate-Verified $path $pane0 7000}
function Selected-PathIs([string]$path) {
    Chord $pane0 @('Ctrl','Shift') 0x43 0
    ([string](Get-Clipboard -Raw)).TrimEnd([char[]]"`r`n") -ceq $path
}
function Rename-New([string] $name,[string]$original='') {
    $ready=Wait-Until {
        $edits=Pane-Edits $pane0
        if($edits.Count -ne 2 -or -not [FpUi]::IsWindowVisible($edits[1])){return $false}
        $text=Window-Text $edits[1]
        if(-not $text -or ($original -and $text -cne $original)){return $false}
        $dot=$text.LastIndexOf('.');$stem=if($dot -gt 0){$dot}else{$text.Length}
        $selection=[WslRenameProbe]::Selection($edits[1])
        ($selection -band 0xffff) -eq 0 -and ($selection -shr 16) -eq $stem
    } 5000
    Check 'Linux creation enters rename mode' $ready
    if(-not $ready){throw 'Linux rename editor did not open within 5000ms'}
    $edits=Pane-Edits $pane0
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $before=Window-Text $edits[1]
    $selection=[WslRenameProbe]::Selection($edits[1])
    $expected=$before.Substring(0,($selection -band 0xffff))+$name+$before.Substring(($selection -shr 16))
    Send-AcknowledgedText $edits[1] $name (Input-Remaining $clock 3000)
    if((Window-Text $edits[1]) -cne $expected){throw 'Linux rename text differs from the acknowledged input'}
    Send-AcknowledgedKey $edits[1] 13 (Input-Remaining $clock 3000)
}
$name="openpilot_workflow_$PID"; $linux="/tmp/$name"
$distro=Native ('D=/tmp/{0}; mkdir -p $D/src/Tree $D/dst $D/sub; printf payload > $D/src/Tree/run; chmod 754 $D/src/Tree/run; ln -s missing $D/src/Tree/broken; echo $WSL_DISTRO_NAME' -f $name)
if($distro -notmatch '^[\w.-]+$'){throw 'A running WSL distro is required for this acceptance gate'}
$base="\\wsl.localhost\$distro\tmp\$name"
$scratch=Join-Path $env:TEMP $name;New-Item -ItemType Directory -Force $scratch | Out-Null
$p=$null
try {
    $p=Start-Demo $exe @("$base\src","$base\dst")
    Check 'Linux source folder opens' (Wait-Until { (Title) -eq 'src - Folderwing' })
    Check 'unopened Operations has no input target or stale result' ((Slot) -eq [IntPtr]::Zero -and (Result) -eq '')
    Start-Sleep -Milliseconds 800
    Transfer 0
    Check 'Linux copy retains executable permission and broken symlink' ((Native "stat -c %a $linux/dst/Tree/run; readlink $linux/dst/Tree/broken") -eq "754`nmissing")
    Transfer 1
    Check 'Skip existing records exact Linux conflict path' ($script:snapshot -match '2 skipped' -and $script:snapshot.Contains("$base\dst\Tree\run"))
    Transfer 2
    Check 'Replace keeps the Linux symlink intact' ((Native "readlink $linux/dst/Tree/broken") -eq 'missing')
    Key $pane0 0x76 # F7
    Rename-New 'Case'
    Check 'new folder and rename run in Linux' (Wait-Until { (Test-Path "$base\src\Case") -and (Selected-PathIs "$base\src\Case") })
    Key $pane0 0x71;Rename-New 'case' 'Case'
    Check 'Linux case-only rename succeeds' (Wait-Until {
        (Native "if test -d $linux/src/case && test ! -d $linux/src/Case; then printf yes; fi") -eq 'yes'
    })
    Chord $pane0 'Shift' 0x76 # Shift+F7
    Rename-New 'note:linux'
    Check 'new text file runs in Linux' (Wait-Until { (Test-Path "$base\src\note$([char]0xF03A)linux.txt") -and (Selected-PathIs "$base\src\note$([char]0xF03A)linux.txt") })
    Key $pane0 0x71
    Check 'Linux rename box shows the actual reserved character' (Wait-Until { (Pane-Edits $pane0).Count -eq 2 -and (Window-Text (Pane-Edits $pane0)[1]) -eq 'note:linux.txt' })
    $edit=(Pane-Edits $pane0)[1];Key $edit 0x1B
    Shot $out '1_linux_operations'
    Check 'operations app closes while results remain retained' ((Apps 'folderwing-operations').Count -eq 0)
    Stop-Demo $p | Out-Null; $p=$null
    Set-Content "$scratch\bridge.txt" 'Windows contents'
    $p=Start-Demo $exe @($scratch,"$base\dst")
    Start-Sleep -Milliseconds 700
    Transfer 0
    Check 'Windows to Linux transfer writes actual contents' ((Native "cat $linux/dst/bridge.txt") -match 'Windows contents')
    Stop-Demo $p | Out-Null; $p=$null
    $p=Start-Demo $exe @($base,$scratch)
    Check 'Linux terminal folder opens' (Wait-Until { (Title) -eq "$name - Folderwing" })
    Chord $pane0 'Ctrl' 0xC0
    $ready=Wait-Until { Terminal-Prompt $base } 5000
    Check 'auto opens the WSL panel terminal' $ready (Terminal-State)
    if(-not $ready){throw 'WSL terminal did not reach its first shell prompt within5000ms'}
    $termPid=(Apps 'folderwing-terminal')[0].Id
    Check 'Bash starts in the Linux pane folder' (Terminal-Command 'pwd > shell_here' $base { (Test-Path "$base\shell_here") -and (Get-Content "$base\shell_here") -eq $linux })
    Check 'pane follows Bash cd' (Terminal-Command 'cd sub' $base { (Title) -eq 'sub - Folderwing' })
    Key $pane0 0x08
    Check 'pane returns to Linux parent' (Wait-Until { (Title) -eq "$name - Folderwing" })
    Check 'idle Bash follows Linux pane navigation' (Terminal-Command 'pwd > pane_here' $base { (Test-Path "$base\pane_here") -and (Get-Content "$base\pane_here") -eq $linux })
    # A quote and colon must survive both argument passing and the cwd report.
    Native "mkdir -p $linux/quote:folder" | Out-Null
    $mapped="quote$([char]0xF03A)folder"
    Start-Sleep -Milliseconds 400
    Send-Text $pane0 'quote';Key $pane0 0x0D
    Check 'reserved Linux folder opens in the pane' (Wait-Until { (Title) -eq 'quote:folder - Folderwing' })
    Check 'pane-to-shell follow decodes reserved Linux name characters' (Terminal-Command 'pwd > quoted_here' "$base\$mapped" { Test-Path "$base\$mapped\quoted_here" })
    Shot $out '2_wsl_terminal'
    Stop-Demo $p | Out-Null;$p=$null
    Check 'terminal process ends with OpenPilot' (Wait-Until { -not (Get-Process -Id $termPid -ErrorAction SilentlyContinue) })
    # Byte-distinct siblings exercise both directions of terminal path transport.
    $fixture="import os;b=os.fsencode('$linux');[(os.mkdir(b+b'/'+n)) for n in (b'A',b'A ',b'raw'+bytes([255]),b'raw'+bytes([239,191,189]))]"
    $fixtureHex=[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($fixture))
    Native "python3 -c 'import sys;exec(bytes.fromhex(sys.argv[1]))' $fixtureHex" | Out-Null
    $spaced="$base\A ";$plain="$base\A"
    $raw="$base\raw$([char]0xDCFF)";$replacement="$base\raw$([char]0xFFFD)"
    $p=Start-Demo $exe @($base,$scratch)
    Chord $pane0 'Ctrl' 0xC0
    Check 'exact-path terminal starts in fixture root' (Wait-Until { Terminal-Prompt $base } 5000) (Terminal-State)
    Check 'shell-to-pane report keeps trailing folder space' (Terminal-Command "cd -- 'A '" $base { (Terminal-Prompt $spaced) -and (Title) -eq 'A  - Folderwing' })
    Check 'spaced sibling receives only its own terminal write' (Terminal-Command 'printf spaced > exact_here' $spaced {
        (Native "if test -f '$linux/A /exact_here' && test ! -e '$linux/A/exact_here'; then printf yes; fi") -eq 'yes'
    })
    Navigate $plain
    Check 'pane-to-shell follow keeps plain sibling separate' (Terminal-Command 'printf plain > plain_here' $plain {
        (Native "if test -f '$linux/A/plain_here' && test ! -e '$linux/A /plain_here'; then printf yes; fi") -eq 'yes'
    })
    Check 'raw-byte cwd reaches the exact pane' (Terminal-Command 'cd -- ../raw$''\xff''' $plain { Terminal-Prompt $raw })
    Check 'raw-byte sibling receives only its own terminal write' (Terminal-Command 'printf raw > raw_here' $raw {
        Native-ExactFile $linux '726177ff' 'raw_here' '726177efbfbd'
    })
    Navigate $replacement
    Check 'pane-to-shell follow keeps replacement sibling separate' (Terminal-Command 'printf replacement > replacement_here' $replacement {
        Native-ExactFile $linux '726177efbfbd' 'replacement_here' '726177ff'
    })
    Navigate $raw
    Check 'pane-to-shell follow keeps raw-byte sibling separate' (Terminal-Command 'printf followed > followed_here' $raw {
        Native-ExactFile $linux '726177ff' 'followed_here' '726177efbfbd'
    })
    Stop-Demo $p | Out-Null;$p=$null
    $p=Start-Demo $exe @($base,$scratch)
    Navigate $raw
    Chord $pane0 'Ctrl' 0xC0
    Check 'terminal opens directly in raw-byte folder' (Wait-Until { Terminal-Prompt $raw } 5000) (Terminal-State)
    Check 'direct raw-byte open targets only that folder' (Terminal-Command 'printf direct > open_here' $raw {
        Native-ExactFile $linux '726177ff' 'open_here' '726177efbfbd'
    })
    Stop-Demo $p | Out-Null;$p=$null
    $settings=Join-Path $scratch 'wsl.ini'
    Set-Content $settings "[shell.terminal]`nshell = wsl"
    $mounted=((& wsl.exe --exec wslpath -u $scratch)-join '').Trim()
    if($LASTEXITCODE -or -not $mounted.StartsWith('/')){throw 'Cannot resolve the owned Windows fixture mount path'}
    $terminalCwd="\\wsl.localhost\$distro$($mounted.Replace('/','\'))"
    $p=Start-Demo $exe @("--settings=$settings",$scratch,$base)
    Chord $pane0 'Ctrl' 0xC0
    $ready=Wait-Until { Terminal-Prompt $terminalCwd }
    Check 'explicit wsl starts a panel terminal from Windows' $ready (Terminal-State)
    if(-not $ready){throw 'Explicit WSL terminal did not reach its first shell prompt within3000ms'}
    Check 'explicit WSL enters the mounted Windows folder' (Terminal-Command 'printf bridge-shell > shell_from_windows' $terminalCwd { (Test-Path "$scratch\shell_from_windows") -and (Get-Content "$scratch\shell_from_windows") -eq 'bridge-shell' })
    Stop-Demo $p | Out-Null;$p=$null
} finally {
    if($p){Stop-Demo $p | Out-Null}
    Native "rm -rf $linux" | Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
Write-Host "`n$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
