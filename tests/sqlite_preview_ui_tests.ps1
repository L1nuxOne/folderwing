<# Off-screen, real helper and owned synthetic SQLite file; no physical input. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-sqlite-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
try {
 Copy-Item (Join-Path $BuildDirectory 'folderwing-sqlite.dll') $scratch
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe';$exe=Join-Path $scratch 'sqlite-ui-test.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll (Join-Path $PSScriptRoot 'sqlite_preview_ui_tests.cs')
 if($LASTEXITCODE){throw 'SQLite UI probe compilation failed'}
 & $exe (Join-Path $BuildDirectory 'folderwing-database.exe') $scratch
 if($LASTEXITCODE){throw 'SQLite UI acceptance failed'}
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
