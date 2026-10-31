$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-word-package-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch|Out-Null
try {
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe';$exe=Join-Path $scratch 'word-package-test.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Xml.dll /reference:System.Xml.Linq.dll /reference:System.IO.Compression.dll (Join-Path $root 'app\word\WordPackage.cs') (Join-Path $PSScriptRoot 'word_package_tests.cs')
 if($LASTEXITCODE){throw 'Word package test compilation failed'}
 & $exe (Join-Path $PSScriptRoot 'fixtures\word\synthetic.docx') (Join-Path $scratch 'locked.docx')
 if($LASTEXITCODE){throw 'Word package tests failed'}
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
