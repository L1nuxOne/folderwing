<# Tests real helper decoding in an owned off-screen window and a private asset copy. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-ini-encoding-'+[guid]::NewGuid().ToString('N'))
$assets=Join-Path $scratch 'markdown';New-Item -ItemType Directory -Force (Join-Path $assets 'vendor') | Out-Null
try {
 foreach($name in @('folderwing-config.exe','Microsoft.Web.WebView2.Core.dll','Microsoft.Web.WebView2.WinForms.dll','WebView2Loader.dll')){Copy-Item (Join-Path $BuildDirectory $name) $scratch}
 foreach($name in @('config.html','config-parser.js','structured.js','structured.css')){Copy-Item (Join-Path $root "app\markdown\$name") $assets}
 Copy-Item (Join-Path $root 'app\markdown\vendor\config-parsers.js') (Join-Path $assets 'vendor')
 [IO.File]::AppendAllText((Join-Path $assets 'structured.js'),[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'ini_encoding_ui_fixture.js')))
 $document=Join-Path $scratch 'sample.ini';[IO.File]::WriteAllBytes($document,[Text.Encoding]::GetEncoding(1252).GetBytes("[General]`r`nname=café`r`n"))
 $probe=Join-Path $scratch 'probe.cs';$source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'markdown_host_smoke.cs')).Replace('m.Msg==0x8055','m.Msg==0x8055 && m.WParam.ToInt32()==99');[IO.File]::WriteAllText($probe,$source)
 $exe=Join-Path $scratch 'probe.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll $probe
 if($LASTEXITCODE){throw 'INI encoding probe compilation failed'}
 & $exe (Join-Path $scratch 'folderwing-config.exe') $document 99
 if($LASTEXITCODE){throw 'INI encoding selection test failed'}
 Write-Output 'INI automatic and manual encoding checks passed.'
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
