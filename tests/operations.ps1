<#
.SYNOPSIS
  Operations acceptance: real transfers, conflicts, errors, cancellation,
  clearing, and results surviving a closed or crashed panel. Scratch only.
  Run with PowerShell 7 after build.ps1 -Test. Screenshots: build/operations.
#>
param([string] $Config = 'release')
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
. "$PSScriptRoot\operations_contract.ps1"
Add-Type -AssemblyName System.Windows.Forms
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\$Config\folderwing.exe"
$out = Join-Path $root 'build\operations'
New-Item -ItemType Directory -Force $out | Out-Null
$failed = 0
function Check([string] $name, [bool] $ok) {
    if ($ok) { Write-Host "  ok    $name" } else { Write-Host "  FAIL  $name"; $script:failed++ }
}
function Apps { @(Get-Process folderwing-operations -ErrorAction SilentlyContinue | Where-Object { $_.Parent.Id -eq $script:demoPid }) }
function Slot {
    $side = [FpUi]::FindWindowExW($main, [IntPtr]::Zero, 'OpenPilotSide', [IntPtr]::Zero)
    [FpUi]::FindWindowExW($side, [IntPtr]::Zero, 'OpenPilotSideSlot', [IntPtr]::Zero)
}
function Result {
    $slot=Slot
    if($slot -eq [IntPtr]::Zero){return ''}
    $sentinel = "openpilot-result-await-$([guid]::NewGuid().ToString('N'))"
    $reset = $false
    for ($attempt=0; $attempt -lt 5 -and -not $reset; $attempt++) {
        Set-Clipboard $sentinel
        $reset = Wait-Until {
            try { (Get-Clipboard -Raw) -ceq $sentinel } catch { $false }
        } 300
    }
    if (-not $reset) {
        $observed = try { Get-Clipboard -Raw } catch { $null }
        throw "Could not reset clipboard before Copy result (observed length=$($observed.Length), sentinel length=$($sentinel.Length))"
    }
    Chord-Acknowledged $slot 'Ctrl' 0x43 1000
    Start-Sleep -Milliseconds 100
    if (-not (Wait-Until { Test-FreshClipboard (Get-Clipboard -Raw) $sentinel } 1000)) { return '' }
    Get-Clipboard -Raw
}
function Prepare([int] $policy = 0) {
    Set-Clipboard 'waiting for a new operation'
    Key $pane0 0x74 0 # F5
    $ready=Wait-Until { (Apps).Count -eq 1 -and (Slot) -ne [IntPtr]::Zero }
    Check 'Operations starts only when requested' $ready
    if(-not $ready){throw "Operations input target did not open; host=$script:demoPid; pane=$pane0; $(Address-State $pane0 (Pane-Edits $pane0)[0])"}
    Check 'transfer waits for Start' (Wait-Until {
        $snapshot = Result
        $identity = [regex]::Match($snapshot, 'Operation #(\d+)')
        if ($snapshot -match 'Ready to start' -and $identity.Success -and [uint64]$identity.Groups[1].Value -gt $script:lastToken) {
            $script:currentToken = [uint64]$identity.Groups[1].Value; return $true
        }
        $false
    })
    $script:lastToken = $script:currentToken
    for ($i=0; $i -lt $policy; $i++) { Key (Slot) 0x27 20 }
}
function Finish {
    Set-Clipboard 'waiting for the operation to finish'
    Key (Slot) 0x0D 0
    Check 'job reaches a terminal state' (Wait-Until {
        $snapshot = Result
        if ($snapshot.Contains("Operation #$script:currentToken`r`n") -and $snapshot -match 'Completed|Finished with|Cancelled|Could not start') {
            $script:finishedSnapshot = $snapshot; return $true
        }
        $false
    } 10000)
    $script:finishedSnapshot
}
$scratch = Join-Path $env:TEMP "openpilot_operations_$PID"
$left = Join-Path $scratch 'left'; $right = Join-Path $scratch 'right'
New-Item -ItemType Directory -Force $left, $right | Out-Null
Set-Content "$left\a.txt" 'new contents'; Set-Content "$right\a.txt" 'old contents'
$script:lastToken = 0; $script:currentToken = 0
$p = Start-Demo $exe @($left, $right)
try {
    Start-Sleep -Milliseconds 500
    Check 'no Operations process at startup' ((Apps).Count -eq 0)
    Set-Clipboard "Operation #999`r`nCopy: Finished with skips`r`n1/1 items processed; 0 errors; 1 skipped"
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Check 'empty Operations panel opens an owned helper' (Wait-Until { @(Apps).Count -eq 1 -and (Slot) -ne [IntPtr]::Zero })
    Check 'Copy without a selected replay cannot reuse a stale result' ((Result) -eq '')
    Chord (Slot) @('Ctrl','Shift') 0x4F
    Check 'empty Operations helper closes cleanly' (Wait-Until { @(Apps).Count -eq 0 })
    Prepare 1
    Shot $out '0_ready'
    Check 'no numbered copy before approval' (-not (Test-Path "$right\a (2).txt"))
    $result = Finish
    Check 'Skip existing preserves destination contents' ((Get-Content -Raw "$right\a.txt") -match 'old contents')
    Check 'skipped file has its exact token, paths, counts and reason' (
        (Test-OperationResult $result $script:currentToken 'Finished with skips' "$left\a.txt" $right 1 1 0 1 'Destination already exists') -and
        (Get-Content -Raw "$right\a.txt") -match 'old contents')
    Shot $out '1_skipped'
    $oldPanel = @(Apps)[0]; $oldPanelId = $oldPanel.Id
    Chord (Slot) @('Ctrl','Shift') 0x4F
    Check 'closing the panel releases its process' (Wait-Until { (Apps).Count -eq 0 -and $oldPanel.HasExited })
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Check 'reopened panel is a new owned helper' (Wait-Until { @(Apps).Count -eq 1 -and @(Apps)[0].Id -ne $oldPanelId -and (Slot) -ne [IntPtr]::Zero })
    Check 'closing and reopening replays the exact result' (Wait-Until {
        $snapshot = Result
        Test-OperationResult $snapshot $script:currentToken 'Finished with skips' "$left\a.txt" $right 1 1 0 1 'Destination already exists'
    } 5000)
    $panel = @(Apps)[0]; $crashedPanelId = $panel.Id; Stop-Process -Id $crashedPanelId -Force
    Check 'crashed helper exits' (Wait-Until { $panel.HasExited -and @(Apps | Where-Object Id -eq $crashedPanelId).Count -eq 0 })
    Check 'panel crash leaves explorer alive' (-not $p.HasExited)
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Check 'crash recovery owns a new helper' (Wait-Until { @(Apps).Count -eq 1 -and @(Apps)[0].Id -ne $crashedPanelId -and (Slot) -ne [IntPtr]::Zero })
    Check 'a restarted panel replays the exact result' (Wait-Until {
        $snapshot = Result
        Test-OperationResult $snapshot $script:currentToken 'Finished with skips' "$left\a.txt" $right 1 1 0 1 'Destination already exists'
    } 5000)
    Prepare 0
    $result = Finish
    Check 'Keep both creates a numbered copy' ((Test-Path "$right\a (2).txt") -and (Get-Content -Raw "$right\a (2).txt") -match 'new contents')
    Prepare 2
    $result = Finish
    Check 'Replace files overwrites only after approval' ((Get-Content -Raw "$right\a.txt") -match 'new contents' -and $result -match 'Completed')
    # Hold the destination exclusively: failure must retain the path and OS reason.
    $locked = [IO.File]::Open("$right\a.txt", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try { Prepare 2; $result = Finish } finally { $locked.Dispose() }
    $correctError = $result -match 'Finished with errors' -and $result.Contains('a.txt') -and $result -match 'code 32'
    if (-not $correctError) { Write-Host "Unexpected error result ($($result.GetType().Name)): $result" }
    Check 'failed copy reports the file and Windows error' $correctError
    [FpUi]::MoveWindow($main, 80, 80, 1000, 680, $true) | Out-Null
    Check 'resizing preserves readable operation results' (Wait-Until { (Result) -match 'Finished with errors' })
    [FpUi]::MoveWindow($main, 80, 80, 1280, 800, $true) | Out-Null
    Shot $out '2_error' 
    # Cut/paste must survive cancelling before the move starts.
    Chord (Slot) @('Ctrl','Shift') 0x4F
    Chord $pane0 'Ctrl' 0x58 # Cut
    Chord $pane1 'Ctrl' 0x56 # Paste
    Check 'cut/paste enters Operations' (Wait-Until { (Apps).Count -eq 1 })
    Start-Sleep -Milliseconds 300
    # Pending controls: Start has focus; Tab -> Cancel, Enter cancels.
    Key (Slot) 0x09 50; Key (Slot) 0x0D 100
    Check 'cancelling pending paste preserves source and cut clipboard' ((Test-Path "$left\a.txt") -and [Windows.Forms.Clipboard]::ContainsFileDropList())
    Check 'pending cancellation has a retained result' (Wait-Until { (Result) -match 'Cancelled' })
    # Clearing removes completed rows while preserving a new pending request.
    Prepare 0
    Click (Slot) ([int](65*$scale)) ([int](48*$scale))
    Check 'Clear completed keeps pending work' (Wait-Until { (Result) -match 'Ready to start' })
    Key (Slot) 0x0D 50 # focus is on Clear, so select Start explicitly with its click
    # One row: list ends at 128, heading 32, choices 75 => Start at 247.
    Click (Slot) ([int](65*$scale)) ([int](260*$scale))
    Check 'pending work still starts after clearing' (Wait-Until { (Result) -match 'Completed' } 10000)
} finally {
    Stop-Demo $p | Out-Null
    Remove-Item -Recurse -Force $scratch
}
# An active directory copy cancelled through the actual panel control.
$scratch = Join-Path $env:TEMP "openpilot_cancel_$PID"
$left = Join-Path $scratch 'left'; $right = Join-Path $scratch 'right'
New-Item -ItemType Directory -Force "$left\many", $right | Out-Null
$data = New-Object byte[] 4096
foreach ($i in 1..3000) { [IO.File]::WriteAllBytes("$left\many\$i.bin", $data) }
$script:lastToken = 0; $script:currentToken = 0
$p = Start-Demo $exe @($left, $right)
try {
    Start-Sleep -Milliseconds 400
    Prepare 0
    Key (Slot) 0x0D 0
    Check 'large transfer is running' (Wait-Until { (Result) -match 'Running|Counting items' })
    Key (Slot) 0x0D 0 # Start/Cancel control retains keyboard focus
    Check 'Cancel stops the active job' (Wait-Until { (Result) -match 'Cancelled' } 10000)
    $copied = @(Get-ChildItem "$right\many" -ErrorAction SilentlyContinue).Count
    Check 'cancellation leaves the source intact and stops before all files copy' ((Get-ChildItem "$left\many").Count -eq 3000 -and $copied -lt 3000)
    Shot $out '3_cancelled'
} finally {
    Stop-Demo $p | Out-Null
    Remove-Item -Recurse -Force $scratch
}
# Crash the side-panel helper while a host-owned copy is visibly in progress.
# Many small files keep the operation active long enough to observe the handoff.
Add-Type @"
using System;
using System.IO;
public static class ActiveCopyFixture {
    public static void Build(string path, int count) {
        Directory.CreateDirectory(path);
        for (int i=1; i<=count; i++) {
            var bytes=new byte[4096]; BitConverter.GetBytes(i).CopyTo(bytes,0);
            File.WriteAllBytes(Path.Combine(path,i.ToString()+".bin"),bytes);
        }
    }
    public static bool Verify(string path, int count) {
        for (int i=1; i<=count; i++) {
            var name=Path.Combine(path,i.ToString()+".bin");
            if (!File.Exists(name)) return false;
            var bytes=File.ReadAllBytes(name);
            if (bytes.Length!=4096 || BitConverter.ToInt32(bytes,0)!=i) return false;
            for(int j=4;j<bytes.Length;j++) if(bytes[j]!=0) return false;
        }
        return true;
    }
}
"@
$scratch = Join-Path $env:TEMP "openpilot_active_recovery_$PID"
$left = Join-Path $scratch 'left'; $right = Join-Path $scratch 'right'
$fileCount = 8000
New-Item -ItemType Directory -Force $left, $right | Out-Null
[ActiveCopyFixture]::Build((Join-Path $left 'many'), $fileCount)
$script:lastToken = 0; $script:currentToken = 0
$p = Start-Demo $exe @($left, $right)
try {
    Prepare 0
    $activeToken = $script:currentToken
    Key (Slot) 0x0D 0
    $destination = Join-Path $right 'many'
    Check 'copy is active before the helper crash' (Wait-Until {
        $snapshot = Result
        $copied = @(Get-ChildItem $destination -File -ErrorAction SilentlyContinue).Count
        $snapshot.Contains("Operation #$activeToken`r`n") -and $snapshot -match 'Copy: Running' -and
            $copied -gt 0 -and $copied -lt $fileCount
    } 10000)
    $copiedAtCrash = @(Get-ChildItem $destination -File -ErrorAction SilentlyContinue).Count
    $activeHelper = @(Apps)[0]; $activeHelperId = $activeHelper.Id
    Stop-Process -Id $activeHelperId -Force
    Check 'active helper exits while explorer retains the job' (Wait-Until { $activeHelper.HasExited -and -not $p.HasExited })
    Check 'host copy progresses after the helper crash' (Wait-Until {
        @(Get-ChildItem $destination -File -ErrorAction SilentlyContinue).Count -gt $copiedAtCrash
    } 10000)
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Chord $pane0 @('Ctrl','Shift') 0x4F
    Check 'active-copy recovery starts a new owned helper' (Wait-Until {
        @(Apps).Count -eq 1 -and @(Apps)[0].Id -ne $activeHelperId -and (Slot) -ne [IntPtr]::Zero
    } 5000)
    $completed = ''
    $script:lastActiveSnapshot = ''; $script:completedActiveSnapshot = ''
    Check 'recovered helper reports same-token completed copy' (Wait-Until {
        $snapshot = Result
        $script:lastActiveSnapshot = $snapshot
        $counts = [regex]::Match($snapshot, '(?m)^(\d+)/(\d+) items processed; 0 errors; 0 skipped\r?$')
        if (-not $counts.Success) { return $false }
        $done = [int]$counts.Groups[1].Value; $total = [int]$counts.Groups[2].Value
        if ($done -ne $total -or $total -lt $fileCount) { return $false }
        if (-not (Test-OperationResult $snapshot $activeToken 'Completed' (Join-Path $left 'many') $right $done $total 0 0)) { return $false }
        $script:completedActiveSnapshot = $snapshot
        $true
    } 15000)
    if(-not $script:completedActiveSnapshot){Write-Host "Active recovery result: $script:lastActiveSnapshot";Write-Host "Active recovery files: $(@(Get-ChildItem $destination -File -ErrorAction SilentlyContinue).Count)/$fileCount"}
    Check 'destination has every expected byte after helper crash' ([ActiveCopyFixture]::Verify($destination, $fileCount))
    $finalHelper = @(Apps)[0]
    Chord (Slot) @('Ctrl','Shift') 0x4F
    Check 'recovered helper cleans up after closing the panel' (Wait-Until { @(Apps).Count -eq 0 -and $finalHelper.HasExited })
} finally {
    Stop-Demo $p | Out-Null
    Remove-Item -Recurse -Force $scratch
}
# Disabling the panel retains the original F5 confirmation and direct copy.
$scratch = Join-Path $env:TEMP "openpilot_direct_$PID"
$left = Join-Path $scratch 'left'; $right = Join-Path $scratch 'right'
New-Item -ItemType Directory -Force $left, $right | Out-Null
Set-Content "$left\note.txt" 'direct'
Set-Content "$scratch\settings.ini" "[functions]`noperations.show = off"
$p = Start-Demo $exe @("--settings=$scratch\settings.ini", $left, $right)
try {
    Start-Sleep -Milliseconds 400
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $inputTask=Start-ModalKey $pane0 0x74 8000
    try {Check 'disabled Operations preserves the F5 confirmation' (Confirm-Dialog 'Copy')}
    finally {Complete-ModalKey $inputTask (Input-Remaining $clock 8000)}
    Check 'disabled Operations copies directly' (Wait-Until { Test-Path "$right\note.txt" })
    Check 'disabled Operations never starts its panel' ((Apps).Count -eq 0)
} finally {
    Stop-Demo $p | Out-Null
    Remove-Item -Recurse -Force $scratch
}
"screenshots: $out"
exit $failed
