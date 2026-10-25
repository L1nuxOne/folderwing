<# Actual Windows writers terminated without cleanup at commit boundaries.
   Private fixtures only; requires a second NTFS volume for source-removal tests.
   Process termination verifies visible state, not device/VM power-loss behavior. #>
param([string]$OtherRoot='D:\',[string]$Config='release')
$ErrorActionPreference='Stop'
$env:SystemRoot='C:\Windows';$env:windir='C:\Windows';$env:SystemDrive='C:';$env:ComSpec='C:\Windows\System32\cmd.exe';$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
$env:ZIG_GLOBAL_CACHE_DIR='C:\Work\openpilot\build\f05_mounts\zig-cache'
$root=Split-Path $PSScriptRoot -Parent
. "$root\tools\build-output.ps1"
$nonce=[guid]::NewGuid().ToString('N')
$local=Join-Path $env:TEMP "openpilot_crash_$nonce"
$other=Join-Path $OtherRoot "openpilot_crash_$nonce"
$made=@();$script:checks=0;$complete=$false
$clock=[Diagnostics.Stopwatch]::StartNew();$setupMs=0;$callMs=0
function Check([bool]$ok,[string]$label){if(-not $ok){throw "FAIL $label"};$script:checks++;Write-Output "PASS $label"}
function Put([string]$path,[string]$text){[IO.File]::WriteAllText($path,$text,[Text.UTF8Encoding]::new($false))}
function Content([string]$path,[string]$text){return (Test-Path -LiteralPath $path -PathType Leaf) -and [IO.File]::ReadAllText($path) -ceq $text}
function RunCrash([string]$image,[string[]]$arguments){
 $start=[Diagnostics.ProcessStartInfo]::new($image);$start.UseShellExecute=$false
 $start.RedirectStandardError=$true
 foreach($item in $arguments){$start.ArgumentList.Add($item)}
 $process=[Diagnostics.Process]::Start($start)
 $diagnostics=$process.StandardError.ReadToEndAsync()
 try{
  if(-not $process.WaitForExit(30000)){$process.Kill();$process.WaitForExit();throw 'Crash boundary not reached within 30s'}
  $detail=$diagnostics.GetAwaiter().GetResult()
  Write-Output "CRASH boundary=$($arguments[0]) exit=$($process.ExitCode)"
  if($detail){Write-Output $detail.TrimEnd()}
  Check ($process.ExitCode -eq 86) "uncatchable exit at $($arguments[0]) (actual exit $($process.ExitCode))"
 }finally{$process.Dispose()}
}
function ReviewStages([string]$folder,[int]$expected){
 $wsl=Join-Path ([Environment]::GetFolderPath('System')) 'wsl.exe'
 $tool=(& $wsl --exec wslpath -a -u "$root\tools\staging-review.py"|Out-String).Trim()
 if($LASTEXITCODE -ne 0){throw 'Cannot resolve review tool in WSL'}
 $path=(& $wsl --exec wslpath -a -u $folder|Out-String).Trim()
 if($LASTEXITCODE -ne 0){throw 'Cannot resolve private fixture in WSL'}
 $before=@(Get-ChildItem -LiteralPath $folder -Force -File|Get-FileHash|Sort-Object Path|Select-Object Path,Hash)|ConvertTo-Json -Compress
 $text=(& $wsl --exec python3 $tool $path|Out-String)
 Check ($LASTEXITCODE -eq 0) 'review succeeds on Windows-mounted crash fixtures'
 $report=$text|ConvertFrom-Json
 Check ($report.candidates.Count -eq $expected -and -not $report.errors.Count -and -not $report.truncated) 'review finds exact candidate count'
 Check ($report.ownership -eq 'unverified' -and $report.activity -eq 'unknown' -and $report.policy -eq 'keep-for-review') 'review preserves uncertainty'
 $after=@(Get-ChildItem -LiteralPath $folder -Force -File|Get-FileHash|Sort-Object Path|Select-Object Path,Hash)|ConvertTo-Json -Compress
 Check ($before -ceq $after) 'review preserves all file names and content'
}
try{
 foreach($path in @($local,$other)){
  if(Test-Path -LiteralPath $path){throw 'Refusing to reuse a fixture directory'}
  New-Item -ItemType Directory -Path $path|Out-Null;$made+=$path
  Put (Join-Path $path '.openpilot-owner') $nonce
 }
 $a=[IO.Path]::GetPathRoot($local);$b=[IO.Path]::GetPathRoot($other)
 Check ($a -ne $b) 'different fixture volumes'
 foreach($drive in @($a,$b)){Check ((Get-Volume -DriveLetter $drive[0]).FileSystemType -eq 'NTFS') "NTFS $drive"}
 $broker=Join-Path $root "build\$Config\folderwing-durable.exe"
 Check (Test-Path -LiteralPath $broker -PathType Leaf) 'release durability helper is available'
 $brokerHash=(Get-FileHash -LiteralPath $broker).Hash
 Copy-Item -LiteralPath $broker -Destination "$local\folderwing-durable.exe"
 Check ((Get-FileHash -LiteralPath "$local\folderwing-durable.exe").Hash -ceq $brokerHash) 'fixture durability helper matches the release image'
 $zig=Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'
 $flags=@('-target','x86_64-windows-gnu','-std=c11','-municode','-Wall','-Wextra','-Wno-unused-parameter','-Wno-missing-field-initializers',"-I$root\include",'-DUNICODE','-D_UNICODE','-O2','-DNDEBUG','-s')
 $engine=Get-ChildItem "$root\src\*.c"|ForEach-Object FullName
 $libs=@('-ladvapi32','-lkernel32','-luser32','-lshell32','-lole32','-lmpr')
 $native=$engine|Where-Object{(Split-Path $_ -Leaf) -ne 'jobs.c'}
 Invoke-BuildCompiler -Compiler $zig -OutputDirectory $local -Name 'native-crash.exe' -Arguments ($flags+$native+@("$PSScriptRoot\fixtures\windows_crash.c")+$libs+@('-o',"$local\native-crash.exe"))
 $stream=$engine|Where-Object{(Split-Path $_ -Leaf) -ne 'wsl.c'}
 Invoke-BuildCompiler -Compiler $zig -OutputDirectory $local -Name 'stream-crash.exe' -Arguments ($flags+@('-DFP_CRASH_STREAM')+$stream+@("$PSScriptRoot\fixtures\windows_crash.c")+$libs+@('-o',"$local\stream-crash.exe"))
 $setupMs=$clock.Elapsed.TotalMilliseconds;$clock.Restart()
 foreach($boundary in @('prepublish','published','before-remove','removed')){
  $source=Join-Path $local "source-$boundary";Put $source 'complete selected bytes'
  $dest=Join-Path $other "native-$boundary";New-Item -ItemType Directory $dest|Out-Null
  $target=Join-Path $dest "source-$boundary";Put $target 'previous target'
  RunCrash "$local\native-crash.exe" @($boundary,$source,$dest,'move')
  Check (Content $target $(if($boundary -eq 'prepublish'){'previous target'}else{'complete selected bytes'})) "native $boundary destination"
  Check $(if($boundary -eq 'removed'){-not(Test-Path -LiteralPath $source)}else{Content $source 'complete selected bytes'}) "native $boundary source"
  $stages=@(Get-ChildItem -LiteralPath $dest -Force -Filter '.openpilot-copy-*.tmp')
  Check ($stages.Count -eq $(if($boundary -eq 'prepublish'){1}else{0})) "native $boundary staging retained"
  foreach($stage in $stages){Check (Content $stage.FullName 'complete selected bytes') "native $boundary staged bytes"}
  ReviewStages $dest $stages.Count
 }
 foreach($boundary in @('partial','prepublish','published','lost-ack')){
  $source=Join-Path $local "stream-source-$boundary";Put $source 'complete selected bytes'
  $dest=Join-Path $local "stream-$boundary";New-Item -ItemType Directory $dest|Out-Null
  $target=Join-Path $dest 'selected';Put $target 'previous target'
  RunCrash "$local\stream-crash.exe" @($boundary,$source,$dest)
  $published=$boundary -in @('published','lost-ack')
  Check (Content $target $(if($published){'complete selected bytes'}else{'previous target'})) "stream $boundary destination"
  Check (Content $source 'complete selected bytes') "stream $boundary source"
  $stages=@(Get-ChildItem -LiteralPath $dest -Force -Filter '.openpilot-host-*.tmp')
  Check ($stages.Count -eq $(if($published){0}else{1})) "stream $boundary staging retained"
  foreach($stage in $stages){Check (Content $stage.FullName $(if($boundary -eq 'partial'){'complete se'}else{'complete selected bytes'})) "stream $boundary staged bytes"}
  ReviewStages $dest $stages.Count
 }
 foreach($boundary in @('prepublish','batch-prefix','published')){
  $source=Join-Path $local "batch-source-$boundary";New-Item -ItemType Directory $source|Out-Null
  for($i=0;$i -lt 32;$i++){Put (Join-Path $source ('child-{0:d2}' -f $i)) 'small complete bytes'}
  $dest=Join-Path $local "batch-dest-$boundary";New-Item -ItemType Directory $dest|Out-Null
  RunCrash "$local\native-crash.exe" @($boundary,$source,$dest,'copy')
  $target=Join-Path $dest (Split-Path $source -Leaf)
  $files=@(Get-ChildItem -LiteralPath $target -File -Force)
  $stages=@($files|Where-Object Name -like '.openpilot-copy-*.tmp')
  $published=@($files|Where-Object Name -notlike '.openpilot-copy-*.tmp')
  $expected=switch($boundary){'prepublish'{0};'batch-prefix'{16};'published'{32}}
  Check ($published.Count -eq $expected -and $stages.Count -eq (32-$expected)) "batch $boundary published prefix and retained stages"
  Check (@(Get-ChildItem -LiteralPath $source -File).Count -eq 32) "batch $boundary all sources remain"
  foreach($file in $files){Check (Content $file.FullName 'small complete bytes') "batch $boundary bytes $($file.Name)"}
  ReviewStages $target $stages.Count
 }
 Write-Output "$script:checks checks passed; no skips, no retries"
 $callMs=$clock.Elapsed.TotalMilliseconds
 $complete=$true
}finally{
 if($setupMs -gt 0 -and -not $callMs){$callMs=$clock.Elapsed.TotalMilliseconds}
 $clock.Restart()
 foreach($path in $made){
  if([IO.File]::ReadAllText((Join-Path $path '.openpilot-owner')) -cne $nonce){throw 'Fixture ownership changed; refusing cleanup'}
  if(-not $complete){Write-Output "RETAIN failed fixture $path";continue}
  Remove-Item -LiteralPath $path -Recurse -Force
  Write-Output "CLEANUP $path removed=$(-not(Test-Path -LiteralPath $path))"
 }
 Write-Output "TIMING setup_ms=$setupMs call_ms=$callMs teardown_ms=$($clock.Elapsed.TotalMilliseconds)"
}
