<#
.SYNOPSIS
  Speed and memory guardrails. Measures startup, idle memory, the slowest
  UI-thread message under a scripted workload, engine read/sort times and
  binary sizes; compares them with the previous run and with fixed budgets.
  Each run is appended to build/budget/history.csv. Exits with the number
  of budgets exceeded. Run ./build.ps1 first; keep the machine otherwise idle.

.EXAMPLE
  ./tests/budget.ps1               # measure, compare, record
  ./tests/budget.ps1 -NoRecord     # measure and compare only
#>
param(
    [string] $Config = 'release',
    [int]    $StartupRuns = 10,
    [double] $StartupMs = 150,     # median, process creation -> first frame
    [double] $PrivateMB = 5,       # idle, two panes open, profiling off, default settings (type
                                   # icons: no system image list; view.icons style = windows adds ~2.3 MB)
    [double] $UiStallMs = 16,      # slowest single UI message (one 60 Hz frame)
    [int]    $WorkloadRuns = 2,
    [switch] $NoRecord
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\budget_contract.ps1"
if ($StartupRuns -lt 1 -or $WorkloadRuns -lt 1) { throw 'StartupRuns and WorkloadRuns must both be positive' }
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root "build\$Config"
$exe = Join-Path $bin 'folderwing.exe'
$dir = Join-Path $root 'build\budget'
New-Item -ItemType Directory -Force $dir | Out-Null
$history = Join-Path $dir 'history.csv'
$evidence = Join-Path $dir ("run-{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID)
New-Item -ItemType Directory -Force $evidence | Out-Null
$big = 'C:\Windows\WinSxS'          # ~27k entries: the stress case
$mid = 'C:\Windows\System32'        # ~5k entries
function Stop-BudgetHost($process) {
    $handle = $process.Handle
    $closed = Stop-Demo $process
    $code = [uint32]::MaxValue
    if (-not $closed -or -not [FpUi]::GetExitCodeProcess($handle, [ref]$code) -or $code -ne 0) {
        throw "Budget host did not exit cleanly (closed=$closed, code=$code)"
    }
}

$r = [ordered]@{}
$r.when = Get-Date -Format 'yyyy-MM-dd HH:mm'
$commit = (git -C $root rev-parse --short HEAD 2>$null)
if (git -C $root status --porcelain 2>$null) { $commit = "$commit+dirty" }
$r.commit = $commit

# 1. startup
'measuring startup ({0} launches)...' -f $StartupRuns
& "$PSScriptRoot\startup_bench.ps1" -Runs 3 -Config $Config -EvidenceDir (Join-Path $evidence 'warmup') | Out-Null
& "$PSScriptRoot\startup_bench.ps1" -Runs $StartupRuns -Config $Config -EvidenceDir (Join-Path $evidence 'startup') | Out-Null
$startup = Get-Content -LiteralPath (Join-Path $evidence 'startup\result.json') -Raw | ConvertFrom-Json
if ($startup.Failure -or $startup.CompletedRuns -ne $StartupRuns -or @($startup.Samples).Count -ne $StartupRuns -or
    $null -eq $startup.MedianMs) { throw 'Startup benchmark did not produce the required complete run set' }
$r.startup_ms = [double]$startup.MedianMs

# 2. idle memory, profiling off
'measuring idle memory...'
$p = Start-Demo $exe @($env:USERPROFILE, $mid)
try {
    Start-Sleep -Milliseconds 1500
    $p.Refresh()
    if ($p.HasExited -or $script:main -eq [IntPtr]::Zero -or (Title) -eq '') { throw 'Idle-memory host was not ready' }
    $r.private_mb = [math]::Round($p.PrivateMemorySize64 / 1MB, 2)
    $r.working_set_mb = [math]::Round($p.WorkingSet64 / 1MB, 2)
    $r.handles = $p.HandleCount
    $r.threads = $p.Threads.Count
} finally { Stop-BudgetHost $p }

# 3. scripted workload with profiling on (headless), slowest UI message.
# Keep every run in the gate; show the worst run's message and zone details
# alongside its maximum so the report identifies the event that set the budget.
function Run-Workload([string] $csv) {
if (Test-Path $csv) { throw "Workload evidence already exists: $csv" }
$p = Start-Demo $exe @("--profile-out=$csv", $big, $mid)
try {
    if (-not (Wait-Until { (Title) -eq 'WinSxS - Folderwing' } 5000)) { throw 'WinSxS workload was not ready' }
    Start-Sleep -Milliseconds 300
    foreach ($c in 'Size', 'Modified', 'Name', 'Name') { Click-Header $pane0 $c }
    Post $pane0 $WM_CHAR 0x1C; Start-Sleep -Milliseconds 300         # single pane: Type column appears
    foreach ($c in 'Type', 'Name') { Click-Header $pane0 $c }
    Post $pane0 $WM_CHAR 0x1C; Start-Sleep -Milliseconds 300
    Send-Text $pane0 'amd64_microsoft'
    for ($i = 0; $i -lt 15; $i++) { Key $pane0 0x08 30 }              # Backspace edits the filter
    for ($i = 0; $i -lt 10; $i++) { Key $pane0 0x22 30 }              # PageDown
    Key $pane0 0x23; Key $pane0 0x24                                   # End, Home
    for ($i = 0; $i -lt 10; $i++) { Post $pane0 $WM_MOUSEWHEEL (-120 -shl 16) 0; Start-Sleep -Milliseconds 30 }
    Key $pane0 0x08                                                    # up to C:\Windows
    if (-not (Wait-Until { (Title) -eq 'Windows - Folderwing' })) { throw 'Parent navigation was not ready' }
    Post $pane0 $WM_SYSKEYDOWN 0x25 0x20000000                         # Alt+Left: WinSxS again, from cache
    if (-not (Wait-Until { (Title) -eq 'WinSxS - Folderwing' })) { throw 'Cached navigation was not ready' }
    Start-Sleep -Milliseconds 300
    Key $pane1 0x0D                                                    # open the first folder on the right
    Start-Sleep -Milliseconds 300
    Key $pane1 0x08
    Click $tree ([int](80 * $scale)) ([int]((6 + 12) * $scale))       # tree: home
    Start-Sleep -Milliseconds 500
} finally {
    Stop-BudgetHost $p
}
if (-not (Test-Path $csv)) { throw "Workload profile missing: $csv" }
Import-Csv $csv
}

$runs = @()
for ($run = 1; $run -le $WorkloadRuns; $run++) {
    "running the UI workload ($run/$WorkloadRuns)..."
    $csv = Join-Path $evidence "workload$run.csv"
    $this = @(Run-Workload $csv)
    $profile = Assert-WorkloadProfile $this $UiStallMs
    $runs += [pscustomobject]@{ Run=$run; Csv=$csv; Profile=$profile; Rows=$this }
}
$batch = Assert-WorkloadBatch $runs $WorkloadRuns $UiStallMs
$best = $batch.BestRun; $worst = $batch.WorstRun
$rows = $worst.Rows
$r.ui_slowest_ms = [math]::Round($batch.WorstMs, 2)
$r.ui_slowest_msg = $worst.Profile.SlowestMessage
$r.ui_over_16ms = [int]$batch.TotalOver16
$r.ui_messages = [int](($runs | ForEach-Object { $_.Profile.DispatchCalls } | Measure-Object -Sum).Sum)
$r.gdi_objects = [int]$worst.Profile.GdiObjects
[pscustomobject]@{ ExpectedRuns=$WorkloadRuns; Runs=@($runs | ForEach-Object {
    [pscustomobject]@{ Run=$_.Run; Csv=$_.Csv; Profile=$_.Profile } });
    SelectedBestRun=$best.Run; SelectedWorstRun=$worst.Run;
    BestMs=$batch.BestMs; WorstMs=$batch.WorstMs;
    AllMeetUiContract=$batch.AllMeetContract } | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $evidence 'workloads.json')

# 4. engine
'running the engine benchmark...'
$bench = @(& (Join-Path $bin 'folderwing-cli.exe') bench $mid)
if ($LASTEXITCODE -ne 0) { throw "Engine benchmark exited $LASTEXITCODE" }
$bench | Set-Content -LiteralPath (Join-Path $evidence 'engine-bench.txt')
$r.read_system32_ms = Get-UniqueBenchMs $bench '^fpe_dir_read \(warm\)\s+([\d.]+) ms' 'warm directory read'
$r.sort_system32_ms = Get-UniqueBenchMs $bench '^sort name \(natural\)\s+([\d.]+) ms' 'natural sort'

# 5. binaries
$r.fpdemo_kb = [math]::Round((Get-Item $exe).Length / 1KB)
$r.fpe_dll_kb = [math]::Round((Get-Item (Join-Path $bin 'folderwing-engine.dll')).Length / 1KB)

# compare
$prev = if (Test-Path $history) { Import-Csv $history | Select-Object -Last 1 } else { $null }
$budgets = @{ startup_ms = $StartupMs; private_mb = $PrivateMB; ui_slowest_ms = $UiStallMs; ui_over_16ms = 0 }
$failed = 0
$table = foreach ($k in $r.Keys) {
    if ($k -in 'when', 'commit') { continue }
    $now = $r[$k]
    $before = if ($prev) { $prev.$k } else { $null }
    $change = ''
    if ($null -ne $before -and $before -ne '' -and $now -is [ValueType]) {
        $d = [double]$now - [double]$before
        $change = if ([double]$before) { '{0:+0.0;-0.0;0}%' -f (100 * $d / [double]$before) } else { '{0:+0.##;-0.##;0}' -f $d }
    }
    $status = ''
    if ($budgets.ContainsKey($k)) {
        $ok = [double]$now -le $budgets[$k]
        if (-not $ok) { $failed++ }
        $status = if ($ok) { 'ok' } else { 'OVER' }
    }
    [pscustomobject]@{ metric = $k; now = $now; previous = $before; change = $change
                       budget = if ($budgets.ContainsKey($k)) { $budgets[$k] } else { '' }; status = $status }
}
''
"commit $($r.commit)" + $(if ($prev) { "   (previous run: $($prev.commit), $($prev.when))" } else { '' })
$table | Format-Table -AutoSize | Out-String -Width 200

"slowest zones in worst UI run $($worst.Run) (max per call):"
"all $WorkloadRuns UI runs: best $($batch.BestMs) ms (run $($best.Run)); worst $($batch.WorstMs) ms (run $($worst.Run)); over-16 events $($batch.TotalOver16)"
$rows | Where-Object { $_.kind -eq 'zone' -and $_.name -ne 'ui.dispatch' -and $_.max_ms } |
    Sort-Object { [double]$_.max_ms } -Descending | Select-Object -First 8 |
    ForEach-Object { '  {0,-32} {1,8:N2} ms max  {2,6} calls' -f $_.name, [double]$_.max_ms, $_.calls }

if (-not $NoRecord) {
    [pscustomobject]$r | Export-Csv $history -Append -NoTypeInformation
    "`nrecorded in $history"
}
if ($failed) { "`n$failed budget(s) exceeded" } else { "`nall budgets met" }
exit $failed
