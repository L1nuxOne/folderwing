<# Publish the exact manifest-listed runtime and notices, preserving mapped images. #>
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;$source=Join-Path $root 'third_party/mpv';$pin=Get-Content (Join-Path $PSScriptRoot 'media-dependencies.json') -Raw|ConvertFrom-Json
if($pin.schema -ne 2 -or -not $pin.distribution_files){throw 'Missing LGPL distribution file manifest'}
. (Join-Path $PSScriptRoot 'build-output.ps1')
foreach($entry in $pin.distribution_files.psobject.Properties){
 if($entry.Name -match '(^[/\\]|:|(^|[/\\])\.\.([/\\]|$))' -or $entry.Value -match '(^[/\\]|:|(^|[/\\])\.\.([/\\]|$))'){throw 'Unsafe media distribution path'}
 $inputFile=Join-Path $source $entry.Name;$expected=$pin.files.psobject.Properties[$entry.Name].Value.sha256
 if(-not $expected -or (Get-FileHash -LiteralPath $inputFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected){throw ('Media input mismatch: '+$entry.Name)}
 $outputFile=Join-Path $OutputDirectory $entry.Value;$directory=Split-Path $outputFile -Parent;New-Item -ItemType Directory -Force $directory|Out-Null;Publish-BuildAsset $inputFile $directory ([IO.Path]::GetFileName($outputFile))
}
