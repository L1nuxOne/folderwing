<# Focused Duplicate acceptance. Owns its fixtures/processes; no desktop input or WSL shutdown. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\duplicates\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh result directory; prior evidence is retained'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
function Invoke-OwnedWsl([string[]]$Arguments){
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($argument in $Arguments){[void]$pi.ArgumentList.Add($argument)}
 $p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture command timed out'};$code=$p.ExitCode;$p.Dispose();if($code){throw ('Owned WSL fixture command failed: '+$e.Result)};return $o.Result
}
$linux='/tmp/openpilot-duplicates-'+[guid]::NewGuid().ToString('N');$process=$null
$setup=@'
import os,sys
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-duplicates-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700)
open(p+'/.owner','w').write('openpilot-duplicates/v1')
p=p+'/data';os.mkdir(p)
open(p+'/a.txt','wb').write(b'same content')
os.link(p+'/a.txt',p+'/alias.txt')
open(p+'/a:b.txt','wb').write(b'native data')
open(os.fsencode(p)+b'/raw-\xff','wb').write(b'native data')
open(p+'/empty','wb').close()
os.symlink('.',p+'/loop');os.mkfifo(p+'/fifo')
print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $distro=(Invoke-OwnedWsl @('--exec','python3','-c',$setup,$linux)).Trim();if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Owned Linux fixture setup failed'}
 $unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\data'
 $images=[ordered]@{};foreach($name in @('duplicate_tests.exe','duplicate_host_tests.exe','folderwing-duplicates.exe','folderwing.exe','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $sources=[ordered]@{};foreach($name in @('app/duplicatewire.h','app/duplicateproto.h','app/duplicates.h','app/duplicates.c','app/duplicateread.h','app/duplicateread.c','app/fpduplicates.c','app/fpdemo.c','linux/fpduplicate.c','linux/fpsha256.h','linux/fpwsl.c','tests/duplicate_tests.c','tests/duplicate_host_tests.c','tests/duplicates.ps1','tools/build-duplicates.py','build.ps1','tests/native-impact.json')){$sources[$name]=(Get-FileHash (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
 foreach($selector in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/duplicate_tests.c::') -or $_.Name.StartsWith('tests/duplicate_host_tests.c::')}){foreach($name in $selector.Value.sources){$path=Join-Path $root $name;if(Test-Path $path -PathType Leaf){$sources[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 $nodeids=@($registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/duplicate_tests.c::') -or $_.Name.StartsWith('tests/duplicate_host_tests.c::')}|ForEach-Object Name)
 $os=Get-CimInstance Win32_OperatingSystem;$cpu=Get-CimInstance Win32_Processor
 $profile=[ordered]@{os=[Environment]::OSVersion.VersionString;pwsh=$PSVersionTable.PSVersion.ToString();cpu_load=@($cpu|ForEach-Object LoadPercentage);free_memory_kib=$os.FreePhysicalMemory;total_memory_kib=$os.TotalVisibleMemorySize;cache='uncontrolled warm development host';containers='unchanged, not used by fixtures';external_services='none';network='local WSL only';workers=1}
 $setupMs=$overall.Elapsed.TotalMilliseconds
 $input=[ordered]@{nodeids=$nodeids;runner='tests/duplicates.ps1';profile=$profile;schema='openpilot-duplicates-input/v1';images=$images;sources=$sources;workers=1;desktop_input=$false;linux_fixture='owned hardlink/symlink/FIFO/punctuation/invalid-UTF8 duplicate tree';full_suite=$false}
 $json=$input|ConvertTo-Json -Depth 8 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory 'duplicate_tests.exe';$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['OPENPILOT_DUPLICATE_WSL_FIXTURE']=$unc;$pi.Environment['OPENPILOT_DUPLICATE_SCREENSHOT']=Join-Path $ResultDirectory 'duplicates-panel.bmp'
 $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(-not $process.WaitForExit(120000)){$process.Kill($true);throw 'Owned Duplicate acceptance exceeded 120 s'};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$stderr.Result)
 @{schema='openpilot-duplicates-acceptance/v1';outcome=$(if($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
 Write-Output $stdout.Result;if($exit -ne 0){throw 'Duplicate acceptance failed; evidence retained'}
 $left=Join-Path $ResultDirectory 'host-fixtures\left';$right=Join-Path $ResultDirectory 'host-fixtures\right';New-Item -ItemType Directory $left,$right|Out-Null
 [IO.File]::WriteAllText((Join-Path $left 'notes.md'),"# Duplicate preview`nExact owned fixture`n");[IO.File]::Copy((Join-Path $left 'notes.md'),(Join-Path $left 'copy.md'));[IO.File]::Copy((Join-Path $left 'notes.md'),(Join-Path $right 'other.md'))
 $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory 'duplicate_host_tests.exe';$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;[void]$pi.ArgumentList.Add($left);[void]$pi.ArgumentList.Add($right)
 $watch.Restart();$process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(-not $process.WaitForExit(60000)){$process.Kill($true);throw 'Owned Duplicate host acceptance exceeded 60 s'};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'host-stdout.txt'),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'host-stderr.txt'),$stderr.Result)
 @{schema='openpilot-duplicates-host-acceptance/v1';outcome=$(if($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'HOST.json')
 Write-Output $stdout.Result;if($exit -ne 0){throw 'Duplicate host acceptance failed; evidence retained'}
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 # Remove only exact known entries under the fresh UUID root with its owner marker.
 $cleanup=@'
import os,sys
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-duplicates-') and len(p.rsplit('-',1)[1])==32
if not os.path.isdir(p): sys.exit(0)
assert not os.path.islink(p)
assert open(p+'/.owner').read()=='openpilot-duplicates/v1'
for n in [b'a.txt',b'alias.txt',b'a:b.txt',b'raw-\xff',b'empty',b'loop',b'fifo']:os.unlink(os.fsencode(p)+b'/data/'+n)
os.rmdir(p+'/data');os.unlink(p+'/.owner');os.rmdir(p)
'@
 try{Invoke-OwnedWsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null;@{outcome='passed';owned_linux_fixture_removed=$true;setup_ms=$setupMs;wall_ms=$overall.Elapsed.TotalMilliseconds}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLEANUP.json')}catch{@{outcome='failed';reason='Owned Linux fixture cleanup incomplete; preserved for review';wall_ms=$overall.Elapsed.TotalMilliseconds}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLEANUP.json');throw}
}
