# Windows replacement can leave a live DrvFs executable mapping attached to
# stale pages. Re-publish identical helper bytes through Linux rename so new
# launches get a fresh inode while existing processes keep their old image.
# Windows-only machines never launch WSL during this step.
function Get-BuildWsl([string]$OutputDirectory) {
    $registration='HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'
    $registered=@(Get-ChildItem $registration -ErrorAction SilentlyContinue |
        Get-ItemProperty -ErrorAction SilentlyContinue | Where-Object {$_.DistributionName})
    if(-not $registered.Count){return $null}
    $wsl=Join-Path ([Environment]::GetFolderPath('System')) 'wsl.exe'
    if(-not(Test-Path -LiteralPath $wsl)){return $null}
    return $wsl
}

function Publish-BuildFile([string]$Candidate,[string]$Target) {
    try { [IO.File]::Move($Candidate, $Target, $true) }
    catch {
        # A running image permits renaming but cannot be replaced. Keep it for
        # that process and publish the completed build for the next launch.
        if (-not (Test-Path -LiteralPath $Target)) { throw }
        $backup = "$Target.$([DateTime]::UtcNow.Ticks).old"
        [IO.File]::Move($Target, $backup)
        try { [IO.File]::Move($Candidate, $Target) }
        catch {
            if (-not (Test-Path -LiteralPath $Target)) { [IO.File]::Move($backup, $Target) }
            throw
        }
    }
}

function Publish-BuildAsset([string]$Source,[string]$OutputDirectory,[string]$Name) {
    if(-not(Test-Path -LiteralPath $Source -PathType Leaf)){throw "Missing build asset: $Source"}
    $target=Join-Path $OutputDirectory $Name
    $staging=Join-Path $OutputDirectory '.link'
    New-Item -ItemType Directory -Force $staging|Out-Null
    $candidate=Join-Path $staging $Name
    Copy-Item -LiteralPath $Source -Destination $candidate -Force
    $expected=(Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if((Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ne $expected){throw "Staged build asset changed: $Name"}
    $helper=$Name -ceq 'folderwing-wsl'
    $pending=Join-Path $OutputDirectory '.folderwing-wsl.publish-pending'
    if((Test-Path -LiteralPath $target) -and (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $expected -and
       (-not $helper -or -not(Test-Path -LiteralPath $pending))){
        Remove-Item -LiteralPath $candidate -Force
        Write-Host "      unchanged $Name"
        return
    }
    # A copied Linux executable needs the same fresh native inode as a newly
    # compiled helper. Windows hash equality does not establish that new WSL
    # launches see its current executable pages. Resume pending publication too.
    if($helper){
        $wsl=Get-BuildWsl $OutputDirectory
        if($wsl){Publish-WslBuildHelper $OutputDirectory $candidate $target $wsl $null;return}
    }
    Publish-BuildFile $candidate $target
    if($helper -and (Test-Path -LiteralPath $pending)){Remove-Item -LiteralPath $pending -Force}
    if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $expected){throw "Published build asset changed: $Name"}
}

function Retire-BuildArtifacts([string]$OutputDirectory,[string[]]$Names) {
    foreach($name in $Names){
        $target=Join-Path $OutputDirectory $name
        if(-not(Test-Path -LiteralPath $target)){continue}
        try{Remove-Item -LiteralPath $target -Force -ErrorAction Stop}
        catch{[IO.File]::Move($target,"$target.$([DateTime]::UtcNow.Ticks).old")}
        if(Test-Path -LiteralPath $target){throw "Could not retire stale build artifact: $name"}
        Write-Host "      retired $name"
    }
}

function Assert-BuildArtifactSet([string]$OutputDirectory,[string[]]$Present,[string[]]$Absent,
                                  [hashtable]$Sources=@{}) {
    foreach($name in $Present){
        $target=Join-Path $OutputDirectory $name
        if(-not(Test-Path -LiteralPath $target -PathType Leaf)){throw "Missing published build artifact: $name"}
        if($Sources.ContainsKey($name) -and
           (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne
           (Get-FileHash -LiteralPath $Sources[$name] -Algorithm SHA256).Hash){
            throw "Published build artifact differs from input: $name"
        }
    }
    foreach($name in $Absent){
        if(Test-Path -LiteralPath (Join-Path $OutputDirectory $name)){
            throw "Stale optional build artifact remains: $name"
        }
    }
}

function Publish-WslBuildHelper([string]$OutputDirectory,[string]$Candidate,[string]$Target,
                                [string]$Wsl,[scriptblock]$AfterNativePublish) {
    $pending=Join-Path $OutputDirectory '.folderwing-wsl.publish-pending'
    $expected=(Get-FileHash -LiteralPath $Candidate -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($pending,$expected)
    $nativePublish=@'
set -eu
test -f ./.link/folderwing-wsl && test ! -L ./.link/folderwing-wsl
tmp=$(mktemp ./.folderwing-wsl-publish.XXXXXX)
trap 'rm -f -- "$tmp"' EXIT HUP INT TERM
cp -- ./.link/folderwing-wsl "$tmp"
cmp -s -- ./.link/folderwing-wsl "$tmp"
chmod 755 "$tmp"
mv -f -- "$tmp" ./folderwing-wsl
'@
    & $Wsl --cd $OutputDirectory --exec sh -c $nativePublish
    if($LASTEXITCODE -ne 0){throw 'Native WSL helper publication failed'}
    if($AfterNativePublish){& $AfterNativePublish}
    if((Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash -ne $expected){throw 'Native WSL publication changed helper bytes'}
    Remove-Item -LiteralPath $Candidate -Force
    Remove-Item -LiteralPath $pending -Force
}

# Compile beside the published output. Failed and byte-identical builds leave
# the working image in place, including its file identity and timestamps.
function Invoke-BuildCompiler([string]$Compiler, [string]$OutputDirectory, [string]$Name,
                              [string[]]$Arguments, [scriptblock]$AfterNativePublish) {
    Write-Host "  cc  $Name"
    $target = Join-Path $OutputDirectory $Name
    $staging = Join-Path $OutputDirectory '.link'
    New-Item -ItemType Directory -Force $staging | Out-Null
    $candidate = Join-Path $staging $Name
    $compileArguments = @($Arguments)
    $found = $false
    for ($i = 0; $i -lt $compileArguments.Count - 1; $i++) {
        if ($compileArguments[$i] -eq '-o' -and $compileArguments[$i + 1] -eq $target) {
            $compileArguments[$i + 1] = $candidate
            $found = $true
        }
    }
    if (-not $found) { throw "Missing output argument for $Name" }
    Remove-Item -LiteralPath $candidate -Force -ErrorAction SilentlyContinue
    & $Compiler cc @compileArguments
    if ($LASTEXITCODE -ne 0) { throw "build failed: $Name" }
    if (-not (Test-Path -LiteralPath $candidate)) { throw "Compiler did not produce $Name" }
    $pending=Join-Path $OutputDirectory '.folderwing-wsl.publish-pending'
    if ((Test-Path -LiteralPath $target) -and
        (Get-Item -LiteralPath $candidate).Length -eq (Get-Item -LiteralPath $target).Length -and
        (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -eq
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -and
        ($Name -cne 'folderwing-wsl' -or -not(Test-Path -LiteralPath $pending))) {
        Remove-Item -LiteralPath $candidate
        Write-Host "      unchanged $Name"
        return
    }
    if($Name -ceq 'folderwing-wsl'){
        $wsl=Get-BuildWsl $OutputDirectory
        if($wsl){Publish-WslBuildHelper $OutputDirectory $candidate $target $wsl $AfterNativePublish;return}
    }
    Publish-BuildFile $candidate $target
    if($Name -ceq 'folderwing-wsl' -and (Test-Path -LiteralPath $pending)){Remove-Item -LiteralPath $pending -Force}
}
