<# Package boundary acceptance against an actual release build and ZIP. No desktop input. #>
param([Parameter(Mandatory)][string]$BuildDirectory,[Parameter(Mandatory)][string]$DistributionDirectory,[Parameter(Mandatory)][string]$ResultDirectory,[string]$Python='python')
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if([IO.Path]::GetFullPath($ResultDirectory).StartsWith([IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Package evidence must be outside the candidate directory'}
if(Test-Path $ResultDirectory){throw 'Use fresh package evidence'};New-Item -ItemType Directory $ResultDirectory|Out-Null
$pin=Get-Content (Join-Path $root 'tools\media-dependencies.json') -Raw|ConvertFrom-Json
$source=Join-Path $DistributionDirectory $pin.source_bundle.filename
$manifest=Get-Content (Join-Path $DistributionDirectory 'RELEASE-MANIFEST.json') -Raw|ConvertFrom-Json
$images=[ordered]@{};foreach($p in $manifest.binaries.psobject.Properties){$images[$p.Name]=(Get-FileHash (Join-Path $BuildDirectory $p.Name)).Hash.ToLowerInvariant()}
$sourceHashes=[ordered]@{};foreach($name in @('tools/package-release.py','tools/release_common.py','tools/release-files.json','tools/release-inputs.json','tools/dependency-inventory.json','tests/release_packaging.ps1')){$sourceHashes[$name]=(Get-FileHash (Join-Path $root $name)).Hash.ToLowerInvariant()}
$input=[ordered]@{schema='folderwing-package-acceptance-input/v1';sources=$sourceHashes;images=$images;release_manifest_sha256=(Get-FileHash (Join-Path $DistributionDirectory 'RELEASE-MANIFEST.json')).Hash.ToLowerInvariant();workers=1;physical_input=$false;network='none';full_suite=$false;os=[Environment]::OSVersion.VersionString}
$text=$input|ConvertTo-Json -Depth 8 -Compress;[IO.File]::WriteAllText((Join-Path $ResultDirectory 'INPUT.json'),$text);$key=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($text))).ToLowerInvariant()
$checks=[Collections.Generic.List[object]]::new()
function Check([string]$Name,[bool]$Okay){$checks.Add(@{name=$Name;outcome=$(if($Okay){'passed'}else{'failed'})});if(-not $Okay){throw "Package check failed: $Name"};Write-Output ('ok '+$Name)}
function Owned([string]$Exe,[string[]]$Arguments){
 $pi=[Diagnostics.ProcessStartInfo]::new($Exe);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true
 foreach($a in $Arguments){[void]$pi.ArgumentList.Add($a)}
 $p=[Diagnostics.Process]::Start($pi);$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync();if(-not $p.WaitForExit(60000)){$p.Kill($true);throw 'Owned package command timed out'};$code=$p.ExitCode;$p.Dispose();return @{code=$code;stdout=$out.Result;stderr=$err.Result}
}
function Package([string]$Build,[string]$Source,[string]$Output,[switch]$Public){$a=@((Join-Path $root 'tools\package-release.py'),'--build-directory',$Build,'--media-source-archive',$Source,'--output-directory',$Output);if($Public){$a+='--publish-ready'};return Owned $Python $a}
try{
 foreach($a in $manifest.artifacts.psobject.Properties){Check ('artifact checksum '+$a.Name) ((Get-FileHash (Join-Path $DistributionDirectory $a.Name)).Hash.ToLowerInvariant() -ceq $a.Value.sha256)}
 $pdfpin=(Get-Content (Join-Path $root 'tools\release-inputs.json') -Raw|ConvertFrom-Json).dependencies.pdfium
 $pdfrecipe=Join-Path $DistributionDirectory $pdfpin.recipe_bundle.filename
 Check 'accepted PDFium recipe accompanies the release' ((Get-FileHash $pdfrecipe).Hash.ToLowerInvariant() -ceq $pdfpin.recipe_bundle.sha256)
 $zipPath=Get-ChildItem $DistributionDirectory -Filter 'Folderwing-*-windows-x64.zip'|Select-Object -ExpandProperty FullName
 $extract=Join-Path $ResultDirectory 'Extracted Folderwing 你好';[IO.Compression.ZipFile]::ExtractToDirectory($zipPath,$extract)
 $app=Get-ChildItem $extract -Directory|Select-Object -ExpandProperty FullName
 $info=Get-Content (Join-Path $app 'BUILDINFO.json') -Raw|ConvertFrom-Json
 Check 'package uses the accepted GNU PDFium DLL' ((Get-FileHash (Join-Path $app 'pdfium.dll')).Hash.ToLowerInvariant() -ceq $pdfpin.source_sha256.'bin/pdfium.dll')
 Check 'GNU runtime notices replace the active Microsoft runtime note' ((Test-Path (Join-Path $app 'licenses\third-party\pdfium-gnu-runtime\COPYING.winpthreads.txt')) -and -not(Test-Path (Join-Path $app 'licenses\third-party\pdfium\MICROSOFT-RUNTIME-NOTE.txt')))
 foreach($member in $info.files.psobject.Properties){Check ('ZIP member '+$member.Name) ((Get-FileHash (Join-Path $app $member.Name)).Hash.ToLowerInvariant() -ceq $member.Value.sha256)}
 $files=@(Get-ChildItem $app -File|ForEach-Object Name);Check 'ZIP excludes tests, old executables, settings and Direct' (@($files|Where-Object {$_ -match '(^fp.*\.(exe|dll)$|tests\.exe$|\.ini$|^direct|\.old$)'}).Count -eq 0)
 $exe=Join-Path $app 'folderwing.exe';$r=Owned $exe @('--functions','--settings=','--session=');Check 'extracted actual app runs with spaces and Unicode in its path' ($r.code -eq 0 -and $r.stdout.Contains('Folderwing functions'))
 $r=Owned (Join-Path $app 'folderwing-cli.exe') @('ls',$app);Check 'extracted CLI lists its own package' ($r.code -eq 0 -and $r.stdout.Contains('folderwing.exe'))
 $dll=[Runtime.InteropServices.NativeLibrary]::Load((Join-Path $app 'folderwing-engine.dll'));try{Check 'extracted engine exports remain loadable' ([Runtime.InteropServices.NativeLibrary]::GetExport($dll,'fpe_engine_create') -ne [IntPtr]::Zero)}finally{[Runtime.InteropServices.NativeLibrary]::Free($dll)}
 $wrong=Join-Path $ResultDirectory $pin.source_bundle.filename;[IO.File]::WriteAllText($wrong,'owned wrong corresponding sources')
 $badOut=Join-Path $ResultDirectory 'bad-source';$r=Package $BuildDirectory $wrong $badOut
 Check 'wrong corresponding-source hash refuses before creating artifacts' ($r.code -ne 0 -and $r.stderr.Contains('Corresponding-source checksum mismatch') -and -not(Test-Path $badOut))
 $existing=Join-Path $ResultDirectory 'existing';New-Item -ItemType Directory $existing|Out-Null;[IO.File]::WriteAllText((Join-Path $existing 'keep.txt'),'preserve this directory');$r=Package $BuildDirectory $source $existing
 Check 'existing output directory and sentinel are preserved' ($r.code -ne 0 -and [IO.File]::ReadAllText((Join-Path $existing 'keep.txt')) -eq 'preserve this directory')
 $tampered=Join-Path $ResultDirectory 'tampered';Copy-Item -LiteralPath $BuildDirectory -Destination $tampered -Recurse
 [IO.File]::WriteAllText((Join-Path $tampered 'folderwing-cli.exe'),'owned changed image');$badOut=Join-Path $ResultDirectory 'bad-image';$r=Package $tampered $source $badOut
 Check 'changed runtime image refuses before creating artifacts' ($r.code -ne 0 -and $r.stderr.Contains('Release input mismatch: folderwing-cli.exe') -and -not(Test-Path $badOut))
 if(-not $manifest.public_release_ready){$badOut=Join-Path $ResultDirectory 'not-cleared';$r=Package $BuildDirectory $source $badOut -Public;Check 'public package gate retains known dependency-review blockers' ($r.code -ne 0 -and $r.stderr.Contains('Public-release dependency closure remains open') -and -not(Test-Path $badOut))}
 foreach($p in $sourceHashes.GetEnumerator()){if((Get-FileHash (Join-Path $root $p.Key)).Hash.ToLowerInvariant() -cne $p.Value){throw 'Acceptance source changed'}}
 @{schema='folderwing-package-acceptance/v1';outcome='passed';input_key=$key;checks=$checks.ToArray();workers=1;physical_input=$false;wsl_stopped=$false;full_suite=$false}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
}finally{$checks.ToArray()|ConvertTo-Json -Depth 6|Set-Content (Join-Path $ResultDirectory 'CHECKS.json')}
