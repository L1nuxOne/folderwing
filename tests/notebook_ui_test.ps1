<# Off-screen Chromium test using only synthetic notebook data and an owned profile. #>
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-notebook-ui-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force (Join-Path $scratch 'vendor') | Out-Null
try {
 foreach($name in @('notebook.html','notebook.css','notebook.js','notebook-util.js','math-config.js','renderer.js')){Copy-Item (Join-Path $root "app\markdown\$name") $scratch}
 foreach($name in @('markdown-it.min.js','tex-svg.js','highlight.min.js','highlight-theme.css')){Copy-Item (Join-Path $root "app\markdown\vendor\$name") (Join-Path $scratch 'vendor')}
 Copy-Item (Join-Path $PSScriptRoot 'notebook_ui_fixture.js') (Join-Path $scratch 'fixture.js')
 Copy-Item "$root\app\navigation\rail.js" (Join-Path $scratch 'navigation.js');Copy-Item "$root\app\navigation\rail.css" (Join-Path $scratch 'navigation.css')
 Add-Content (Join-Path $scratch 'fixture.js') -Value ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'navigation_mock_bridge.js')))
 $html=Join-Path $scratch 'notebook.html';$content=[IO.File]::ReadAllText($html).Replace('<script defer src="notebook.js">','<script defer src="fixture.js"></script><script defer src="notebook.js">');[IO.File]::WriteAllText($html,$content)
 $edge=Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe';if(!(Test-Path $edge)){throw 'Microsoft Edge is required for the notebook browser test'}
 $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$edge;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($arg in @('--headless','--disable-gpu','--disable-background-networking','--disable-extensions','--no-first-run',('--user-data-dir='+ (Join-Path $scratch 'profile')),'--virtual-time-budget=12000','--dump-dom',([Uri]$html).AbsoluteUri)){[void]$start.ArgumentList.Add($arg)}
 $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(!$process.WaitForExit(45000)){$process.Kill($true);throw 'Notebook browser test timed out'}
 $output=$stdout.Result;$errors=$stderr.Result;$exit=$process.ExitCode;$process.Dispose()
 if($exit -ne 0 -or $output -notmatch 'data-test="passed"'){throw "Notebook browser test failed (exit $exit)"}
 Write-Output 'Notebook browser interaction and PNG decode passed.'
} finally {Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
