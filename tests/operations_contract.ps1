function Test-FreshClipboard([string]$Observed, [string]$Sentinel) {
    -not [string]::IsNullOrEmpty($Observed) -and $Observed -cne $Sentinel
}
function Test-OperationResult([string]$Snapshot, [uint64]$Token, [string]$State,
    [string]$Source, [string]$Destination, [int]$Done, [int]$Total,
    [int]$Errors, [int]$Skipped, [string]$Detail = '') {
    if ([string]::IsNullOrEmpty($Snapshot)) { return $false }
    $lines = @($Snapshot -split "`r?`n")
    $required = @("Operation #$Token", "Copy: $State", "Source: $Source",
        "Destination: $Destination", "$Done/$Total items processed; $Errors errors; $Skipped skipped")
    foreach ($line in $required) { if ($lines -cnotcontains $line) { return $false } }
    if ($Detail -and -not $Snapshot.Contains($Detail)) { return $false }
    $true
}
