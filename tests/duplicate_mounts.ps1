<# Actual native mount-boundary integration in private per-helper namespaces. #>
param([Parameter(Mandatory)][string]$BuildDirectory,[Parameter(Mandatory)][string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(Test-Path $ResultDirectory){throw 'Use a fresh result directory; earlier evidence is retained'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
function Linux-Path([string]$p){$p=[IO.Path]::GetFullPath($p);if($p -notmatch '^C:\\'){throw 'Fixture build must be on C:'};return '/mnt/c/'+$p.Substring(3).Replace('\','/')}
function Owned-Wsl([string[]]$Arguments){
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)}
 $p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync()
 if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned mount fixture command timed out'};$code=$p.ExitCode;$p.Dispose();if($code){throw $e.Result};return $o.Result
}
$linux='/tmp/openpilot-duplicate-mount-'+[guid]::NewGuid().ToString('N');$stage=Join-Path $ResultDirectory 'mount-candidate';$process=$null
$fixture=Linux-Path (Join-Path $PSScriptRoot 'fixtures\native\duplicate_mount_fixture.py')
try{
 $distro=(Owned-Wsl @('--exec','python3',$fixture,'setup',$linux,(Linux-Path $stage),(Linux-Path (Join-Path $BuildDirectory 'folderwing-wsl')),(Linux-Path (Join-Path $PSScriptRoot 'fixtures\native\duplicate_mount_entry.py')))).Trim()
 if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Invalid distro identity'}
 Copy-Item (Join-Path $BuildDirectory 'duplicate_mount_tests.exe') $stage
 $images=[ordered]@{};foreach($name in @('duplicate_mount_tests.exe','folderwing-wsl-real','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $stage $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json;$sources=[ordered]@{};$nodeids=@()
 foreach($s in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/duplicate_mount_tests.c::')}){$nodeids+=$s.Name;foreach($n in $s.Value.sources){$sources[$n]=(Get-FileHash (Join-Path $root $n) -Algorithm SHA256).Hash.ToLowerInvariant()}}
 if($nodeids.Count -ne 2){throw 'Mount fixture selectors must be impact-bound'}
 $input=[ordered]@{schema='openpilot-duplicate-mount-input/v1';nodeids=$nodeids;images=$images;sources=$sources;workers=1;full_suite=$false;private_user_mount_namespaces=$true;global_mount_mutations=$false}
 $json=$input|ConvertTo-Json -Depth 8 -Compress;[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json);$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $stage 'duplicate_mount_tests.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;[void]$pi.ArgumentList.Add(('\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\parent'))
 $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$o=$process.StandardOutput.ReadToEndAsync();$e=$process.StandardError.ReadToEndAsync();$timeout=-not $process.WaitForExit(120000);if($timeout){$process.Kill($true);$process.WaitForExit()};$watch.Stop();$code=$(if($timeout){124}else{$process.ExitCode});$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$o.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$e.Result)
 @{outcome=$(if($code){'failed'}else{'passed'});exit_code=$code;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;timed_out=$timeout;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
 Write-Output $o.Result;if($code){throw 'Live mount integration failed; evidence retained'}
 Owned-Wsl @('--exec','python3',$fixture,'check',$linux)|Set-Content (Join-Path $ResultDirectory 'NAMESPACES.json')
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 Owned-Wsl @('--exec','python3',$fixture,'cleanup',$linux)|Out-Null
 @{outcome='passed';owned_fixture_removed=$true;global_mount_unchanged=$true;wsl_stopped=$false;docker_stopped=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLEANUP.json')
}
