<# Focused main Linux tree acceptance. Private registry, owned windows and Linux folders. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\wsl_tree\'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh evidence directory'};New-Item -ItemType Directory $ResultDirectory|Out-Null
function Owned-Wsl([string[]]$Arguments){$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)};$p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture timed out'};$code=$p.ExitCode;$p.Dispose();if($code){throw $e.Result};return $o.Result}
$linux='/tmp/openpilot-tree-'+[guid]::NewGuid().ToString('N');$registry='Software\OpenPilot\Tests\Tree'+[guid]::NewGuid().ToString('N');$process=$null;$created=$false
$setup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-tree-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700);open(p+'/.owner','w').write('openpilot-tree/v1');os.mkdir(p+'/child');os.mkdir(p+'/Child');open(p+'/notes.txt','w').write('Owned tree navigation fixture\n');print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($registry);$key.SetValue('Owner','openpilot-tree/v1');$key.Dispose()
 $distro=(Owned-Wsl @('--exec','python3','-c',$setup,$linux)).Trim();$created=$true;if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Invalid distro identity'};$unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')
 $left=Join-Path $ResultDirectory 'fixtures\left';$right=Join-Path $ResultDirectory 'fixtures\right';New-Item -ItemType Directory $left,$right|Out-Null;[IO.File]::WriteAllText((Join-Path $left 'left.txt'),'Owned left');[IO.File]::WriteAllText((Join-Path $right 'right.txt'),'Owned right')
 $images=[ordered]@{};foreach($name in @('wsl_tree_host_tests.exe','folderwing.exe','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $bindings=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json;$sources=[ordered]@{};$nodeids=@()
 foreach($s in $bindings.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/wsl_tree_host_tests.c::')}){$nodeids+=$s.Name;foreach($file in $s.Value.sources){$sources[$file]=(Get-FileHash (Join-Path $root $file) -Algorithm SHA256).Hash.ToLowerInvariant()}}
 if($nodeids.Count -ne 3){throw 'New tree selectors must be impact-bound'}
 $os=Get-CimInstance Win32_OperatingSystem;$profile=[ordered]@{workers=1;os=[Environment]::OSVersion.VersionString;pwsh=$PSVersionTable.PSVersion.ToString();free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);cache='uncontrolled development host';network='local WSL only';containers='unchanged; unused';external_services='none'};$setupMs=$overall.Elapsed.TotalMilliseconds
 $input=[ordered]@{schema='openpilot-wsl-tree-input/v1';nodeids=$nodeids;images=$images;sources=$sources;profile=$profile;runner='tests/wsl_tree.ps1';workers=1;physical_input=$false;full_suite=$false;fixture='private registry catalog including WSL1/2 and invalid registrations; real existing distro root and owned case-sensitive children';user_distribution_registry_changed=$false}
 $json=$input|ConvertTo-Json -Depth 10 -Compress;[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json);$id=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
 $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory 'wsl_tree_host_tests.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['OPENPILOT_TREE_WSL']=$unc;foreach($a in @($left,$right,$registry)){[void]$pi.ArgumentList.Add($a)}
 $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$o=$process.StandardOutput.ReadToEndAsync();$e=$process.StandardError.ReadToEndAsync();$timeout=-not $process.WaitForExit(90000);if($timeout){$process.Kill($true);$process.WaitForExit()};$watch.Stop();$code=$(if($timeout){124}else{$process.ExitCode});$process.Dispose();$process=$null
 [IO.File]::WriteAllText((Join-Path $ResultDirectory 'stdout.txt'),$o.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory 'stderr.txt'),$e.Result)
 @{outcome=$(if($code){'failed'}else{'passed'});exit_code=$code;timed_out=$timeout;input_key=$id;elapsed_ms=$watch.Elapsed.TotalMilliseconds;setup_ms=$setupMs;wall_ms=$overall.Elapsed.TotalMilliseconds;wsl_stopped=$false;docker_stopped=$false;user_distribution_registry_changed=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json');Write-Output $o.Result;if($code){throw 'Linux tree acceptance failed; evidence retained'}
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 if($created){$cleanup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-tree-') and len(p.rsplit('-',1)[1])==32
assert not os.path.islink(p) and open(p+'/.owner').read()=='openpilot-tree/v1'
assert set(os.listdir(p))=={'.owner','notes.txt','child','Child'}
os.unlink(p+'/notes.txt');os.rmdir(p+'/child');os.rmdir(p+'/Child');os.unlink(p+'/.owner');os.rmdir(p)
'@
 Owned-Wsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null}
 if($registry -notmatch '^Software\\OpenPilot\\Tests\\Tree[0-9a-f]{32}$'){throw 'Invalid private registry cleanup target'}
 $key=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($registry);if($key){if($key.GetValue('Owner') -ne 'openpilot-tree/v1'){$key.Dispose();throw 'Foreign registry fixture retained'};foreach($name in $key.GetSubKeyNames()){if($name -notmatch '^00[1-7]$'){$key.Dispose();throw 'Unknown registry child retained'}};$key.Dispose();[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($registry)}
 @{outcome='passed';owned_linux_fixture_removed=$created;private_registry_removed=$true;user_distribution_registry_changed=$false;wsl_stopped=$false;docker_stopped=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLEANUP.json')
}
