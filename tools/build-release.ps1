<# Full-feature portable release build from pinned inputs; never publishes. #>
param(
 [string]$Python='python',
 [string]$MediaArchive=$env:FOLDERWING_MEDIA_ARCHIVE,
 [string]$MediaSourceArchive=$env:FOLDERWING_MEDIA_SOURCE_ARCHIVE,
 [string]$MediaBaseUrl=$env:FOLDERWING_MEDIA_BASE_URL,
 [string]$PdfiumArchive=$env:FOLDERWING_PDFIUM_ARCHIVE,
 [string]$PdfiumRecipeArchive=$env:FOLDERWING_PDFIUM_RECIPE_ARCHIVE,
 [string]$PdfiumBaseUrl=$env:FOLDERWING_PDFIUM_BASE_URL,
 [string]$OutputDirectory,
 [switch]$SkipBootstrap,
 [switch]$PublishReady
)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root 'build\distribution'}
$oldZig=$env:ZIG;$oldExtensions=$env:PATHEXT
try{
 $env:PATHEXT=[Environment]::GetEnvironmentVariable('PATHEXT','Machine')
 if(-not $SkipBootstrap){
  $args_=@((Join-Path $PSScriptRoot 'bootstrap-release.py'),'--with-toolchain-source')
  if($MediaArchive){$args_+=@('--media-archive',$MediaArchive)}
  if($MediaSourceArchive){$args_+=@('--media-source-archive',$MediaSourceArchive)}
  if($MediaBaseUrl){$args_+=@('--media-base-url',$MediaBaseUrl)}
  if($PdfiumArchive){$args_+=@('--pdfium-archive',$PdfiumArchive)}
  if($PdfiumRecipeArchive){$args_+=@('--pdfium-recipe-archive',$PdfiumRecipeArchive)}
  if($PdfiumBaseUrl){$args_+=@('--pdfium-base-url',$PdfiumBaseUrl)}
  & $Python @args_;if($LASTEXITCODE){throw 'Pinned release bootstrap failed'}
 }
 $env:ZIG=Join-Path $root 'third_party\zig\zig.exe'
 $files=Get-Content (Join-Path $PSScriptRoot 'release-files.json') -Raw|ConvertFrom-Json
 $build=Join-Path $root 'build\release-candidate'
 $targets=@($files.binaries|Where-Object {$_ -notin @('pdfium.dll','libmpv-2.dll')})+@('cli_tests.exe','workspace_tests.exe','ntfs_model_tests.exe','shell_protocol_tests.exe','branding_host_tests.exe','preview_navigation_tests.exe')
 # No silent reduced-feature release if a dependency bootstrap was skipped.
 foreach($path in @('third_party\pdfium\bin\pdfium.dll','third_party\libvterm\include\vterm.h','build\bootstrap-receipt.json')){if(-not(Test-Path (Join-Path $root $path) -PathType Leaf)){throw "Release prerequisite missing: $path"}}
 $sourceBefore=& $Python (Join-Path $PSScriptRoot 'release-build-receipt.py') --source-key
 if($LASTEXITCODE){throw 'Could not capture build source inputs'}
 & (Join-Path $root 'build.ps1') -Config release -OutputDirectory $build -Targets $targets
 if($LASTEXITCODE){throw 'Release compilation failed'}
 & $Python (Join-Path $PSScriptRoot 'release-build-receipt.py') --build-directory $build --compiler $env:ZIG --expected-source-key $sourceBefore
 if($LASTEXITCODE){throw 'Release build input verification failed'}
 & (Join-Path $root 'tests\release_smoke.ps1') -BuildDirectory $build -ResultDirectory (Join-Path $build ('smoke-'+[guid]::NewGuid().ToString('N')))
 $mpv=Get-Content (Join-Path $PSScriptRoot 'media-dependencies.json') -Raw|ConvertFrom-Json
 $source=Join-Path $root ('build\downloads\'+$mpv.source_bundle.filename)
 $args_=@((Join-Path $PSScriptRoot 'package-release.py'),'--build-directory',$build,'--media-source-archive',$source,'--output-directory',$OutputDirectory)
 if($PublishReady){$args_+='--publish-ready'}
 & $Python @args_;if($LASTEXITCODE){throw 'Release packaging failed'}
}finally{$env:ZIG=$oldZig;$env:PATHEXT=$oldExtensions}
