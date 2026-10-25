<# Bounded NTFS analyzer acceptance. Normal owner launches only its own elevated
read-only worker. No physical input, security changes or WSL/Docker shutdown. #>
param([Parameter(Mandatory)][string]$BuildDirectory,[Parameter(Mandatory)][string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(Test-Path -LiteralPath $ResultDirectory){throw 'Use a fresh evidence directory'}
New-Item -ItemType Directory -Path $ResultDirectory|Out-Null
$selected=@('tests/ntfs_model_tests.c','tests/ntfs_panel_tests.c')
$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
$sources=[ordered]@{};$nodeids=[Collections.Generic.List[string]]::new()
foreach($item in $registry.selectors.psobject.Properties){if($selected -contains $item.Name.Split('::')[0]){$nodeids.Add($item.Name);foreach($name in $item.Value.sources){$p=Join-Path $root $name;if(Test-Path -LiteralPath $p -PathType Leaf){$sources[$name]=(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLowerInvariant()}}}}
$images=[ordered]@{};foreach($name in @('ntfs_model_tests.exe','ntfs_panel_tests.exe','folderwing-ntfs.exe','folderwing-space.exe')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
$input=[ordered]@{schema='openpilot-ntfs-analyzer-input/v1';sources=$sources;images=$images;selectors=@($nodeids.ToArray());workers=1;physical_input=$false;wsl_stopped=$false;volume='C:';cache='isolated owned cache; baseline then reopen';cpu_before=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);free_memory_kib=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory}
$json=$input|ConvertTo-Json -Depth 8 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
$results=[Collections.Generic.List[object]]::new()
foreach($name in @('ntfs_model_tests.exe','ntfs_panel_tests.exe')){
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory $name));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;if($name -eq 'ntfs_panel_tests.exe'){[void]$pi.ArgumentList.Add((Join-Path $ResultDirectory 'fixture'))}
 $watch=[Diagnostics.Stopwatch]::StartNew();$p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();$peak=0L
 while(-not $p.WaitForExit(250)){$p.Refresh();$peak=[Math]::Max($peak,$p.PeakWorkingSet64);if($watch.Elapsed.TotalSeconds -gt 240){$p.Kill($true);throw "Owned $name exceeded 240 seconds"}}
 $watch.Stop();$code=$p.ExitCode;$ownedId=$p.Id;$p.Dispose();[IO.File]::WriteAllText((Join-Path $ResultDirectory ($name+'.stdout.txt')),$o.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($name+'.stderr.txt')),$e.Result)
 $results.Add(@{image=$name;pid=$ownedId;exit_code=$code;elapsed_ms=$watch.Elapsed.TotalMilliseconds;peak_panel_bytes=$peak});$results.ToArray()|ConvertTo-Json -Depth 6|Set-Content (Join-Path $ResultDirectory 'RUNS.json')
 if($code){throw "Owned $name failed with $code; evidence retained"}
 # Reject a surviving worker from this exact owner invocation, leaving others alone.
 $owned=@(Get-CimInstance Win32_Process -Filter "Name='folderwing-ntfs.exe'"|Where-Object {$_.CommandLine -match ('(?i)folderwing-ntfs\.exe"?\s+'+[regex]::Escape([string]$ownedId)+'\s')});if($owned.Count){throw 'Owned NTFS worker outlived its client'}
}
# A receipt is only passing if the governed source/image inputs stayed fixed.
foreach($name in $sources.Keys){if((Get-FileHash -LiteralPath (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sources[$name]){throw "Source changed during acceptance: $name"}}
foreach($name in $images.Keys){if((Get-FileHash -LiteralPath (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $images[$name]){throw "Image changed during acceptance: $name"}}
@{schema='openpilot-ntfs-analyzer-acceptance/v1';outcome='passed';input_key=$key;selectors=@($nodeids.ToArray());runs=@($results.ToArray());workers=1;physical_input=$false;wsl_stopped=$false;full_suite=$false;skips=0;xfail=0;automatic_retries=0}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
Write-Output 'NTFS model and live panel acceptance passed; owned fixtures/cache retained.'
