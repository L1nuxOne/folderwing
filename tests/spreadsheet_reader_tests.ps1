param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-xlsx-reader-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch|Out-Null
try {
 & python (Join-Path $PSScriptRoot 'fixtures\spreadsheet\create_variants.py') $scratch
 if($LASTEXITCODE){throw 'Spreadsheet fixture setup failed'}
 & (Join-Path $root 'tools\get-spreadsheet.ps1')
 $lib=Join-Path $root 'third_party\spreadsheet';Copy-Item "$lib\*.dll" $scratch
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe';$exe=Join-Path $scratch 'reader-tests.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" "/reference:$lib\ExcelDataReader.dll" "/reference:$lib\ExcelNumberFormat.dll" /reference:System.Xml.dll /reference:System.Data.dll /reference:System.IO.Compression.dll (Join-Path $root 'app\spreadsheet\WorkbookPackage.cs') (Join-Path $root 'app\spreadsheet\FormulaCursor.cs') (Join-Path $root 'app\spreadsheet\SpreadsheetReader.cs') (Join-Path $PSScriptRoot 'spreadsheet_reader_tests.cs')
 if($LASTEXITCODE){throw 'Spreadsheet reader test compilation failed'}
 & $exe $scratch
 if($LASTEXITCODE){throw 'Spreadsheet reader tests failed'}
} finally {Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue}
