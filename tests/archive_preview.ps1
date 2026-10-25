<# Focused archive acceptance. No physical input, external clipboard writes, WSL/guest changes. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\archive_preview\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[Guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $ResultDirectory){throw 'Use a new result directory; earlier results are retained.'}
New-Item -ItemType Directory -Path $ResultDirectory|Out-Null
$fixture=Join-Path $ResultDirectory 'fixtures';New-Item -ItemType Directory -Path $fixture|Out-Null
$package=Join-Path $PSScriptRoot 'fixtures\archives\preview-fixtures.zip'
$manifest=Get-Content (Join-Path $PSScriptRoot 'fixtures\archives\manifest.json') -Raw|ConvertFrom-Json
if((Get-FileHash $package -Algorithm SHA256).Hash.ToLowerInvariant() -ne $manifest.bundle_sha256){throw 'Archive fixture package checksum mismatch'}
[IO.Compression.ZipFile]::ExtractToDirectory($package,$fixture)
foreach($file in $manifest.files.psobject.Properties){if((Get-FileHash (Join-Path $fixture $file.Name) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value){throw "Archive fixture checksum mismatch: $($file.Name)"}}
New-Item -ItemType Directory -Path (Join-Path $fixture 'destination')|Out-Null
$images=[ordered]@{};foreach($name in @('archive_tests.exe','folderwing-zip.exe','folderwing-archive.exe','folderwing-text.exe','folderwing-image.exe','folderwing-pdf.exe','pdfium.dll','folderwing-operations.exe')){
 $path=Join-Path $BuildDirectory $name;if(-not(Test-Path $path -PathType Leaf)){throw "Archive acceptance needs $name"};$images[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
# Freeze semantic inputs before launching the acceptance process.
$registryPath=Join-Path $PSScriptRoot 'native-impact.json';$registry=Get-Content $registryPath -Raw|ConvertFrom-Json
$selectors=@($registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/archive_tests.c::')}|Sort-Object Name)
$sourceNames=@($selectors|ForEach-Object {$_.Value.sources}|Sort-Object -Unique);$sourceHashes=[ordered]@{}
foreach($name in $sourceNames){$source=Join-Path $root $name;if(Test-Path $source -PathType Container){foreach($child in Get-ChildItem $source -Recurse -File|Sort-Object FullName){$relative=[IO.Path]::GetRelativePath($root,$child.FullName).Replace('\','/');$sourceHashes[$relative]=(Get-FileHash $child.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}else{$sourceHashes[$name]=(Get-FileHash $source -Algorithm SHA256).Hash.ToLowerInvariant()}}
$revision=(Get-Content (Join-Path $root '.git\HEAD') -Raw).Trim()
if($revision.StartsWith('ref: ')){$refName=$revision.Substring(5);$refPath=Join-Path (Join-Path $root '.git') $refName;if(Test-Path $refPath){$revision=(Get-Content $refPath -Raw).Trim()}else{$packed=Join-Path $root '.git\packed-refs';if(Test-Path $packed){$match=Get-Content $packed|Where-Object {$_ -match ('^([0-9a-f]{40}) '+[Regex]::Escape($refName)+'$')}|Select-Object -First 1;if($match){$revision=$match.Substring(0,40)}}}}
$input=[ordered]@{schema='openpilot-archive-acceptance-input/v1';source_revision=$revision;images=$images;selectors=@($selectors|ForEach-Object {$_.Name});sources=$sourceHashes;fixture_bundle_sha256=$manifest.bundle_sha256;workers=1;desktop='owned nonactivated Windows windows';physical_input=$false;external_clipboard=$false;helper_memory_limit_mib=512;wsl='not exercised';docker='unchanged';full_suite=$false}
$inputJson=$input|ConvertTo-Json -Depth 12 -Compress
$sha=[Security.Cryptography.SHA256]::Create();$inputKey=[Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($inputJson))).ToLowerInvariant();$sha.Dispose()
[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$inputJson)
$pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory 'archive_tests.exe';$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
[void]$pi.ArgumentList.Add($fixture);$pi.Environment['OPENPILOT_ARCHIVE_SCREENSHOT']=Join-Path $ResultDirectory 'archive-preview.bmp'
$watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$pidOwned=$process.Id;$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync()
$timedOut=-not $process.WaitForExit(120000);if($timedOut){$process.Kill($true);[void]$process.WaitForExit(5000)}
$watch.Stop();$exit=$process.ExitCode;$process.Dispose();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$out.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$err.Result)
@{schema='openpilot-archive-acceptance/v1';input_key=$inputKey;outcome=$(if($timedOut){'timed_out'}elseif($exit -eq 0){'passed'}else{'failed'});timed_out=$timedOut;exit_code=$exit;elapsed_ms=$watch.Elapsed.TotalMilliseconds;owned_pid=$pidOwned;images=$images;fixture_bundle_sha256=$manifest.bundle_sha256;workers=1;desktop_input=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
Write-Output $out.Result
if($timedOut){throw 'Owned archive acceptance exceeded 120 s; output retained'};if($exit -ne 0){throw "Archive acceptance failed; evidence retained at $ResultDirectory"}
Write-Output "Archive acceptance passed; evidence at $ResultDirectory"
