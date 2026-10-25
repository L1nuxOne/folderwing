<# Focused content search acceptance. Owned windows/fixtures, no physical input or WSL stop. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\content_search\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh result directory; preserve earlier evidence'};New-Item -ItemType Directory $ResultDirectory|Out-Null
function Invoke-OwnedWsl([string[]]$Arguments){$pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $env:SystemRoot 'System32\wsl.exe'));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)};$p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned WSL fixture command timed out'};$exit=$p.ExitCode;$p.Dispose();if($exit){throw ('Owned WSL fixture failed: '+$e.Result)};return $o.Result}
$linux='/tmp/openpilot-content-'+[guid]::NewGuid().ToString('N');$process=$null
$setup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-content-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700);open(p+'/.owner','w').write('openpilot-content/v1')
b=p+'/data';os.mkdir(b);os.mkdir(b+'/nested')
open(b+'/notes.txt','wb').write('intro\r\nAlpha beta alpha\r\nalphabet alpha_ ALPHA\r\nunicode café 😀\r\n'.encode('utf-8'))
open(b+'/nested/code.py','wb').write(b'alpha\nfinal alpha\n')
open(b+'/readme.custom','wb').write(b'some alpha content')
open(b+'/utf16.txt','wb').write('one\r\nALPHA beta\r\n'.encode('utf-16'))
open(b+'/a:b.txt','wb').write(b'alpha')
open(b+'/binary.bin','wb').write(bytes([0,1,0,0,255,4]))
with open(b+'/large.bin','wb') as f: f.truncate(2*1024*1024+1)
os.symlink('.',b+'/loop');os.mkfifo(b+'/fifo')
b=p+'/partial';os.mkdir(b);os.mkdir(b+'/denied');open(b+'/denied/secret.txt','w').write('alpha');open(b+'/visible.txt','w').write('alpha visible');os.chmod(b+'/denied',0)
b=p+'/preview';os.mkdir(b);open(b+'/notes.md','w').write('first line\na needle found\n')
print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $distro=(Invoke-OwnedWsl @('--exec','python3','-c',$setup,$linux)).Trim();if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Unexpected distro identity'};$unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')
 $left=Join-Path $ResultDirectory 'host-fixtures\left';$right=Join-Path $ResultDirectory 'host-fixtures\right';New-Item -ItemType Directory $left,$right|Out-Null
 $lines=@();for($i=1;$i -le 100;$i++){$lines+= $(if($i -eq 80){'match needle here'}else{"ordinary line $i"})};[IO.File]::WriteAllText((Join-Path $left 'notes.md'),($lines -join "`n"));[IO.File]::WriteAllText((Join-Path $right 'other.txt'),'Another folder')
 $images=[ordered]@{};foreach($name in @('content_tests.exe','content_host_tests.exe','folderwing-content.exe','folderwing.exe','folderwing-text.exe','folderwing-wsl')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $sources=[ordered]@{};$registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json
 foreach($selector in $registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/content_tests.c::') -or $_.Name.StartsWith('tests/content_host_tests.c::')}){foreach($name in $selector.Value.sources){$path=Join-Path $root $name;if(Test-Path $path -PathType Leaf){$sources[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 $input=[ordered]@{schema='openpilot-content-input/v1';images=$images;sources=$sources;workers=1;desktop_input=$false;external_clipboard=$false;fixture='owned UTF-8/UTF-16/binary/oversized/literal-name/denied tree';full_suite=$false};$json=$input|ConvertTo-Json -Depth 10 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 foreach($module in @('content_tests.exe','content_host_tests.exe')){
  $pi=[Diagnostics.ProcessStartInfo]::new();$pi.FileName=Join-Path $BuildDirectory $module;$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
  $pi.Environment['OPENPILOT_CONTENT_WSL_FIXTURE']=$unc+'\data';$pi.Environment['OPENPILOT_CONTENT_WSL_PARTIAL']=$unc+'\partial';$pi.Environment['OPENPILOT_CONTENT_WSL_PREVIEW']=$unc+'\preview';$pi.Environment['OPENPILOT_CONTENT_SCREENSHOT']=Join-Path $ResultDirectory 'content-panel.bmp'
  if($module -eq 'content_host_tests.exe'){[void]$pi.ArgumentList.Add($left);[void]$pi.ArgumentList.Add($right)}
  $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$out=$process.StandardOutput.ReadToEndAsync();$err=$process.StandardError.ReadToEndAsync();if(-not $process.WaitForExit(90000)){$process.Kill($true);throw 'Owned content acceptance exceeded 90 s'};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
  [IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stdout.txt')),$out.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($module+'-stderr.txt')),$err.Result)
  @{schema='openpilot-content-acceptance/v1';module=$module;outcome=$(if($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;external_clipboard=$false;wsl_stopped=$false;docker_stopped=$false;full_suite=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory ($module+'-RESULT.json'))
  Write-Output $out.Result;if($exit){throw "Content acceptance failed in $module; evidence retained"}
 }
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 $cleanup=@'
import os,sys
p=sys.argv[1];assert p.startswith('/tmp/openpilot-content-') and len(p.rsplit('-',1)[1])==32
if not os.path.isdir(p):sys.exit(0)
assert not os.path.islink(p) and open(p+'/.owner').read()=='openpilot-content/v1'
for n in ['notes.txt','nested/code.py','readme.custom','utf16.txt','a:b.txt','binary.bin','large.bin','loop','fifo']:os.unlink(p+'/data/'+n)
os.rmdir(p+'/data/nested');os.rmdir(p+'/data')
os.chmod(p+'/partial/denied',0o700);os.unlink(p+'/partial/denied/secret.txt');os.unlink(p+'/partial/visible.txt');os.rmdir(p+'/partial/denied');os.rmdir(p+'/partial')
os.unlink(p+'/preview/notes.md');os.rmdir(p+'/preview');os.unlink(p+'/.owner');os.rmdir(p)
'@
 try{Invoke-OwnedWsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null}catch{Write-Warning 'Owned Linux cleanup incomplete; retained for review'}
}
