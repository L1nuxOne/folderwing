<# Focused Space acceptance. Owns its fixtures/processes; no desktop input or WSL shutdown. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\space_analyzer\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh result directory; prior evidence is retained'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
function Invoke-OwnedWsl([string[]]$Arguments){
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($argument in $Arguments){[void]$pi.ArgumentList.Add($argument)}
 $p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture command timed out'};$code=$p.ExitCode;$p.Dispose();if($code){throw ('Owned WSL fixture command failed: '+$e.Result)};return $o.Result
}
$linux='/tmp/openpilot-space-'+[guid]::NewGuid().ToString('N');$process=$null
$setup=@'
import os,sys
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-space-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700)
open(p+'/.owner','w').write('openpilot-space/v1')
p=p+'/data';os.mkdir(p)
os.mkdir(p+'/nested'); os.mkdir(p+'/empty')
open(p+'/one.txt','wb').write(b'abc')
open(p+'/nested/two.txt','wb').write(b'12345')
open(p+'/raw.bin','wb').write(b'123456789')
os.link(p+'/one.txt',p+'/alias.txt')
with open(p+'/sparse.dat','wb') as f: f.truncate(8*1024*1024)
open(p+'/a:b.txt','wb').close()
os.symlink('.',p+'/loop'); os.mkfifo(p+'/fifo')
p=sys.argv[1]+'/partial';os.mkdir(p);os.mkdir(p+'/denied');open(p+'/denied/secret','wb').write(b'secret');open(p+'/visible','wb').write(b'visible');os.chmod(p+'/denied',0)
p=sys.argv[1]+'/analysis';os.mkdir(p);os.mkdir(p+'/denied');open(p+'/denied/secret.txt','wb').write(b'secret');open(p+'/visible.txt','wb').write(b'visible')
base=sys.argv[1]+'/pages';os.mkdir(base);os.mkdir(base+'/child');os.mkdir(base+'/empty')
open(base+'/child/child.txt','wb').write(b'child')
for i in range(33017): open(base+('/p%05d.txt'%i),'wb').close()
open(base+'/'+'L'*240+'.txt','wb').close()
os.mkdir(sys.argv[1]+'/boundary')
for i in range(4096): open(sys.argv[1]+('/boundary/p%05d.txt'%i),'wb').close()
print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $distro=(Invoke-OwnedWsl @('--exec','python3','-c',$setup,$linux)).Trim();if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Owned Linux fixture setup failed'}
 $unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\data'
 $images=[ordered]@{};foreach($name in @('space_tests.exe','space_host_tests.exe','folderwing-space.exe','folderwing.exe','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $sources=[ordered]@{};foreach($name in @('app/spacewire.h','app/spaceproto.h','app/space.h','app/space.c','app/spacescan.c','app/fpspace.c','app/termutil.c','app/termutil.h','src/namecodec.h','linux/fpspace.c','linux/fpmount.h','linux/fpwsl.c','tests/space_tests.c','tests/space_analyzer.ps1','build.ps1','tests/native-impact.json')){$sources[$name]=(Get-FileHash (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
 foreach($selector in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/space_tests.c::') -or $_.Name.StartsWith('tests/space_host_tests.c::')}){foreach($name in $selector.Value.sources){$path=Join-Path $root $name;if(Test-Path $path -PathType Leaf){$sources[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 $nodeids=@($registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/space_tests.c::') -or $_.Name.StartsWith('tests/space_host_tests.c::')}|ForEach-Object Name)
 $os=Get-CimInstance Win32_OperatingSystem;$profile=[ordered]@{workers=1;pwsh=$PSVersionTable.PSVersion.ToString();os=[Environment]::OSVersion.VersionString;free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);cache='uncontrolled development host';containers='unchanged; unused';network='local WSL only';external_services='none'};$setupMs=$overall.Elapsed.TotalMilliseconds
 $input=[ordered]@{schema='openpilot-space-input/v1';nodeids=$nodeids;profile=$profile;runner='tests/space_analyzer.ps1';images=$images;sources=$sources;workers=1;desktop_input=$false;linux_fixture='owned sparse/hardlink/symlink/FIFO/literal-name tree and 33018-file / exact-4096-entry pagination fixtures; child access withdrawn between complete summary and detail analysis, restored in test and cleanup';full_suite=$false}
 $json=$input|ConvertTo-Json -Depth 8 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory 'space_tests.exe';$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['OPENPILOT_SPACE_WSL_FIXTURE']=$unc;$pi.Environment['OPENPILOT_SPACE_WSL_PARTIAL']='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\partial';$pi.Environment['OPENPILOT_SPACE_WSL_PAGES']='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\pages';$pi.Environment['OPENPILOT_SPACE_WSL_BOUNDARY']='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\boundary';$pi.Environment['OPENPILOT_SPACE_WSL_ANALYSIS']='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\analysis';$pi.Environment['OPENPILOT_SPACE_SCREENSHOT']=Join-Path $ResultDirectory 'space-panel.bmp'
 $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();$timedOut=-not $process.WaitForExit(120000);if($timedOut){$process.Kill($true);[void]$process.WaitForExit(5000)};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$stderr.Result)
 @{schema='openpilot-space-acceptance/v1';setup_ms=$setupMs;outcome=$(if($timedOut){'timed_out'}elseif($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false;timed_out=$timedOut}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
 Write-Output $stdout.Result;if($timedOut){throw 'Owned Space acceptance exceeded 120 s; output retained'};if($exit -ne 0){throw 'Space acceptance failed; evidence retained'}
 $left=Join-Path $ResultDirectory 'host-fixtures\left';$right=Join-Path $ResultDirectory 'host-fixtures\right';New-Item -ItemType Directory $left,$right|Out-Null
 [IO.File]::WriteAllText((Join-Path $left 'notes.md'),"# Space preview`nExact owned fixture`n");[IO.File]::WriteAllText((Join-Path $right 'other.txt'),'Another root')
 $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory 'space_host_tests.exe';$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;[void]$pi.ArgumentList.Add($left);[void]$pi.ArgumentList.Add($right)
 $watch.Restart();$process=[Diagnostics.Process]::Start($pi);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(-not $process.WaitForExit(60000)){$process.Kill($true);throw 'Owned Space host acceptance exceeded 60 s'};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'host-stdout.txt'),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'host-stderr.txt'),$stderr.Result)
 @{schema='openpilot-space-host-acceptance/v1';wall_ms=$overall.Elapsed.TotalMilliseconds;outcome=$(if($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'HOST.json')
 Write-Output $stdout.Result;if($exit -ne 0){throw 'Space host acceptance failed; evidence retained'}
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 # Remove only exact known entries under the fresh UUID root with its owner marker.
 $cleanup=@'
import os,sys
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-space-') and len(p.rsplit('-',1)[1])==32
if not os.path.isdir(p): sys.exit(0)
assert not os.path.islink(p)
assert open(p+'/.owner').read()=='openpilot-space/v1'
assert not os.path.islink(p+'/analysis') and not os.path.islink(p+'/analysis/denied')
os.chmod(p+'/analysis/denied',0o700)
os.unlink(p+'/analysis/denied/secret.txt');os.unlink(p+'/analysis/visible.txt');os.rmdir(p+'/analysis/denied');os.rmdir(p+'/analysis')
for n in ['one.txt','nested/two.txt','raw.bin','alias.txt','sparse.dat','a:b.txt','loop','fifo']: os.unlink(p+'/data/'+n)
for n in ['nested','empty']: os.rmdir(p+'/data/'+n)
os.rmdir(p+'/data')
for i in range(33017): os.unlink(p+('/pages/p%05d.txt'%i))
os.unlink(p+'/pages/'+'L'*240+'.txt');os.unlink(p+'/pages/child/child.txt')
for n in ['new-after-page.txt','cancel-after-page.txt']:
 try: os.unlink(p+'/pages/'+n)
 except FileNotFoundError: pass
for n in ['child','empty']: os.rmdir(p+'/pages/'+n)
os.rmdir(p+'/pages')
for i in range(4096): os.unlink(p+('/boundary/p%05d.txt'%i))
os.rmdir(p+'/boundary')
os.chmod(p+'/partial/denied',0o700);os.unlink(p+'/partial/denied/secret');os.unlink(p+'/partial/visible');os.rmdir(p+'/partial/denied');os.rmdir(p+'/partial');os.unlink(p+'/.owner');os.rmdir(p)
'@
 try{Invoke-OwnedWsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null}catch{Write-Warning 'Owned Linux fixture cleanup incomplete; preserved for review'}
}
