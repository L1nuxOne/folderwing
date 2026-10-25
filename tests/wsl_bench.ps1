<# Compare supported transports; unsupported durable 9P destinations are refusal checks, not timing samples. #>
param([string] $Config='release',[int] $Files=2000)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root "build\$Config\folderwing-cli.exe"
$out=Join-Path $root 'build\wsl';New-Item -ItemType Directory -Force $out | Out-Null
$name="openpilot_bench_$PID"; $linux="/tmp/$name"
$windows=Join-Path $env:TEMP $name;New-Item -ItemType Directory -Force $windows | Out-Null
$distro=(& wsl.exe --exec sh -c "mkdir -p $linux/source $linux/native $linux/share; echo `$WSL_DISTRO_NAME").Trim()
if($LASTEXITCODE -ne 0){throw 'WSL fixture failed'}
$base="\\wsl.localhost\$distro\tmp\$name"
# Python is only a fixture generator, never the copy implementation.
$fixture="import pathlib; p=pathlib.Path('$linux/source'); [(p/('file_%05d.txt'%i)).write_bytes(('payload-%d'%i).encode()) for i in range($Files)]"
& wsl.exe --exec python3 -c $fixture
if($LASTEXITCODE -ne 0){throw 'Linux fixture failed'}
New-Item -ItemType Directory -Force "$windows\source", "$windows\native", "$windows\share" | Out-Null
Get-ChildItem "$base\source" | Copy-Item -Destination "$windows\source"
$rows=@();$complete=$false
function Files([string]$folder){@(Get-ChildItem -LiteralPath $folder -File|Sort-Object Name|ForEach-Object{[ordered]@{name=$_.Name;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})|ConvertTo-Json -Compress}
& $exe ls $windows -n 0 *> $null
if($LASTEXITCODE){throw 'CLI warm-up failed'}
try {
    foreach($case in @(
        @{Name='Linux to Linux';Source="$base\source";Native="$base\native";Share="$base\share"},
        @{Name='Windows to Linux';Source="$windows\source";Native="$base\native";Share="$base\share"},
        @{Name='Linux to Windows';Source="$base\source";Native="$windows\native";Share="$windows\share"}
    )) {
        foreach($transport in 'native','share') {
            $dest=if($transport -eq 'native'){$case.Native}else{$case.Share}
            # Remove only the previous isolated fixture copy, outside timing.
            if(Test-Path "$dest\source"){Remove-Item -Recurse -Force "$dest\source"}
            $args=@('cp',$case.Source,$dest);if($transport -eq 'share'){$args+='--9p'}
            $before=Files $case.Source
            $clock=[Diagnostics.Stopwatch]::StartNew()
            & $exe @args *> (Join-Path $out "bench_$($case.Name.Replace(' ','_'))_$transport.log")
            $code=$LASTEXITCODE;$clock.Stop()
            if($transport -eq 'share' -and $case.Name -ne 'Linux to Windows'){
                $log=Get-Content (Join-Path $out "bench_$($case.Name.Replace(' ','_'))_$transport.log") -Raw
                if($code -ne 1 -or $log -notmatch '\(50\)' -or $log -notmatch 'Transfer refused before mutation; use native WSL transfers' -or
                   (Test-Path "$dest\source") -or (Files $case.Source) -cne $before){throw 'Expected pre-mutation 9P refusal/source retention was not established'}
                $rows += [pscustomobject]@{case=$case.Name;transport=$transport;files=$Files;outcome='expected-refusal';elapsed_ms=$null}
                Write-Host "$($case.Name) $transport expected-refusal (directory durability unsupported)"
                continue
            }
            if($code){throw "$($case.Name) $transport failed (exit $code)"}
            $copied=@(Get-ChildItem "$dest\source" -File)
            if($copied.Count -ne $Files){throw 'copy count mismatch'}
            if((Get-Content -Raw "$dest\source\file_00000.txt") -ne 'payload-0'){throw 'copy content mismatch'}
            if((Files "$dest\source") -cne $before -or (Files $case.Source) -cne $before){throw 'Exact file hashes/source retention mismatch'}
            $rows += [pscustomobject]@{case=$case.Name;transport=$transport;files=$Files;outcome='copied';elapsed_ms=$clock.ElapsedMilliseconds}
            Write-Host "$($case.Name) $transport $($clock.ElapsedMilliseconds) ms"
        }
    }
    $rows | ConvertTo-Json | Set-Content (Join-Path $out 'benchmark.json');$complete=$true
} finally {
    if($complete){
        & wsl.exe --exec rm -rf $linux
        Remove-Item -Recurse -Force $windows -ErrorAction SilentlyContinue
    }else{Write-Host "RETAIN failed benchmark fixtures Windows=$windows Linux=$linux"}
}
