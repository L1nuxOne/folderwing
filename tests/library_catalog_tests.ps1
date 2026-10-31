param([string]$BuildDirectory,[string[]]$InspectLibrary)
$ErrorActionPreference='Stop';$env:PATHEXT=[Environment]::GetEnvironmentVariable('PATHEXT','Machine');$root=Split-Path $PSScriptRoot -Parent;if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'build\preview-navigation'}
$scratch=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-library-data-'+[guid]::NewGuid().ToString('N'));New-Item -ItemType Directory $scratch|Out-Null
try{Copy-Item "$BuildDirectory\folderwing-sqlite.dll" $scratch;$exe=Join-Path $scratch 'library-tests.exe';$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
 & $csc /nologo /target:exe /platform:x64 "/out:$exe" /reference:System.Core.dll "$root\app\library\CatalogDb.cs" "$root\app\library\Catalog.cs" "$PSScriptRoot\library_catalog_tests.cs";if($LASTEXITCODE){throw 'Catalogue test compilation failed'}
 & python "$PSScriptRoot\library_fixtures.py" "$scratch\fixtures";if($LASTEXITCODE){throw 'Catalogue fixture creation failed'}
 & $exe "$scratch\fixtures";if($LASTEXITCODE){throw 'Catalogue tests failed'}
 foreach($entry in $InspectLibrary){$spec=$entry.Split('|',2);if($spec.Count -ne 2){throw 'Use type|path for an inspection entry'};& $exe --inspect $spec[0] $spec[1];if($LASTEXITCODE){throw 'Read-only real library inspection failed'}}
}finally{Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}
