<# Restore the three pinned spreadsheet assemblies from checksum-verified NuGet packages. #>
param([string]$Destination)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$dest=if($Destination){$Destination}else{Join-Path $root 'third_party\spreadsheet'}
$manifest=Get-Content (Join-Path $PSScriptRoot 'spreadsheet-dependencies.json') -Raw|ConvertFrom-Json
if($manifest.schema -ne 'folderwing-spreadsheet-dependencies/v1'){throw 'Invalid spreadsheet dependency manifest'}
function Verify([string]$directory){
 foreach($item in $manifest.packages){$file=Join-Path $directory $item.dll;if(!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.dll_sha256){throw "Spreadsheet dependency checksum mismatch: $file"}}
}
if(Test-Path -LiteralPath $dest){Verify $dest;return}
$stage="$dest.stage-$PID-$([guid]::NewGuid().ToString('N'))"
$temps=@()
try {
 New-Item -ItemType Directory -Force $stage|Out-Null
 Add-Type -AssemblyName System.IO.Compression.FileSystem
 foreach($item in $manifest.packages){
  $temp=Join-Path ([IO.Path]::GetTempPath()) ('folderwing-spreadsheet-'+[guid]::NewGuid().ToString('N')+'.nupkg');$temps+= $temp
  $cache=Join-Path $root ('build\downloads\'+$item.id+'.'+$item.version+'.nupkg')
  if(Test-Path -LiteralPath $cache){Copy-Item -LiteralPath $cache -Destination $temp}
  else{& curl.exe --fail --silent --show-error --location --proto '=https' --proto-redir '=https' --max-time 90 --retry 0 --output $temp $item.url;if($LASTEXITCODE){throw "Download failed: $($item.id)"}}
  if((Get-Item -LiteralPath $temp).Length -ne $item.archive_bytes -or (Get-FileHash -LiteralPath $temp -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.archive_sha256){throw "Spreadsheet package checksum mismatch: $($item.id)"}
  $zip=[IO.Compression.ZipFile]::OpenRead($temp)
  try{$entry=$zip.GetEntry($item.entry);if(!$entry){throw "Missing spreadsheet DLL: $($item.entry)"};[IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $stage $item.dll))}finally{$zip.Dispose()}
 }
 Verify $stage
 if(Test-Path -LiteralPath $dest){throw 'Spreadsheet dependency destination appeared during setup'}
 Move-Item -LiteralPath $stage -Destination $dest
 Verify $dest
}finally{foreach($temp in $temps){Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue};if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force}}
