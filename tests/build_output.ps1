<# Native compiler publication: stable images, failed builds and loaded DLLs. #>
param([string]$Zig)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
. "$root\tools\build-output.ps1"
if(-not $Zig){$Zig=Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}
$scratch=Join-Path $env:TEMP "openpilot_build_output_$PID"
New-Item -ItemType Directory -Force $scratch|Out-Null
$script:passed=0;$script:failed=0
function Check([string]$name,[bool]$ok){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name";$script:failed++}}
$source=Join-Path $scratch 'value.c';$target=Join-Path $scratch 'value.o'
$arguments=@('-target','x86_64-windows-gnu','-std=c11','-c',$source,'-o',$target)
$loaded=[IntPtr]::Zero;$loadedAsset=[IntPtr]::Zero;$loadedOptional=[IntPtr]::Zero;$held=$null;$elfProcess=$null
try {
    Assert-BuildArtifactSet $scratch @() @('folderwing-pdf.exe','pdfium.dll','folderwing-terminal.exe','terminal_tests.exe')
    Check 'clean optional output starts absent' (-not(Test-Path (Join-Path $scratch 'pdfium.dll')))
    Set-Content $source 'int value(void) { return 1; }'
    Invoke-BuildCompiler $Zig $scratch 'value.o' $arguments
    Check 'a successful compilation publishes the completed output' (Test-Path $target)
    $hash=(Get-FileHash $target).Hash
    $stamp=[DateTime]::new(2000,1,1,0,0,0,[DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($target,$stamp)
    # Sharing only reads makes replacing or renaming this file fail. Identical
    # compilation must succeed by leaving this exact image untouched.
    $held=[IO.File]::Open($target,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    Invoke-BuildCompiler $Zig $scratch 'value.o' $arguments
    Check 'an identical build succeeds while the published file cannot be replaced' ((Get-FileHash $target).Hash -eq $hash)
    Check 'an identical build retains the published modification time' ((Get-Item $target).LastWriteTimeUtc -eq $stamp)
    Check 'an identical build creates no old image' (@(Get-ChildItem $scratch -Filter '*.old').Count -eq 0)
    $held.Dispose();$held=$null
    Set-Content $source 'int value(void) { return 2; }'
    Invoke-BuildCompiler $Zig $scratch 'value.o' $arguments
    $changed=(Get-FileHash $target).Hash
    Check 'changed source publishes a changed output' ($changed -ne $hash)
    Check 'publication consumes the completed staging output' (-not(Test-Path "$scratch\.link\value.o"))
    Set-Content $source 'int value(void) { this is invalid C; }'
    $rejected=$false
    try { Invoke-BuildCompiler $Zig $scratch 'value.o' $arguments 2> "$scratch\expected-compiler-error.log" }
    catch { $rejected=$true }
    Check 'a failed compilation reports failure' $rejected
    Check 'a failed compilation retains the working output' ((Get-FileHash $target).Hash -eq $changed)
    $rejected=$false
    try { Invoke-BuildCompiler $Zig $scratch 'value.o' @('-c',$source) }
    catch { $rejected=$true }
    Check 'a missing output argument fails before publishing' $rejected
    Check 'invalid compiler arguments retain the working output' ((Get-FileHash $target).Hash -eq $changed)

    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class BuildImage {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    public static extern IntPtr LoadLibraryW(string path);
    [DllImport("kernel32.dll")] public static extern bool FreeLibrary(IntPtr image);
}
'@
    $dll=Join-Path $scratch 'active.dll'
    $c=Join-Path $scratch 'active.c'
    Set-Content $c '__declspec(dllexport) int value(void) { return 11; }'
    # Match the stripped release-image profile used by the product build.
    $dllArgs=@('-target','x86_64-windows-gnu','-std=c11','-O2','-DNDEBUG','-s','-shared',$c,'-o',$dll)
    Invoke-BuildCompiler $Zig $scratch 'active.dll' $dllArgs
    $oldHash=(Get-FileHash $dll).Hash
    $loaded=[BuildImage]::LoadLibraryW($dll)
    Check 'the native locked-image fixture loads' ($loaded -ne [IntPtr]::Zero)
    if($loaded -eq [IntPtr]::Zero){throw 'Could not load the private build fixture'}
    Invoke-BuildCompiler $Zig $scratch 'active.dll' $dllArgs
    Check 'an identical loaded DLL is retained without an old-image copy' (@(Get-ChildItem $scratch -Filter 'active.dll.*.old').Count -eq 0)
    Set-Content $c '__declspec(dllexport) int value(void) { return 22; }'
    Invoke-BuildCompiler $Zig $scratch 'active.dll' $dllArgs
    Check 'a changed loaded DLL publishes the new image for the next launch' ((Get-FileHash $dll).Hash -ne $oldHash)
    $old=@(Get-ChildItem $scratch -Filter 'active.dll.*.old')
    Check 'the running DLL retains its complete previous image' ($old.Count -eq 1 -and (Get-FileHash $old[0].FullName).Hash -eq $oldHash)
    $liveAsset=Join-Path $scratch 'pdfium-live.dll'
    Publish-BuildAsset $old[0].FullName $scratch 'pdfium-live.dll'
    $loadedAsset=[BuildImage]::LoadLibraryW($liveAsset)
    Check 'the optional DLL is mapped before replacement' ($loadedAsset -ne [IntPtr]::Zero)
    if($loadedAsset -eq [IntPtr]::Zero){throw 'Could not map the private asset fixture'}
    Publish-BuildAsset $dll $scratch 'pdfium-live.dll'
    $liveOld=@(Get-ChildItem $scratch -Filter 'pdfium-live.dll.*.old')
    Check 'a changed mapped optional DLL publishes new bytes and retains its old image' (
        (Get-FileHash $liveAsset).Hash -eq (Get-FileHash $dll).Hash -and $liveOld.Count -eq 1 -and
        (Get-FileHash $liveOld[0].FullName).Hash -eq $oldHash)
    $assetSource=Join-Path $scratch 'pdfium-source.dll'
    $asset=Join-Path $scratch 'pdfium.dll'
    [IO.File]::WriteAllBytes($assetSource,[byte[]](1,2,3,4,5,6))
    Publish-BuildAsset $assetSource $scratch 'pdfium.dll'
    Assert-BuildArtifactSet $scratch @('pdfium.dll') @() @{'pdfium.dll'=$assetSource}
    $assetStamp=[DateTime]::new(2000,1,1,0,0,0,[DateTimeKind]::Utc)
    [IO.File]::SetLastWriteTimeUtc($asset,$assetStamp)
    Publish-BuildAsset $assetSource $scratch 'pdfium.dll'
    Check 'identical optional DLL retains its published identity' ((Get-Item $asset).LastWriteTimeUtc -eq $assetStamp)
    [IO.File]::WriteAllBytes($assetSource,[byte[]](6,5,4,3,2,1))
    Publish-BuildAsset $assetSource $scratch 'pdfium.dll'
    Check 'same-size changed optional DLL publishes by hash' ((Get-FileHash $asset).Hash -eq (Get-FileHash $assetSource).Hash -and
        (Get-Item $asset).LastWriteTimeUtc -ne $assetStamp)
    Assert-BuildArtifactSet $scratch @('pdfium.dll') @() @{'pdfium.dll'=$assetSource}
    [IO.File]::WriteAllBytes((Join-Path $scratch 'folderwing-pdf.exe'),[byte[]](1,2,3))
    Retire-BuildArtifacts $scratch @('folderwing-pdf.exe','pdfium.dll')
    Assert-BuildArtifactSet $scratch @() @('folderwing-pdf.exe','pdfium.dll')
    Check 'missing PDF dependency retires the helper and DLL' (-not(Test-Path $asset) -and
        -not(Test-Path (Join-Path $scratch 'folderwing-pdf.exe')))
    Publish-BuildAsset $assetSource $scratch 'pdfium.dll'
    Check 'restored optional dependency republishes its DLL' ((Get-FileHash $asset).Hash -eq (Get-FileHash $assetSource).Hash)
    [IO.File]::WriteAllBytes((Join-Path $scratch 'folderwing-terminal.exe'),[byte[]](1))
    [IO.File]::WriteAllBytes((Join-Path $scratch 'terminal_tests.exe'),[byte[]](1))
    Retire-BuildArtifacts $scratch @('folderwing-terminal.exe','terminal_tests.exe')
    Assert-BuildArtifactSet $scratch @() @('folderwing-terminal.exe','terminal_tests.exe')
    Check 'missing terminal dependency retires app and test helper' (-not(Test-Path (Join-Path $scratch 'folderwing-terminal.exe')) -and
        -not(Test-Path (Join-Path $scratch 'terminal_tests.exe')))
    $optional=Join-Path $scratch 'optional.dll'
    Copy-Item $dll $optional
    $loadedOptional=[BuildImage]::LoadLibraryW($optional)
    Check 'optional output loads before retirement' ($loadedOptional -ne [IntPtr]::Zero)
    if($loadedOptional -eq [IntPtr]::Zero){throw 'Could not load the private optional image fixture'}
    Retire-BuildArtifacts $scratch @('optional.dll')
    Check 'loaded optional output is absent from the published artifact set' (-not(Test-Path $optional))
    # Linux helpers are native images too. Keep the old one mapped while a
    # Windows compiler publishes changed bytes; check both process generations.
    $registration='HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss'
    $registered=@(Get-ChildItem $registration -ErrorAction SilentlyContinue |
        Get-ItemProperty -ErrorAction SilentlyContinue | Where-Object {$_.DistributionName})
    if($registered.Count){
        $wsl=Join-Path ([Environment]::GetFolderPath('System')) 'wsl.exe'
        $elf=Join-Path $scratch 'folderwing-wsl';$elfSource=Join-Path $scratch 'elf.c'
        $elfTemplate='#include <stdio.h>
int main(int argc,char **argv){puts("VALUE");fflush(stdout);if(argc>1){getchar();puts("VALUE");fflush(stdout);}return 0;}'
        Set-Content $elfSource ($elfTemplate.Replace('VALUE','11'))
        $elfArgs=@('-target','x86_64-linux-musl','-static','-std=c11','-O2',$elfSource,'-o',$elf)
        Invoke-BuildCompiler $Zig $scratch 'folderwing-wsl' $elfArgs
        $value=(& $wsl --cd $scratch --exec ./folderwing-wsl)-join ''
        Check 'a published Linux helper executes its current bytes' ($LASTEXITCODE -eq 0 -and $value -eq '11')
        $start=[Diagnostics.ProcessStartInfo]::new($wsl)
        foreach($arg in @('--cd',$scratch,'--exec','./folderwing-wsl','--hold')){$start.ArgumentList.Add($arg)}
        $start.UseShellExecute=$false;$start.CreateNoWindow=$true
        $start.RedirectStandardInput=$true;$start.RedirectStandardOutput=$true
        $elfProcess=[Diagnostics.Process]::Start($start)
        $first=$elfProcess.StandardOutput.ReadLineAsync()
        if(-not $first.Wait(5000) -or $first.Result -ne '11'){throw 'Owned Linux image did not start'}
        $elfHash=(Get-FileHash $elf).Hash;$elfStamp=(Get-Item $elf).LastWriteTimeUtc
        Invoke-BuildCompiler $Zig $scratch 'folderwing-wsl' $elfArgs
        Check 'an identical Linux build retains the mapped image and timestamp' ((Get-FileHash $elf).Hash -eq $elfHash -and (Get-Item $elf).LastWriteTimeUtc -eq $elfStamp)
        Set-Content $elfSource ($elfTemplate.Replace('VALUE','22'))
        Invoke-BuildCompiler $Zig $scratch 'folderwing-wsl' $elfArgs
        $value=(& $wsl --cd $scratch --exec ./folderwing-wsl)-join ''
        Check 'a changed Linux helper starts the new image while the old image is mapped' ($LASTEXITCODE -eq 0 -and $value -eq '22')
        $elfProcess.StandardInput.WriteLine('');$elfProcess.StandardInput.Close()
        $last=$elfProcess.StandardOutput.ReadLineAsync()
        Check 'the running Linux helper retains its complete old image' ($last.Wait(5000) -and $last.Result -eq '11' -and $elfProcess.WaitForExit(5000) -and $elfProcess.ExitCode -eq 0)
        Set-Content $elfSource ($elfTemplate.Replace('VALUE','33'))
        $interrupted=$false
        try { Invoke-BuildCompiler $Zig $scratch 'folderwing-wsl' $elfArgs -AfterNativePublish {throw 'Injected post-publication failure'} }
        catch { $interrupted=$true }
        $pending=Join-Path $scratch '.folderwing-wsl.publish-pending'
        Check 'post-publication failure remains explicitly pending' ($interrupted -and (Test-Path $pending))
        $faultHash=(Get-FileHash $elf).Hash
        Invoke-BuildCompiler $Zig $scratch 'folderwing-wsl' $elfArgs
        $value=(& $wsl --cd $scratch --exec ./folderwing-wsl)-join ''
        Check 'identical rebuild completes pending native publication' (-not(Test-Path $pending) -and
            (Get-FileHash $elf).Hash -eq $faultHash -and $LASTEXITCODE -eq 0 -and $value -eq '33')

        # Copying a prebuilt helper must use the same native publication rule
        # as compilation, including when an identical copy resumes a pending
        # publication. Hash equality alone cannot validate executable mappings.
        $elfProcess.Dispose();$elfProcess=$null;$elfProcess=[Diagnostics.Process]::Start($start)
        $first=$elfProcess.StandardOutput.ReadLineAsync()
        if(-not $first.Wait(5000) -or $first.Result -ne '33'){throw 'Owned asset image did not start'}
        $elfHash=(Get-FileHash $elf).Hash;$elfStamp=(Get-Item $elf).LastWriteTimeUtc
        Publish-BuildAsset $elf $scratch 'folderwing-wsl'
        Check 'an identical helper asset retains the mapped image and timestamp' (
            (Get-FileHash $elf).Hash -eq $elfHash -and (Get-Item $elf).LastWriteTimeUtc -eq $elfStamp)
        $helperAssets=Join-Path $scratch 'helper-assets';New-Item -ItemType Directory -Force $helperAssets|Out-Null
        $helperAsset=Join-Path $helperAssets 'folderwing-wsl'
        Set-Content $elfSource ($elfTemplate.Replace('VALUE','44'))
        $assetArgs=@($elfArgs);$assetArgs[$assetArgs.Count-1]=$helperAsset
        Invoke-BuildCompiler $Zig $helperAssets 'folderwing-wsl' $assetArgs
        Publish-BuildAsset $helperAsset $scratch 'folderwing-wsl'
        $value=(& $wsl --cd $scratch --exec ./folderwing-wsl)-join ''
        Check 'a changed helper asset starts its current image while the old image is mapped' (
            $LASTEXITCODE -eq 0 -and $value -eq '44' -and (Get-FileHash $elf).Hash -eq (Get-FileHash $helperAsset).Hash)
        $elfProcess.StandardInput.WriteLine('');$elfProcess.StandardInput.Close()
        $last=$elfProcess.StandardOutput.ReadLineAsync()
        Check 'a copied helper asset leaves the running old image intact' (
            $last.Wait(5000) -and $last.Result -eq '33' -and $elfProcess.WaitForExit(5000) -and $elfProcess.ExitCode -eq 0)
        [IO.File]::WriteAllText($pending,(Get-FileHash $elf).Hash)
        Publish-BuildAsset $helperAsset $scratch 'folderwing-wsl'
        $value=(& $wsl --cd $scratch --exec ./folderwing-wsl)-join ''
        Check 'an identical helper asset finishes pending native publication' (
            -not(Test-Path $pending) -and $LASTEXITCODE -eq 0 -and $value -eq '44')
    }
    # Exercise the Windows-only route without launching or reconfiguring WSL.
    $lookup=(Get-Item Function:Get-BuildWsl).ScriptBlock
    try {
        Set-Item Function:Get-BuildWsl {param([string]$OutputDirectory) return $null}
        $windowsOnly=Join-Path $scratch 'windows-only';New-Item -ItemType Directory -Force $windowsOnly|Out-Null
        $windowsPending=Join-Path $windowsOnly '.folderwing-wsl.publish-pending'
        [IO.File]::WriteAllText($windowsPending,'private pending fixture')
        Publish-BuildAsset $assetSource $windowsOnly 'folderwing-wsl'
        Check 'helper assets publish normally without an available WSL distro' (
            (Get-FileHash (Join-Path $windowsOnly 'folderwing-wsl')).Hash -eq (Get-FileHash $assetSource).Hash)
        Check 'Windows-only asset publication clears its pending marker' (-not(Test-Path $windowsPending))
    } finally { Set-Item Function:Get-BuildWsl $lookup }
} finally {
    if($elfProcess){
        if(-not $elfProcess.HasExited){$elfProcess.StandardInput.Close();if(-not $elfProcess.WaitForExit(3000)){$elfProcess.Kill();$elfProcess.WaitForExit()}}
        $elfProcess.Dispose()
    }
    if($held){$held.Dispose()}
    if($loadedAsset -ne [IntPtr]::Zero){[BuildImage]::FreeLibrary($loadedAsset)|Out-Null}
    if($loadedOptional -ne [IntPtr]::Zero){[BuildImage]::FreeLibrary($loadedOptional)|Out-Null}
    if($loaded -ne [IntPtr]::Zero){[BuildImage]::FreeLibrary($loaded)|Out-Null}
    Remove-Item -LiteralPath $scratch -Recurse -Force
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
