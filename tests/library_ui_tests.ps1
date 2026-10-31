param([string]$BuildDirectory)
$ErrorActionPreference='Stop';$env:PATHEXT=[Environment]::GetEnvironmentVariable('PATHEXT','Machine');$root=Split-Path $PSScriptRoot -Parent;if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\preview-navigation'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-library-ui-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $scratch|Out-Null
try{Copy-Item "$BuildDirectory\folderwing-sqlite.dll" $scratch;$exe=Join-Path $scratch 'library-ui.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 /main:LibraryUiTests "/out:$exe" /reference:System.Core.dll /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Web.Extensions.dll "$root\app\library\CatalogDb.cs" "$root\app\library\Catalog.cs" "$root\app\library\LibraryState.cs" "$root\app\library\LibraryHost.cs" "$PSScriptRoot\library_ui_tests.cs";if($LASTEXITCODE){throw 'Library UI probe compilation failed'}
 & python "$PSScriptRoot\library_fixtures.py" "$scratch\fixtures" --ui-attachments;if($LASTEXITCODE){throw 'Catalogue fixture creation failed'}
 $p=Start-Process $exe -ArgumentList ('"'+$scratch+'\fixtures"') -PassThru -NoNewWindow -Wait;if($p.ExitCode){throw 'Library UI checks failed'}
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
