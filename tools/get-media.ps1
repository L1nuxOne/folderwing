<# Restore only the exact Folderwing LGPL bundle; no fallback to a GPL build. #>
param([string]$ArchivePath=$env:FOLDERWING_MEDIA_ARCHIVE)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$target=Join-Path $root 'third_party/mpv';$pin=Get-Content (Join-Path $PSScriptRoot 'media-dependencies.json') -Raw|ConvertFrom-Json
if($pin.schema -ne 2 -or $pin.license -ne 'LGPL-3.0-or-later'){throw 'Expected the pinned Folderwing LGPL media manifest'}
function Safe-Path([string]$Directory,[string]$Name){if(-not $Name -or $Name -match '(^[/\\]|:|(^|[/\\])\.\.([/\\]|$))'){throw 'Unsafe media asset path'};return Join-Path $Directory $Name}
function Valid-MediaInputs([string]$Directory){foreach($entry in $pin.files.psobject.Properties){$file=Safe-Path $Directory $entry.Name;if(-not(Test-Path -LiteralPath $file -PathType Leaf)){return $false};if((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value.sha256){return $false}};return $true}
if(Valid-MediaInputs $target){return}
if(-not $ArchivePath){$local=Join-Path $root ('build/dependencies/'+$pin.archive_name);if(Test-Path -LiteralPath $local){$ArchivePath=$local}}
if(-not $ArchivePath -and $pin.archive_url){$ArchivePath=Join-Path ([IO.Path]::GetTempPath()) $pin.archive_name;Invoke-WebRequest -Uri $pin.archive_url -OutFile $ArchivePath}
if(-not $ArchivePath -or -not(Test-Path -LiteralPath $ArchivePath -PathType Leaf)){throw 'Pinned LGPL media bundle missing. Build it using docs/MEDIA_LGPL_BUILD.md, then set FOLDERWING_MEDIA_ARCHIVE or pass -ArchivePath. No unreviewed replacement will be downloaded.'}
if((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant() -ne $pin.archive_sha256){throw 'Pinned LGPL media bundle checksum mismatch'}
$stage=$target+'.stage-'+[guid]::NewGuid().ToString('N');$backup=$target+'.previous-'+[guid]::NewGuid().ToString('N');New-Item -ItemType Directory -Force (Split-Path $target -Parent),$stage|Out-Null
try{
 $zip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ArchivePath));try{foreach($entry in $zip.Entries){$file=Safe-Path $stage $entry.FullName;if($entry.FullName.EndsWith('/')){New-Item -ItemType Directory -Force $file|Out-Null;continue};if(-not $pin.files.psobject.Properties[$entry.FullName]){throw 'Unexpected file in media bundle'};New-Item -ItemType Directory -Force (Split-Path $file -Parent)|Out-Null;[IO.Compression.ZipFileExtensions]::ExtractToFile($entry,$file,$false)}}finally{$zip.Dispose()}
 if(-not(Valid-MediaInputs $stage)){throw 'LGPL runtime/header/notices set is incomplete or changed'}
 $moved=$false
 if(Test-Path -LiteralPath $target){Move-Item -LiteralPath $target -Destination $backup;$moved=$true}
 try{Move-Item -LiteralPath $stage -Destination $target}catch{if($moved -and -not(Test-Path -LiteralPath $target)){Move-Item -LiteralPath $backup -Destination $target};throw}
 Write-Output ('Restored '+$pin.version+'; previous dependency retained at '+$(if($moved){$backup}else{'none'}))
}finally{if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force}}
