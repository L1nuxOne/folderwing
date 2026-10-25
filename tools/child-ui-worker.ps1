# Fixed acceptance profiles only. Task Scheduler dispatches this with the
# existing interactive token into the exact child session; no credentials.
param([Parameter(Mandatory)][string]$LeasePath)
$ErrorActionPreference='Stop'
. "$PSScriptRoot\child-ui-common.ps1"
Initialize-ChildUiWindows
$lease=Read-ChildUiLease $LeasePath
$root=Split-Path $PSScriptRoot -Parent
$clock=[Diagnostics.Stopwatch]::StartNew();$lanes=@();$errorMessage=$null
function ChildApps {
    @(Get-Process folderwing,folderwing-settings,folderwing-image,folderwing-pdf,folderwing-operations,folderwing-text,folderwing-shell,folderwing-terminal -ErrorAction SilentlyContinue|Where-Object SessionId -EQ ([Diagnostics.Process]::GetCurrentProcess().SessionId)|ForEach-Object {@{pid=$_.Id;birth_ticks=$_.StartTime.ToUniversalTime().Ticks;image=$_.Path;session=$_.SessionId}})
}
try {
    $ready=Assert-ChildUiWorker $lease
    $null=Assert-ChildUiInputs $lease
    if((ChildApps).Count){throw 'Existing child OpenPilot processes; never adopt them'}
    $env:OPENPILOT_CHILD_UI_LEASE=$LeasePath
    Write-ChildUiJson (Join-Path $lease.root 'worker-ready.json') @{pid=$PID;session=$ready.child_session;token=$lease.token;birth_ticks=[Diagnostics.Process]::GetCurrentProcess().StartTime.ToUniversalTime().Ticks}
    $cases=@()
    if($lease.profile -in @('Smoke','All')){$cases+='Smoke'}
    if($lease.profile -in @('Settings','All')){$cases+=@('N08','N10-same','N10-count','N09','N12-same','N12-switch','N12-reopen','N12-reload')}
    if($lease.profile -in @('Shell','All')){$cases+='Shell'}
    foreach($case in $cases){
        $null=Assert-ChildUiWorker $lease
        $null=Assert-ChildUiInputs $lease
        $scriptFile=switch($case){Smoke{'ui_smoke.ps1'};Shell{'shell_ui.ps1'};default{'settings_ui.ps1'}}
        $arguments=@('-NoProfile','-STA','-File',(Join-Path $root "tests\$scriptFile"),'-Config',$lease.config,'-EvidenceRoot',(Join-Path $lease.root $case))
        if($case -notin @('Smoke','Shell')){$arguments+=@('-Case',$case)}
        $si=[Diagnostics.ProcessStartInfo]::new($lease.powershell);$si.WorkingDirectory=$root;$si.UseShellExecute=$false
        $si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true
        foreach($a in $arguments){$si.ArgumentList.Add($a)}
        $p=[Diagnostics.Process]::Start($si);$birth=$p.StartTime.ToUniversalTime().Ticks
        $output=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();$laneClock=[Diagnostics.Stopwatch]::StartNew()
        # Outer containment only; standing script phase deadlines are unchanged.
        $limit=if($case -eq 'Smoke'){600000}else{90000}
        $ended=$p.WaitForExit($limit)
        if(-not $ended){
            $null=Assert-ChildUiProcess $p.Id $birth $ready.child_session $lease.powershell
            $p.Kill();$p.WaitForExit()
        }
        $stdout=$output.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $lease.root "$case.log"),$stdout)
        [IO.File]::WriteAllText((Join-Path $lease.root "$case.stderr.log"),$stderr.GetAwaiter().GetResult())
        $residual=ChildApps
        $structured=$null;$resultFile=Join-Path $lease.root "$case\$(if($case -eq 'Smoke'){'smoke-result.json'}else{'prerequisite-result.json'})"
        if(Test-Path $resultFile){$structured=Get-Content $resultFile -Raw|ConvertFrom-Json}
        $optional=@([regex]::Matches($stdout,'(?im)^\s*skip\s+([^\r\n]+)')|ForEach-Object {@{kind='existing optional resource absence';scope=$_.Groups[1].Value;owner=$scriptFile;review='qualify separately when available'}})
        $result=@{case=$case;exit_code=$p.ExitCode;finished=$ended;pid=$p.Id;birth_ticks=$birth;session=$ready.child_session;wall_ms=$laneClock.ElapsedMilliseconds;residual=$residual;result=$structured;arguments=$arguments;optional_absences=$optional}
        $lanes+=,$result
        Write-ChildUiJson (Join-Path $lease.root "$case-result.json") $result
        if(-not $ended){throw "Outer containment deadline for $case; phase results retained"}
        if($residual.Count){throw "Child OpenPilot residue after $case; dependent lanes stopped"}
    }
    $null=Assert-ChildUiInputs $lease
}catch{$errorMessage=$_.Exception.Message}
$failed=@($lanes|Where-Object {$_.exit_code -ne 0 -or -not $_.finished -or $_.residual.Count}).Count
$outcome=if($errorMessage){'blocked'}elseif($failed){'failed'}else{'passed'}
Write-ChildUiJson (Join-Path $lease.root 'worker-result.json') @{outcome=$outcome;token=$lease.token;input_key=$lease.input_key;lanes=$lanes;error=$errorMessage;residual=(ChildApps);session=[Diagnostics.Process]::GetCurrentProcess().SessionId;wall_ms=$clock.ElapsedMilliseconds;worker_count=1;retries=0;skips=@($lanes|ForEach-Object {$_.optional_absences}).Count}
if($outcome -ne 'passed'){exit 1}
