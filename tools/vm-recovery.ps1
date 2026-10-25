<# Isolated Hyper-V recovery controller. Plan is read-only and is the default.
   No existing VM can be adopted. Never manages WSL, services or host features. #>
param(
 [ValidateSet('Plan','Create','NewCase','Start','Interrupt','Collect')][string]$Action='Plan',
 [string]$BaseDisk,[string]$Lease,[string]$Case,
 [ValidateSet('partial-write','before-payload-sync','after-payload-sync','before-destination-sync','after-publication-sync','before-source-unlink','after-source-unlink','after-source-sync')][string]$Phase,
 [string]$FixtureSha256,[string]$NewSha256,[string]$OldSha256,[switch]$AllowInterrupt,
 [ValidateRange(1,300)][int]$TimeoutSeconds=30
)
$ErrorActionPreference='Stop'
function Assert-OpVM([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
function Get-OpVMLabBase { [IO.Path]::GetFullPath((Join-Path (Split-Path $PSScriptRoot -Parent) 'build\vm-labs')) }
function Assert-OpVMAdmin {
 $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
 Assert-OpVM (([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) 'Run the owned-VM action from an elevated PowerShell session'
}
function Assert-OpVMToken([string]$Value){Assert-OpVM ($Value -cmatch '^[0-9a-f]{32}$' -and ([guid]$Value).ToString('N') -ceq $Value) 'Invalid lab/case token'}
function Assert-OpVMPath([string]$Path){
 Assert-OpVM ([IO.Path]::IsPathFullyQualified($Path)) 'Expected an absolute Windows path'
 $full=[IO.Path]::GetFullPath($Path)
 $item=Get-Item -LiteralPath $full -Force
 while($item){
  Assert-OpVM (-not($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Reparse paths are not lab custody'
  $item=if($item -is [IO.FileInfo]){$item.Directory}else{$item.Parent}
 }
 return $full
}
function Assert-OpVMUniqueJson($Element,[int]$Depth=0){
 Assert-OpVM ($Depth -lt 16) 'JSON is too deeply nested'
 if($Element.ValueKind.ToString() -eq 'Object'){
  $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
  foreach($property in $Element.EnumerateObject()){
   Assert-OpVM ($seen.Add($property.Name)) 'Duplicate JSON property'
   Assert-OpVMUniqueJson $property.Value ($Depth+1)
  }
 }elseif($Element.ValueKind.ToString() -eq 'Array'){
  foreach($value in $Element.EnumerateArray()){Assert-OpVMUniqueJson $value ($Depth+1)}
 }
}
function ConvertFrom-OpVMJson([string]$Text){
 Assert-OpVM ($Text.Length -le 32768) 'Oversized JSON'
 $document=[Text.Json.JsonDocument]::Parse($Text)
 try{Assert-OpVMUniqueJson $document.RootElement}finally{$document.Dispose()}
 return ($Text|ConvertFrom-Json)
}
function Read-OpVMJson([string]$Path){
 $full=Assert-OpVMPath $Path
 Assert-OpVM ((Get-Item -LiteralPath $full).Length -le 32768) 'Oversized lab record'
 return ConvertFrom-OpVMJson ([IO.File]::ReadAllText($full))
}
function Write-OpVMNewJson([string]$Path,$Value){
 $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 12))
 $stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 try{$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)}finally{$stream.Dispose()}
}
function Open-OpVMLabLock($Data){
 return [IO.FileStream]::new((Join-Path $Data.root 'lab.lock'),[IO.FileMode]::CreateNew,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None,4096,[IO.FileOptions]::DeleteOnClose)
}
function Get-OpVMLease([string]$Path){
 $data=Read-OpVMJson $Path
 Assert-OpVM ($data.schema -ceq 'openpilot-hyperv-lab/v1') 'Unknown lab schema'
 Assert-OpVMToken $data.token
 $root=Join-Path (Get-OpVMLabBase) $data.token
 Assert-OpVM ([IO.Path]::GetFullPath($data.root) -ieq $root) 'Lab root differs from owned storage'
 Assert-OpVM ((Assert-OpVMPath $Path) -ieq (Join-Path $root 'lease.json')) 'Lease is outside its owned root'
 Assert-OpVM ($data.name -ceq ('OpenPilotRecovery-'+$data.token)) 'VM name does not match lab token'
 Assert-OpVM ($data.pipe -ceq ('\\.\pipe\OpenPilotRecovery-'+$data.token)) 'Serial pipe differs from owned lab'
 Assert-OpVM ($data.vm_id -and [guid]::Parse($data.vm_id) -ne [guid]::Empty) 'Missing exact VM identity'
 $expected=@('boot.vhdx','source.vhdx','destination.vhdx'|ForEach-Object{Join-Path $root $_})
 Assert-OpVM (@($data.disks).Count -eq 3) 'Expected exactly three private VHDX files'
 $found=@($data.disks|ForEach-Object{Assert-OpVMPath $_})
 Assert-OpVM (@(Compare-Object ($expected|Sort-Object) ($found|Sort-Object)).Count -eq 0) 'Lab disks differ from private paths'
 return $data
}
function Get-OpVMOwned($Data){
 $vm=Get-VM -Id ([guid]$Data.vm_id) -ErrorAction Stop
 Assert-OpVM ($vm.Id.ToString() -ieq $Data.vm_id -and $vm.Name -ceq $Data.name) 'VM identity changed'
 Assert-OpVM ($vm.Notes -ceq ('OpenPilot disposable recovery lab:'+$Data.token)) 'VM ownership note changed'
 $paths=@(Get-VMHardDiskDrive -VM $vm -ErrorAction Stop|ForEach-Object{$_.Path})
 Assert-OpVM ($paths.Count -eq 3 -and @(Compare-Object ($Data.disks|Sort-Object) ($paths|Sort-Object)).Count -eq 0) 'Attached disks differ; no interruption allowed'
 foreach($disk in $Data.disks){
  Assert-OpVMPath $disk|Out-Null
  $metadata=Get-VHD -Path $disk -ErrorAction Stop
  Assert-OpVM (-not $metadata.ParentPath) 'External/differencing disk parent is not isolated lab custody'
 }
 Assert-OpVM (@(Get-VMNetworkAdapter -VM $vm -ErrorAction Stop).Count -eq 0) 'Recovery VM must have no network adapters'
 $port=Get-VMComPort -VM $vm -Number 1 -ErrorAction Stop
 Assert-OpVM ($port.Path -ceq $Data.pipe) 'VM serial pipe changed'
 return $vm
}
function Get-OpVMCase($Data,[string]$Path){
 $item=Read-OpVMJson $Path;Assert-OpVMToken $item.case_id
 Assert-OpVM ($item.schema -ceq 'openpilot-hyperv-case/v1' -and $item.lab_token -ceq $Data.token) 'Case belongs to a different lab'
 Assert-OpVM ((Assert-OpVMPath $Path) -ieq (Join-Path (Join-Path $Data.root 'cases') ($item.case_id+'.json'))) 'Case is outside its owned root'
 Assert-OpVM ($item.phase -in @('partial-write','before-payload-sync','after-payload-sync','before-destination-sync','after-publication-sync','before-source-unlink','after-source-unlink','after-source-sync')) 'Unknown checkpoint'
 Assert-OpVM ($item.fixture_sha256 -cmatch '^[0-9a-f]{64}$') 'Missing exact private fixture hash'
 Assert-OpVM ($item.new_sha256 -cmatch '^[0-9a-f]{64}$' -and $item.old_sha256 -cmatch '^[0-9a-f]{64}$' -and $item.new_sha256 -cne $item.old_sha256) 'Missing independent payload contract'
 return $item
}
function Test-OpVMRecovery($Event,$Item){
 Assert-OpVM ($Event.PSObject.Properties.Name -contains 'source_sha256' -and $Event.PSObject.Properties.Name -contains 'target_sha256') 'Missing recovery hashes'
 $source=@($Item.new_sha256);$target=@($Item.new_sha256)
 if($Item.phase -in @('partial-write','before-payload-sync','after-payload-sync')){$target=@($Item.old_sha256)}
 if($Item.phase -eq 'before-destination-sync'){$target=@($Item.old_sha256,$Item.new_sha256)}
 if($Item.phase -eq 'after-source-unlink'){$source=@($null,$Item.new_sha256)}
 if($Item.phase -eq 'after-source-sync'){$source=@($null)}
 return ($source -ccontains $Event.source_sha256) -and ($target -ccontains $Event.target_sha256)
}
function Read-OpVMEvent($Stream,[int]$Seconds){
 $cancel=[Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds($Seconds));$line=[Collections.Generic.List[byte]]::new();$one=[byte[]]::new(1)
 try{
  while($true){
   $count=$Stream.ReadAsync($one,0,1,$cancel.Token).GetAwaiter().GetResult()
   Assert-OpVM ($count -eq 1) 'Serial pipe ended before a recovery event'
   if($one[0] -eq 10){
    $text=[Text.UTF8Encoding]::new($false,$true).GetString($line.ToArray()).TrimEnd("`r");$line.Clear()
    if($text.StartsWith('OPVM1 ',[StringComparison]::Ordinal)){return ConvertFrom-OpVMJson $text.Substring(6)}
   }else{$line.Add($one[0]);Assert-OpVM ($line.Count -lt 16384) 'Oversized serial line'}
  }
 }finally{$cancel.Dispose()}
}
function Get-OpVMEvent($Data,[int]$Seconds){
 $stream=[IO.Pipes.NamedPipeClientStream]::new('.',$Data.pipe.Substring(9),[IO.Pipes.PipeDirection]::InOut)
 try{$stream.Connect($Seconds*1000);return Read-OpVMEvent $stream $Seconds}finally{$stream.Dispose()}
}
function Assert-OpVMEvent($Event,$Data,$Item,[string]$Kind){
 foreach($field in @('schema','event','lab_token','case_id','phase','fixture_sha256','boot_id')){Assert-OpVM ($Event.$field -is [string]) 'Event identity fields must be strings'}
 Assert-OpVM ($Event.schema -ceq 'openpilot-vm-event/v1' -and $Event.event -ceq $Kind) 'Wrong event kind/schema'
 Assert-OpVM ($Event.lab_token -ceq $Data.token -and $Event.case_id -ceq $Item.case_id) 'Foreign/replayed case event'
 Assert-OpVM ($Event.phase -ceq $Item.phase -and $Event.fixture_sha256 -ceq $Item.fixture_sha256) 'Event differs from planned checkpoint/fixture'
 Assert-OpVM ($Event.boot_id -cmatch '^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$') 'Missing guest boot identity'
 if($Kind -eq 'checkpoint'){
  Assert-OpVM ($Event.worker_stopped -is [bool] -and $Event.worker_stopped -eq $true -and $Event.checkpoint.phase -ceq $Item.phase -and $Event.checkpoint.fixture_version -eq 1) 'Worker stop/checkpoint not established'
  if($Item.phase -in @('after-publication-sync','before-source-unlink','after-source-unlink','after-source-sync')){
   Assert-OpVM ($Event.checkpoint.destination_parent_synced -eq 1 -and $Event.checkpoint.stage_parent_synced -eq 1) 'Publication barriers were not observed'
  }
  if($Item.phase -in @('after-source-unlink','after-source-sync')){Assert-OpVM ($Event.checkpoint.source_unlinked -eq 1) 'Source unlink was not observed'}
 }
}
function Invoke-OpVMInterrupt($Data,$Item,[bool]$Allowed,[int]$Seconds){
 Assert-OpVM $Allowed 'Interrupt requires -AllowInterrupt for this exact owned lab case'
 $record=Join-Path (Join-Path $Data.root 'cases') ($Item.case_id+'.interruption.json')
 Assert-OpVM (-not(Test-Path -LiteralPath $record)) 'Case already interrupted; replay refused'
 $vm=Get-OpVMOwned $Data;Assert-OpVM ($vm.State.ToString() -eq 'Running') 'Owned VM must be running'
 $event=Get-OpVMEvent $Data $Seconds;Assert-OpVMEvent $event $Data $Item 'checkpoint'
 # Revalidate after waiting: no cached ownership assumption authorizes power-off.
 $vm=Get-OpVMOwned $Data
 Assert-OpVM ($vm.State.ToString() -eq 'Running') 'VM state changed before interruption'
 Stop-VM -VM $vm -TurnOff -Confirm:$false -ErrorAction Stop
 $after=Get-VM -Id ([guid]$Data.vm_id) -ErrorAction Stop
 Assert-OpVM ($after.State.ToString() -eq 'Off') 'Owned VM power-off not confirmed'
 # Persist host evidence AFTER power-off, avoiding an evidence flush before the crash.
 $result=@{schema='openpilot-vm-interruption/v1';lab_token=$Data.token;case_id=$Item.case_id;vm_id=$Data.vm_id;checkpoint=$event;powered_off=$true;recovery_verified=$false}
 Write-OpVMNewJson $record $result
 return $result
}
function Invoke-OpVMCollect($Data,$Item,[int]$Seconds){
 $prior=Read-OpVMJson (Join-Path (Join-Path $Data.root 'cases') ($Item.case_id+'.interruption.json'))
 Assert-OpVM ($prior.schema -ceq 'openpilot-vm-interruption/v1' -and $prior.powered_off -eq $true -and $prior.vm_id -ieq $Data.vm_id -and $prior.lab_token -ceq $Data.token -and $prior.case_id -ceq $Item.case_id) 'Missing matching host power-off evidence'
 Assert-OpVMEvent $prior.checkpoint $Data $Item 'checkpoint'
 $vm=Get-OpVMOwned $Data;Assert-OpVM ($vm.State.ToString() -eq 'Running') 'Recovery VM must be running'
 $event=Get-OpVMEvent $Data $Seconds;Assert-OpVMEvent $event $Data $Item 'recovered'
 $vm=Get-OpVMOwned $Data;Assert-OpVM ($vm.State.ToString() -eq 'Running') 'VM state changed during recovery observation'
 Assert-OpVM ($event.boot_id -cne $prior.checkpoint.boot_id) 'Same-boot observation is not recovery evidence'
 Assert-OpVM ($event.observation_only -is [bool] -and $event.observation_only -eq $true -and $event.outcome -cin @('passed','failed')) 'Missing guest recovery oracle'
 $oracle=if(Test-OpVMRecovery $event $Item){'passed'}else{'failed'}
 Assert-OpVM ($oracle -ceq $event.outcome) 'Host and guest recovery oracles disagree'
 $result=@{schema='openpilot-vm-recovery-result/v1';outcome=$event.outcome;lab_token=$Data.token;case_id=$Item.case_id;vm_id=$Data.vm_id;interruption=$prior;recovery=$event;physical_power_loss_certified=$false}
 Write-OpVMNewJson (Join-Path (Join-Path $Data.root 'cases') ($Item.case_id+'.result.json')) $result
 return $result
}
function New-OpVMLab([string]$Base){
 $basePath=Assert-OpVMPath $Base;Assert-OpVM ($basePath.EndsWith('.vhdx',[StringComparison]::OrdinalIgnoreCase)) 'Supply a clean Linux generation-2 VHDX template'
 Assert-OpVM (-not(Get-VHD -Path $basePath -ErrorAction Stop).ParentPath) 'Base template must be standalone, not differencing'
 $id=[guid]::NewGuid().ToString('N');$root=Join-Path (Get-OpVMLabBase) $id;$name='OpenPilotRecovery-'+$id
 Assert-OpVM (-not(Get-VM -Name $name -ErrorAction SilentlyContinue)) 'Refusing to adopt an existing VM'
 Assert-OpVM (-not(Test-Path -LiteralPath $root)) 'Lab directory must be new'
 New-Item -ItemType Directory -Path $root|Out-Null;Assert-OpVMPath $root|Out-Null
 New-Item -ItemType Directory -Path (Join-Path $root 'cases')|Out-Null
 $boot=Join-Path $root 'boot.vhdx';$source=Join-Path $root 'source.vhdx';$destination=Join-Path $root 'destination.vhdx'
 Copy-Item -LiteralPath $basePath -Destination $boot
 New-VHD -Path $source -Dynamic -SizeBytes 2GB -ErrorAction Stop|Out-Null
 New-VHD -Path $destination -Dynamic -SizeBytes 2GB -ErrorAction Stop|Out-Null
 $vm=New-VM -Name $name -Generation 2 -MemoryStartupBytes 1GB -VHDPath $boot -Path $root -ErrorAction Stop
 Set-VM -VM $vm -Notes ('OpenPilot disposable recovery lab:'+$id) -AutomaticStartAction Nothing -AutomaticStopAction ShutDown -AutomaticCheckpointsEnabled $false -CheckpointType Disabled
 Set-VMProcessor -VM $vm -Count 1
 Set-VMMemory -VM $vm -DynamicMemoryEnabled $false
 Set-VMFirmware -VM $vm -EnableSecureBoot Off
 Get-VMNetworkAdapter -VM $vm|Remove-VMNetworkAdapter -Confirm:$false
 Add-VMHardDiskDrive -VM $vm -Path $source
 Add-VMHardDiskDrive -VM $vm -Path $destination
 $pipe='\\.\pipe\OpenPilotRecovery-'+$id;Set-VMComPort -VM $vm -Number 1 -Path $pipe
 $data=@{schema='openpilot-hyperv-lab/v1';token=$id;root=$root;name=$name;vm_id=$vm.Id.ToString();pipe=$pipe;disks=@($boot,$source,$destination)}
 $leasePath=Join-Path $root 'lease.json';Write-OpVMNewJson $leasePath $data
 Get-OpVMOwned (Get-OpVMLease $leasePath)|Out-Null
 return @{outcome='created_off';lease=$leasePath;lab_token=$id;vm_id=$data.vm_id;notes='Fresh copied boot disk and two blank data VHDX files; no VM was started. Failed provisioning is retained for review, never automatically removed.'}
}
if($MyInvocation.InvocationName -eq '.'){return}
if($Action -eq 'Plan'){
 @{schema='openpilot-vm-recovery-plan/v1';executed=$false;backend='Hyper-V';storage=Get-OpVMLabBase;memory_mib=1024;vcpus=1;data_disks_mib=@(2048,2048);network_adapters=0;guest='Clean Linux generation-2 standalone VHDX with Python 3.10+ and util-linux findmnt';scope='Eight native Linux cross-filesystem overwrite-move checkpoints; no Windows/WSL VM recovery claim';wsl_action='none'}|ConvertTo-Json -Depth 5
 return
}
Assert-OpVMAdmin
if($Action -eq 'Create'){New-OpVMLab $BaseDisk|ConvertTo-Json -Depth 8;return}
$data=Get-OpVMLease $Lease
$lock=Open-OpVMLabLock $data
try{
$vm=Get-OpVMOwned $data
if($Action -eq 'NewCase'){
 Assert-OpVM ($Phase -and $FixtureSha256 -cmatch '^[0-9a-f]{64}$' -and $NewSha256 -cmatch '^[0-9a-f]{64}$' -and $OldSha256 -cmatch '^[0-9a-f]{64}$' -and $NewSha256 -cne $OldSha256) 'Supply an exact checkpoint, fixture hash and independent new/old payload hashes'
 $id=[guid]::NewGuid().ToString('N');$path=Join-Path (Join-Path $data.root 'cases') ($id+'.json')
 $item=@{schema='openpilot-hyperv-case/v1';lab_token=$data.token;case_id=$id;phase=$Phase;fixture_sha256=$FixtureSha256;new_sha256=$NewSha256;old_sha256=$OldSha256}
 Write-OpVMNewJson $path $item;@{outcome='case_planned';case=$path;guest_lab_token=$data.token;guest_case_id=$id;phase=$Phase}|ConvertTo-Json;return
}
if($Action -eq 'Start'){
 Assert-OpVM ($vm.State.ToString() -eq 'Off') 'Owned VM must be off before start'
 Start-VM -VM $vm -ErrorAction Stop|Out-Null
 @{outcome='started_owned_vm';vm_id=$data.vm_id}|ConvertTo-Json;return
}
$item=Get-OpVMCase $data $Case
 if($Action -eq 'Interrupt'){Invoke-OpVMInterrupt $data $item $AllowInterrupt.IsPresent $TimeoutSeconds|ConvertTo-Json -Depth 10;return}
 $result=Invoke-OpVMCollect $data $item $TimeoutSeconds
 $result|ConvertTo-Json -Depth 12
 if($result.outcome -ne 'passed'){exit 1}
}finally{$lock.Dispose()}
