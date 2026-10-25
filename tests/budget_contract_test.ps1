$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\budget_contract.ps1"
$passed = 0
function Accept([string]$name, [scriptblock]$body) { & $body; $script:passed++; "ok $name" }
function Reject([string]$name, [scriptblock]$body) {
    $failed = $false
    try { & $body | Out-Null } catch { $failed = $true }
    if (-not $failed) { throw "Oracle accepted $name" }
    $script:passed++; "ok rejects $name"
}
Accept 'matching startup identity' { if ((Assert-StartupSample @('42,123456,12.5') 42 123456) -ne 12.5) { throw 'Wrong sample' } }
Reject 'absent startup line' { Assert-StartupSample @() 42 123456 }
Reject 'duplicate startup lines' { Assert-StartupSample @('42,123456,12.5','42,123456,12.5') 42 123456 }
Reject 'wrong startup PID' { Assert-StartupSample @('41,123456,12.5') 42 123456 }
Reject 'wrong startup generation' { Assert-StartupSample @('42,123455,12.5') 42 123456 }
Reject 'invalid startup duration' { Assert-StartupSample @('42,123456,NaN') 42 123456 }
Accept 'owned ready window' { Assert-OwnedReadyWindow 42 1001 42 $true $true }
Reject 'wrong ready-window owner' { Assert-OwnedReadyWindow 42 1001 41 $true $true }
Reject 'hidden ready window' { Assert-OwnedReadyWindow 42 1001 42 $false $true }
Reject 'missing ready window' { Assert-OwnedReadyWindow 42 0 42 $true $true }
$rows = @(
    [pscustomobject]@{kind='metric';name='slowest_message_ms';value='8.5';calls='';max_ms=''},
    [pscustomobject]@{kind='metric';name='startup_ms';value='20';calls='';max_ms=''},
    [pscustomobject]@{kind='metric';name='gdi_objects';value='20';calls='';max_ms=''},
    [pscustomobject]@{kind='metric';name='slowest_message';value='WM_PAINT';calls='';max_ms=''},
    [pscustomobject]@{kind='zone';name='ui.dispatch';value='';calls='100';max_ms='8.5'}
)
$profile = Assert-WorkloadProfile $rows 16
Accept 'complete workload profile' { if (-not $profile.MeetsUiContract) { throw 'Expected pass' } }
Reject 'absent workload metric' { Assert-WorkloadProfile @($rows | Where-Object name -ne 'slowest_message_ms') 16 }
Reject 'missing dispatch profile' { Assert-WorkloadProfile @($rows | Where-Object name -ne 'ui.dispatch') 16 }
Reject 'duplicate workload metric' { Assert-WorkloadProfile @($rows + $rows[0]) 16 }
$slow = Assert-WorkloadProfile @($rows | ForEach-Object {
    if ($_.name -eq 'slowest_message_ms') { [pscustomobject]@{kind='metric';name='slowest_message_ms';value='25';calls='';max_ms=''} }
    else { $_ }
}) 16
$good = [pscustomobject]@{Run=1;Profile=$profile}
$bad = [pscustomobject]@{Run=2;Profile=$slow}
Accept 'slow run survives best-run selection' {
    $batch = Assert-WorkloadBatch @($good,$bad) 2 16
    if ($batch.AllMeetContract -or $batch.BestMs -ne 8.5 -or $batch.WorstMs -ne 25) { throw 'Slow run was hidden' }
}
Reject 'missing workload run' { Assert-WorkloadBatch @($good) 2 16 }
Reject 'misordered workload run' { Assert-WorkloadBatch @($bad,$good) 2 16 }
# Audit / budget reporting: budget_contract.ps1 run attribution and budget.ps1 report evidence.
Accept 'worst duration retains its message and raw evidence' {
    $treeProfile = $slow.PSObject.Copy()
    $treeProfile.SlowestMessage = 'tree WM_PAINT'
    $fast = [pscustomobject]@{Run=1;Profile=$profile;Rows=@('fast pane evidence')}
    $slowRun = [pscustomobject]@{Run=2;Profile=$treeProfile;Rows=@('slow tree evidence')}
    $batch = Assert-WorkloadBatch @($fast,$slowRun) 2 16
    if ($batch.BestRun.Run -ne 1 -or $batch.WorstRun.Run -ne 2 -or
        $batch.WorstRun.Profile.SlowestMs -ne $batch.WorstMs -or
        $batch.WorstRun.Profile.SlowestMessage -ne 'tree WM_PAINT' -or
        $batch.WorstRun.Rows[0] -ne 'slow tree evidence') { throw 'Worst-run attribution was lost' }
}
Accept 'worst run can precede the best run' {
    $first = [pscustomobject]@{Run=1;Profile=$slow;Rows=@('slow first')}
    $second = [pscustomobject]@{Run=2;Profile=$profile;Rows=@('fast second')}
    $batch = Assert-WorkloadBatch @($first,$second) 2 16
    if ($batch.BestRun.Run -ne 2 -or $batch.WorstRun.Run -ne 1 -or
        $batch.WorstRun.Rows[0] -ne 'slow first') { throw 'Run order changed attribution' }
}
Accept 'equal maxima select the first complete run' {
    $first = [pscustomobject]@{Run=1;Profile=$profile;Rows=@('first evidence')}
    $second = [pscustomobject]@{Run=2;Profile=$profile;Rows=@('second evidence')}
    $batch = Assert-WorkloadBatch @($first,$second) 2 16
    if ($batch.BestRun.Run -ne 1 -or $batch.WorstRun.Run -ne 1 -or
        $batch.WorstRun.Rows[0] -ne 'first evidence') { throw 'Tie selection was inconsistent' }
}
Accept 'single workload supplies both report roles' {
    $only = [pscustomobject]@{Run=1;Profile=$slow;Rows=@('only evidence')}
    $batch = Assert-WorkloadBatch @($only) 1 16
    if ($batch.BestRun.Run -ne 1 -or $batch.WorstRun.Run -ne 1 -or
        $batch.BestMs -ne $batch.WorstMs -or $batch.AllMeetContract -or
        $batch.WorstRun.Rows[0] -ne 'only evidence') { throw 'Single-run report was inconsistent' }
}
Accept 'exact engine benchmark line' { if ((Get-UniqueBenchMs @('sort name (natural) 1.25 ms') '^sort name \(natural\)\s+([\d.]+) ms' 'sort') -ne 1.25) { throw 'Wrong benchmark' } }
Reject 'missing engine benchmark' { Get-UniqueBenchMs @('other 1.25 ms') '^sort name \(natural\)\s+([\d.]+) ms' 'sort' }
"$passed checks passed"
