$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\operations_contract.ps1"
$sample = "Operation #7`r`nCopy: Finished with skips`r`nSource: C:\left\a.txt`r`nDestination: C:\right`r`n1/1 items processed; 0 errors; 1 skipped`r`nDestination already exists`r`n"
$passed = 0
function Check([string]$name, [bool]$ok) { if (-not $ok) { throw "Failed: $name" }; $script:passed++; "ok $name" }
Check 'fresh clipboard response' (Test-FreshClipboard $sample 'waiting-for-copy')
Check 'withheld Copy response fails' (-not (Test-FreshClipboard 'waiting-for-copy' 'waiting-for-copy'))
Check 'exact recovered result' (Test-OperationResult $sample 7 'Finished with skips' 'C:\left\a.txt' 'C:\right' 1 1 0 1 'Destination already exists')
Check 'wrong replay token fails' (-not (Test-OperationResult $sample 8 'Finished with skips' 'C:\left\a.txt' 'C:\right' 1 1 0 1))
Check 'wrong replay path fails' (-not (Test-OperationResult $sample 7 'Finished with skips' 'C:\left\b.txt' 'C:\right' 1 1 0 1))
Check 'wrong replay count fails' (-not (Test-OperationResult $sample 7 'Finished with skips' 'C:\left\a.txt' 'C:\right' 0 1 0 1))
Check 'missing detail fails' (-not (Test-OperationResult $sample 7 'Finished with skips' 'C:\left\a.txt' 'C:\right' 1 1 0 1 'Different reason'))
"$passed checks passed"
