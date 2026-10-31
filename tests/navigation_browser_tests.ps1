# Real renderer assets in one owned headless Edge profile per format; no desktop input.
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-navigation-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $scratch|Out-Null
try {
 foreach($name in @('markdown','word','spreadsheet')){Copy-Item "$root\app\$name" $scratch -Recurse;Copy-Item "$root\app\navigation\rail.js" "$scratch\$name\navigation.js";Copy-Item "$root\app\navigation\rail.css" "$scratch\$name\navigation.css"}
 $word=[Convert]::ToBase64String([IO.File]::ReadAllBytes("$root\tests\fixtures\word\synthetic.docx"));$fixture=[IO.File]::ReadAllText("$PSScriptRoot\navigation_browser_fixture.js")
 $cases=[ordered]@{markdown='markdown/index.html';json='markdown/structured.html';jsonl='markdown/structured.html';yaml='markdown/config.html';toml='markdown/config.html';ini='markdown/config.html';xml='markdown/xml.html';notebook='markdown/notebook.html';table='markdown/table.html';spreadsheet='spreadsheet/index.html';word='word/index.html'}
 foreach($case in $cases.Keys){
  $source=Join-Path $scratch $cases[$case];$directory=Split-Path $source;$page=Join-Path $directory "nav-$case.html"
  [IO.File]::WriteAllText((Join-Path $directory "fixture-$case.js"),"window.navigationCase='$case';window.navigationDocx='$word';"+$fixture)
  [IO.File]::WriteAllText($page,[IO.File]::ReadAllText($source).Replace('<head>','<head><script defer src="fixture-'+$case+'.js"></script>'))
  $start=[Diagnostics.ProcessStartInfo]::new((Join-Path ${env:ProgramFiles(x86)} 'Microsoft\Edge\Application\msedge.exe'));$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
  foreach($arg in @('--headless','--disable-gpu','--disable-background-networking','--disable-extensions','--no-first-run','--allow-file-access-from-files',"--user-data-dir=$scratch\profile-$case",'--virtual-time-budget=15000','--dump-dom',([Uri]$page).AbsoluteUri)){[void]$start.ArgumentList.Add($arg)}
  $p=[Diagnostics.Process]::Start($start);$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(45000)){$p.Kill($true);throw "$case timed out"};$exit=$p.ExitCode;$p.Dispose();$html=$out.Result
  if($exit -or $html -notmatch 'data-nav-test="passed"'){$detail=if($html -match 'data-nav-test="([^"]+)"'){$Matches[1]}else{'no result'};throw "$case rail acceptance failed: $detail (exit $exit)"};Write-Output "PASS $case rail, bookmark jump/remove, context and idle checks"
 }
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
