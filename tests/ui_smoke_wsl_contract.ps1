# Real fixture lifecycle, including wrong-owner deletion refusal. Never stops WSL.
param([string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\..\tools\child-ui-common.ps1"
Initialize-ChildUiWindows
. "$PSScriptRoot\ui_smoke_wsl.ps1"
$repo=Split-Path $PSScriptRoot -Parent
if(-not $EvidenceRoot){$EvidenceRoot=Join-Path $repo "build\ui-smoke-wsl-contract\$([Guid]::NewGuid().ToString('N'))"}
$null=New-Item -ItemType Directory $EvidenceRoot;$out=$EvidenceRoot
$inputFiles=@{};foreach($file in @('tests\ui_smoke_wsl_contract.ps1','tests\ui_smoke_wsl.ps1','tools\child-ui-common.ps1')){$inputFiles[$file]=(Get-FileHash (Join-Path $repo $file)).Hash.ToLowerInvariant()}
$inputs=@{schema='openpilot-wsl-fixture-contract-input/v1';sources=$inputFiles;powershell_sha256=(Get-FileHash ([Diagnostics.Process]::GetCurrentProcess().Path)).Hash.ToLowerInvariant();command='tests/ui_smoke_wsl_contract.ps1';worker_count=1;profile='ordinary Windows parent and default WSL distro; owned temp fixture';retries=0;skips=0}
$inputKey=Get-ChildUiDigest $inputs;Write-ChildUiJson (Join-Path $EvidenceRoot 'input.json') $inputs
$wslOwner=[Guid]::NewGuid().ToString('N');$wslRoot='';$ok=$false;$cases=@();$errorMessage=$null
try{
    $setup=Invoke-SmokeWsl setup
    $lines=@(($setup.output -split '\r?\n')|Where-Object {$_})
    if(-not $setup.finished -or $setup.exit_code -ne 0 -or $lines.Count -ne 2 -or $lines[1] -cnotmatch '^/tmp/openpilot-ui-[a-zA-Z0-9]{8}$'){throw 'WSL setup failed; raw evidence retained'}
    $wslRoot=$lines[1];$cases+=@{case='fresh mktemp root and marker';passed=$true}
    $realOwner=$wslOwner;$wslOwner=[Guid]::NewGuid().ToString('N')
    $wrong=Invoke-SmokeWsl cleanup
    foreach($suffix in @('json','stdout.log','stderr.log')){Copy-Item -LiteralPath (Join-Path $out "wsl-cleanup.$suffix") -Destination (Join-Path $out "wrong-owner-cleanup.$suffix")}
    if(-not $wrong.finished -or $wrong.exit_code -ne 73){throw 'Wrong-owner cleanup was not refused'}
    $cases+=@{case='wrong-owner deletion rejected by real Linux guard';passed=$true};$wslOwner=$realOwner
    $outside=$wslRoot;$wslRoot='/tmp'
    $threw=$false;try{$null=Invoke-SmokeWsl cleanup}catch{$threw=$true}
    if(-not $threw){throw 'Outside-root command was admitted'}
    $cases+=@{case='outside root rejected before starting WSL';passed=$true};$wslRoot=$outside
    $mutation=Invoke-SmokeWsl mutation
    if(-not $mutation.finished -or $mutation.exit_code -ne 0){throw 'Correct owner could not mutate its preserved root'}
    $cases+=@{case='correct owner can mutate after rejected cleanup';passed=$true}
    $cleanup=Invoke-SmokeWsl cleanup
    if(-not $cleanup.finished -or $cleanup.exit_code -ne 0){throw 'Correct owned cleanup failed'}
    $cases+=@{case='correct owner cleanup completes';passed=$true};$ok=$true
}catch{$errorMessage=$_.Exception.Message}
$result=@{outcome=$(if($ok){'passed'}else{'failed'});input_key=$inputKey;cases=$cases;error=$errorMessage;fixture_root=$wslRoot;owner=$wslOwner;failed_fixtures='retained';wsl_stops=0;retries=0;skips=0}
$result.receipt_key=Get-ChildUiDigest $result
Write-ChildUiJson (Join-Path $EvidenceRoot 'FINAL.json') $result
$result|ConvertTo-Json -Depth 6
if(-not $ok){exit 1}
