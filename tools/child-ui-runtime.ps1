# Frozen controller helpers and fixed-profile dispatch. No effect on dot-source.
function Start-ChildUiProgram([string]$Image,[string[]]$Arguments,[string]$OutStem,[switch]$Wait){
    $si=[Diagnostics.ProcessStartInfo]::new($Image);$si.UseShellExecute=$false;$si.WorkingDirectory=(Get-ChildUiRepository)
    if($Wait){$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true}
    foreach($a in $Arguments){$si.ArgumentList.Add($a)}
    $p=[Diagnostics.Process]::Start($si)
    if($Wait){
        $stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
        if(-not $p.WaitForExit(120000)){$p.Kill();throw 'Local launcher compiler exceeded 120 seconds'}
        [IO.File]::WriteAllText("$OutStem.log",$stdout.GetAwaiter().GetResult());[IO.File]::WriteAllText("$OutStem.stderr.log",$stderr.GetAwaiter().GetResult())
        if($p.ExitCode -ne 0){throw "Local compiler failed; see $OutStem.log"}
    }
    $p
}

function Get-ChildUiCache {
    $repo=Get-ChildUiRepository;$tools=Join-Path $repo 'tools'
    $controlFiles=@('child-ui.ps1','child-ui-session.ps1','child-ui-runtime.ps1','child-ui-common.ps1','child-ui-feature.ps1','child-ui-worker.ps1','child-ui-host.cs','child-ui-agent.cs')
    $frozenScripts=@('child-ui-session.ps1','child-ui-runtime.ps1','child-ui-common.ps1','child-ui-feature.ps1')
    $windows=[Environment]::GetFolderPath('Windows')
    $compiler=Join-Path $windows 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    $typeLib=Join-Path $windows 'System32\mstscax.dll'
    $sdk=Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs\Windows\v10.0A\bin'
    $importer=Get-ChildItem -LiteralPath $sdk -Filter TlbImp.exe -Recurse|Where-Object FullName -Match '[\\/]NETFX 4\.8 Tools[\\/]x64[\\/]'|Sort-Object FullName -Descending|Select-Object -First 1
    if(-not $importer){throw 'Install the Windows SDK .NET Framework 4.8 x64 tools (TlbImp.exe)'}
    $buildInputs=[ordered]@{}
    foreach($path in (@($compiler,$typeLib,$importer.FullName)+@($controlFiles|ForEach-Object {Join-Path $tools $_}))){$buildInputs[$path]=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
    $buildKey=Get-ChildUiDigest $buildInputs
    $cache=Assert-ChildUiPath (Join-Path $repo "build\child-ui-cache\$buildKey")
    $cached=Join-Path $cache 'cache.json'
    if(Test-Path $cache){
        if(-not(Test-Path $cached)){throw "Incomplete launcher cache retained for review: $cache"}
        $meta=Get-Content $cached -Raw|ConvertFrom-Json
        if($meta.key -cne $buildKey){throw 'Launcher cache identity differs'}
        foreach($name in (@('ChildUiHost.exe','ChildUiAgent.exe','OpenPilotRdp.dll')+$frozenScripts)){if((Get-FileHash (Join-Path $cache $name)).Hash.ToLowerInvariant() -cne $meta.images.$name){throw 'Launcher cache image changed'}}
    }else{
        $null=New-Item -ItemType Directory -Path $cache
        $null=Start-ChildUiProgram $importer.FullName @($typeLib,"/out:$cache\OpenPilotRdp.dll",'/namespace:OpenPilotRdp','/silent') (Join-Path $cache 'import') -Wait
        $null=Start-ChildUiProgram $compiler @('/nologo','/target:winexe','/platform:x64',"/out:$cache\ChildUiHost.exe",'/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll','/reference:System.Web.Extensions.dll',"/reference:$cache\OpenPilotRdp.dll",(Join-Path $tools 'child-ui-host.cs')) (Join-Path $cache 'compile') -Wait
        $null=Start-ChildUiProgram $compiler @('/nologo','/target:exe','/platform:x64',"/out:$cache\ChildUiAgent.exe",'/reference:System.Web.Extensions.dll',(Join-Path $tools 'child-ui-agent.cs')) (Join-Path $cache 'agent-compile') -Wait
        foreach($name in $frozenScripts){Copy-Item -LiteralPath (Join-Path $tools $name) -Destination (Join-Path $cache $name)}
        $hashes=[ordered]@{};foreach($name in (@('ChildUiHost.exe','ChildUiAgent.exe','OpenPilotRdp.dll')+$frozenScripts)){$hashes[$name]=(Get-FileHash (Join-Path $cache $name)).Hash.ToLowerInvariant()}
        Write-ChildUiJson $cached @{key=$buildKey;inputs=$buildInputs;images=$hashes}
    }

    $meta=Get-Content $cached -Raw|ConvertFrom-Json
    [pscustomobject]@{path=$cache;key=$buildKey;images=@($meta.images.PSObject.Properties.Name)}
}
function New-ChildUiInput([string]$Profile,[string]$Config,$Cache) {
    $repo=Get-ChildUiRepository;$self=[Diagnostics.Process]::GetCurrentProcess();$identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $hostImage=Join-Path $Cache.path 'ChildUiHost.exe'
    $images=[ordered]@{}
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $repo "build\$Config") -File){$images[[IO.Path]::GetRelativePath($repo,$file.FullName).Replace('\','/')]=(Get-FileHash $file.FullName).Hash.ToLowerInvariant()}
    foreach($name in $Cache.images){$path=Join-Path $Cache.path $name;$images[[IO.Path]::GetRelativePath($repo,$path).Replace('\','/')]=(Get-FileHash $path).Hash.ToLowerInvariant()}
    if(-not(Test-Path (Join-Path $repo "build\$Config\folderwing.exe"))){throw 'Build OpenPilot first'}
    $sources=Get-ChildUiSnapshot $repo
    $os=Get-CimInstance Win32_OperatingSystem
    $revision=(Get-Content (Join-Path $repo '.git\HEAD') -Raw).Trim()
    if($revision.StartsWith('ref: ')){
        $ref=$revision.Substring(5);$loose=Join-Path $repo ('.git\'+$ref)
        if(Test-Path $loose){$revision=(Get-Content $loose -Raw).Trim()}else{$revision=@(Get-Content (Join-Path $repo '.git\packed-refs')|Where-Object {$_ -match ('^[a-f0-9]+ '+[regex]::Escape($ref)+'$')})[0].Split(' ')[0]}
    }
    $input=@{schema='openpilot-child-ui-input/v1';sources=$sources;images=$images;profile=$Profile;config=$Config;build_key=$Cache.key;powershell=$self.Path;powershell_sha256=(Get-FileHash $self.Path).Hash.ToLowerInvariant();parent_session=$self.SessionId;sid=$identity.User.Value;worker_count=1;desktop=@{width=1920;height=1080};retries=0;skips=0;phase_deadlines='unchanged in standing scripts';resource=@{free_physical_kib=$os.FreePhysicalMemory;total_physical_kib=$os.TotalVisibleMemorySize;cpu_load_percent=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);network='loopback RDP only; no downloads';containers='not modified';database='none';scheduling='serial one worker'};os=[Environment]::OSVersion.VersionString;revision=$revision;bindings_sha256=(Get-FileHash (Join-Path $repo 'tests\native-impact.json')).Hash.ToLowerInvariant();selected_profiles=@{Smoke='tests/ui_smoke.ps1, native Explorer enabled';Settings='tests/settings_ui.ps1: N08,N10-same,N10-count,N09,N12-same,N12-switch,N12-reopen,N12-reload';Shell='tests/shell_ui.ps1'};source_digest=(Get-ChildUiDigest $sources)}

    $input
}
function Invoke-ChildUiFeature($Lease,[string]$Action,[string]$CachePath) {
    $args='-NoProfile -File "'+(Join-Path $CachePath 'child-ui-feature.ps1')+'" -LeasePath "'+(Join-Path $Lease.root 'lease.json')+'" -Action '+$Action
    $p=Start-Process -FilePath $Lease.powershell -ArgumentList $args -Verb RunAs -PassThru -Wait
    $result=Join-Path $Lease.root "$Action-result.json"
    if($p.ExitCode -ne 0 -or -not(Test-Path $result)){throw "Feature $Action failed or was cancelled; inspect the session evidence"}
    $value=Get-Content $result -Raw|ConvertFrom-Json
    if($value.outcome -cne 'passed' -or $value.token -cne $Lease.token){throw "Feature $Action acknowledgement differs"}
}
function Get-ChildUiPointer {
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    Join-Path (Get-ChildUiRepository) "build\child-ui\active-$sid-$([Diagnostics.Process]::GetCurrentProcess().SessionId).json"
}
function Set-ChildUiState($Session,[string]$Phase,[string]$RunToken='') {
    $value=@{token=$Session.token;phase=$Phase;run_token=$RunToken;controller_pid=$PID;updated_ticks=[DateTime]::UtcNow.Ticks}
    $path=Join-Path $Session.root 'state.json';$temporary=$path+'.'+[Guid]::NewGuid().ToString('N')
    Write-ChildUiJson $temporary $value;[IO.File]::Move($temporary,$path,$true)
}
function Remove-ChildUiTask($Folder,[string]$Name,[string]$Xml,[string]$InstanceGuid) {
    $task=$Folder.GetTask($Name)
    $active=@(Assert-ChildUiTaskIdentity $task $Xml $InstanceGuid)
    foreach($one in $active){$one.Stop()}
    $clock=[Diagnostics.Stopwatch]::StartNew()
    while(@($task.GetInstances(0)).Count -and $clock.ElapsedMilliseconds -lt 5000){Start-Sleep -Milliseconds 100}
    if(@($task.GetInstances(0)).Count){throw 'Owned task instance did not stop'}
    if($Folder.GetTask($Name).Xml -cne $Xml){throw 'Task changed before deletion'}
    $Folder.DeleteTask($Name,0)
}
function Write-ChildUiReceipt([string]$Root,$Value) {
    $artifacts=[ordered]@{}
    $unsealed=@()
    foreach($f in Get-ChildItem -LiteralPath $Root -Recurse -File|Sort-Object FullName){
        $relative=[IO.Path]::GetRelativePath($Root,$f.FullName).Replace('\','/')
        if($relative -in @('controller.log','controller.stderr.log')){$unsealed+=$relative;continue}
        $artifacts[$relative]=(Get-FileHash $f.FullName).Hash.ToLowerInvariant()
    }
    if($unsealed.Count){$Value.unsealed_process_logs=$unsealed}
    $Value.artifacts=$artifacts;$Value.receipt_key=Get-ChildUiDigest $Value
    Write-ChildUiJson (Join-Path $Root 'FINAL.json') $Value
}
function Assert-ChildUiRequest($Request,$Session,[string]$FileName) {
    if($Request.schema -cne 'openpilot-child-ui-request/v1' -or $Request.session_token -cne $Session.token -or
       $Request.run_token -cnotmatch '^[a-f0-9]{32}$' -or $FileName -cne ($Request.run_token+'.json')){throw 'Unknown or replayed session request identity'}
    $expected=Join-Path (Get-ChildUiRepository) "build\child-ui\$($Request.run_token)"
    if($Request.run_root -ine $expected){throw 'Request escapes owned run root'}
    $run=Read-ChildUiLease (Join-Path $expected 'lease.json')
    if($run.session_token -cne $Session.token -or $run.session_root -ine $Session.root -or $run.input_key -cne $Request.input_key){throw 'Request/run/session binding differs'}
    if(Test-Path (Join-Path $run.root 'FINAL.json')){throw 'Run already has a receipt; replay refused'}
    $run
}
function Invoke-ChildUiRun($Session,$Lease) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$failure=$null;$cancelled=$false;$registered=$null;$xml=$null;$instance=$null;$cleanup=@();$reusable=$false;$result=$null;$agentResult=$null
    try{
        $null=Get-ChildUiSession (Join-Path $Session.root 'lease.json')
        $ready=Get-Content (Join-Path $Lease.root 'host-ready.json') -Raw|ConvertFrom-Json
        $null=Assert-ChildUiReusableRun $Lease $ready
        $null=Assert-ChildUiInputs $Lease
        $service=New-Object -ComObject Schedule.Service;$service.Connect();$folder=$service.GetFolder('\');$definition=$service.NewTask(0)
        $definition.RegistrationInfo.Description="Owned OpenPilot acceptance $($Lease.token); reusable session $($Session.token); no triggers/password/elevation"
        $definition.Principal.UserId=$Session.sid;$definition.Principal.LogonType=3;$definition.Principal.RunLevel=0
        $definition.Settings.Enabled=$true;$definition.Settings.AllowDemandStart=$true;$definition.Settings.ExecutionTimeLimit='PT25M';$definition.Settings.DisallowStartIfOnBatteries=$false;$definition.Settings.StopIfGoingOnBatteries=$false
        $action=$definition.Actions.Create(0);$action.Path=Join-Path $Session.cache_path 'ChildUiAgent.exe';$action.WorkingDirectory=Get-ChildUiRepository
        $action.Arguments='"'+(Join-Path $Lease.root 'lease.json')+'"'
        $name='OpenPilotChildUi-'+$Lease.token
        $registered=$folder.RegisterTaskDefinition($name,$definition,2,$Session.sid,$null,3,$null)
        $xml=$registered.Xml;[IO.File]::WriteAllText((Join-Path $Lease.root 'owned-task.xml'),$xml)
        $instance=$registered.RunEx($null,4,$ready.child_session,$null)
        Write-ChildUiJson (Join-Path $Lease.root 'task-launch.json') @{name=$name;instance=$instance.InstanceGuid;child_session=$ready.child_session;logon_type=3;run_level=0}
        while(-not(Test-Path (Join-Path $Lease.root 'worker-result.json'))){
            if(Test-Path (Join-Path $Session.root 'stop-session.json')){throw [OperationCanceledException]::new('Explicit session stop during acceptance')}
            $null=Get-ChildUiSession (Join-Path $Session.root 'lease.json')
            if($clock.Elapsed.TotalSeconds -ge 1500){throw 'Acceptance outer containment deadline exceeded'}
            $instance.Refresh()
            if($instance.State -notin @(2,4) -and $clock.Elapsed.TotalSeconds -gt 10){throw 'Child worker ended without a result'}
            Start-Sleep -Milliseconds 500
        }
        $result=Get-Content (Join-Path $Lease.root 'worker-result.json') -Raw|ConvertFrom-Json
        while(-not(Test-Path (Join-Path $Lease.root 'agent-result.json')) -and $clock.Elapsed.TotalSeconds -lt 1500){
            if(Test-Path (Join-Path $Session.root 'stop-session.json')){throw [OperationCanceledException]::new('Explicit session stop during bootstrap cleanup')}
            Start-Sleep -Milliseconds 100
        }
        $agentResult=Get-Content (Join-Path $Lease.root 'agent-result.json') -Raw|ConvertFrom-Json
        if($agentResult.outcome -cne 'completed' -or -not $agentResult.desktop_restored){throw 'Private desktop bootstrap/cleanup failed'}
        if($result.token -cne $Lease.token -or $result.input_key -cne $Lease.input_key -or $result.residual -or $result.outcome -cnotin @('passed','failed')){throw 'Worker result identity/residue/infrastructure failure'}
        $null=Assert-ChildUiInputs $Lease
        $null=Get-ChildUiSession (Join-Path $Session.root 'lease.json')
        $reusable=$true
    }catch{$failure=$_.Exception.Message;$cancelled=$_.Exception -is [OperationCanceledException]}
    finally{
        if($registered -and $xml){
            try{Remove-ChildUiTask $folder $name $xml $(if($instance){$instance.InstanceGuid}else{''});$cleanup+=@{phase='task';outcome='passed';name=$name}}
            catch{$cleanup+=@{phase='task';outcome='blocked';error=$_.Exception.Message};$reusable=$false}
        }
        $outcome=if($cancelled){'cancelled'}elseif($failure){'blocked'}elseif(-not $reusable){'blocked'}elseif($result.outcome -eq 'passed'){'passed'}else{'failed'}
        $receipt=@{schema='openpilot-child-ui-receipt/v2';token=$Lease.token;input_key=$Lease.input_key;profile=$Lease.profile;outcome=$outcome;error=$failure;cleanup=$cleanup;session_token=$Session.token;session_root=$Session.root;session_retained=$reusable;session_cleanup_receipt=(Join-Path $Session.root 'FINAL.json');worker_count=1;wall_ms=$clock.ElapsedMilliseconds;retries=0;skips=$(if($result){$result.skips}else{0})}
        if($reusable){Write-ChildUiReceipt $Lease.root $receipt}
        else{Write-ChildUiJson (Join-Path $Lease.root 'pending-run.json') $receipt}
    }
    [pscustomobject]@{outcome=$outcome;reusable=$reusable;run_token=$Lease.token;root=$Lease.root;task_cleanup_passed=(-not @($cleanup|Where-Object outcome -NE 'passed').Count)}
}
