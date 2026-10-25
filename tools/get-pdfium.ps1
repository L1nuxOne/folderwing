<# Restore pinned GNU PDFium. Recognized legacy inputs can be backed up and migrated explicitly. #>
param([string]$Archive=$env:FOLDERWING_PDFIUM_ARCHIVE,[string]$BaseUrl=$env:FOLDERWING_PDFIUM_BASE_URL,[string]$Python='python',[switch]$MigrateExisting)
$ErrorActionPreference='Stop';$oldExtensions=$env:PATHEXT
try {
 $env:PATHEXT=[Environment]::GetEnvironmentVariable('PATHEXT','Machine')
 $arguments=@((Join-Path $PSScriptRoot 'get-pdfium.py'))
 if($Archive){$arguments+=@('--archive',$Archive)}
 if($BaseUrl){$arguments+=@('--base-url',$BaseUrl)}
 if($MigrateExisting){$arguments+='--migrate-existing'}
 & $Python @arguments
 if($LASTEXITCODE){throw 'Pinned PDFium restore failed; inspect the retained input/backup.'}
} finally { $env:PATHEXT=$oldExtensions }
