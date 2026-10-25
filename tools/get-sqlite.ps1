<# Pinned public-domain SQLite source, linked only into the static Linux helper.
   Archive SHA-256 is sealed from the official SHA3-256-verified 3.54.0 release. #>
param([string] $Destination)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = if ($Destination) { $Destination } else { Join-Path $root 'third_party\sqlite' }
$sources = @{
    'sqlite3.c' = '2da161e310ef0aba3465f22a4d12b8a43723236f0942fa76e8aac523c3317243'
    'sqlite3.h' = 'cd20a2877bc82b94e36c1093b727dd87cfa3c1d581aac6e240925535cbd90166'
}
function Verify-Sources([string] $directory) {
    foreach ($name in $sources.Keys) {
        $path = Join-Path $directory $name
        if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sources[$name]) {
            throw "SQLite 3.54.0 source checksum mismatch: $path"
        }
    }
}
if (Test-Path $dest) { Verify-Sources $dest; return }
$tmp = Join-Path ([IO.Path]::GetTempPath()) "openpilot-sqlite-$PID.zip"
$stage = "$dest.stage-$PID"
try {
    curl.exe -sSfL -o $tmp 'https://sqlite.org/2026/sqlite-amalgamation-3540000.zip'
    if ($LASTEXITCODE -ne 0) { throw 'SQLite download failed' }
    if ((Get-FileHash $tmp -Algorithm SHA256).Hash.ToLowerInvariant() -ne '68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d') {
        throw 'SQLite archive checksum mismatch'
    }
    New-Item -ItemType Directory -Path $stage | Out-Null
    $zip = [IO.Compression.ZipFile]::OpenRead($tmp)
    try {
        foreach ($name in $sources.Keys) {
            $entry = $zip.GetEntry("sqlite-amalgamation-3540000/$name")
            if (-not $entry) { throw "SQLite archive missing $name" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $stage $name))
        }
    } finally { $zip.Dispose() }
    Verify-Sources $stage
    @{version='3.54.0';archive_sha256='68e913b0fe8ec6b4e5f391c594a28872884e80d6cd042a1e50c957be46b5aa4d';source_sha256=$sources} |
        ConvertTo-Json | Set-Content (Join-Path $stage 'VERSION.openpilot.json')
    Move-Item $stage $dest
} finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
}
