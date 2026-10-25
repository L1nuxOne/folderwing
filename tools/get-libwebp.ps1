<# Downloads the pinned BSD-licensed WebP decoder source for folderwing-image.exe. #>
$ErrorActionPreference = 'Stop'
$version = '1.6.0'
$sha256 = 'e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564'
$url = "https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-$version.tar.gz"
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root 'third_party\libwebp'
$stamp = Join-Path $dest 'VERSION.openpilot'
if ((Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $version) { "libwebp $version already downloaded"; return }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "openpilot-libwebp-$PID.tgz"
try {
    curl.exe -sSfL -o $tmp $url
    if ($LASTEXITCODE -ne 0) { throw 'libwebp download failed' }
    if ((Get-FileHash $tmp -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) { throw 'libwebp checksum mismatch' }
    $stage = "$dest.stage-$PID"
    New-Item -ItemType Directory -Force $stage | Out-Null
    tar.exe -xzf $tmp -C $stage --strip-components=1
    if ($LASTEXITCODE -ne 0) { throw 'libwebp extraction failed' }
    if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
    Move-Item $stage $dest
    Set-Content $stamp $version
    "libwebp $version downloaded and verified"
} finally { Remove-Item $tmp -ErrorAction SilentlyContinue }
