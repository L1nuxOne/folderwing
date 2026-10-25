# Negative controls for launcher admission and custody. No input, session
# creation, feature changes, scheduled tasks or credential UI in this lane.
param([string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\..\tools\child-ui-common.ps1"
. "$PSScriptRoot\..\tools\child-ui-runtime.ps1"
Initialize-ChildUiWindows;Import-ChildUiNative
$repo=Split-Path $PSScriptRoot -Parent
$agent=$null
foreach($entry in Get-ChildItem -LiteralPath (Join-Path $repo 'build\child-ui-cache') -Directory|Sort-Object LastWriteTimeUtc -Descending){
    $metaFile=Join-Path $entry.FullName 'cache.json'
    if(-not(Test-Path $metaFile)){continue}
    $meta=Get-Content $metaFile -Raw|ConvertFrom-Json
    $source=Join-Path $repo 'tools\child-ui-agent.cs'
    if($meta.inputs.$source -cne (Get-FileHash $source).Hash.ToLowerInvariant()){continue}
    $candidate=Join-Path $entry.FullName 'ChildUiAgent.exe'
    if((Get-FileHash $candidate).Hash.ToLowerInvariant() -cne $meta.images.'ChildUiAgent.exe'){throw 'Agent cache image changed'}
    $agent=$candidate;break
}
if(-not $agent){throw 'Run tools/child-ui.ps1 -BuildOnly before the launcher contracts'}
if(-not $EvidenceRoot){$EvidenceRoot=Join-Path $repo "build\child-ui-contract\$([Guid]::NewGuid().ToString('N'))"}
$null=New-Item -ItemType Directory -Path $EvidenceRoot
$contractInputs=@{schema='openpilot-child-ui-contract-input/v1';sources=(Get-ChildUiSnapshot $repo);agent_sha256=(Get-FileHash $agent).Hash.ToLowerInvariant();powershell_sha256=(Get-FileHash ([Diagnostics.Process]::GetCurrentProcess().Path)).Hash.ToLowerInvariant();command='tests/child_session_contract.ps1';worker_count=1;profile='ordinary Windows parent; no UI/session/feature/task mutation';retries=0;skips=0}
$contractInputKey=Get-ChildUiDigest $contractInputs
Write-ChildUiJson (Join-Path $EvidenceRoot 'input.json') $contractInputs
$cases=@()
function Accept([string]$Name,[scriptblock]$Code){& $Code;$script:cases+=@{case=$Name;passed=$true}}
function Reject([string]$Name,[scriptblock]$Code){$threw=$false;try{& $Code|Out-Null}catch{$threw=$true};if(-not $threw){throw "Negative control admitted: $Name"};$script:cases+=@{case=$Name;passed=$true}}
Accept 'canonical object order is immaterial, array order is material' {
    if((Get-ChildUiDigest @{a=1;b=@(2,3)}) -cne (Get-ChildUiDigest @{b=@(2,3);a=1}) -or (Get-ChildUiDigest @(2,3)) -ceq (Get-ChildUiDigest @(3,2))){throw 'Canonical digest contract'}
}
Reject 'relative lease path' {Assert-ChildUiPath 'relative\lease.json'}
Reject 'quoted Windows path' {Assert-ChildUiPath 'C:\bad"path'}
$self=[Diagnostics.Process]::GetCurrentProcess();$ticks=$self.StartTime.ToUniversalTime().Ticks
Accept 'exact live process identity' {$null=Assert-ChildUiProcess $PID $ticks $self.SessionId $self.Path}
Reject 'recycled PID birth' {Assert-ChildUiProcess $PID ($ticks-1) $self.SessionId $self.Path}
Reject 'other process session' {Assert-ChildUiProcess $PID $ticks ($self.SessionId+1) $self.Path}
Reject 'other process image' {Assert-ChildUiProcess $PID $ticks $self.SessionId 'C:\different.exe'}
$token=[Guid]::NewGuid().ToString('N');$owned=Join-Path $repo "build\child-ui\$token";$null=New-Item -ItemType Directory $owned
$path=Join-Path $owned 'lease.json'
$lease=@{schema='openpilot-child-ui/v1';token=$token;root=$owned;sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;profile='Smoke';parent_session=$self.SessionId;host_image=$self.Path}
function Save-Lease { $lease|ConvertTo-Json|Set-Content -LiteralPath $path }
try{
    Save-Lease
    Accept 'lease bound to account and root' {$read=Read-ChildUiLease $path;if($read.token -cne $token){throw 'Lease identity'}}
    Accept 'detached owner refuses stale client before feature or session mutation' {
        $cachePath=Split-Path $agent -Parent
        $bootstrap=@{schema='openpilot-child-ui-bootstrap/v1';token=$token;root=$owned;sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value;parent_session=$self.SessionId;idle_seconds=300;host_seconds=3630;cache_path=$cachePath;cache_key=(Split-Path $cachePath -Leaf);client_pid=$PID;client_birth_ticks=($ticks-1);powershell=$self.Path}
        Write-ChildUiJson (Join-Path $owned 'bootstrap.json') $bootstrap
        $p=[OpenPilotChildUi]::StartOwner($self.Path,(Join-Path $cachePath 'child-ui-session.ps1'),(Join-Path $owned 'bootstrap.json'),$owned)
        if(-not $p.WaitForExit(15000)){throw 'Detached owner refusal deadline'}
        $blocked=Get-Content (Join-Path $owned 'startup-blocked.json') -Raw|ConvertFrom-Json
        if($p.ExitCode -eq 0 -or $blocked.error -notmatch 'PID/birth/session/image changed' -or (Test-Path (Join-Path $owned 'Enable-result.json')) -or (Test-Path (Join-Path $owned 'host-launch.json'))){throw 'Detached stale-client refusal failed'}
        Copy-Item -LiteralPath (Join-Path $owned 'startup-blocked.json') -Destination (Join-Path $EvidenceRoot 'owner-stale-client-refusal.json')
    }
    @{token=$token;child_mode=$true;redirect_clipboard=$false;child_session=$self.SessionId;pid=$PID;birth_ticks=$ticks}|ConvertTo-Json|Set-Content (Join-Path $owned 'host-ready.json')
    Accept 'compiled bootstrap refuses parent before creating or switching desktop' {
        $si=[Diagnostics.ProcessStartInfo]::new($agent);$si.UseShellExecute=$false;$si.ArgumentList.Add($path)
        $p=[Diagnostics.Process]::Start($si);if(-not $p.WaitForExit(10000)){throw 'Bootstrap refusal deadline'}
        $result=Get-Content (Join-Path $owned 'agent-result.json') -Raw|ConvertFrom-Json
        if($p.ExitCode -eq 0 -or $result.desktop_switched -or $result.error -notmatch 'Exact child account/session required before desktop mutation' -or (Test-Path (Join-Path $owned 'desktop-ready.json'))){throw 'Bootstrap parent refusal contract'}
        Copy-Item -LiteralPath (Join-Path $owned 'agent-result.json') -Destination (Join-Path $EvidenceRoot 'agent-parent-refusal.json')
    }
    $lease.sid='S-1-0-0';Save-Lease;Reject 'other account lease' {Read-ChildUiLease $path};$lease.sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    $lease.root=$EvidenceRoot;Save-Lease;Reject 'lease root escape' {Read-ChildUiLease $path};$lease.root=$owned
    $lease.profile='arbitrary-command';Save-Lease;Reject 'arbitrary profile' {Read-ChildUiLease $path};$lease.profile='Smoke';Save-Lease
    @{token=$token;child_mode=$true;redirect_clipboard=$false;child_session=$self.SessionId;pid=$PID;birth_ticks=$ticks}|ConvertTo-Json|Set-Content (Join-Path $owned 'host-ready.json')
    Reject 'parent session cannot act as its child' {Assert-ChildUiWorker ([pscustomobject]$lease)}
    $lease.parent_session=$self.SessionId+1
    @{token=$token;child_mode=$true;redirect_clipboard=$true;child_session=$self.SessionId;pid=$PID;birth_ticks=$ticks}|ConvertTo-Json|Set-Content (Join-Path $owned 'host-ready.json')
    Reject 'clipboard redirection cannot qualify isolation' {Assert-ChildUiWorker ([pscustomobject]$lease)}
    $image=Join-Path $owned 'owned-image.txt';[IO.File]::WriteAllText($image,'original')
    $images=@{};$images[[IO.Path]::GetRelativePath($repo,$image).Replace('\','/')]=(Get-FileHash $image).Hash.ToLowerInvariant()
    $input=@{sources=(Get-ChildUiSnapshot $repo);images=$images;powershell_sha256=(Get-FileHash $self.Path).Hash.ToLowerInvariant()}
    $lease.powershell=$self.Path;$lease.input_key=Get-ChildUiDigest $input
    Write-ChildUiJson (Join-Path $owned 'input.json') $input
    Accept 'exact source and runner manifest' {$null=Assert-ChildUiInputs ([pscustomobject]$lease)}
    [IO.File]::WriteAllText($image,'changed')
    Reject 'changed executable bytes' {Assert-ChildUiInputs ([pscustomobject]$lease)}
    [IO.File]::WriteAllText($image,'original')
    $lease.input_key='not-the-recorded-key'
    Reject 'changed input manifest identity' {Assert-ChildUiInputs ([pscustomobject]$lease)}
    $owner=[pscustomobject]@{token=('1'*32);root=(Join-Path $repo ('build\child-ui\'+('1'*32)))}
    $lease.session_token=$owner.token;$lease.session_root=$owner.root;Save-Lease
    $request=@{schema='openpilot-child-ui-request/v1';session_token=$owner.token;run_token=$token;run_root=$owned;input_key=$lease.input_key}
    Accept 'fresh fixed-profile request binding' {$null=Assert-ChildUiRequest ([pscustomobject]$request) $owner ($token+'.json')}
    Reject 'request filename identity differs' {Assert-ChildUiRequest ([pscustomobject]$request) $owner 'different.json'}
    $request.run_root=$EvidenceRoot;Reject 'request cannot escape owned run root' {Assert-ChildUiRequest ([pscustomobject]$request) $owner ($token+'.json')};$request.run_root=$owned
    $request.session_token='other';Reject 'request cannot adopt another session' {Assert-ChildUiRequest ([pscustomobject]$request) $owner ($token+'.json')};$request.session_token=$owner.token
    $request.input_key='different';Reject 'request cannot switch manifests' {Assert-ChildUiRequest ([pscustomobject]$request) $owner ($token+'.json')};$request.input_key=$lease.input_key
    Write-ChildUiJson (Join-Path $owned 'FINAL.json') @{fixture='not an acceptance receipt'}
    Reject 'completed run cannot be replayed' {Assert-ChildUiRequest ([pscustomobject]$request) $owner ($token+'.json')}
}finally{
    # Only the exact newly owned contract files, never recursive generic cleanup.
    Remove-Item -LiteralPath $path,(Join-Path $owned 'host-ready.json'),(Join-Path $owned 'owned-image.txt'),(Join-Path $owned 'input.json'),(Join-Path $owned 'agent-result.json'),(Join-Path $owned 'FINAL.json') -ErrorAction SilentlyContinue
    foreach($name in @('bootstrap.json','startup-blocked.json','controller.log','controller.stderr.log')){Remove-Item -LiteralPath (Join-Path $owned $name) -ErrorAction SilentlyContinue}
    [IO.Directory]::Delete($owned,$false)
}
$binding=[pscustomobject]@{persistent=$true;token='session';cache_key='cache';sid='account';parent_session=1;launcher_pid=10;launcher_birth_ticks=100;powershell='C:\pwsh.exe'}
$ready=[pscustomobject]@{token='session';cache_key='cache';sid='account';parent_session=1;child_session=2;controller_pid=10;controller_birth_ticks=100;controller_image='C:\pwsh.exe';host_pid=20;host_birth_ticks=200}
$hostFacts=[pscustomobject]@{token='session';parent_session=1;child_session=2;pid=20;birth_ticks=200;child_mode=$true;redirect_clipboard=$false}
Accept 'exact authenticated session binding' {Assert-ChildUiSessionBinding $binding $ready $hostFacts}
foreach($field in @('token','cache_key','sid','parent_session','child_session','controller_pid','controller_birth_ticks','controller_image','host_pid','host_birth_ticks')){
    $prior=$ready.$field;$ready.$field=if($prior -is [string]){'different'}else{0}
    Reject "reused session rejects changed $field" {Assert-ChildUiSessionBinding $binding $ready $hostFacts}
    $ready.$field=$prior
}
$hostFacts.redirect_clipboard=$true;Reject 'reused session refuses clipboard redirection' {Assert-ChildUiSessionBinding $binding $ready $hostFacts};$hostFacts.redirect_clipboard=$false
$hostFacts.child_mode=$false;Reject 'reused session refuses ordinary RDP context' {Assert-ChildUiSessionBinding $binding $ready $hostFacts};$hostFacts.child_mode=$true
$fake=[pscustomobject]@{Xml='<owned/>';Instances=@([pscustomobject]@{InstanceGuid='owned'})}
$fake|Add-Member ScriptMethod GetInstances {param($flags) $this.Instances}
Accept 'exact task and instance' {$actual=@(Assert-ChildUiTaskIdentity $fake '<owned/>' 'owned');if($actual.Count -ne 1){throw 'Owned task observation'}}
Reject 'changed task XML' {Assert-ChildUiTaskIdentity $fake '<other/>' 'owned'}
Reject 'foreign instance of same task' {Assert-ChildUiTaskIdentity $fake '<owned/>' 'other'}
Reject 'uncaptured task instance' {Assert-ChildUiTaskIdentity $fake '<owned/>' ''}
$write=Join-Path $EvidenceRoot 'create-new.json';Write-ChildUiJson $write @{original=1}
Reject 'receipt cannot overwrite previous evidence' {Write-ChildUiJson $write @{replacement=1}}
if((Get-Content $write -Raw|ConvertFrom-Json).original -ne 1){throw 'CreateNew changed existing evidence'}
if((Get-ChildUiDigest (Get-ChildUiSnapshot $repo)) -cne (Get-ChildUiDigest $contractInputs.sources)){throw 'Contract source set changed during execution'}
$result=@{outcome='passed';input_key=$contractInputKey;cases=$cases;assertions=$cases.Count;os_input=0;feature_mutations=0;task_mutations=0;session_mutations=0;retries=0;skips=0}
$result.receipt_key=Get-ChildUiDigest $result
Write-ChildUiJson (Join-Path $EvidenceRoot 'FINAL.json') $result
$result|ConvertTo-Json -Depth 6
