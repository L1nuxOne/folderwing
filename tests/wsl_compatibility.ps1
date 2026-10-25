<# Functional legacy-9P compatibility/refusal contract. No throughput thresholds. #>
param([string]$Config='release',[string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent;$exe=Join-Path $root "build\$Config\folderwing-cli.exe"
$out=if($EvidenceRoot){$EvidenceRoot}else{Join-Path $root "build\wsl_compatibility\$PID"};New-Item -ItemType Directory -Force $out|Out-Null
$nonce=[guid]::NewGuid().ToString('N');$local=Join-Path $env:TEMP "openpilot-compat-$nonce";$linux="/tmp/openpilot-compat-$nonce";$complete=$false;$made=$false;$script:checks=0;$rows=@()
function Check([bool]$ok,[string]$label){if(-not $ok){throw "FAIL $label"};$script:checks++;"PASS $label"}
function State([string]$path){[ordered]@{files=@(Get-ChildItem -LiteralPath $path -Recurse -Force|Sort-Object FullName|ForEach-Object{[ordered]@{name=$_.FullName.Substring($path.Length);directory=$_.PSIsContainer;sha256=$(if(-not $_.PSIsContainer){(Get-FileHash -LiteralPath $_.FullName).Hash}else{''})}})}|ConvertTo-Json -Depth 5 -Compress}
function Payload([string]$path){@(Get-ChildItem -LiteralPath $path -File|Sort-Object Name|ForEach-Object{[ordered]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})|ConvertTo-Json -Compress}
function Run([string]$name,[string[]]$arguments){
 $start=[Diagnostics.ProcessStartInfo]::new($exe);$start.UseShellExecute=$false;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($arg in $arguments){$start.ArgumentList.Add($arg)}
 $p=[Diagnostics.Process]::Start($start);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
 try{if(-not $p.WaitForExit(60000)){$p.Kill();$p.WaitForExit();throw "CLI deadline: $name"}
  $text=$stdout.GetAwaiter().GetResult();[IO.File]::WriteAllText("$out\$name.log",$text);[IO.File]::WriteAllText("$out\$name.stderr.log",$stderr.GetAwaiter().GetResult())
  @{name=$name;args=$arguments;exit=$p.ExitCode;text=$text}
 }finally{$p.Dispose()}
}
try{
 Check (-not(Test-Path $local)) 'new Windows fixture root'
 New-Item -ItemType Directory $local,"$local\source"|Out-Null;[IO.File]::WriteAllText("$local\.openpilot-owner",$nonce)
 $distro=((& wsl.exe --exec sh -c 'printf %s "$WSL_DISTRO_NAME"')-join'').Trim();Check ($LASTEXITCODE -eq 0 -and $distro -match '^[A-Za-z0-9._-]+$') 'usable default distro'
 $script="from pathlib import Path;p=Path('$linux');p.mkdir();(p/'.openpilot-owner').write_text('$nonce');(p/'source').mkdir();[(p/'source'/('file_%02d.txt'%i)).write_text('payload-%d'%i) for i in range(16)]"
 & wsl.exe --exec python3 -c $script;Check ($LASTEXITCODE -eq 0) 'new Linux fixture root';$made=$true
 $base="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
 for($i=0;$i -lt 16;$i++){[IO.File]::WriteAllText((Join-Path "$local\source" ('file_{0:d2}.txt' -f $i)),"payload-$i")}
 $index=0
 foreach($alias in @('wsl.localhost','wsl$')){
  $unc="\\$alias\$distro$($linux.Replace('/','\'))"
  foreach($case in @(
   @{op='cp';src="$local\source";destLinux=$true},@{op='cp';src="$unc\source";destLinux=$true},
   @{op='cp';src="$local\source\file_00.txt";destLinux=$true},@{op='cp';src="$unc\source\file_00.txt";destLinux=$true},
   @{op='mv';src="$local\source";destLinux=$true},@{op='mv';src="$unc\source";destLinux=$false},
   @{op='mv';src="$unc\source\file_00.txt";destLinux=$false},@{op='mv';src="$local\source\file_00.txt";destLinux=$true}
  )){
   $index++;$dest=if($case.destLinux){"$unc\refuse-$index"}else{"$local\refuse-$index"}
   New-Item -ItemType Directory $dest|Out-Null
   [IO.File]::WriteAllText("$dest\sentinel",'old destination bytes')
   if(Test-Path $case.src -PathType Leaf){[IO.File]::WriteAllText((Join-Path $dest (Split-Path $case.src -Leaf)),'old overwrite bytes')}
   $beforeSource=State "$local\source";$beforeLinux=State "$base\source";$beforeDest=State $dest
   $result=Run "refuse-$index" @($case.op,$case.src,$dest,'--overwrite','--9p');$rows+=,$result
   Check ($result.exit -eq 1 -and $result.text -match 'failed' -and $result.text -match '\(50\)' -and $result.text -match 'Transfer refused before mutation; use native WSL transfers') "9P $index explicit unsupported outcome"
   Check ((State "$local\source") -ceq $beforeSource -and (State "$base\source") -ceq $beforeLinux) "9P $index both source trees retained"
   Check ((State $dest) -ceq $beforeDest) "9P $index destination and namespace unchanged"
  }
 }
 foreach($case in @(
  @{name='native-linux-linux';src="$base\source";dst="$base\native-ll";plain=$false},
  @{name='native-windows-linux';src="$local\source";dst="$base\native-wl";plain=$false},
  @{name='native-linux-windows';src="$base\source";dst="$local\native-lw";plain=$false},
  @{name='9p-linux-windows-copy';src="$base\source";dst="$local\legacy-lw";plain=$true},
  @{name='9p-windows-windows-copy';src="$local\source";dst="$local\legacy-ww";plain=$true}
 )){
  New-Item -ItemType Directory $case.dst|Out-Null;$arguments=@('cp',$case.src,$case.dst);if($case.plain){$arguments+='--9p'}
  $before=State $case.src;$expected=Payload $case.src;$result=Run $case.name $arguments;$rows+=,$result
  Check ($result.exit -eq 0) "$($case.name) succeeds"
  Check ((Payload "$($case.dst)\source") -ceq $expected -and (State $case.src) -ceq $before) "$($case.name) exact bytes and source retained"
 }
 $rows|ConvertTo-Json -Depth 5|Set-Content "$out\results.json";$complete=$true
 "$script:checks checks passed; no skips, no retries"
}finally{
 if($complete){
  if([IO.File]::ReadAllText("$local\.openpilot-owner") -cne $nonce -or [IO.File]::ReadAllText("$base\.openpilot-owner") -cne $nonce){throw 'Fixture ownership changed; preserve for review'}
  Remove-Item -LiteralPath $local -Recurse -Force
  & wsl.exe --exec python3 -c "from pathlib import Path;import shutil;p=Path('$linux');assert (p/'.openpilot-owner').read_text()=='$nonce';shutil.rmtree(p)"
  if($LASTEXITCODE){throw 'Owned Linux fixture cleanup failed'}
 }else{"RETAIN failed fixtures Windows=$local Linux=$linux created=$made"}
}
