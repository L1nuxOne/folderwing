<# Manage the independently owned Direct runtime; never submits a provider turn.
   Config contains paths/model only. Control credentials stay inside its profile. #>
param([ValidateSet('Start','Status','Stop')][string]$Action='Start',[string]$Config=(Join-Path $PSScriptRoot 'direct-runtime.json'))
$ErrorActionPreference='Stop'
$cfg=Get-Content -LiteralPath $Config -Raw|ConvertFrom-Json
foreach($name in @('node','hostScript','profileRoot','projectRoot','projectId','model','reasoningEffort','authFile')){if(-not $cfg.$name){throw "Missing Direct configuration field: $name"}}
$base=Split-Path -Parent $Config
function Read-OwnedStatus {
 $file=Join-Path $cfg.profileRoot 'runtime-host.json'
 if(-not(Test-Path -LiteralPath $file -PathType Leaf)){return $null}
 try{
  $d=Get-Content -LiteralPath $file -Raw|ConvertFrom-Json
  if($d.schema -ne 'direct_runtime_host@1' -or $d.host -ne '127.0.0.1' -or $d.port -lt 1 -or $d.port -gt 65535 -or $d.token -notmatch '^[a-f0-9]{64}$'){throw 'Invalid Direct discovery'}
  $s=Invoke-RestMethod -Uri ('http://127.0.0.1:'+([int]$d.port)+'/v1/status') -Headers @{Authorization=('Bearer '+$d.token)} -TimeoutSec 3 -MaximumRedirection 0 -NoProxy
  if($s.schema -ne 'direct_runtime_host@1' -or $s.generation -ne $d.generation -or $s.projectId -ne $cfg.projectId -or $s.model -ne $cfg.model -or $s.reasoningEffort -ne $cfg.reasoningEffort){throw 'Direct runtime configuration differs'}
  return @{descriptor=$d;status=$s}
 }catch{return $null}
}
function Report($owned,[string]$result){
 if(-not $owned){@{outcome='not-running';profile=$cfg.profileRoot}|ConvertTo-Json -Compress;return}
 @{outcome=$result;pid=$owned.descriptor.pid;generation=$owned.status.generation;readiness=$owned.status.readiness;project=$owned.status.projectId;model=$owned.status.model;reasoningEffort=$owned.status.reasoningEffort;profile=$cfg.profileRoot;provider_request_submitted=$false}|ConvertTo-Json -Compress
}
$owned=Read-OwnedStatus
if($Action -eq 'Status'){Report $owned 'running';exit 0}
if($Action -eq 'Stop'){
 if(-not $owned){Report $null 'not-running';exit 0}
 $d=$owned.descriptor
 $null=Invoke-RestMethod -Method Post -Uri ('http://127.0.0.1:'+([int]$d.port)+'/v1/shutdown') -Headers @{Authorization=('Bearer '+$d.token)} -ContentType 'application/json' -Body '{}' -TimeoutSec 15 -MaximumRedirection 0 -NoProxy
 $until=[DateTime]::UtcNow.AddSeconds(15)
 while((Read-OwnedStatus) -and [DateTime]::UtcNow -lt $until){Start-Sleep -Milliseconds 100}
 if(Read-OwnedStatus){throw 'Direct runtime did not shut down; no process was killed'}
 @{outcome='stopped';pid=$d.pid;profile=$cfg.profileRoot}|ConvertTo-Json -Compress;exit 0
}
if($owned){Report $owned 'reused';exit 0}
foreach($file in @($cfg.node,$cfg.hostScript,$cfg.authFile)){if(-not(Test-Path -LiteralPath $file -PathType Leaf)){throw "Direct input is missing: $file"}}
if(-not(Test-Path -LiteralPath $cfg.projectRoot -PathType Container)){throw 'Direct project folder is unavailable'}
$logs=Join-Path $base 'logs';New-Item -ItemType Directory -Force $logs|Out-Null
$stamp=[DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')+'-'+[guid]::NewGuid().ToString('N')
$stdout=Join-Path $logs ($stamp+'.stdout.log');$stderr=Join-Path $logs ($stamp+'.stderr.log')
$bootstrap=Join-Path $PSScriptRoot 'direct-runtime-start.mjs'
$pi=[Diagnostics.ProcessStartInfo]::new($cfg.node);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
foreach($arg in @($bootstrap,'--config',$Config,'--stdout',$stdout,'--stderr',$stderr)){[void]$pi.ArgumentList.Add($arg)}
$starter=[Diagnostics.Process]::Start($pi);$output=$starter.StandardOutput.ReadToEndAsync();$errorOutput=$starter.StandardError.ReadToEndAsync()
try{
 if(-not $starter.WaitForExit(10000)){$starter.Kill($true);throw 'Direct bootstrap timed out; inspect the runtime profile before restarting'}
 if($starter.ExitCode -ne 0){throw ('Could not start the detached Direct runtime: '+$errorOutput.Result)}
 $child=$output.Result|ConvertFrom-Json
 if(-not $child.pid){throw 'Direct bootstrap returned no process identity'}
}finally{$starter.Dispose()}
try{$p=[Diagnostics.Process]::GetProcessById([int]$child.pid)}catch{throw "Direct runtime exited. Inspect $stderr"}
$until=[DateTime]::UtcNow.AddSeconds(30)
try{
 do{
  $owned=Read-OwnedStatus
  if($owned){Report $owned 'started';exit 0}
  $p.Refresh();if($p.HasExited){throw "Direct runtime exited. Inspect $stderr"}
  Start-Sleep -Milliseconds 100
 }while([DateTime]::UtcNow -lt $until)
 throw "Direct runtime is not ready yet. Inspect $stderr; the owned process was left running."
}finally{$p.Dispose()}
