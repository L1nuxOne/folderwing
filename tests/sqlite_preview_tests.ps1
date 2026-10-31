<# Focused read-only SQLite tests. All fixture data lives in a private temporary directory. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$dll=Join-Path $BuildDirectory 'folderwing-sqlite.dll'
if(!(Test-Path $dll)){throw 'Build folderwing-sqlite.dll first'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-sqlite-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
try {
 Copy-Item $dll $scratch
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 $exe=Join-Path $scratch 'sqlite-test.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" (Join-Path $root 'app\sqlite\SqliteReader.cs') (Join-Path $PSScriptRoot 'sqlite_preview_tests.cs')
 if($LASTEXITCODE){throw 'SQLite test compilation failed'}
 & $exe $scratch
 if($LASTEXITCODE){throw 'SQLite preview tests failed'}
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
