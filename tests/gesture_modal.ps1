<# Exact N11 changed-view/modal acceptance. Ordinary parent desktop only:
   its durability broker requires the normal Windows elevation facility.
   Uses only newly owned native windows/files and posted menu messages.
   Coordinate one minute of physical keyboard/mouse idleness before this lane. #>
param([ValidateSet('release','debug')][string]$Config='release',[string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\..\tools\child-ui-common.ps1"
Initialize-ChildUiWindows;Import-ChildUiNative
$root=Split-Path $PSScriptRoot -Parent
if([Diagnostics.Process]::GetCurrentProcess().SessionId -ne [OpenPilotChildUi]::WTSGetActiveConsoleSessionId()){throw 'This lane requires the ordinary active parent console, not a private child desktop'}
if(([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Use the ordinary token; only the existing fixed durability broker may elevate'}
if(-not $EvidenceRoot){$EvidenceRoot=Join-Path $root ('build\gesture_modal\'+[Guid]::NewGuid().ToString('N'))}
$null=New-Item -ItemType Directory -Path $EvidenceRoot -ErrorAction Stop
$image=Join-Path $root "build\$Config\drag_tests.exe"
$input=@{schema='openpilot-gesture-modal-input/v1';sources=(Get-ChildUiSnapshot $root);native_image=$image;native_sha256=(Get-FileHash $image).Hash.ToLowerInvariant();broker_sha256=(Get-FileHash (Join-Path $root "build\$Config\folderwing-durable.exe")).Hash.ToLowerInvariant();profile='--gesture-modal';worker_count=1;session=[Diagnostics.Process]::GetCurrentProcess().SessionId;sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;resource='Ordinary parent Default desktop; six fresh Windows source/destination trees; actual modal menus and registered OLE target; no WSL or external clipboard fixture; no source-side physical mouse routing';os=([Environment]::OSVersion.VersionString);retries=0;skips=0}
$inputKey=Get-ChildUiDigest $input;Write-ChildUiJson (Join-Path $EvidenceRoot 'input.json') $input
$process=$null;$birth=0;$failure=$null;$clock=[Diagnostics.Stopwatch]::StartNew()
try {
    $process=Start-Process -FilePath $image -ArgumentList '--gesture-modal' -WorkingDirectory $root -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $EvidenceRoot 'native.log') -RedirectStandardError (Join-Path $EvidenceRoot 'native.stderr.log') -Environment @{OPENPILOT_DRAG_PROFILE=(Join-Path $EvidenceRoot 'profile.csv')}
    $birth=$process.StartTime.ToUniversalTime().Ticks
    Write-ChildUiJson (Join-Path $EvidenceRoot 'launch.json') @{pid=$process.Id;birth_ticks=$birth;image=$image;session=$input.session}
    if(-not $process.WaitForExit(60000)){throw 'Native modal acceptance containment deadline'}
    $process.WaitForExit() # drain redirected output before observing the final CASE row
    $text=Get-Content (Join-Path $EvidenceRoot 'native.log') -Raw
    if($process.ExitCode -ne 0 -or $text -notmatch 'CASE test_gesture_changed_view_modal_context_and_drop assertions=\d+ failures=0' -or @([regex]::Matches($text,'(?m)^N11_MODAL ')).Count -ne 6){throw 'Native modal sequence has failed or missing assertions; retain its owned fixture'}
    if((Get-ChildUiDigest (Get-ChildUiSnapshot $root)) -cne (Get-ChildUiDigest $input.sources) -or (Get-FileHash $image).Hash.ToLowerInvariant() -cne $input.native_sha256 -or (Get-FileHash (Join-Path $root "build\$Config\folderwing-durable.exe")).Hash.ToLowerInvariant() -cne $input.broker_sha256){throw 'Acceptance source/image changed'}
}catch{$failure=$_.Exception.Message}
finally{
    if($process -and -not $process.HasExited){$null=Assert-ChildUiProcess $process.Id $birth $input.session $image;$process.Kill();$process.WaitForExit()}
}
$artifacts=[ordered]@{};foreach($f in Get-ChildItem $EvidenceRoot -File){$artifacts[$f.Name]=(Get-FileHash $f.FullName).Hash.ToLowerInvariant()}
$v=@{schema='openpilot-gesture-modal-result/v1';outcome=$(if($failure){'failed'}else{'passed'});input_key=$inputKey;error=$failure;worker_count=1;wall_ms=$clock.ElapsedMilliseconds;source_mouse_dispatch='Observed stale-dispatch refusal; source DoDragDrop/physical mouse routing not exercised';modal_interactions='Actual ordinary menu, real IDropTarget/CF_HDROP Escape cancellation and Copy here';sequence_cases=6;skips=0;retries=0;native_process_absent=$true;artifacts=$artifacts}
$v.receipt_key=Get-ChildUiDigest $v;Write-ChildUiJson (Join-Path $EvidenceRoot 'FINAL.json') $v
$v|ConvertTo-Json -Depth 4
if($failure){exit 1}
