<# Off-screen owned parent, actual WebView2 helper and locked extended path. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$helper=Join-Path $BuildDirectory 'folderwing-word.exe'
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-word-smoke-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $scratch|Out-Null
$lock=$null
try {
 $doc=Join-Path $scratch 'sample.docx';Copy-Item (Join-Path $PSScriptRoot 'fixtures\word\synthetic.docx') $doc
 $bad=Join-Path $scratch 'bad.docx';[IO.File]::WriteAllText($bad,'not a ZIP')
 $exe=Join-Path $scratch 'probe.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll (Join-Path $PSScriptRoot 'word_host_probe.cs')
 if($LASTEXITCODE){throw 'Word host probe compilation failed'}
 $lock=[IO.File]::Open($doc,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
 & $exe $helper ('\\?\'+$doc) 14 0
 if($LASTEXITCODE){throw 'Word helper did not render the locked fixture'}
 $lock.Dispose();$lock=$null
 $textbox=Join-Path $scratch 'textbox.docx';& python (Join-Path $PSScriptRoot 'fixtures\word\make_textbox_fixture.py') $textbox
 if($LASTEXITCODE){throw 'Word textbox fixture setup failed'}
 & $exe $helper $textbox 15 0
 if($LASTEXITCODE){throw 'Word helper did not render the textbox fixture'}
 & $exe $helper $bad 0 1
 if($LASTEXITCODE){throw 'Invalid Word document error did not render'}
 Write-Output 'Real Word WebView2 helper, locked path and error checks passed.'
}finally{if($lock){$lock.Dispose()};Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
