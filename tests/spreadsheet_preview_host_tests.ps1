<# Compile the real app host into an owned off-screen acceptance probe. #>
param([string]$BuildDirectory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\release'}
$zig=$env:ZIG;if(!$zig){$zig=Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}
$testdir=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-spreadsheet-host-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $testdir | Out-Null
$exe=Join-Path $BuildDirectory ('spreadsheet-host-test-'+[guid]::NewGuid().ToString('N')+'.exe')
try {
 $build=[IO.File]::ReadAllText((Join-Path $root 'build.ps1'))
 $match=[regex]::Match($build,'(?s)\$app\s*=\s*@\((.*?)\)\s*# Reproducible')
 if(!$match.Success){throw 'Could not identify the application source list'}
 $sources=@([regex]::Matches($match.Groups[1].Value,'\$root\\app\\[a-zA-Z0-9_]+\.c')|ForEach-Object {$_.Value.Replace('$root',$root)}|Where-Object {$_ -notmatch '\\(fpdemo|archiveview)\.c$'})
 $engine=@(Get-ChildItem (Join-Path $root 'src\*.c')|ForEach-Object FullName)
 $fixture=Join-Path $testdir 'fixture'
 & python (Join-Path $PSScriptRoot 'fixtures\native\spreadsheet_preview_host_fixtures.py') $fixture
 if($LASTEXITCODE){throw 'Spreadsheet host fixture setup failed'}
 $flags=@('cc','-target','x86_64-windows-gnu','-std=c11','-municode','-O2','-DNDEBUG','-DUNICODE','-D_UNICODE',"-I$root\include",'-Wno-unused-parameter','-Wno-missing-field-initializers')
 & $zig @flags @engine @sources (Join-Path $PSScriptRoot 'fixtures\native\spreadsheet_preview_host.c') '-ladvapi32' '-lkernel32' '-luser32' '-lshell32' '-lole32' '-lmpr' '-lgdi32' '-lmsimg32' '-ldwmapi' '-luxtheme' '-luuid' '-limm32' '-o' $exe
 if($LASTEXITCODE){throw 'Spreadsheet host probe compilation failed'}
 & $exe $fixture
 if($LASTEXITCODE){throw 'Spreadsheet app host acceptance failed'}
} finally {Remove-Item -LiteralPath $exe -Force -ErrorAction SilentlyContinue;Remove-Item -LiteralPath $testdir -Recurse -Force -ErrorAction SilentlyContinue}
