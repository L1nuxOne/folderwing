# Frozen ordinary-user session owner; accepts fixed-profile requests only.
param([Parameter(Mandatory)][string]$BootstrapPath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\child-ui-common.ps1"
. "$PSScriptRoot\child-ui-runtime.ps1"
Initialize-ChildUiWindows;Import-ChildUiNative
$repo=Get-ChildUiRepository;$self=[Diagnostics.Process]::GetCurrentProcess();$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$session=$null;$hostProcess=$null;$enabledByOwner=$false;$locked=$false;$failure=$null;$cleanup=@();$runs=@();$reason='startup-failed';$bootstrapOwned=$false
$lifetime=[Diagnostics.Stopwatch]::StartNew();$idle=[Diagnostics.Stopwatch]::StartNew()
$mutex=[Threading.Mutex]::new($false,('Local\OpenPilotChildUi-'+$sid))
try{
    $path=Assert-ChildUiPath $BootstrapPath
    $boot=Get-Content $path -Raw|ConvertFrom-Json -AsHashtable
    if($boot.schema -cne 'openpilot-child-ui-bootstrap/v1' -or $boot.token -cnotmatch '^[a-f0-9]{32}$' -or
       $boot.root -ine (Join-Path $repo "build\child-ui\$($boot.token)") -or $path -ine (Join-Path $boot.root 'bootstrap.json') -or
       $boot.sid -cne $sid -or $boot.parent_session -ne $self.SessionId -or [OpenPilotChildUi]::WTSGetActiveConsoleSessionId() -ne $self.SessionId){throw 'Unknown parent/bootstrap identity'}
    $bootstrapOwned=$true
    if($boot.idle_seconds -lt 300 -or $boot.idle_seconds -gt 14400 -or $boot.host_seconds -lt 3600 -or $boot.host_seconds -gt 43230){throw 'Unbounded session lifetime refused'}
    if($boot.cache_path -ine $PSScriptRoot -or $boot.cache_key -cne (Split-Path $PSScriptRoot -Leaf)){throw 'Controller must run from its frozen cache'}
    $meta=Get-Content (Join-Path $PSScriptRoot 'cache.json') -Raw|ConvertFrom-Json
    if($meta.key -cne $boot.cache_key){throw 'Frozen controller cache changed'}
    foreach($entry in $meta.images.PSObject.Properties){if((Get-FileHash (Join-Path $PSScriptRoot $entry.Name)).Hash.ToLowerInvariant() -cne $entry.Value){throw 'Frozen controller image changed'}}
    $null=Assert-ChildUiProcess $boot.client_pid $boot.client_birth_ticks $self.SessionId $boot.powershell
    try{$locked=$mutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(-not $locked){throw 'Another launcher already owns the child-session feature'}
    $child=[uint32]0
    if([OpenPilotChildUi]::WTSGetChildSessionId([ref]$child) -or $child -ne [uint32]::MaxValue){throw 'Existing child session; never adopt or log it off'}
    $original=$false;if(-not [OpenPilotChildUi]::WTSIsChildSessionsEnabled([ref]$original) -or $original -ne $boot.original_enabled){throw 'Feature changed before session ownership'}
    $session=$boot.Clone();$session.schema='openpilot-child-ui/v1';$session.persistent=$true
    $session.launcher_pid=$PID;$session.launcher_birth_ticks=$self.StartTime.ToUniversalTime().Ticks
    $null=$session.Remove('client_pid');$null=$session.Remove('client_birth_ticks')
    Write-ChildUiJson (Join-Path $session.root 'lease.json') $session
    Set-ChildUiState $session 'starting'
    if(-not $original){$enabledByOwner=$true;Invoke-ChildUiFeature $session Enable $PSScriptRoot}
    $hostProcess=Start-ChildUiProgram $session.host_image @((Join-Path $session.root 'lease.json')) ''
    Write-ChildUiJson (Join-Path $session.root 'host-launch.json') @{pid=$hostProcess.Id;birth_ticks=$hostProcess.StartTime.ToUniversalTime().Ticks;image=$session.host_image;session=$self.SessionId}
    $connect=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path (Join-Path $session.root 'host-ready.json'))){
        if($hostProcess.HasExited){throw 'Child host exited before authentication'}
        $null=Assert-ChildUiProcess $boot.client_pid $boot.client_birth_ticks $self.SessionId $boot.powershell
        if(Test-Path (Join-Path $session.root 'stop-session.json')){throw 'Explicit stop before authentication'}
        if($connect.Elapsed.TotalSeconds -ge 310){throw 'Normal Windows sign-in deadline expired'}
        Start-Sleep -Milliseconds 250
    }
    $hostFacts=Get-Content (Join-Path $session.root 'host-ready.json') -Raw|ConvertFrom-Json
    $ready=@{token=$session.token;cache_key=$session.cache_key;sid=$sid;parent_session=$self.SessionId;child_session=$hostFacts.child_session;controller_pid=$PID;controller_birth_ticks=$self.StartTime.ToUniversalTime().Ticks;controller_image=$self.Path;host_pid=$hostFacts.pid;host_birth_ticks=$hostFacts.birth_ticks}
    Assert-ChildUiSessionBinding ([pscustomobject]$session) ([pscustomobject]$ready) $hostFacts
    Write-ChildUiJson (Join-Path $session.root 'session-ready.json') $ready
    Set-ChildUiState $session 'idle';$idle.Restart();$reason='explicit-stop'
    while($true){
        if(Test-Path (Join-Path $session.root 'stop-session.json')){break}
        if($idle.Elapsed.TotalSeconds -ge $session.idle_seconds){$reason='idle-expired';break}
        if($lifetime.Elapsed.TotalSeconds -ge $session.host_seconds-30){$reason='maximum-lifetime';break}
        $null=Get-ChildUiSession (Join-Path $session.root 'lease.json')
        $request=Get-ChildItem -LiteralPath (Join-Path $session.root 'requests') -File -Filter '*.json'|Where-Object {-not(Test-Path (Join-Path $session.root ('accepted-'+$_.BaseName+'.json')))}|Sort-Object Name|Select-Object -First 1
        if($request){
            $message=Get-Content $request.FullName -Raw|ConvertFrom-Json
            $lease=Assert-ChildUiRequest $message $session $request.Name
            $null=Assert-ChildUiProcess $message.client_pid $message.client_birth_ticks $session.parent_session $session.powershell
            Set-ChildUiState $session 'running' $lease.token
            Write-ChildUiJson (Join-Path $session.root ('accepted-'+$lease.token+'.json')) @{token=$lease.token;input_key=$lease.input_key;session_token=$session.token}
            $run=Invoke-ChildUiRun $session $lease;$runs+=,$run
            if(-not $run.reusable){
                if($run.outcome -eq 'cancelled' -and (Test-Path (Join-Path $session.root 'stop-session.json'))){$reason='explicit-stop';break}
                $reason='run-infrastructure-failure';throw 'Run left the authenticated session unqualified for reuse'
            }
            Set-ChildUiState $session 'idle';$idle.Restart()
        }
        Start-Sleep -Milliseconds 250
    }
}catch{$failure=$_.Exception.Message}
finally{
    if($session){
        Set-ChildUiState $session 'closing'
        if($hostProcess){
            try{
                $forced=$false
                if(-not $hostProcess.HasExited){
                    [IO.File]::WriteAllText((Join-Path $session.root 'stop-host'),'owned session cleanup')
                    if(-not $hostProcess.WaitForExit(30000)){
                        $current=[uint32]0
                        if([OpenPilotChildUi]::WTSGetChildSessionId([ref]$current) -or $current -ne [uint32]::MaxValue){throw 'Host cleanup deadline with a child; no process termination or adoption'}
                        $launch=Get-Content (Join-Path $session.root 'host-launch.json') -Raw|ConvertFrom-Json
                        $null=Assert-ChildUiProcess $launch.pid $launch.birth_ticks $session.parent_session $session.host_image
                        $hostProcess.Kill();if(-not $hostProcess.WaitForExit(5000)){throw 'Exact pending host did not exit'};$forced=$true
                    }
                }
                $hostResult=Get-Content (Join-Path $session.root 'host-result.json') -Raw|ConvertFrom-Json
                if($hostResult.outcome -cne 'completed' -and -not($forced -and $hostResult.child_session -eq [uint32]::MaxValue)){throw 'Host cleanup blocked; inspect exact captured identities'}
                $cleanup+=@{phase='host';outcome='passed';result=$hostResult;exact_pending_host_terminated=$forced}
            }catch{$cleanup+=@{phase='host';outcome='blocked';error=$_.Exception.Message}}
        }
        if($enabledByOwner){try{Invoke-ChildUiFeature $session Restore $PSScriptRoot;$cleanup+=@{phase='feature';outcome='passed'}}catch{$cleanup+=@{phase='feature';outcome='blocked';error=$_.Exception.Message}}}
        $cleanup+=@{phase='run-tasks';outcome=$(if(@($runs|Where-Object {-not $_.task_cleanup_passed}).Count){'blocked'}else{'passed'})}
        $after=$false;$known=[OpenPilotChildUi]::WTSIsChildSessionsEnabled([ref]$after);$child=[uint32]0;$hasChild=[OpenPilotChildUi]::WTSGetChildSessionId([ref]$child)
        $clean=$known -and $after -eq $session.original_enabled -and -not $hasChild -and $child -eq [uint32]::MaxValue -and -not @($cleanup|Where-Object outcome -NE 'passed').Count
        if(-not $hasChild -and $child -eq [uint32]::MaxValue){
            foreach($run in $runs|Where-Object {-not $_.reusable}){
                $pending=Get-Content (Join-Path $run.root 'pending-run.json') -Raw|ConvertFrom-Json -AsHashtable
                $pending.session_cleanup_completed=$clean
                Write-ChildUiReceipt $run.root $pending
            }
        }
        Set-ChildUiState $session $(if($clean){'closed'}else{'blocked'})
        Write-ChildUiReceipt $session.root @{schema='openpilot-child-ui-session-receipt/v1';outcome=$(if($clean -and -not $failure){'passed'}else{'blocked'});token=$session.token;reason=$reason;error=$failure;cleanup=$cleanup;runs=$runs;feature_restored=($known -and $after -eq $session.original_enabled);child_absent=(-not $hasChild -and $child -eq [uint32]::MaxValue);wall_ms=$lifetime.ElapsedMilliseconds;cache_key=$session.cache_key}
    }elseif($bootstrapOwned -and -not(Test-Path (Join-Path $boot.root 'startup-blocked.json'))){Write-ChildUiJson (Join-Path $boot.root 'startup-blocked.json') @{outcome='blocked';error=$failure}}
    if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()
}
if($failure){exit 1}
