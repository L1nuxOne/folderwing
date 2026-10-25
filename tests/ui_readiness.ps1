<# Opt-in acceptance readiness: owned HWNDs, acknowledged input and failure-only evidence.
   Dot-source after uilib.ps1. Uses existing input/navigation budgets; no automatic retries. #>
. "$PSScriptRoot\ui_input.ps1"
if(-not ('FpReadiness' -as [type])){
 Add-Type @"
using System;using System.Runtime.InteropServices;
public static class FpReadiness {
 [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern uint GetClipboardSequenceNumber();
 [DllImport("user32.dll")] public static extern IntPtr GetOpenClipboardWindow();
 [DllImport("user32.dll")] public static extern IntPtr GetClipboardOwner();
}
"@
}
$script:readinessSettingsPending=$false
$script:readinessHelperHistory=@{}
function Find-OwnedWindow([string]$class,[string]$title=''){
 $after=[IntPtr]::Zero
 while(($after=[FpUi]::FindWindowExW([IntPtr]::Zero,$after,$class,[IntPtr]::Zero)) -ne [IntPtr]::Zero){
  $owner=[uint32]0;[FpUi]::GetWindowThreadProcessId($after,[ref]$owner)|Out-Null
  # Discovery must tolerate destruction while a close/cancel is being polled.
  # Do not send an acknowledged WM_GETTEXT to a window that may be exiting.
  if($owner -eq $script:demoPid -and (-not $title -or (Window-Text $after) -ceq $title)){return $after}
 }
 [IntPtr]::Zero
}
function Get-ReadinessHelpers([string]$name){
 $found=@()
 foreach($process in @(Get-Process $name -ErrorAction SilentlyContinue)){
  try{
   if($process.Parent.Id -ne $script:demoPid){continue}
   $found+=,$process
   $key="$($script:demoPid)/$($process.Id)/$($process.StartTime.Ticks)"
   if(-not $script:readinessHelperHistory.ContainsKey($key)){
    $script:readinessHelperHistory[$key]=@{parent=$script:demoPid;pid=$process.Id;name=$process.ProcessName;born=$process.StartTime;first_observed=[DateTime]::UtcNow;path=$process.Path}
   }
  }catch{} # A process may exit between the parent and birth queries.
 }
 $found
}
function Read-ReadinessText([IntPtr]$hwnd){
 try{[FpUi]::ReadText($hwnd,50)}catch{"<unavailable: $($_.Exception.Message)>"}
}
function Get-UiReadinessState([string]$label){
 $state=[ordered]@{label=$label;utc=[DateTime]::UtcNow;pid=$script:demoPid;main=$script:main.ToInt64();settings_pending=$script:readinessSettingsPending;settings_before=$script:readinessSettingsBefore;panes=@();readers=@();helpers=@()}
 if([FpReadiness]::IsWindow($script:main)){
  [FpUi]::OwnedWindow($script:main,$script:demoPid)
  $state.settings_revision=[FpUi]::GetPropW($script:main,'Settings.Revision').ToInt64()
  $state.settings_watching=[FpUi]::GetPropW($script:main,'Settings.Watching').ToInt64()
  foreach($pane in @($script:pane0,$script:pane1)){
   $row=[ordered]@{hwnd=$pane.ToInt64();live=[FpReadiness]::IsWindow($pane)}
   if($row.live){
    [FpUi]::OwnedWindow($pane,$script:demoPid)
    $row.visible=[FpUi]::IsWindowVisible($pane)
    $row.caption=Read-ReadinessText $pane
    foreach($key in @('Request','Loading','Resolving','Error')){$row[$key]=[FpUi]::GetPropW($pane,"Address.$key").ToInt64()}
    $edit=(Pane-Edits $pane)[0]
    if($edit){[FpUi]::OwnedWindow($edit,$script:demoPid);$row.address=Read-ReadinessText $edit}
   }
   $state.panes+=,$row
  }
  foreach($class in @('OpenPilotText','OpenPilotPdf','OpenPilotImage','OpenPilotThumbnails')){
   $h=[FpUi]::FindWindowExW($script:main,[IntPtr]::Zero,$class,[IntPtr]::Zero)
   if($h -ne [IntPtr]::Zero){$state.readers+=,@{class=$class;hwnd=$h.ToInt64();visible=[FpUi]::IsWindowVisible($h);title=(Read-ReadinessText $h)}}
  }
 }
 $side=[FpUi]::FindWindowExW($script:main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
 $slot=if($side -ne [IntPtr]::Zero){[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)}else{[IntPtr]::Zero}
 $state.operations=@{side=$side.ToInt64();slot=$slot.ToInt64();slot_visible=($slot -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($slot));helpers=@(Get-ReadinessHelpers 'folderwing-operations'|ForEach-Object {@{pid=$_.Id;born=$_.StartTime;path=$_.Path}});ready_contract='No public Operations.Ready property; visibility/process birth are prerequisites, not proof of model/request readiness';request_model='Host-private; inspect native admission-control facts, never infer slow rendering from missing target'}
 $dialog=Find-OwnedWindow 'OpenPilotFolderCompare' 'Compare folders'
 $state.comparison=[ordered]@{hwnd=$dialog.ToInt64();phase='window-absent'}
 if($dialog -ne [IntPtr]::Zero){
  foreach($key in @('Busy','Done','Error','Count','Cancelled')){$state.comparison[$key]=[FpUi]::GetPropW($dialog,"FolderCompare.$key").ToInt64()}
  $state.comparison.phase=if($state.comparison.Busy){'worker-busy'}elseif($state.comparison.Done){'finished'}else{'window-building'}
 }
 foreach($process in @(Get-Process folderwing-pdf,folderwing-text,folderwing-image,folderwing-operations -ErrorAction SilentlyContinue)){
  try{if($process.Parent.Id -eq $script:demoPid){$state.helpers+=,@{pid=$process.Id;name=$process.ProcessName;born=$process.StartTime;path=$process.Path}}}catch{}
 }
 $state.helpers_observed=@($script:readinessHelperHistory.Values)
 $state.clipboard=@{sequence=[FpReadiness]::GetClipboardSequenceNumber();holder=[FpReadiness]::GetOpenClipboardWindow().ToInt64();owner=[FpReadiness]::GetClipboardOwner().ToInt64()}
 $state
}
function Write-UiReadinessFailure([string]$label){
 try{
  $json=Get-UiReadinessState $label|ConvertTo-Json -Depth 8 -Compress
  Write-Output "READINESS $json"
  if($script:readinessEvidence){Add-Content -LiteralPath $script:readinessEvidence -Value $json}
 }catch{Write-Output "READINESS observer_error=$($_.Exception.Message); label=$label"}
}
function Navigate-Ready([string]$path,[IntPtr]$pane=$script:pane0,[int]$ms=7000){
 try{$null=Navigate-Verified $path $pane $ms}catch{Write-UiReadinessFailure "navigation: $path";throw}
}
function Set-UiSettings([string]$path,[string]$text){
 $script:readinessSettingsBefore=[FpUi]::GetPropW($script:main,'Settings.Revision').ToInt64()
 $script:readinessSettingsPending=$true
 Set-Content -LiteralPath $path -Value $text
}
function Test-UiSettingsApplied {
 if(-not $script:readinessSettingsPending){return $true}
 if([FpUi]::GetPropW($script:main,'Settings.Revision').ToInt64() -eq $script:readinessSettingsBefore){return $false}
 $script:readinessSettingsPending=$false;$true
}
function Wait-UiSettings([int]$ms=500){
 if(-not(Wait-Until {Test-UiSettingsApplied} $ms)){
  Write-UiReadinessFailure 'settings revision did not advance'
  throw "Settings revision did not advance within ${ms}ms"
 }
}
