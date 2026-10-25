<# Real window/OLE dragging integration, isolated files and native WSL transfers. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$dist=@(& wsl.exe -l -q|ForEach-Object {$_.Replace([string][char]0,'').Trim()}|Where-Object {$_})[0]
if(-not $dist){throw 'Dragging acceptance requires an available WSL distro'}
$linux="/tmp/openpilot-dragging-$PID"
$previous=$env:OPENPILOT_DRAG_WSL_DEST
try{
    & wsl.exe -d $dist -- mkdir -p $linux
    if($LASTEXITCODE){throw 'Could not create the private Linux destination'}
    $env:OPENPILOT_DRAG_WSL_DEST="\\wsl.localhost\$dist$($linux.Replace('/','\'))"
    Push-Location $root
    try{& "$root\build\$Config\drag_tests.exe";$code=$LASTEXITCODE}finally{Pop-Location}
    if($code){exit $code}
    $profile=Import-Csv "$root\build\dragging\drag-profile.csv"
    $ui=$profile|Where-Object {$_.kind -eq 'zone' -and $_.name -eq 'ui.dispatch'}
    if([double]$ui.max_ms -ge 16){throw "Dragging UI dispatch exceeded 16 ms: $($ui.max_ms)"}
    '  ok    dragging UI dispatch stays below 16 ms'
    '1 passed, 0 failed'
}finally{
    $env:OPENPILOT_DRAG_WSL_DEST=$previous
    & wsl.exe -d $dist -- rm -rf $linux
}
