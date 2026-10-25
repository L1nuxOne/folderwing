<# Focused workspace acceptance. Private catalog, owned windows/fixtures, no physical input. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\workspaces\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh result directory; preserve earlier evidence'};New-Item -ItemType Directory $ResultDirectory|Out-Null
function Invoke-OwnedWsl([string[]]$Arguments){$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)};$p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture command timed out'};$exit=$p.ExitCode;$p.Dispose();if($exit){throw ('Owned WSL fixture failed: '+$e.Result)};return $o.Result}
$linux='/tmp/openpilot-workspace-'+[guid]::NewGuid().ToString('N');$process=$null
$setup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-workspace-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700);open(p+'/.owner','w').write('openpilot-workspace/v1');os.mkdir(p+'/data');open(p+'/data/notes.txt','w').write('Owned native Linux workspace fixture\n');print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $distro=(Invoke-OwnedWsl @('--exec','python3','-c',$setup,$linux)).Trim();if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Unexpected distro identity'};$unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')+'\data'
 $left=Join-Path $ResultDirectory 'host-fixtures\left';$right=Join-Path $ResultDirectory 'host-fixtures\right';New-Item -ItemType Directory $left,$right|Out-Null
 [IO.File]::WriteAllText((Join-Path $left 'left.txt'),"Owned left`n");[IO.File]::WriteAllText((Join-Path $right 'right.txt'),"Owned right`n")
 $session=Join-Path $ResultDirectory 'host-fixtures\test-session.ini'
 $images=[ordered]@{};foreach($name in @('workspace_tests.exe','workspace_host_tests.exe','folderwing.exe','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $sources=[ordered]@{};$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
 foreach($selector in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/workspace_tests.c::') -or $_.Name.StartsWith('tests/workspace_host_tests.c::')}){foreach($name in $selector.Value.sources){$path=Join-Path $root $name;if(Test-Path $path -PathType Leaf){$sources[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 if($sources.Count -eq 0){throw 'Workspace selectors must be impact-bound before acceptance'}
 $input=[ordered]@{schema='openpilot-workspaces-input/v1';images=$images;sources=$sources;workers=1;desktop_input=$false;external_clipboard=$false;fixture='private Windows catalog and folders, native Linux folder; explicit persistence, restore, failures, conflict';full_suite=$false};$json=$input|ConvertTo-Json -Depth 10 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 foreach($module in @('workspace_tests.exe','workspace_host_tests.exe')){
  $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory $module));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;$pi.Environment['OPENPILOT_WORKSPACE_WSL']=$unc
  if($module -eq 'workspace_host_tests.exe'){foreach($arg in @($left,$right,$session)){[void]$pi.ArgumentList.Add($arg)}}
  $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync(); $timedOut=-not $process.WaitForExit(90000);if($timedOut){$process.Kill($true);$process.WaitForExit()};$watch.Stop();$exit=$(if($timedOut){124}else{$process.ExitCode});$process.Dispose();$process=$null
  [IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stdout.txt')),$out.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stderr.txt')),$err.Result)
  @{schema='openpilot-workspaces-acceptance/v1';module=$module;outcome=$(if($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;external_clipboard=$false;user_catalog_changed=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory ($module+'-RESULT.json'))
  Write-Output $out.Result;if($exit){throw "Workspace acceptance failed in $module; evidence retained"}
 }
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 $cleanup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-workspace-') and len(p.rsplit('-',1)[1])==32
if not os.path.isdir(p):sys.exit(0)
assert not os.path.islink(p) and open(p+'/.owner').read()=='openpilot-workspace/v1'
os.unlink(p+'/data/notes.txt');os.rmdir(p+'/data');os.unlink(p+'/.owner');os.rmdir(p)
'@
 try{Invoke-OwnedWsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null}catch{Write-Warning 'Owned Linux cleanup incomplete; retained for review'}
}
