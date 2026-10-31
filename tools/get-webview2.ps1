# Microsoft WebView2 SDK 1.0.4258.31, verified against the NuGet package and selected files.
param([string]$Destination)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$dest=if($Destination){$Destination}else{Join-Path $root 'third_party\webview2'}
$version='1.0.4258.31'
$archiveHash='56f7f4b8bf9aee4b8efefbbdd4f67d5f74ebd1b100ed0806da71bf76af481aa9'
$files=@{
 'Microsoft.Web.WebView2.Core.dll'=@('lib/net462/Microsoft.Web.WebView2.Core.dll','d60e6e94245078cb56e44ebd5f360dc04bf18e460136be93bfc4cfca43b98505')
 'Microsoft.Web.WebView2.WinForms.dll'=@('lib/net462/Microsoft.Web.WebView2.WinForms.dll','6a9e6856f23c463783c3afb0ca5d78f56324c86657162ccb16d5227b3233b32a')
 'WebView2Loader.dll'=@('build/native/x64/WebView2Loader.dll','3426dcc55fdfb8b5e7ac623cf33b4ccf283fbf68a0c5a65bddfdb4d749a8abee')
}
function Verify {
 foreach($name in $files.Keys){$path=Join-Path $dest $name;if(!(Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $files[$name][1]){throw "WebView2 SDK checksum mismatch: $path"}}
}
if(Test-Path $dest){Verify;return}
$cache=Join-Path $env:USERPROFILE ".nuget\packages\microsoft.web.webview2\$version\microsoft.web.webview2.$version.nupkg"
$temp=Join-Path ([IO.Path]::GetTempPath()) ("folderwing-webview2-"+[guid]::NewGuid().ToString('N')+'.zip')
$stage="$dest.stage-$PID"
try {
 if(Test-Path $cache){Copy-Item $cache $temp}else{& curl.exe -sSfL -o $temp "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$version/microsoft.web.webview2.$version.nupkg";if($LASTEXITCODE){throw 'WebView2 SDK download failed'}}
 if((Get-FileHash $temp -Algorithm SHA256).Hash.ToLowerInvariant() -ne $archiveHash){throw 'WebView2 SDK archive checksum mismatch'}
 Add-Type -AssemblyName System.IO.Compression.FileSystem
 New-Item -ItemType Directory -Force $stage | Out-Null
 $zip=[IO.Compression.ZipFile]::OpenRead($temp)
 try {foreach($name in $files.Keys){$entry=$zip.GetEntry($files[$name][0]);if(!$entry){throw "Missing SDK file: $name"};[IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $stage $name))}} finally {$zip.Dispose()}
 Move-Item $stage $dest
 Verify
} finally {Remove-Item $temp -ErrorAction SilentlyContinue;if(Test-Path $stage){Remove-Item $stage -Recurse -Force}}
