# Opt-in geometry only: ordinary desktop runs never enter this function.
function Set-ChildUiPlacement($Process,[IntPtr]$Window) {
    . "$PSScriptRoot\..\tools\child-ui-common.ps1"
    $lease=Read-ChildUiLease $env:OPENPILOT_CHILD_UI_LEASE
    $ready=Assert-ChildUiWorker $lease
    $null=Assert-ChildUiProcess $Process.Id $Process.StartTime.ToUniversalTime().Ticks $ready.child_session $Process.Path
    [FpUi]::OwnedWindow($Window,[uint32]$Process.Id)
    $rect=[FpUi+RECT]::new();if(-not [FpUi]::GetWindowRect($Window,[ref]$rect)){throw 'Owned child window geometry unavailable'}
    $monitor=[OpenPilotChildUi+MONITOR]::new();$monitor.Size=[Runtime.InteropServices.Marshal]::SizeOf($monitor)
    if(-not [OpenPilotChildUi]::GetMonitorInfoW([OpenPilotChildUi]::MonitorFromWindow($Window,2),[ref]$monitor)){throw 'Child work area unavailable'}
    $width=$rect.R-$rect.L;$height=$rect.B-$rect.T;$area=$monitor.Work
    if($width -gt $area.R-$area.L -or $height -gt $area.B-$area.T){throw 'Child desktop cannot contain the unchanged test window size'}
    if(-not [FpUi]::MoveWindow($Window,$area.L,$area.T,$width,$height,$true)){throw 'Child test placement failed'}
    $after=[FpUi+RECT]::new();$null=[FpUi]::GetWindowRect($Window,[ref]$after)
    if($after.L -ne $area.L -or $after.T -ne $area.T -or $after.R-$after.L -ne $width -or $after.B-$after.T -ne $height){throw 'Child placement readback differs'}
    @{pid=$Process.Id;birth_ticks=$Process.StartTime.ToUniversalTime().Ticks;session=$Process.SessionId;hwnd=$Window.ToInt64();before=$rect;after=$after;resized=$false;focus_requested=$false}|ConvertTo-Json -Compress -Depth 5|Add-Content (Join-Path $lease.root 'placement.jsonl')
}
