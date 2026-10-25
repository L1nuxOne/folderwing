<# Hosted-runner-compatible functional lane: no WSL, audio/GPU, physical input or elevation required. #>
param([Parameter(Mandatory)][string]$BuildDirectory,[Parameter(Mandatory)][string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(Test-Path $ResultDirectory){throw 'Use a fresh smoke result directory'};New-Item -ItemType Directory $ResultDirectory|Out-Null
$modules=@('cli_tests.exe','workspace_tests.exe','ntfs_model_tests.exe','shell_protocol_tests.exe')
$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
$rows=@($registry.selectors.psobject.Properties|Where-Object {$registry.modules.($_.Name.Split('::')[0]).executable -in ($modules+@('branding_host_tests.exe'))})
$sources=[ordered]@{};foreach($row in $rows){foreach($n in $row.Value.sources){$path=Join-Path $root $n;if(Test-Path $path -PathType Leaf){$sources[$n]=(Get-FileHash $path).Hash.ToLowerInvariant()}}}
$images=[ordered]@{};foreach($name in ($modules+@('branding_host_tests.exe','folderwing.exe'))){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name)).Hash.ToLowerInvariant()}
$input=[ordered]@{schema='folderwing-release-smoke-input/v1';nodeids=@($rows|ForEach-Object Name);sources=$sources;images=$images;workers=1;os=[Environment]::OSVersion.VersionString;pwsh=$PSVersionTable.PSVersion.ToString();physical_input=$false;wsl='not required or exercised';audio_gpu='not required or exercised';full_suite=$false}
$text=$input|ConvertTo-Json -Depth 8 -Compress;[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$text);$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).ToLowerInvariant()
$runs=[Collections.Generic.List[object]]::new()
foreach($name in $modules){
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory $name));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 $watch=[Diagnostics.Stopwatch]::StartNew();$p=[Diagnostics.Process]::Start($pi);$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync();$timeout=-not $p.WaitForExit(120000);if($timeout){$p.Kill($true);$p.WaitForExit()};$code=$(if($timeout){124}else{$p.ExitCode});$p.Dispose();$watch.Stop()
 [IO.File]::WriteAllText((Join-Path $ResultDirectory ($name+'.stdout.txt')),$out.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($name+'.stderr.txt')),$err.Result)
 $runs.Add(@{module=$name;exit_code=$code;elapsed_ms=$watch.Elapsed.TotalMilliseconds;outcome=$(if($code){'failed'}else{'passed'})})
 $runs.ToArray()|ConvertTo-Json -Depth 6|Set-Content (Join-Path $ResultDirectory 'RUNS.json')
 if($code){throw "Release smoke failed in $name; evidence retained"}
}
& (Join-Path $PSScriptRoot 'branding.ps1') -BuildDirectory $BuildDirectory -ResultDirectory (Join-Path $ResultDirectory 'branding')
foreach($n in $sources.Keys){if((Get-FileHash (Join-Path $root $n)).Hash.ToLowerInvariant() -ne $sources[$n]){throw "Source changed during smoke: $n"}}
foreach($n in $images.Keys){if((Get-FileHash (Join-Path $BuildDirectory $n)).Hash.ToLowerInvariant() -ne $images[$n]){throw "Image changed during smoke: $n"}}
@{schema='folderwing-release-smoke/v1';outcome='passed';input_key=$key;modules=$modules+@('branding_host_tests.exe');selectors=@($rows|ForEach-Object Name);physical_input=$false;wsl_stopped=$false;full_suite=$false;workers=1}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
Write-Output 'Portable release functional smoke passed.'
