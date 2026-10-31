<# Owned off-screen parent, real WebView2 helper and locked extended-length paths. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$helper=Join-Path $BuildDirectory 'folderwing-spreadsheet.exe'
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-spreadsheet-smoke-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch|Out-Null
$lock=$null
try {
 & python (Join-Path $PSScriptRoot 'fixtures\spreadsheet\create_variants.py') $scratch
 if($LASTEXITCODE){throw 'Fixture setup failed'}
 $exe=Join-Path $scratch 'probe.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll (Join-Path $PSScriptRoot 'markdown_host_smoke.cs')
 if($LASTEXITCODE){throw 'Host probe compilation failed'}
 $doc=Join-Path $scratch 'base.xlsx';$lock=[IO.File]::Open($doc,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 & $exe $helper ('\\?\'+$doc) 3 0
 if($LASTEXITCODE){throw 'Real workbook helper failed'}
 $lock.Dispose();$lock=$null
 & $exe $helper (Join-Path $scratch 'inflated.xlsx') 0 1
 if($LASTEXITCODE){throw 'Invalid workbook error did not render'}
 Write-Output 'Real spreadsheet WebView2 host and locked archive-style path checks passed.'
}finally{if($lock){$lock.Dispose()};Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
