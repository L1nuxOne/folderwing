<#
.SYNOPSIS
  Downloads libvterm, the terminal emulator the panel terminal uses, into
  third_party/libvterm.

  libvterm (https://www.leonerd.org.uk/code/libvterm/) is MIT-licensed and
  used by Neovim. The source release is built by build.ps1 straight into
  folderwing-terminal.exe; the version and the archive's SHA-256 are pinned below, and a
  download that doesn't match is refused. Without it everything else builds,
  and the panel terminal is simply not available.
.EXAMPLE
  ./tools/get-libvterm.ps1
#>
$ErrorActionPreference = 'Stop'
$version = '0.3.3'
$sha256 = '09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0'
$url = "https://www.leonerd.org.uk/code/libvterm/libvterm-$version.tar.gz"

$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root 'third_party\libvterm'
$stamp = Join-Path $dest 'VERSION.openpilot'
if ((Test-Path $stamp) -and (Get-Content $stamp -Raw).Trim() -eq $version) {
    "libvterm $version is already in $dest"
    return
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) "libvterm-$PID.tgz"
"downloading $url"
curl.exe -sSfL -o $tmp $url
if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE)" }
$got = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLowerInvariant()
if ($got -ne $sha256) {
    Remove-Item $tmp
    throw "checksum mismatch: expected $sha256, got $got"
}
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Force $dest | Out-Null
tar.exe -xzf $tmp -C $dest --strip-components=1
if ($LASTEXITCODE -ne 0) { throw "extract failed ($LASTEXITCODE)" }
Remove-Item $tmp
Set-Content $stamp $version
"libvterm $version -> $dest (sha256 ok)"
