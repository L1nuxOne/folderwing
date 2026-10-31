$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-table-test-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
try {
 $exe=Join-Path $scratch 'tests.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" (Join-Path $root 'app\markdown\DelimitedReader.cs') (Join-Path $root 'tests\delimited_reader_tests.cs')
 if($LASTEXITCODE){throw 'Delimited reader test compilation failed'}
 & $exe
 if($LASTEXITCODE){throw 'Delimited reader tests failed'}
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
