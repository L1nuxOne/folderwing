<# Owned headless browser and synthetic VML text boxes; no physical input. #>
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-word-textbox-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory "$scratch\word\vendor","$scratch\markdown\vendor" -Force|Out-Null
try {
 Copy-Item "$root\app\word\*.html","$root\app\word\*.js","$root\app\word\*.css" "$scratch\word" -Force
 Copy-Item "$root\app\word\vendor\*.js" "$scratch\word\vendor" -Force
 Copy-Item "$root\app\markdown\math-config.js" "$scratch\markdown" -Force
 Copy-Item "$root\app\markdown\vendor\tex-svg.js" "$scratch\markdown\vendor" -Force
 $doc=Join-Path $scratch 'textbox.docx';& python (Join-Path $PSScriptRoot 'fixtures\word\make_textbox_fixture.py') $doc
 if($LASTEXITCODE){throw 'Word textbox fixture setup failed'}
 $encoded=[Convert]::ToBase64String([IO.File]::ReadAllBytes($doc))
 [IO.File]::WriteAllText("$scratch\word\fixture.js",'window.syntheticTextboxDocx="'+$encoded+'";'+[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'word_textbox_ui_fixture.js')))
 Copy-Item "$root\app\navigation\rail.js" (Join-Path "$scratch\word" 'navigation.js');Copy-Item "$root\app\navigation\rail.css" (Join-Path "$scratch\word" 'navigation.css')
 Add-Content "$scratch\word\fixture.js" -Value ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'navigation_mock_bridge.js')))
 $page="$scratch\word\index.html";$body=[IO.File]::ReadAllText($page).Replace('<script defer src="viewer.js">','<script defer src="fixture.js"></script><script defer src="viewer.js">');[IO.File]::WriteAllText($page,$body)
 $edge=Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe';if(!(Test-Path $edge)){throw 'Microsoft Edge is required for the Word browser test'}
 $start=[Diagnostics.ProcessStartInfo]::new();$start.FileName=$edge;$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($arg in @('--headless','--disable-gpu','--disable-background-networking','--disable-extensions','--no-first-run','--allow-file-access-from-files',('--user-data-dir='+(Join-Path $scratch 'profile')),'--virtual-time-budget=10000','--dump-dom',([Uri]$page).AbsoluteUri)){[void]$start.ArgumentList.Add($arg)}
 $proc=[Diagnostics.Process]::Start($start);$stdout=$proc.StandardOutput.ReadToEndAsync();$stderr=$proc.StandardError.ReadToEndAsync()
 if(!$proc.WaitForExit(45000)){$proc.Kill($true);throw 'Word textbox browser test timed out'}
 $output=$stdout.Result;$exit=$proc.ExitCode;$proc.Dispose()
 if($exit -ne 0 -or $output -notmatch 'data-test="passed"'){$detail=if($output -match 'data-test="([^"]+)"'){$Matches[1]}else{'no result'};throw "Word textbox browser test failed (exit $exit; $detail)"}
 Write-Output 'Word textbox reflow, contrast, math, search, copy and safety checks passed.'
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
