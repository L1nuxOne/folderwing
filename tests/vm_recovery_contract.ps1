<# Private controller contracts. Hyper-V calls are mocked; no VM is created,
   started, stopped or adopted. Only a uniquely owned temporary fixture is used. #>
$ErrorActionPreference='Stop'
$env:SystemRoot='C:\Windows';$env:windir='C:\Windows';$env:SystemDrive='C:';$env:ComSpec='C:\Windows\System32\cmd.exe';$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
. "$PSScriptRoot\..\tools\vm-recovery.ps1"
$root=Join-Path ([IO.Path]::GetTempPath()) ('openpilot_vm_contract_'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root|Out-Null
$script:labBase=$root
function Get-OpVMLabBase {$script:labBase}
function Get-VM { [CmdletBinding()]param([guid]$Id,[string]$Name) return $script:vm }
function Get-VMHardDiskDrive { [CmdletBinding()]param($VM) foreach($path in $script:attached){[pscustomobject]@{Path=$path}} }
function Get-VMNetworkAdapter { [CmdletBinding()]param($VM) return $script:network }
function Get-VMComPort { [CmdletBinding()]param($VM,[int]$Number) return [pscustomobject]@{Path=$script:port} }
function Get-VHD { [CmdletBinding()]param([string]$Path) return [pscustomobject]@{ParentPath=$script:parent} }
function Stop-VM { [CmdletBinding()]param($VM,[switch]$TurnOff,[switch]$Confirm)
 Assert-OpVM ($VM.Id -eq [guid]$script:data.vm_id -and $TurnOff.IsPresent) 'Controller did not bind the exact owned VM object and TurnOff'
 $script:stops++;$script:vm.State='Off'
}
function Start-VM {throw 'Unexpected native VM start'}
function New-VM {throw 'Unexpected native VM creation'}
function New-VHD {throw 'Unexpected native disk creation'}
function Add-VMHardDiskDrive {throw 'Unexpected native disk attachment'}
function Get-OpVMEvent {param($Data,[int]$Seconds)
 if($script:stateAfterEvent){$script:vm.State='Off'}
 return ($script:event|ConvertTo-Json -Depth 8|ConvertFrom-Json)
}
$script:results=@()
function Check([string]$Name,[scriptblock]$Body){
 $clock=[Diagnostics.Stopwatch]::StartNew()
 Reset-Model
 try{& $Body;$script:results+=@{case=$Name;outcome='passed';call_ms=$clock.Elapsed.TotalMilliseconds};Write-Output "CASE $Name passed"}
 catch{$script:results+=@{case=$Name;outcome='failed';error=$_.Exception.Message;call_ms=$clock.Elapsed.TotalMilliseconds};Write-Output "CASE $Name failed: $($_.Exception.Message)"}
}
function Reject([scriptblock]$Body){
 $rejected=$false;try{& $Body|Out-Null}catch{$rejected=$true}
 Assert-OpVM $rejected 'Invalid controller action was accepted'
 Assert-OpVM ($script:stops -eq 0) 'Invalid action reached VM power-off'
}
function Reset-Model {
 $script:vm=[pscustomobject]@{Id=[guid]$script:data.vm_id;Name=$script:data.name;Notes=('OpenPilot disposable recovery lab:'+$script:data.token);State='Running'}
 $script:attached=@($script:data.disks);$script:network=@();$script:port=$script:data.pipe;$script:parent='';$script:stops=0;$script:stateAfterEvent=$false
 $script:item=[pscustomobject]@{schema='openpilot-hyperv-case/v1';lab_token=$script:data.token;case_id=('34'*16);phase='after-publication-sync';fixture_sha256=('c'*64);new_sha256=('a'*64);old_sha256=('b'*64)}
 $script:event=[pscustomobject]@{schema='openpilot-vm-event/v1';event='checkpoint';lab_token=$script:data.token;case_id=$script:item.case_id;phase=$script:item.phase;fixture_sha256=$script:item.fixture_sha256;boot_id='11111111-2222-3333-4444-555555555555';worker_stopped=$true;checkpoint=[pscustomobject]@{phase=$script:item.phase;fixture_version=1;destination_parent_synced=1;stage_parent_synced=1;source_unlinked=0}}
 $record=Join-Path (Join-Path $script:data.root 'cases') ($script:item.case_id+'.interruption.json')
 if(Test-Path -LiteralPath $record){Remove-Item -LiteralPath $record -Force}
 $record=Join-Path (Join-Path $script:data.root 'cases') ($script:item.case_id+'.result.json')
 if(Test-Path -LiteralPath $record){Remove-Item -LiteralPath $record -Force}
}
function Prepare-RecoveryEvent {
 $record=Join-Path (Join-Path $script:data.root 'cases') ($script:item.case_id+'.interruption.json')
 Write-OpVMNewJson $record @{schema='openpilot-vm-interruption/v1';powered_off=$true;lab_token=$script:data.token;case_id=$script:item.case_id;vm_id=$script:data.vm_id;checkpoint=$script:event}
 $script:event=[pscustomobject]@{schema='openpilot-vm-event/v1';event='recovered';lab_token=$script:data.token;case_id=$script:item.case_id;phase=$script:item.phase;fixture_sha256=$script:item.fixture_sha256;boot_id='66666666-7777-8888-9999-aaaaaaaaaaaa';observation_only=$true;outcome='passed';source_sha256=$script:item.new_sha256;target_sha256=$script:item.new_sha256}
}
try{
 $id='12'*16;$folder=Join-Path $root $id;New-Item -ItemType Directory (Join-Path $folder 'cases') -Force|Out-Null
 $disks=@('boot.vhdx','source.vhdx','destination.vhdx'|ForEach-Object{Join-Path $folder $_})
 foreach($path in $disks){[IO.File]::WriteAllBytes($path,[byte[]](1,2,3))}
 $script:data=[pscustomobject]@{schema='openpilot-hyperv-lab/v1';token=$id;root=$folder;name=('OpenPilotRecovery-'+$id);vm_id=[guid]::NewGuid().ToString();pipe=('\\.\pipe\OpenPilotRecovery-'+$id);disks=$disks}
 $lease=Join-Path $folder 'lease.json';Write-OpVMNewJson $lease $script:data
 Check 'valid_owned_lease_and_vm' {Get-OpVMOwned (Get-OpVMLease $lease)|Out-Null}
 Check 'foreign_vm_identity_rejected' {$script:vm.Id=[guid]::NewGuid();Reject {Get-OpVMOwned $script:data}}
 Check 'foreign_ownership_note_rejected' {$script:vm.Notes='User VM';Reject {Get-OpVMOwned $script:data}}
 Check 'extra_disk_rejected' {$script:attached+=Join-Path $root 'foreign.vhdx';Reject {Get-OpVMOwned $script:data}}
 Check 'replaced_disk_rejected' {$script:attached[0]=Join-Path $root 'foreign.vhdx';Reject {Get-OpVMOwned $script:data}}
 Check 'external_parent_disk_rejected' {$script:parent='C:\foreign.vhdx';Reject {Get-OpVMOwned $script:data}}
 Check 'network_attachment_rejected' {$script:network=@([pscustomobject]@{Name='NIC'});Reject {Get-OpVMOwned $script:data}}
 Check 'changed_serial_pipe_rejected' {$script:port='\\.\pipe\Foreign';Reject {Get-OpVMOwned $script:data}}
 Check 'explicit_interrupt_opt_in_required' {Reject {Invoke-OpVMInterrupt $script:data $script:item $false 1}}
 Check 'foreign_case_event_rejected' {$script:event.case_id='56'*16;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'foreign_lab_event_rejected' {$script:event.lab_token='56'*16;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'wrong_phase_event_rejected' {$script:event.phase='partial-write';Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'changed_fixture_event_rejected' {$script:event.fixture_sha256='d'*64;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'unstopped_worker_rejected' {$script:event.worker_stopped=$false;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'worker_stop_must_be_boolean' {$script:event.worker_stopped='True';Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'missing_destination_fence_rejected' {$script:event.checkpoint.destination_parent_synced=0;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'missing_staging_fence_rejected' {$script:event.checkpoint.stage_parent_synced=0;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'missing_source_unlink_rejected' {$script:item.phase='after-source-sync';$script:event.phase=$script:item.phase;$script:event.checkpoint.phase=$script:item.phase;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'state_rechecked_after_serial_wait' {$script:stateAfterEvent=$true;Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}}
 Check 'duplicate_json_keys_rejected' {Reject {ConvertFrom-OpVMJson '{"case_id":"one","case_id":"two"}'}}
 Check 'nested_duplicate_json_keys_rejected' {Reject {ConvertFrom-OpVMJson '{"checkpoint":{"phase":"one","phase":"two"}}'}}
 Check 'serial_noise_is_not_checkpoint_evidence' {
  $bytes=[Text.Encoding]::UTF8.GetBytes("boot log`nOPVM1 $($script:event|ConvertTo-Json -Depth 8 -Compress)`n")
  $stream=[IO.MemoryStream]::new($bytes);try{$event=Read-OpVMEvent $stream 1;Assert-OpVMEvent $event $script:data $script:item 'checkpoint'}finally{$stream.Dispose()}
 }
 Check 'serial_eof_rejected' {$stream=[IO.MemoryStream]::new([byte[]]@());try{Reject {Read-OpVMEvent $stream 1}}finally{$stream.Dispose()}}
 Check 'oversized_serial_line_rejected' {$stream=[IO.MemoryStream]::new([Text.Encoding]::UTF8.GetBytes(('x'*16384)+"`n"));try{Reject {Read-OpVMEvent $stream 1}}finally{$stream.Dispose()}}
 Check 'reparse_lab_path_rejected' {
  $target=Join-Path $root 'junction-target';$link=Join-Path $root 'junction';New-Item -ItemType Directory $target|Out-Null
  New-Item -ItemType Junction -Path $link -Value $target|Out-Null
  try{Reject {Assert-OpVMPath $link}}finally{Remove-Item -LiteralPath $link -Force;Remove-Item -LiteralPath $target -Force}
 }
 Check 'host_oracle_allows_unfenced_rename_outcomes' {
  $script:item.phase='before-destination-sync'
  foreach($target in @($script:item.old_sha256,$script:item.new_sha256)){
   $event=[pscustomobject]@{source_sha256=$script:item.new_sha256;target_sha256=$target};Assert-OpVM (Test-OpVMRecovery $event $script:item) 'Valid unfenced rename outcome rejected'
  }
 }
 Check 'host_oracle_rejects_missing_durable_destination' {
  $event=[pscustomobject]@{source_sha256=$script:item.new_sha256;target_sha256=$null};Assert-OpVM (-not(Test-OpVMRecovery $event $script:item)) 'Missing durable destination accepted'
 }
 Check 'host_oracle_distinguishes_unlink_barriers' {
  $script:item.phase='after-source-unlink';$event=[pscustomobject]@{source_sha256=$script:item.new_sha256;target_sha256=$script:item.new_sha256}
  Assert-OpVM (Test-OpVMRecovery $event $script:item) 'Unfenced source reappearance rejected'
  $script:item.phase='after-source-sync';Assert-OpVM (-not(Test-OpVMRecovery $event $script:item)) 'Fenced source reappearance accepted'
  $event.source_sha256=$null;Assert-OpVM (Test-OpVMRecovery $event $script:item) 'Durable source removal rejected'
 }
 Check 'valid_interrupt_targets_exact_owned_vm_once' {
  $result=Invoke-OpVMInterrupt $script:data $script:item $true 1
  Assert-OpVM ($script:stops -eq 1 -and $result.powered_off -eq $true -and $result.recovery_verified -eq $false -and $script:vm.State -eq 'Off') 'Scoped mock interruption did not preserve evidence'
 }
 Check 'interruption_replay_rejected' {
  Invoke-OpVMInterrupt $script:data $script:item $true 1|Out-Null;$script:stops=0
  Reject {Invoke-OpVMInterrupt $script:data $script:item $true 1}
 }
 Check 'recovery_requires_prior_host_poweroff_record' {Reject {Invoke-OpVMCollect $script:data $script:item 1}}
 Check 'recovery_rejects_same_guest_boot' {Prepare-RecoveryEvent;$script:event.boot_id='11111111-2222-3333-4444-555555555555';Reject {Invoke-OpVMCollect $script:data $script:item 1}}
 Check 'recovery_rechecks_vm_after_serial_wait' {Prepare-RecoveryEvent;$script:stateAfterEvent=$true;Reject {Invoke-OpVMCollect $script:data $script:item 1}}
 Check 'host_does_not_trust_guest_pass_label' {Prepare-RecoveryEvent;$script:event.target_sha256=$script:item.old_sha256;Reject {Invoke-OpVMCollect $script:data $script:item 1}}
 Check 'recovery_records_actual_hash_failure' {
  Prepare-RecoveryEvent;$script:event.target_sha256=$script:item.old_sha256;$script:event.outcome='failed'
  $result=Invoke-OpVMCollect $script:data $script:item 1
  Assert-OpVM ($result.outcome -eq 'failed' -and $script:stops -eq 0) 'Failed hash state was hidden'
 }
 Check 'matching_new_boot_recovery_is_recorded_without_power_action' {
  Prepare-RecoveryEvent;$result=Invoke-OpVMCollect $script:data $script:item 1
  Assert-OpVM ($result.outcome -eq 'passed' -and $result.physical_power_loss_certified -eq $false -and $script:stops -eq 0) 'Recovery result overstated proof or reached power-off'
 }
 Check 'one_lab_lock_excludes_other_case_actions' {
  $first=Open-OpVMLabLock $script:data
  try{Reject {Open-OpVMLabLock $script:data}}finally{$first.Dispose()}
  $next=Open-OpVMLabLock $script:data;$next.Dispose()
 }
 Check 'host_records_never_overwrite_evidence' {
  $path=Join-Path $root 'immutable.json';Write-OpVMNewJson $path @{before=1}
  Reject {Write-OpVMNewJson $path @{after=2}}
  Assert-OpVM ((Read-OpVMJson $path).before -eq 1) 'Evidence was overwritten'
 }
 Check 'existing_vm_cannot_be_adopted' {Reject {New-OpVMLab $script:data.disks[0]}}
 Check 'default_plan_has_no_vm_effects' {
  $plan=(& "$PSScriptRoot\..\tools\vm-recovery.ps1" -Action Plan|ConvertFrom-Json)
  Assert-OpVM ($plan.executed -eq $false -and $plan.wsl_action -eq 'none' -and $script:stops -eq 0) 'Plan performed a VM action'
 }
}finally{
 if(Test-Path -LiteralPath $root){Remove-Item -LiteralPath $root -Recurse -Force}
}
$failed=@($script:results|Where-Object outcome -ne 'passed').Count
@{schema='openpilot-vm-host-contract/v1';checks=$script:results.Count;failed=$failed;cases=$script:results;vm_calls='mocked; no VM mutation';owned_fixture_removed=(-not(Test-Path -LiteralPath $root))}|ConvertTo-Json -Depth 6
exit $failed
