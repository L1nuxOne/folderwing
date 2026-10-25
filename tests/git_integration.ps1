<# Focused read-only Git acceptance. Owned fixtures/windows only; no physical input. #>
param([string]$BuildDirectory,[string]$ResultDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$overall=[Diagnostics.Stopwatch]::StartNew()
if(-not $BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
if(-not $ResultDirectory){$ResultDirectory=Join-Path $root ('build\git_integration\'+[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path $ResultDirectory){throw 'Use a fresh evidence directory'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
function Invoke-Owned([string]$Exe,[string[]]$Arguments,[int[]]$Accepted=@(0)){
 $pi=[Diagnostics.ProcessStartInfo]::new($Exe);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)}
 $p=[Diagnostics.Process]::Start($pi);$o=$p.StandardOutput.ReadToEndAsync();$e=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(30000)){$p.Kill($true);throw 'Owned fixture command timed out'};$code=$p.ExitCode;$p.Dispose();if($code -notin $Accepted){throw ('Owned fixture command failed: '+$e.Result)};return $o.Result
}
$wsl=Join-Path $env:SystemRoot 'System32\wsl.exe';$git=Join-Path $env:ProgramFiles 'Git\cmd\git.exe'
$linux='/tmp/openpilot-git-'+[guid]::NewGuid().ToString('N');$process=$null;$key=$null;$setupMs=0
$setup=@'
import os,sys,subprocess,hashlib
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-git-') and len(p.rsplit('-',1)[1])==32
os.mkdir(p,0o700);open(p+'/.owner','w').write('openpilot-git/v1');os.mkdir(p+'/hooks')
env={k:v for k,v in os.environ.items() if not k.startswith('GIT_')}
def git(root,*args,ok=(0,)):
 r=subprocess.run(['git','-c','core.hooksPath='+p+'/hooks','-c','commit.gpgsign=false','-c','user.name=OpenPilot fixture','-c','user.email=fixture@example.invalid','-C',root,*args],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 assert r.returncode in ok,r.stderr
 return r.stdout
r=p+'/repo';os.mkdir(r);git(r,'init','-b','main');git(r,'config','core.autocrlf','false');os.mkdir(r+'/nested')
for name,data in [('both.md',b'base\n'),('rename-old.txt',b'rename\n'),('deleted.txt',b'deleted\n'),('empty-deleted.txt',b''),('binary.bin',b'\x00\x01\x02'),('nested/clean.txt',b'clean'),('.gitignore',b'*.log\n')]:open(r+'/'+name,'wb').write(data)
git(r,'add','.');git(r,'commit','-m','base');git(r,'worktree','add','-b','linked',p+'/linked')
open(r+'/both.md','wb').write(b'staged\n');git(r,'add','both.md');open(r+'/both.md','wb').write(b'working\n');git(r,'mv','rename-old.txt','renamed.txt');os.unlink(r+'/deleted.txt');os.unlink(r+'/empty-deleted.txt');open(r+'/binary.bin','wb').write(b'\x00\x01\x03');open(r+'/fresh space #%.txt','wb').write(b'fresh');open(r+'/nested/new.txt','wb').write(b'new');open(r+'/ignored.log','wb').write(b'ignored');open(r+'/a:b.txt','wb').write(b'literal');open(os.fsencode(r)+b'/raw-\xff','wb').write(b'raw')
# Native file-type guards are exercised independently of Git tracking.
os.mkfifo(r+'/fifo');os.symlink('/etc/passwd',r+'/link')
u=p+'/initial';os.mkdir(u);git(u,'init','-b','main');open(u+'/new.txt','wb').write(b'new')
c=p+'/conflict';os.mkdir(c);git(c,'init','-b','main');open(c+'/conflict.txt','wb').write(b'base\n');git(c,'add','.');git(c,'commit','-m','base');git(c,'checkout','-b','side');open(c+'/conflict.txt','wb').write(b'side\n');git(c,'commit','-am','side');git(c,'checkout','main');open(c+'/conflict.txt','wb').write(b'main\n');git(c,'commit','-am','main');git(c,'merge','side',ok=(1,))
os.mkdir(p+'/diff');open(p+'/diff/left.txt','wb').write(b'a\nold\nend\n');open(p+'/diff/right.txt','wb').write(b'a\nnew\nend\n')
open(p+'/.index-before','w').write(hashlib.sha256(open(r+'/.git/index','rb').read()).hexdigest())
print(os.environ['WSL_DISTRO_NAME'])
'@
try{
 $distro=(Invoke-Owned $wsl @('--exec','python3','-c',$setup,$linux)).Trim();if($distro -notmatch '^[A-Za-z0-9._-]+$'){throw 'Fixture distro invalid'}
 $unc='\\wsl.localhost\'+$distro+$linux.Replace('/','\')
 $windows=Join-Path $ResultDirectory 'fixtures';$repo=Join-Path $windows 'repo';$other=Join-Path $windows 'other';$hooks=Join-Path $windows 'hooks';New-Item -ItemType Directory $repo,$other,$hooks|Out-Null
 function FixtureGit([string[]]$Arguments){Invoke-Owned $git (@('-c',('core.hooksPath='+$hooks),'-c','commit.gpgsign=false','-c','user.name=OpenPilot fixture','-c','user.email=fixture@example.invalid','-C',$repo)+$Arguments)|Out-Null}
 FixtureGit @('init','-b','main');FixtureGit @('config','core.autocrlf','false');New-Item -ItemType Directory (Join-Path $repo 'nested')|Out-Null
 foreach($item in @(@('both.md',"base`n"),@('rename-old.txt',"rename`n"),@('deleted.txt',"deleted`n"),@('empty-deleted.txt',''),@('nested\clean.txt','clean'),@('.gitignore',"*.log`n"))){[IO.File]::WriteAllText((Join-Path $repo $item[0]),$item[1])}
 [IO.File]::WriteAllBytes((Join-Path $repo 'binary.bin'),[byte[]]@(0,1,2));FixtureGit @('add','.');FixtureGit @('commit','-m','base');FixtureGit @('branch','upstream');FixtureGit @('config','branch.main.remote','.');FixtureGit @('config','branch.main.merge','refs/heads/upstream')
 [IO.File]::WriteAllText((Join-Path $repo 'both.md'),"staged`n");FixtureGit @('add','both.md');[IO.File]::WriteAllText((Join-Path $repo 'both.md'),"working`n");FixtureGit @('mv','rename-old.txt','renamed.txt');[IO.File]::Delete((Join-Path $repo 'deleted.txt'));[IO.File]::Delete((Join-Path $repo 'empty-deleted.txt'));[IO.File]::WriteAllBytes((Join-Path $repo 'binary.bin'),[byte[]]@(0,1,3))
 foreach($name in @('fresh space #%.txt','你好.txt','nested\new.txt','ignored.log')){[IO.File]::WriteAllText((Join-Path $repo $name),'fresh')}
 $indexBefore=(Get-FileHash (Join-Path $repo '.git\index') -Algorithm SHA256).Hash
 $registry=Get-Content (Join-Path $PSScriptRoot 'native-impact.json') -Raw|ConvertFrom-Json;$sources=[ordered]@{}
 foreach($name in @('app/gitwire.h','app/gitproto.h','app/gitmodel.h','app/gitmodel.c','app/gitread.h','app/gitread.c','app/fpgit.c','app/fpdiff.c','app/fpdemo.c','linux/fpgit.c','linux/fpwsl.c','tests/git_tests.c','tests/git_host_tests.c','tests/git_integration.ps1','tools/build-git.py','build.ps1','tests/native-impact.json')){$sources[$name]=(Get-FileHash (Join-Path $root $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $selectors=@($registry.selectors.psobject.Properties|Where-Object {$_.Name.StartsWith('tests/git_tests.c::') -or $_.Name.StartsWith('tests/git_host_tests.c::') -or $_.Name.StartsWith('tests/diff_tests.c::')});foreach($selector in $selectors){foreach($name in $selector.Value.sources){$path=Join-Path $root $name;if(Test-Path $path -PathType Leaf){$sources[$name]=(Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant()}}}
 $images=[ordered]@{};foreach($name in @('git_tests.exe','git_host_tests.exe','diff_tests.exe','folderwing-git.exe','folderwing-diff.exe','folderwing.exe','folderwing-wsl','folderwing-terminal.exe')){$images[$name]=(Get-FileHash (Join-Path $BuildDirectory $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
 $os=Get-CimInstance Win32_OperatingSystem;$profile=[ordered]@{workers=1;pwsh=$PSVersionTable.PSVersion.ToString();os=[Environment]::OSVersion.VersionString;free_memory_kib=$os.FreePhysicalMemory;cpu_load=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);cache='uncontrolled development host';containers='unchanged; unused';network='local WSL only';external_services='none'};$setupMs=$overall.Elapsed.TotalMilliseconds
 $input=[ordered]@{schema='openpilot-git-input/v1';nodeids=@($selectors|ForEach-Object Name);sources=$sources;images=$images;profile=$profile;runner='tests/git_integration.ps1';fixtures='owned Windows/native Linux repositories; edits, rename, captured deletions followed by identical/different/empty-index/type-change/over-limit recreations, binary, conflicts, unborn, linked worktree, raw names and diff regression';full_suite=$false}
 $json=$input|ConvertTo-Json -Depth 8 -Compress;$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant();[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$json)
 foreach($lane in @('git_tests','git_host_tests','diff_tests')){
  $pi=[Diagnostics.ProcessStartInfo]::new((Join-Path $BuildDirectory ($lane+'.exe')));$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
  $pi.Environment['OPENPILOT_GIT_WORKTREE']=$unc+'\linked';$pi.Environment['OPENPILOT_GIT_INITIAL']=$unc+'\initial';$pi.Environment['OPENPILOT_GIT_SCREENSHOT']=Join-Path $ResultDirectory 'git-panel.bmp';$pi.Environment['OPENPILOT_DIFF_WSL_FIXTURE']=$unc+'\diff'
  if($lane -eq 'git_tests'){foreach($arg in @($repo,($unc+'\repo'),($unc+'\conflict'))){[void]$pi.ArgumentList.Add($arg)}}elseif($lane -eq 'git_host_tests'){[void]$pi.ArgumentList.Add($repo);[void]$pi.ArgumentList.Add($other)}
  $watch=[Diagnostics.Stopwatch]::StartNew();$process=[Diagnostics.Process]::Start($pi);$o=$process.StandardOutput.ReadToEndAsync();$e=$process.StandardError.ReadToEndAsync();$timedOut=-not $process.WaitForExit(180000);if($timedOut){$process.Kill($true);[void]$process.WaitForExit(5000)};$watch.Stop();$exit=$process.ExitCode;$process.Dispose();$process=$null
  [IO.File]::WriteAllText((Join-Path $ResultDirectory ($lane+'-stdout.txt')),$o.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($lane+'-stderr.txt')),$e.Result)
  @{outcome=$(if($timedOut){'timed_out'}elseif($exit -eq 0){'passed'}else{'failed'});exit_code=$exit;input_key=$key;elapsed_ms=$watch.Elapsed.TotalMilliseconds;workers=1;physical_input=$false;full_suite=$false;timed_out=$timedOut}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory ($lane+'.json'))
  Write-Output $o.Result;if($timedOut){throw ('Owned Git lane timed out; output retained: '+$lane)};if($exit){throw ('Git lane failed: '+$lane)}
 }
 if((Get-FileHash (Join-Path $repo '.git\index') -Algorithm SHA256).Hash -ne $indexBefore){throw 'Read-only Git feature changed owned Windows index'}
 $check=@'
import os,sys,hashlib
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-git-') and open(p+'/.owner').read()=='openpilot-git/v1'
assert hashlib.sha256(open(p+'/repo/.git/index','rb').read()).hexdigest()==open(p+'/.index-before').read()
print('Native index unchanged')
'@
 Invoke-Owned $wsl @('--exec','python3','-c',$check,$linux)|Out-Null
 @{outcome='passed';windows_index_unchanged=$true;linux_index_unchanged=$true}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'READ_ONLY.json')
}finally{
 if($process){if(-not $process.HasExited){$process.Kill($true)};$process.Dispose()}
 $cleanup=@'
import os,sys,shutil,stat
p=sys.argv[1]
assert p.startswith('/tmp/openpilot-git-') and len(p.rsplit('-',1)[1])==32 and os.path.dirname(p)=='/tmp'
if not os.path.lexists(p):sys.exit(0)
s=os.lstat(p);assert stat.S_ISDIR(s.st_mode) and s.st_uid==os.getuid()
assert open(p+'/.owner').read()=='openpilot-git/v1'
assert shutil.rmtree.avoids_symlink_attacks
shutil.rmtree(p)
'@
 Invoke-Owned $wsl @('--exec','python3','-c',$cleanup,$linux)|Out-Null
 @{outcome='passed';owned_linux_fixture_removed=$true;setup_ms=$setupMs;wall_ms=$overall.Elapsed.TotalMilliseconds;wsl_stopped=$false;docker_stopped=$false}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'CLEANUP.json')
}
