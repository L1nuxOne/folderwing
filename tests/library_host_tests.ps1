param([string]$BuildDirectory)
$ErrorActionPreference='Stop';$env:PATHEXT=[Environment]::GetEnvironmentVariable('PATHEXT','Machine');$root=Split-Path $PSScriptRoot -Parent;if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\preview-navigation'}
$zig=$env:ZIG;if(!$zig){$zig=Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-library-host-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $scratch|Out-Null;$exe=Join-Path $BuildDirectory ('library-host-'+[guid]::NewGuid().ToString('N')+'.exe');$previous=$env:FOLDERWING_LIBRARY_CONFIG
try{
 & python "$PSScriptRoot\library_fixtures.py" "$scratch\fixtures";if($LASTEXITCODE){throw 'Library fixtures failed'}
 # Replace only our synthetic placeholder PDF with an actual standalone PDF fixture.
 $pdf=Join-Path $root 'build\pdf-navigation-fixtures\plain.pdf';if(!(Test-Path $pdf)){throw 'Generate the owned PDF navigation fixtures first'};Copy-Item $pdf "$scratch\fixtures\calibre\Author\First (1)\First book.pdf" -Force
 $registry="[Libraries]`r`nCount=3`r`nHeight=220`r`n";$at=0;foreach($spec in @(@('Calibre','calibre'),@('Zotero','zotero'),@('Empty','empty'))){$at++;$type=if($at -eq 1){'calibre'}else{'zotero'};$registry+="`r`n[Library$at]`r`nName=$($spec[0])`r`nType=$type`r`nPath=$scratch\fixtures\$($spec[1])`r`n"};$config=Join-Path $scratch 'libraries.ini';[IO.File]::WriteAllText($config,$registry,[Text.Encoding]::Unicode);$env:FOLDERWING_LIBRARY_CONFIG=$config
 $build=[IO.File]::ReadAllText("$root\build.ps1");$match=[regex]::Match($build,'(?s)\$app\s*=\s*@\((.*?)\)\s*# Reproducible');if(!$match.Success){throw 'Application source list missing'}
 $sources=@([regex]::Matches($match.Groups[1].Value,'\$root\\app\\[a-zA-Z0-9_]+\.c')|ForEach-Object {$_.Value.Replace('$root',$root)}|Where-Object {$_ -notmatch '\\(fpdemo|archiveview|pdfview)\.c$'});$engine=@(Get-ChildItem "$root\src\*.c"|ForEach-Object FullName)
 & $zig cc -target x86_64-windows-gnu -std=c11 -municode -O2 -DNDEBUG -DUNICODE -D_UNICODE "-I$root\include" -Wno-unused-parameter -Wno-missing-field-initializers @engine @sources "$PSScriptRoot\fixtures\native\library_host.c" "$PSScriptRoot\fixtures\native\library_pdf_probe.c" -ladvapi32 -lkernel32 -luser32 -lshell32 -lole32 -lmpr -lgdi32 -lmsimg32 -ldwmapi -luxtheme -luuid -limm32 -o $exe
 if($LASTEXITCODE){throw 'Library main-host compilation failed'}
 $p=Start-Process $exe -ArgumentList ('"'+$scratch+'\fixtures"') -NoNewWindow -PassThru -Wait;if($p.ExitCode){throw 'Library main-host checks failed'}
}finally{$env:FOLDERWING_LIBRARY_CONFIG=$previous;Remove-Item $exe -Force -ErrorAction SilentlyContinue;Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
