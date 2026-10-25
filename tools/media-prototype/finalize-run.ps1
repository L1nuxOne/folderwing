param([Parameter(Mandatory)][string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if(Test-Path (Join-Path $ResultDirectory 'RESULT.json')){throw 'Preserve existing receipt'}
$input=Get-Content (Join-Path $ResultDirectory 'INPUT.json') -Raw|ConvertFrom-Json
$changed=@();foreach($property in $input.sources_and_binaries.PSObject.Properties){if((Get-FileHash (Join-Path $root $property.Name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $property.Value){$changed+=$property.Name}}
if($changed.Count){throw ('Frozen inputs changed: '+($changed -join ', '))}
$rows=@(Get-Content (Join-Path $ResultDirectory 'ROWS.json') -Raw|ConvertFrom-Json);if($rows.Count -ne 86){throw 'This recovery is limited to the completed 86-row matrix'}
$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $root 'build/media_prototype/20261023/candidate/audio-probe.exe'));$pi.UseShellExecute=$false;$pi.RedirectStandardOutput=$true;$pi.CreateNoWindow=$true
$p=[Diagnostics.Process]::Start($pi);$out=$p.StandardOutput.ReadToEndAsync();if(-not $p.WaitForExit(5000)){$p.Kill($true);throw 'Endpoint probe timed out'};$endpointExit=$p.ExitCode;$p.Dispose();$out.Result|Set-Content (Join-Path $ResultDirectory 'FINAL_ENDPOINTS.txt');if($endpointExit){throw 'No active default endpoint'}
$json=Get-Content (Join-Path $ResultDirectory 'INPUT.json') -Raw;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
@{schema='openpilot-media-comparison/v1';input_key=$key;completed=$true;runs=$rows.Count;input_changed=$changed;final_endpoint_exit=$endpointExit;profile=$input.profile;stopped_wsl=$false;stopped_docker=$false;controller_exit=1;finalization_recovery='All 86 samples finished. Final direct native-command pipeline failed under inherited PATHEXT (.CPL only). Separate explicit-ProcessStartInfo endpoint probe and frozen-input validation completed; no sample replay.';finalizer_sha256=(Get-FileHash $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
