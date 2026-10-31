# Focused acceptance on owned temp data/private desktop; no physical input.
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build/footer-filters'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build/footer-filter-checks/'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a new evidence directory'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
$rows=@($registry.selectors.PSObject.Properties|Where-Object {$_.Name.StartsWith('tests/filter_footer_tests.c::')})
$sources=[ordered]@{}
foreach($source in @($rows|ForEach-Object {$_.Value.sources})+@('tests/footer_filters.ps1')|Sort-Object -Unique){$sources[$source]=(Get-FileHash (Join-Path $root $source)).Hash.ToLowerInvariant()}
$images=[ordered]@{}
foreach($image in @('folderwing.exe','folderwing-engine.dll','filter_footer_tests.exe')){$images[$image]=(Get-FileHash (Join-Path $BuildDirectory $image)).Hash.ToLowerInvariant()}
$input=[ordered]@{schema='folderwing-footer-filter-input/v1';selectors=@($rows.Name);sources=$sources;images=$images;profile=@{workers=1;desktop='owned private desktop';fixtures='new owned Windows temp directory';physical_input=$false;wsl_stopped=$false;network='none';cache='uncontrolled development host'}}
$json=$input|ConvertTo-Json -Depth 10 -Compress
$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
$timer=[Diagnostics.Stopwatch]::StartNew()
$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory 'filter_footer_tests.exe'))
$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
$pi.Environment['FOLDERWING_FILTER_CAPTURE']=Join-Path $ResultDirectory 'pane.bmp'
$p=[Diagnostics.Process]::Start($pi);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
$timeout=-not $p.WaitForExit(90000)
if($timeout){$p.Kill($true);$p.WaitForExit()}
$exit=$p.ExitCode;$p.Dispose();$timer.Stop()
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$stdout.Result)
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$stderr.Result)
$changed=@($sources.Keys|Where-Object {(Get-FileHash (Join-Path $root $_)).Hash.ToLowerInvariant() -cne $sources[$_]})
$result=[ordered]@{schema='folderwing-footer-filter-result/v1';input_key=$key;outcome=$(if(-not $exit -and -not $timeout -and -not $changed.Count){'passed'}else{'failed'});exit_code=$exit;timeout=$timeout;changed_sources=$changed;wall_ms=$timer.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;wsl_stopped=$false;full_suite=$false}
$result|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
Write-Output $stdout.Result;Write-Output $stderr.Result
Write-Output "Evidence: $ResultDirectory"
if($result.outcome -cne 'passed'){throw 'Footer acceptance failed; see retained evidence'}
