<# Synthetic read-only XML parser checks; no desktop, network or user files. #>
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-xml-reader-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch | Out-Null
try {
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe';$exe=Join-Path $scratch 'xml-reader-test.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Xml.dll /reference:System.Web.Extensions.dll (Join-Path $root 'app\markdown\XmlPreviewReader.cs') (Join-Path $PSScriptRoot 'xml_preview_reader_tests.cs')
 if($LASTEXITCODE){throw 'XML reader test compilation failed'}
 & $exe
 if($LASTEXITCODE){throw 'XML reader tests failed'}
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
