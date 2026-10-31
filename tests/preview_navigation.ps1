# Shared store, native source and SQLite acceptance; isolated synthetic fixtures.
param([string]$BuildDirectory)
$ErrorActionPreference='Stop';$root=Split-Path $PSScriptRoot -Parent;if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\preview-navigation'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-preview-navigation-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $scratch|Out-Null
function Run-Owned($exe,$arguments){$si=[Diagnostics.ProcessStartInfo]::new($exe);$si.UseShellExecute=$false;$si.CreateNoWindow=$true;$si.RedirectStandardOutput=$true;$si.RedirectStandardError=$true;foreach($a in $arguments){[void]$si.ArgumentList.Add($a)};$p=[Diagnostics.Process]::Start($si);$out=$p.StandardOutput.ReadToEndAsync();$err=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(120000)){$p.Kill($true);throw 'Owned navigation test timed out'};$exit=$p.ExitCode;$p.Dispose();Write-Output $out.Result;if($exit){Write-Output $err.Result;throw "Navigation test failed: $exit"}}
try{
 Copy-Item "$BuildDirectory\folderwing-sqlite.dll" $scratch
 $csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe';$managed=Join-Path $scratch 'navigation-managed.exe'
 & $csc /nologo /target:exe /platform:x64 /main:NavigationManagedTests "/out:$managed" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "$root\app\navigation\BookmarkStore.cs" "$root\app\navigation\NativeNavigationRail.cs" "$root\app\sqlite\DatabaseHost.cs" "$root\app\sqlite\SqliteReader.cs" "$PSScriptRoot\navigation_managed_tests.cs"
 if($LASTEXITCODE){throw 'Managed navigation test build failed'}
 $text=(0..2999|ForEach-Object {'Line '+$_+': '+('reading content '*6)}) -join "`r`n";foreach($name in @('large.txt','archive-a.txt','archive-b.txt')){[IO.File]::WriteAllText((Join-Path $scratch $name),$text)}
 Run-Owned $managed @($scratch,'before')
 Run-Owned (Join-Path $BuildDirectory 'preview_navigation_tests.exe') @($scratch)
 Run-Owned $managed @($scratch,'after')
 $bridge=Join-Path $scratch 'navigation-bridge.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$bridge" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll "$root\app\navigation\BookmarkStore.cs" "$PSScriptRoot\navigation_bridge_tests.cs"
 if($LASTEXITCODE){throw 'Real WebView bridge probe build failed'}
 $md=Join-Path $scratch 'staging.md';[IO.File]::WriteAllText($md,"# Heading`n`n"+('Reading text '*2000)+"`n`n## Second section`n`nMore text.")
 Run-Owned $bridge @((Join-Path $BuildDirectory 'folderwing-markdown.exe'),$md,(Join-Path $scratch 'bridge.fwb'),("C:\owned.zip"+[char]0x1f+'0061002E006D0064'))
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
