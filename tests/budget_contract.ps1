# Pure validation shared by the live budget gate and its fault-injection checks.
function Assert-StartupSample([string[]]$Lines, [int]$ExpectedPid, [long]$Created) {
    if (@($Lines).Count -ne 1) { throw "Expected exactly one startup sample, got $(@($Lines).Count)" }
    $parts = $Lines[0].Trim() -split ','
    if ($parts.Count -ne 3) { throw 'Startup sample must contain PID, creation time and duration' }
    $parsedPid = 0; $parsedCreated = [long]0; $ms = [double]0
    $style = [Globalization.NumberStyles]::Float
    $culture = [Globalization.CultureInfo]::InvariantCulture
    if (-not [int]::TryParse($parts[0], [ref]$parsedPid) -or
        -not [long]::TryParse($parts[1], [ref]$parsedCreated) -or
        -not [double]::TryParse($parts[2], $style, $culture, [ref]$ms) -or
        $parsedPid -ne $ExpectedPid -or $parsedCreated -ne $Created -or
        -not [double]::IsFinite($ms) -or $ms -le 0) { throw 'Startup sample identity or duration is invalid' }
    $ms
}
function Assert-OwnedReadyWindow([int]$ExpectedPid, [long]$Hwnd, [int]$WindowPid,
    [bool]$Visible, [bool]$InputIdle) {
    if ($Hwnd -eq 0 -or $WindowPid -ne $ExpectedPid -or -not $Visible -or -not $InputIdle) {
        throw 'Startup window was missing, unowned, hidden or not input-ready'
    }
}
function Get-UniqueProfileRow($Rows, [string]$Kind, [string]$Name) {
    $matches = @($Rows | Where-Object { $_.kind -eq $Kind -and $_.name -eq $Name })
    if ($matches.Count -ne 1) { throw "Expected one $Kind/$Name profile row, got $($matches.Count)" }
    $matches[0]
}
function Get-ProfileNumber($Rows, [string]$Kind, [string]$Name, [string]$Field) {
    $row = Get-UniqueProfileRow $Rows $Kind $Name
    $number = [double]0
    if (-not [double]::TryParse($row.$Field, [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$number) -or
        -not [double]::IsFinite($number) -or $number -lt 0) { throw "Invalid $Kind/$Name $Field" }
    $number
}
function Assert-WorkloadProfile($Rows, [double]$UiLimit) {
    if ($UiLimit -le 0) { throw 'UI limit must be positive' }
    $slow = Get-ProfileNumber $Rows 'metric' 'slowest_message_ms' 'value'
    $startup = Get-ProfileNumber $Rows 'metric' 'startup_ms' 'value'
    $gdi = Get-ProfileNumber $Rows 'metric' 'gdi_objects' 'value'
    $dispatchCalls = Get-ProfileNumber $Rows 'zone' 'ui.dispatch' 'calls'
    $dispatchMax = Get-ProfileNumber $Rows 'zone' 'ui.dispatch' 'max_ms'
    $message = (Get-UniqueProfileRow $Rows 'metric' 'slowest_message').value
    if ($slow -le 0 -or $startup -le 0 -or $gdi -le 0 -or $dispatchCalls -le 0 -or
        [string]::IsNullOrWhiteSpace($message)) { throw 'Required workload profile values are empty' }
    $overRows = @($Rows | Where-Object { $_.kind -eq 'zone' -and $_.name -eq 'ui.over_16ms' })
    if ($overRows.Count -gt 1) { throw 'Duplicate ui.over_16ms profile row' }
    $over = if ($overRows.Count) { Get-ProfileNumber $Rows 'zone' 'ui.over_16ms' 'calls' } else { 0 }
    [pscustomobject]@{ SlowestMs=$slow; StartupMs=$startup; GdiObjects=$gdi;
        DispatchCalls=$dispatchCalls; DispatchMaxMs=$dispatchMax; Over16=$over;
        SlowestMessage=$message; MeetsUiContract=($slow -le $UiLimit -and $over -eq 0) }
}
function Assert-WorkloadBatch($Summaries, [int]$Expected, [double]$UiLimit) {
    $list = @($Summaries)
    if ($Expected -lt 1 -or $list.Count -ne $Expected) { throw "Expected $Expected workload runs, got $($list.Count)" }
    for ($i=0; $i -lt $Expected; $i++) {
        if ($list[$i].Run -ne ($i+1) -or $null -eq $list[$i].Profile) { throw "Missing or misordered workload run $($i+1)" }
    }
    $best = $list[0]; $worst = $list[0]; $overTotal = 0
    foreach ($entry in $list) {
        $overTotal += $entry.Profile.Over16
        # Keep the first run on ties so the duration and its evidence stay together.
        if ([double]$entry.Profile.SlowestMs -lt [double]$best.Profile.SlowestMs) { $best = $entry }
        if ([double]$entry.Profile.SlowestMs -gt [double]$worst.Profile.SlowestMs) { $worst = $entry }
    }
    [pscustomobject]@{ BestMs=[double]$best.Profile.SlowestMs;
        WorstMs=[double]$worst.Profile.SlowestMs; BestRun=$best; WorstRun=$worst;
        TotalOver16=$overTotal;
        AllMeetContract=(@($list | Where-Object { -not $_.Profile.MeetsUiContract }).Count -eq 0);
        Runs=$list }
}
function Get-UniqueBenchMs([string[]]$Lines, [string]$Pattern, [string]$Name) {
    $values = @()
    foreach ($line in $Lines) {
        $match = [regex]::Match($line, $Pattern)
        if ($match.Success) { $values += $match.Groups[1].Value }
    }
    if ($values.Count -ne 1) { throw "Expected one $Name benchmark result, got $($values.Count)" }
    $value = [double]0
    if (-not [double]::TryParse($values[0], [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture, [ref]$value) -or
        -not [double]::IsFinite($value) -or $value -le 0) { throw "Invalid $Name benchmark result" }
    $value
}
