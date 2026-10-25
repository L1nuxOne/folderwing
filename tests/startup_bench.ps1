<# Measures process creation to first frame, with one owned sample per launch. #>
param(
    [ValidateRange(1,100)][int] $Runs = 10,
    [string] $Folder = $env:USERPROFILE,
    [string] $Config = 'release',
    [string] $EvidenceDir = ''
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\budget_contract.ps1"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "build\$Config\folderwing.exe"
if (-not $EvidenceDir) { $EvidenceDir = Join-Path $root ("build\budget\startup-{0}-{1}" -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $PID) }
New-Item -ItemType Directory -Force $EvidenceDir | Out-Null
$previousLog = [Environment]::GetEnvironmentVariable('FPDEMO_STARTUP_LOG')
$samples = @(); $failure = $null
try {
    for ($i = 1; $i -le $Runs; $i++) {
        $log = Join-Path $EvidenceDir "sample-$i.txt"
        if (Test-Path $log) { throw "Startup evidence path already exists: $log" }
        $env:FPDEMO_STARTUP_LOG = $log
        $p = $null
        try {
            $p = Start-Quiet $exe "--settings= --session= `"$Folder`""
            $created = $p.StartTime.ToFileTimeUtc()
            $clock = [Diagnostics.Stopwatch]::StartNew()
            $complete = $false
            while ($clock.ElapsedMilliseconds -lt 5000) {
                if ($p.HasExited) { throw "Launch $i exited before the startup sample" }
                if (Test-Path $log) {
                    $raw = Get-Content -LiteralPath $log -Raw
                    if ($raw -and $raw.EndsWith("`n")) { $complete = $true; break }
                }
                Start-Sleep -Milliseconds 20
            }
            if (-not $complete) { throw "Launch $i produced no complete startup sample within 5000 ms" }
            $ms = Assert-StartupSample @(Get-Content -LiteralPath $log) $p.Id $created
            $ready = Wait-Until {
                $p.Refresh(); $h = $p.MainWindowHandle; $owner = [uint32]0
                $h -ne [IntPtr]::Zero -and
                    [FpUi]::GetWindowThreadProcessId($h, [ref]$owner) -ne 0 -and
                    $owner -eq $p.Id -and [FpUi]::IsWindowVisible($h)
            } 1000
            $p.Refresh(); $window = $p.MainWindowHandle; $windowPid = [uint32]0
            if ($window -ne [IntPtr]::Zero) { [FpUi]::GetWindowThreadProcessId($window, [ref]$windowPid) | Out-Null }
            $idle = if ($ready) { $p.WaitForInputIdle(1000) } else { $false }
            Assert-OwnedReadyWindow $p.Id $window.ToInt64() $windowPid $ready $idle
            # Let work after first frame finish before the next launch.
            Start-Sleep -Milliseconds 700
            $processHandle = $p.Handle
            $posted = [FpUi]::PostMessageW($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)
            $exited = if ($posted) { $p.WaitForExit(5000) } else { $false }
            $code = [uint32]::MaxValue
            $codeRead = if ($exited) { [FpUi]::GetExitCodeProcess($processHandle, [ref]$code) } else { $false }
            if (-not $posted -or -not $exited -or -not $codeRead -or $code -ne 0) {
                throw "Launch $i did not exit cleanly (posted=$posted, exited=$exited, code=$code)"
            }
            $samples += [pscustomobject]@{ Run=$i; Pid=$p.Id; Created=$created; StartupMs=$ms;
                Window=$window.ToInt64(); Log=$log; ExitCode=$code }
        } finally {
            if ($p -and -not $p.HasExited) { $p.Kill(); $p.WaitForExit(3000) | Out-Null }
        }
    }
} catch { $failure = $_.Exception.Message }
finally {
    [Environment]::SetEnvironmentVariable('FPDEMO_STARTUP_LOG', $previousLog)
    $ordered = @($samples | Sort-Object StartupMs)
    $median = if ($ordered.Count -eq $Runs) {
        if ($Runs % 2) { [double]$ordered[[int][math]::Floor($Runs/2)].StartupMs }
        else { ([double]$ordered[$Runs/2-1].StartupMs + [double]$ordered[$Runs/2].StartupMs)/2 }
    } else { $null }
    [pscustomobject]@{ ExpectedRuns=$Runs; CompletedRuns=$samples.Count; Samples=$samples;
        MedianMs=$median; Failure=$failure } | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $EvidenceDir 'result.json')
}
if ($failure -or $samples.Count -ne $Runs) { throw "Startup benchmark incomplete: $failure ($($samples.Count)/$Runs samples); evidence: $EvidenceDir" }
'runs {0}  min {1:N1} ms  median {2:N1} ms  max {3:N1} ms' -f $Runs,
    $ordered[0].StartupMs, $median, $ordered[-1].StartupMs
