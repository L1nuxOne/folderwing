<# PDF stdout EOF while the helper remains alive, partial framing and failed launch. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
$root=Split-Path $PSScriptRoot -Parent
$release=Join-Path $root "build\$Config"
$out=Join-Path $root 'build\pdf_transport';New-Item -ItemType Directory -Force $out|Out-Null
$script:readinessEvidence=Join-Path $out "readiness-$PID.jsonl"
$scratch=Join-Path $env:TEMP "openpilot_pdf_transport_$PID"
$bin=Join-Path $scratch 'bin'
$script:passed=0;$script:failed=0;$p=$null;$wslRoot=$null;$completed=$false
$script:pdfHistory=@();$script:pdfDemand=$null;$script:lastPdfHelpers='';$script:pdfErrorShare=$null;$script:pdfPaperShare=$null
function Check([string]$what,[bool]$ok,[string]$detail='') {
    if($ok){"  ok    $what";$script:passed++}else{Write-UiReadinessFailure $what;Write-PdfPhase $what;"  FAIL  $what $detail";$script:failed++}
}
function Reader {[FpUi]::FindWindowExW($script:main,[IntPtr]::Zero,'OpenPilotPdf',[IntPtr]::Zero)}
function Helpers {
    $current=@(Get-ReadinessHelpers 'folderwing-pdf');$ids=(@($current|ForEach-Object {"$($_.Id)/$($_.StartTime.Ticks)"})-join ',')
    if($ids -cne $script:lastPdfHelpers){$script:pdfHistory+=,@{tick=[Environment]::TickCount64;demand=$script:pdfDemand;helpers=@($current|ForEach-Object {@{pid=$_.Id;born=$_.StartTime;path=$_.Path;parent=$script:demoPid}})};$script:lastPdfHelpers=$ids}
    $current
}
function Write-PdfPhase([string]$phase){
    # Failure-only observations follow the existing deadline. Host internals
    # are explicitly unavailable: this is not invented demand/launch evidence.
    try{
        $null=Helpers;$reader=Reader;$state=Get-UiReadinessState $phase
        @{phase=$phase;tick=[Environment]::TickCount64;requested_owned_demand=$script:pdfDemand;reader=@{hwnd=$reader.ToInt64();visible=($reader -ne [IntPtr]::Zero -and [FpUi]::IsWindowVisible($reader));host=$script:demoPid};error_share=$script:pdfErrorShare;paper_share=$script:pdfPaperShare;helper_history=$script:pdfHistory;readiness=$state;actual_internal_demand='not externally exposed by ordinary host; do not infer from requested path';launch_outcome='exact PID/start/path observations plus host profile zones after Close; unobserved short-lived helper remains possible';profile="$out\transport-host-$PID.csv"}|ConvertTo-Json -Depth 10 -Compress|Add-Content "$out\pdf-phases-$PID.jsonl"
    }catch{@{phase=$phase;observer_error=$_.Exception.Message}|ConvertTo-Json -Compress|Add-Content "$out\pdf-phases-$PID.jsonl"}
}
function Color-Share([scriptblock]$match) {
    $null=Helpers # retain exact helper birth/path history in every render wait
    $h=Reader;if($h -eq [IntPtr]::Zero -or -not [FpUi]::IsWindowVisible($h)){return 0}
    $r=[FpUi+RECT]::new();[FpUi]::GetClientRect($h,[ref]$r)|Out-Null
    if($r.R -le 0 -or $r.B -le 0){return 0}
    $bmp=[Drawing.Bitmap]::new($r.R,$r.B);$gr=[Drawing.Graphics]::FromImage($bmp)
    try {
        $hdc=$gr.GetHdc()
        try {[FpUi]::PrintWindow($h,$hdc,2)|Out-Null}finally{$gr.ReleaseHdc($hdc)}
        $hits=0;$all=0
        for($y=0;$y -lt $r.B;$y+=4){for($x=0;$x -lt $r.R;$x+=4){
            $c=$bmp.GetPixel($x,$y);$all++;if(& $match $c){$hits++}
        }}
        if($all){$hits/$all}else{0}
    }finally{$gr.Dispose();$bmp.Dispose()}
}
function Paper-Ready {$script:pdfPaperShare=Color-Share {param($c) $c.R -gt 240 -and $c.G -gt 240 -and $c.B -gt 240};$script:pdfPaperShare}
function Error-Ready {$script:pdfErrorShare=Color-Share {param($c) $c.R -gt 180 -and $c.G -lt 150 -and $c.B -lt 150};$script:pdfErrorShare}
function Red-Marker {Color-Share {param($c) $c.R -gt 180 -and $c.G -lt 80 -and $c.B -lt 80}}
function Blue-Marker {Color-Share {param($c) $c.B -gt 180 -and $c.R -lt 80 -and $c.G -lt 80}}
function Navigate([string]$path){$script:pdfDemand=@{directory=$path;tick=[Environment]::TickCount64;owned_pdf_candidates=@(Get-ChildItem -LiteralPath $path -Filter '*.pdf' -ErrorAction SilentlyContinue|ForEach-Object FullName)};Navigate-Ready $path $script:pane0 7000}
function Copy-All {
    $h=Reader;if($h -eq [IntPtr]::Zero){return ''}
    Chord $h 'Ctrl' 0x41 20
    Set-Clipboard 'before PDF copy'
    Chord $h 'Ctrl' 0x43 20
    $null=Wait-Until {(Get-Clipboard -Raw -ErrorAction SilentlyContinue) -ne 'before PDF copy'} 1000
    Get-Clipboard -Raw -ErrorAction SilentlyContinue
}
function Copy-Content {
    $h=Reader;Click $h 100 ([int](130*$scale))
    Chord $h 'Ctrl' 0x41 20
    Set-Clipboard 'before PDF copy'
    Chord $h 'Ctrl' 0x43 20
    Wait-Until {($script:copied=Get-Clipboard -Raw -ErrorAction SilentlyContinue) -match 'Chapter 1' -and
                $script:copied -match 'the quick brown fox'} 5000
}
try {
    foreach($name in @('bin','empty','eof','partial','valid','launch','other','replacement')){
        New-Item -ItemType Directory -Force (Join-Path $scratch $name)|Out-Null
    }
    Copy-Item "$release\fp*.exe" $bin
    Copy-Item "$release\folderwing-engine.dll","$release\pdfium.dll" $bin
    $zig=$env:ZIG
    if(-not $zig){$cmd=Get-Command zig -ErrorAction SilentlyContinue;$zig=if($cmd){$cmd.Source}else{Join-Path $env:USERPROFILE 'tools\zig-x86_64-windows-0.17.0\zig.exe'}}
    & $zig cc -target x86_64-windows-gnu -std=c11 -municode -O2 -DNDEBUG -DF15_FAULT_TEST -DUNICODE -D_UNICODE `
        "-I$root\third_party\pdfium\include" "$root\app\fppdf.c" "$root\third_party\pdfium\lib\pdfium.dll.lib" `
        -lkernel32 '-Wl,--subsystem,windows' -o "$bin\folderwing-pdf.exe"
    if($LASTEXITCODE){throw "Fault helper build failed: $LASTEXITCODE"}
    & "$PSScriptRoot\make_pdf.ps1" -Out "$scratch\valid\valid.pdf" -Pages 3
    Copy-Item "$scratch\valid\valid.pdf" "$scratch\eof\f15-eof.pdf"
    Copy-Item "$scratch\valid\valid.pdf" "$scratch\partial\f15-partial.pdf"
    Copy-Item "$scratch\valid\valid.pdf" "$scratch\launch\launch.pdf"
    $settings=Join-Path $scratch 'settings.ini'
    Set-Content $settings "[preview.pdf]`nlinger = 0`ncolors = original"
    $p=Start-Demo "$bin\folderwing.exe" @("--settings=$settings","--profile-out=$out\transport-host-$PID.csv","$scratch\empty","$scratch\other")

    foreach($fault in @('eof','partial')) {
        Navigate (Join-Path $scratch $fault)
        Check "$fault fixture starts a PDF helper" (Wait-Until {(Helpers).Count -eq 1} 1200)
        Check "$fault transport failure is shown before delayed process exit" (Wait-Until {(Error-Ready) -gt 0.0005} 1800)
        Check "$fault helper is reaped while its delayed exit is still held" (Wait-Until {(Helpers).Count -eq 0} 1200)
        Navigate "$scratch\valid"
        Check "$fault recovery starts a fresh helper" (Wait-Until {(Helpers).Count -eq 1} 3000)
        Check "$fault recovery renders the real PDF" (Wait-Until {(Paper-Ready) -gt 0.25} 7000)
        Check "$fault recovery copies verified PDF text" (Copy-Content)
    }

    Navigate "$scratch\eof"
    Check 'launch-failure setup retires the current transport' (Wait-Until {(Error-Ready) -gt 0.0005} 1800)
    Check 'launch-failure setup reaps its helper' (Wait-Until {(Helpers).Count -eq 0} 1200)
    Move-Item "$bin\folderwing-pdf.exe" "$bin\fppdf.missing"
    Navigate "$scratch\launch"
    Check 'a failed launch publishes a visible error' (Wait-Until {(Error-Ready) -gt 0.0005} 5000)
    Check 'failed launch leaves no PDF helper' ((Helpers).Count -eq 0)
    Move-Item "$bin\fppdf.missing" "$bin\folderwing-pdf.exe"
    Navigate "$scratch\valid"
    Check 'PDF preview recovers after failed launch' (Wait-Until {(Paper-Ready) -gt 0.25} 7000)
    Check 'recovered PDF content is intact' (Copy-Content)

    # The same path can be replaced with a different document of the same size.
    & "$PSScriptRoot\make_pdf.ps1" -Out "$scratch\other\old.pdf" -Pages 3 -Heading Chapter -Swatch '1 0 0'
    & "$PSScriptRoot\make_pdf.ps1" -Out "$scratch\other\new.pdf" -Pages 3 -Heading Version -Swatch '0 0 1'
    Check 'replacement fixture has equal file size' ((Get-Item "$scratch\other\old.pdf").Length -eq (Get-Item "$scratch\other\new.pdf").Length)
    Copy-Item "$scratch\other\old.pdf" "$scratch\replacement\versioned.pdf"
    Navigate "$scratch\replacement"
    Check 'original PDF renders its red marker' (Wait-Until {(Red-Marker) -gt 0.0005} 5000)
    Check 'original PDF copies its own text' ((Copy-All) -match 'Chapter 1')
    $newBytes=[IO.File]::ReadAllBytes("$scratch\other\new.pdf")
    $sharing=[IO.FileShare]([int][IO.FileShare]::ReadWrite -bor [int][IO.FileShare]::Delete)
    $replacement=[IO.File]::Open("$scratch\replacement\versioned.pdf",[IO.FileMode]::Open,[IO.FileAccess]::Write,$sharing)
    try{$replacement.Write($newBytes,0,$newBytes.Length)}finally{$replacement.Dispose()}
    [IO.File]::SetLastWriteTimeUtc("$scratch\replacement\versioned.pdf",[DateTime]::UtcNow.AddSeconds(5))
    Key $script:pane0 0x74 100 # F5: refreshed listing reports the changed mtime
    Check 'equal-size replacement renders its blue marker' (Wait-Until {(Blue-Marker) -gt 0.0005} 7000)
    Check 'equal-size replacement copies new text' ((Copy-All) -match 'Version 1')

    # A WSL directory may contain both spellings; switching rows must switch documents.
    $distro=(& wsl.exe --exec sh -c 'printf "%s" "$WSL_DISTRO_NAME"' 2>$null | Select-Object -First 1)
    if($LASTEXITCODE -eq 0 -and $distro -match '^[A-Za-z0-9._-]+$'){
        $wslRoot="/tmp/openpilot_pdf_identity_$PID"
        & wsl.exe --exec sh -c "mkdir -p '$wslRoot'"
        if($LASTEXITCODE){throw 'Could not create the WSL PDF fixture directory'}
        $unc="\\wsl.localhost\$distro\tmp\openpilot_pdf_identity_$PID"
        & "$PSScriptRoot\make_pdf.ps1" -Out "$scratch\other\upper.pdf" -Pages 1 -Heading UpperAA -Swatch '1 0 0'
        & "$PSScriptRoot\make_pdf.ps1" -Out "$scratch\other\lower.pdf" -Pages 1 -Heading LowerBB -Swatch '0 0 1'
        Copy-Item "$scratch\other\upper.pdf" "$unc\A.pdf"
        Copy-Item "$scratch\other\lower.pdf" "$unc\a.pdf"
        Navigate $unc
        Key $script:pane0 0x24 100 # Home
        Check 'case-distinct WSL PDF first page renders' (Wait-Until {(Red-Marker) -gt 0.0005 -or (Blue-Marker) -gt 0.0005} 7000)
        $first=Copy-All
        Check 'case-distinct WSL PDF first page copies its own text' ($first -match 'UpperAA 1|LowerBB 1')
        Key $script:pane0 0x28 100 # Down to the other case-distinct file
        if($first -match 'UpperAA 1'){
            Check 'case-distinct WSL PDF second page renders blue' (Wait-Until {(Blue-Marker) -gt 0.0005} 7000)
            Check 'case-distinct WSL PDF second page copies lower-case file' ((Copy-All) -match 'LowerBB 1')
        }else{
            Check 'case-distinct WSL PDF second page renders red' (Wait-Until {(Red-Marker) -gt 0.0005} 7000)
            Check 'case-distinct WSL PDF second page copies upper-case file' ((Copy-All) -match 'UpperAA 1')
        }
    }else{'  skip  WSL case-distinct PDF fixture (no usable default distro)'}

    Check 'test app closes cleanly' (Stop-Demo $p);$p=$null
    Check 'all PDF helpers leave with the app' (Wait-Until {(Helpers).Count -eq 0} 3000)

    $profile=Join-Path $scratch 'unchanged.csv'
    $p=Start-Demo "$bin\folderwing.exe" @("--settings=$settings","--profile-out=$profile","$scratch\empty","$scratch\other")
    Navigate "$scratch\replacement"
    Check 'unchanged PDF is displayed for refresh check' (Wait-Until {(Blue-Marker) -gt 0.0005} 7000)
    1..3|ForEach-Object {Key $script:pane0 0x74 100} # F5 without changing file metadata
    Check 'unchanged PDF remains readable after refreshes' ((Copy-All) -match 'Version 1')
    Check 'refresh-check app closes cleanly' (Stop-Demo $p);$p=$null
    $opens=@(Import-Csv $profile|Where-Object {$_.kind -eq 'zone' -and $_.name -eq 'pdf.document_open'})
    Check 'unchanged PDF is opened only once' ($opens.Count -eq 1 -and [int]$opens[0].calls -eq 1) "(opens $($opens[0].calls))"
    $completed=$true
}finally{
    if($p){Stop-Demo $p|Out-Null}
    if($wslRoot){& wsl.exe --exec sh -c "rm -rf -- '$wslRoot'" | Out-Null}
    if(Test-Path $scratch){if($completed -and -not $script:failed){Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue}else{Write-Output "PDF_OWNED_FIXTURE_RETAINED $scratch";Write-PdfPhase 'retained-after-failure'}}
}
"`n$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
