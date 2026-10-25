# Only the documented feature flag may change here. No command/task/session
# creation or credential/policy interface is accepted by this elevated capsule.
param([Parameter(Mandatory)][string]$LeasePath,[Parameter(Mandatory)][ValidateSet('Enable','Restore')][string]$Action)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\child-ui-common.ps1"
Initialize-ChildUiWindows;Import-ChildUiNative
$lease=Read-ChildUiLease $LeasePath
try{
    $principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if(-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Child-session feature change requires the normal Windows administrator prompt'}
    if([Diagnostics.Process]::GetCurrentProcess().SessionId -ne $lease.parent_session -or [OpenPilotChildUi]::WTSGetActiveConsoleSessionId() -ne $lease.parent_session){throw 'Original parent console changed'}
    $child=[uint32]0
    if([OpenPilotChildUi]::WTSGetChildSessionId([ref]$child) -or $child -ne [uint32]::MaxValue){throw 'Existing child session; feature change refused'}
    $before=$false;if(-not [OpenPilotChildUi]::WTSIsChildSessionsEnabled([ref]$before)){throw 'Feature state unavailable'}
    if($lease.original_enabled){throw 'Originally enabled feature must remain untouched'}
    if($Action -eq 'Enable'){
        $null=Assert-ChildUiProcess $lease.launcher_pid $lease.launcher_birth_ticks $lease.parent_session $lease.powershell
        $null=Assert-ChildUiInputs $lease
        if($before){throw 'Feature changed before owned enable'}
    }
    $wanted=$Action -eq 'Enable'
    if($before -ne $wanted -and -not [OpenPilotChildUi]::WTSEnableChildSessions($wanted)){throw ('Feature API failed: '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())}
    $after=$false;if(-not [OpenPilotChildUi]::WTSIsChildSessionsEnabled([ref]$after) -or $after -ne $wanted){throw 'Feature readback differs'}
    Write-ChildUiJson (Join-Path $lease.root "$Action-result.json") @{outcome='passed';before=$before;after=$after;action=$Action;token=$lease.token}
}catch{
    Write-ChildUiJson (Join-Path $lease.root "$Action-blocked.json") @{outcome='blocked';reason=$_.Exception.Message;action=$Action;token=$lease.token}
    exit 1
}
