<# Sign in once; reuse only this repository's live authenticated session.
   Every Run gets a fresh manifest, private desktop, task and receipt. #>
param([ValidateSet('Run','Start','Status','Stop')][string]$Action='Run',
      [ValidateSet('Smoke','Settings','Shell','All')][string]$Profile='Smoke',
      [ValidateSet('release','debug')][string]$Config='release',
      [ValidateRange(5,240)][int]$IdleMinutes=60,[ValidateRange(1,12)][int]$SessionHours=8,
      [switch]$CloseAfterRun,[switch]$BuildOnly)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\child-ui-common.ps1"
. "$PSScriptRoot\child-ui-runtime.ps1"
Initialize-ChildUiWindows;Import-ChildUiNative
$Profile=switch($Profile){Smoke{'Smoke'};Settings{'Settings'};Shell{'Shell'};All{'All'}};$Config=$Config.ToLowerInvariant()
$repo=Get-ChildUiRepository;$self=[Diagnostics.Process]::GetCurrentProcess();$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
if(([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Start the launcher with an ordinary Windows PowerShell 7 token'}
if($self.SessionId -ne [OpenPilotChildUi]::WTSGetActiveConsoleSessionId()){throw 'Launch from the active parent Windows console'}
$clientMutex=[Threading.Mutex]::new($false,('Local\OpenPilotChildUiClient-'+$identity.User.Value));$locked=$false;$session=$null;$pointerPath=Get-ChildUiPointer
function Read-ActiveSession {
    if(-not(Test-Path $pointerPath)){return $null}
    $pointer=Get-Content (Assert-ChildUiPath $pointerPath) -Raw|ConvertFrom-Json
    if($pointer.token -cnotmatch '^[a-f0-9]{32}$' -or $pointer.sid -cne $identity.User.Value -or $pointer.parent_session -ne $self.SessionId -or
       $pointer.root -ine (Join-Path $repo "build\child-ui\$($pointer.token)")){throw 'Active-session pointer identity changed; leave it untouched'}
    $lease=Read-ChildUiLease (Join-Path $pointer.root 'lease.json')
    if(-not $lease.persistent -or $lease.token -cne $pointer.token -or $lease.launcher_pid -ne $pointer.controller_pid -or $lease.launcher_birth_ticks -ne $pointer.controller_birth_ticks){throw 'Active-session controller differs'}
    if(Test-Path (Join-Path $lease.root 'FINAL.json')){
        $final=Get-Content (Join-Path $lease.root 'FINAL.json') -Raw|ConvertFrom-Json
        if(-not $final.feature_restored -or -not $final.child_absent){throw 'Previous session cleanup is blocked; inspect its receipt'}
        $process=Get-Process -Id $lease.launcher_pid -ErrorAction SilentlyContinue
        if($process -and $process.StartTime.ToUniversalTime().Ticks -eq $lease.launcher_birth_ticks){throw 'Previous controller is still closing'}
        if((Get-Content $pointerPath -Raw|ConvertFrom-Json).token -cne $lease.token){throw 'Pointer changed before retirement'}
        Remove-Item -LiteralPath $pointerPath
        return $null
    }
    if(-not(Test-Path (Join-Path $lease.root 'session-ready.json'))){
        $null=Assert-ChildUiProcess $lease.launcher_pid $lease.launcher_birth_ticks $lease.parent_session $lease.powershell
        if($Action -notin @('Status','Stop')){throw 'Owned session is still awaiting Windows sign-in; no second session will be started'}
    }else{$null=Get-ChildUiSession (Join-Path $lease.root 'lease.json')}
    $lease
}
function Stop-Session($Lease){
    $stop=Join-Path $Lease.root 'stop-session.json'
    if(-not(Test-Path $stop)){Write-ChildUiJson $stop @{schema='openpilot-child-ui-stop/v1';session_token=$Lease.token;sid=$identity.User.Value;parent_session=$self.SessionId}}
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path (Join-Path $Lease.root 'FINAL.json'))){if($clock.Elapsed.TotalSeconds -ge 60){throw 'Session cleanup deadline; inspect exact retained identities'};Start-Sleep -Milliseconds 250}
    $final=Get-Content (Join-Path $Lease.root 'FINAL.json') -Raw|ConvertFrom-Json
    if(-not $final.feature_restored -or -not $final.child_absent -or @($final.cleanup|Where-Object outcome -NE 'passed').Count){throw 'Session cleanup blocked; retained receipt identifies the failure'}
    Write-Output "Session closed and feature restored: $($Lease.root)\FINAL.json"
}
try{
    try{$locked=$clientMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
    if(-not $locked){throw 'Another client is preparing a child UI request; try after its admission completes'}
    if($BuildOnly){$cache=Get-ChildUiCache;Write-Output "Launcher compiled and verified: $($cache.path)";return}
    $session=Read-ActiveSession
    if($Action -eq 'Status'){
        if(-not $session){@{phase='absent'}|ConvertTo-Json;return}
        $state=Get-Content (Join-Path $session.root 'state.json') -Raw|ConvertFrom-Json
        $childId=if(Test-Path (Join-Path $session.root 'session-ready.json')){(Get-Content (Join-Path $session.root 'session-ready.json') -Raw|ConvertFrom-Json).child_session}else{$null}
        @{phase=$state.phase;session_token=$session.token;root=$session.root;child_session=$childId;idle_minutes=$session.idle_seconds/60;maximum_hours=($session.host_seconds-30)/3600}|ConvertTo-Json;return
    }
    if($Action -eq 'Stop'){if($session){Stop-Session $session}else{'No owned authenticated session is active'};return}
    $cache=Get-ChildUiCache
    if($session -and $session.cache_key -cne $cache.key){throw 'Launcher control files changed; run -Action Stop before starting the new version'}
    if(-not $session){
        $child=[uint32]0;if([OpenPilotChildUi]::WTSGetChildSessionId([ref]$child) -or $child -ne [uint32]::MaxValue){throw 'An unowned child session exists; never adopt or log it off'}
        $enabled=$false;if(-not [OpenPilotChildUi]::WTSIsChildSessionsEnabled([ref]$enabled)){throw 'Child-session capability unavailable'}
        $token=[Guid]::NewGuid().ToString('N');$root=Assert-ChildUiPath (Join-Path $repo "build\child-ui\$token");$null=New-Item -ItemType Directory $root
        $null=New-Item -ItemType Directory (Join-Path $root 'requests')
        $input=New-ChildUiInput $Profile $Config $cache;Write-ChildUiJson (Join-Path $root 'input.json') $input
        $boot=@{schema='openpilot-child-ui-bootstrap/v1';token=$token;root=$root;sid=$identity.User.Value;profile=$Profile;config=$Config;parent_session=$self.SessionId;original_enabled=$enabled;client_pid=$PID;client_birth_ticks=$self.StartTime.ToUniversalTime().Ticks;powershell=$self.Path;host_image=(Join-Path $cache.path 'ChildUiHost.exe');cache_path=$cache.path;cache_key=$cache.key;input_key=(Get-ChildUiDigest $input);private_desktop=$true;desktop_width=1920;desktop_height=1080;connect_seconds=300;host_seconds=($SessionHours*3600+30);idle_seconds=($IdleMinutes*60)}
        Write-ChildUiJson (Join-Path $root 'bootstrap.json') $boot
        $controller=[OpenPilotChildUi]::StartOwner($self.Path,(Join-Path $cache.path 'child-ui-session.ps1'),(Join-Path $root 'bootstrap.json'),$root)
        Write-ChildUiJson $pointerPath @{token=$token;root=$root;sid=$identity.User.Value;parent_session=$self.SessionId;controller_pid=$controller.Id;controller_birth_ticks=$controller.StartTime.ToUniversalTime().Ticks}
        Write-Output "Authenticating owned session: $root"
        Write-Output 'Complete normal Windows sign-in once; subsequent Run commands reuse this live session.'
        $clock=[Diagnostics.Stopwatch]::StartNew()
        while(-not(Test-Path (Join-Path $root 'lease.json'))){if($controller.HasExited -or $clock.Elapsed.TotalSeconds -gt 30){throw 'Controller did not publish its owned lease'};Start-Sleep -Milliseconds 100}
        $clientMutex.ReleaseMutex();$locked=$false
        while(-not(Test-Path (Join-Path $root 'session-ready.json'))){
            if($controller.HasExited -or (Test-Path (Join-Path $root 'FINAL.json'))){throw "Session startup failed; inspect $root"}
            if($clock.Elapsed.TotalSeconds -ge 330){throw "Session sign-in deadline; inspect $root"}
            Start-Sleep -Milliseconds 250
        }
        # Authentication releases admission so Status/Stop stay usable. Re-enter
        # before inspecting idle state or publishing the first Run request.
        try{$locked=$clientMutex.WaitOne(0)}catch [Threading.AbandonedMutexException]{$locked=$true}
        if(-not $locked){throw 'Another client is preparing a child UI request; try after its admission completes'}
        $session=Read-ActiveSession
        if(-not $session){throw 'Owned session closed after authentication; no request was published'}
    }
    if($Action -eq 'Start'){Write-Output "Authenticated session ready: $($session.root)";return}
    $state=Get-Content (Join-Path $session.root 'state.json') -Raw|ConvertFrom-Json
    if($state.phase -cne 'idle'){throw 'Owned session is busy or closing; concurrent acceptance is refused'}
    $token=[Guid]::NewGuid().ToString('N');$root=Assert-ChildUiPath (Join-Path $repo "build\child-ui\$token");$null=New-Item -ItemType Directory $root
    $input=New-ChildUiInput $Profile $Config $cache
    $input.session_token=$session.token;$input.session_ready_sha256=(Get-FileHash (Join-Path $session.root 'session-ready.json')).Hash.ToLowerInvariant()
    Write-ChildUiJson (Join-Path $root 'input.json') $input
    $lease=@{schema='openpilot-child-ui/v1';token=$token;root=$root;sid=$session.sid;profile=$Profile;config=$Config;parent_session=$session.parent_session;powershell=$self.Path;host_image=$session.host_image;input_key=(Get-ChildUiDigest $input);private_desktop=$true;session_root=$session.root;session_token=$session.token}
    Write-ChildUiJson (Join-Path $root 'lease.json') $lease
    $ready=Get-Content (Join-Path $session.root 'host-ready.json') -Raw|ConvertFrom-Json -AsHashtable
    $ready.token=$token;$ready.session_token=$session.token;Write-ChildUiJson (Join-Path $root 'host-ready.json') $ready
    $request=@{schema='openpilot-child-ui-request/v1';session_token=$session.token;run_token=$token;run_root=$root;input_key=$lease.input_key;client_pid=$PID;client_birth_ticks=$self.StartTime.ToUniversalTime().Ticks}
    Write-ChildUiJson (Join-Path $session.root "requests\$token.json") $request
    Write-Output "Running $Profile with authenticated session $($session.token): $root"
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(-not(Test-Path (Join-Path $session.root "accepted-$token.json"))){
        $null=Get-ChildUiSession (Join-Path $session.root 'lease.json')
        if($clock.Elapsed.TotalSeconds -ge 30){throw 'Run admission deadline; do not republish this request'}
        Start-Sleep -Milliseconds 100
    }
    $clientMutex.ReleaseMutex();$locked=$false
    while(-not(Test-Path (Join-Path $root 'FINAL.json'))){
        if(Test-Path (Join-Path $session.root 'FINAL.json')){throw 'Session ended with an unsealed run; inspect its blocked cleanup evidence'}
        $null=Assert-ChildUiProcess $session.launcher_pid $session.launcher_birth_ticks $session.parent_session $session.powershell
        if($clock.Elapsed.TotalSeconds -ge 1540){throw 'Run result outer deadline; inspect the owned session'}
        Start-Sleep -Milliseconds 250
    }
    $final=Get-Content (Join-Path $root 'FINAL.json') -Raw|ConvertFrom-Json
    if($final.token -cne $token -or $final.input_key -cne $lease.input_key){throw 'Run receipt identity changed'}
    Write-Output "$($final.outcome) — $root\FINAL.json"
    if($CloseAfterRun){Stop-Session $session}
    if($final.outcome -cne 'passed'){exit 1}
}finally{if($locked){$clientMutex.ReleaseMutex()};$clientMutex.Dispose()}
