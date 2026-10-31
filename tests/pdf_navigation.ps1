# Focused PDF navigation acceptance: synthetic documents, owned hidden windows.
param([string]$BuildDirectory,[string]$FixtureDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build/pdf-navigation'}
if(-not $FixtureDirectory){$FixtureDirectory=Join-Path $root 'build/pdf-navigation-fixtures'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build/pdf-navigation-checks/'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a new evidence directory'}
$manifest=Get-Content (Join-Path $FixtureDirectory 'FIXTURES.json') -Raw|ConvertFrom-Json
if($manifest.schema -cne 'folderwing-pdf-navigation-fixtures/v1' -or $manifest.personal_documents){throw 'Synthetic PDF fixture manifest required'}
New-Item -ItemType Directory $ResultDirectory|Out-Null;$fixtures=Join-Path $ResultDirectory 'fixtures';New-Item -ItemType Directory $fixtures|Out-Null
foreach($name in @('outline.pdf','plain.pdf','cycle.pdf')){if((Get-FileHash (Join-Path $FixtureDirectory $name)).Hash.ToLowerInvariant() -cne $manifest.files.$name){throw 'Fixture hash mismatch'};Copy-Item (Join-Path $FixtureDirectory $name) (Join-Path $fixtures $name)}
$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
$rows=@($registry.selectors.PSObject.Properties|Where-Object {$_.Name.StartsWith('tests/pdf_navigation_tests.c::')})
$sources=[ordered]@{};foreach($source in @($rows|ForEach-Object {$_.Value.sources})+@('tests/pdf_navigation.ps1')|Sort-Object -Unique){$sources[$source]=(Get-FileHash (Join-Path $root $source)).Hash.ToLowerInvariant()}
$images=[ordered]@{};foreach($name in @('folderwing.exe','folderwing-pdf.exe','pdf_navigation_tests.exe','pdfium.dll')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name)).Hash.ToLowerInvariant()}
$os=Get-CimInstance Win32_OperatingSystem
$input=[ordered]@{schema='folderwing-pdf-navigation-input/v1';selectors=@($rows.Name);sources=$sources;images=$images;fixtures=$manifest.files;profile=@{workers=1;desktop='owned hidden windows';physical_input=$false;wsl_stopped=$false;network='none';cache='uncontrolled development host';free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage)}}
$json=$input|ConvertTo-Json -Depth 10 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
$timer=[Diagnostics.Stopwatch]::StartNew();$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory 'pdf_navigation_tests.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
[void]$pi.ArgumentList.Add($fixtures);$pi.Environment['FOLDERWING_PDF_NAV_SCREENSHOT']=Join-Path $ResultDirectory 'pdf-rail.bmp'
$p=[Diagnostics.Process]::Start($pi);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();$timeout=-not $p.WaitForExit(120000);if($timeout){$p.Kill($true);$p.WaitForExit()};$exit=$p.ExitCode;$p.Dispose();$timer.Stop()
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$stderr.Result)
$changed=@($sources.Keys|Where-Object {(Get-FileHash (Join-Path $root $_)).Hash.ToLowerInvariant() -cne $sources[$_]})
$result=[ordered]@{schema='folderwing-pdf-navigation-result/v1';input_key=$key;outcome=$(if(-not $exit -and -not $timeout -and -not $changed.Count){'passed'}else{'failed'});exit_code=$exit;timeout=$timeout;changed_sources=$changed;wall_ms=$timer.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;wsl_stopped=$false;full_suite=$false}
$receipt=$result|ConvertTo-Json -Depth 8 -Compress;$result.receipt_id=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($receipt))).ToLowerInvariant();$result|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
Write-Output $stdout.Result;Write-Output $stderr.Result;Write-Output "Evidence: $ResultDirectory"
if($result.outcome -cne 'passed'){throw 'PDF navigation acceptance failed; see retained evidence'}
