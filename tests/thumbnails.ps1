<# Thumbnail acceptance: real restricted decoders, bounded demand/cache and tile file actions. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $root 'build\thumbnails';New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe"
$script:passed=0;$script:failed=0
$script:readinessEvidence=Join-Path $out "readiness-$PID.jsonl"
function Check([string]$what,[bool]$ok,[string]$detail=''){if($ok){"  ok    $what";$script:passed++}else{Write-UiReadinessFailure $what;"  FAIL  $what $detail";$script:failed++}}
function Caption($h){$s=[Text.StringBuilder]::new(512);[FpUi]::GetWindowTextW($h,$s,512)|Out-Null;$s.ToString()}
function Service { [FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotThumbnails',[IntPtr]::Zero) }
function Metric([string]$name){$text=Caption (Service);if($text -match "$name (\d+)"){[long]$Matches[1]}else{-1}}
function Navigate([string]$path,$pane=$script:pane0){Navigate-Ready $path $pane 7000}
function Grid($pane=$script:pane0){$t=Caption $pane;if($t -match 'Columns (\d+)'){[int]$Matches[1]}else{1}}
function Make-Color([string]$path,[Drawing.Color]$color){$b=[Drawing.Bitmap]::new(320,160);$g=[Drawing.Graphics]::FromImage($b);$g.Clear($color);$b.Save($path,[Drawing.Imaging.ImageFormat]::Png);$g.Dispose();$b.Dispose()}
function Capture($h){$r=[FpUi+RECT]::new();[FpUi]::GetClientRect($h,[ref]$r)|Out-Null;$b=[Drawing.Bitmap]::new($r.R,$r.B);$g=[Drawing.Graphics]::FromImage($b);$dc=$g.GetHdc();[FpUi]::PrintWindow($h,$dc,1)|Out-Null;$g.ReleaseHdc($dc);$g.Dispose();$b}
function Color-Count($h,[string]$color){$b=Capture $h;$count=0;try{for($y=0;$y -lt $b.Height;$y+=3){for($x=0;$x -lt $b.Width;$x+=3){$c=$b.GetPixel($x,$y);if(($color -eq 'red' -and $c.R -gt 220 -and $c.G -lt 60 -and $c.B -lt 60) -or ($color -eq 'blue' -and $c.B -gt 220 -and $c.R -lt 60 -and $c.G -lt 60) -or ($color -eq 'paper' -and $c.R -gt 230 -and $c.G -gt 230 -and $c.B -gt 230)){$count++}}}}finally{$b.Dispose()};$count}
$scratch=Join-Path $env:TEMP "openpilot_thumbnails_$PID";$p=$null;$linux=$null
try {
    foreach($name in @('gallery','many','right','red','blue','pdf','empty','wsl','svg')){New-Item -ItemType Directory -Force "$scratch\$name"|Out-Null}
    New-Item -ItemType Directory "$scratch\gallery\folder"|Out-Null
    Make-Color "$scratch\red\photo.png" ([Drawing.Color]::Red)
    Make-Color "$scratch\blue\photo.png" ([Drawing.Color]::Blue)
    Copy-Item "$scratch\red\photo.png" "$scratch\gallery\01_red.png"
    Copy-Item "$scratch\blue\photo.png" "$scratch\gallery\02_blue.png"
    Copy-Item "$root\tests\fixtures\images\alpha.webp" "$scratch\gallery\03_alpha.webp"
    Copy-Item "$root\tests\fixtures\images\oriented.jpg" "$scratch\gallery\04_oriented.jpg"
    & "$PSScriptRoot\make_pdf.ps1" "$scratch\pdf\paper.pdf"|Out-Null
    Copy-Item "$scratch\pdf\paper.pdf" "$scratch\gallery\05_paper.pdf"
    Set-Content "$scratch\gallery\06_bad.jpg" 'broken image'
    $svg=Get-Content "$root\tests\fixtures\images\shapes.svg" -Raw
    Set-Content -LiteralPath "$scratch\svg\vector.svg" -Value $svg.Replace('width="16" height="8"','width="320" height="160"')
    Set-Content "$scratch\gallery\07_note.dat" 'unsupported data'
    for($i=0;$i -lt 180;$i++){Copy-Item "$scratch\red\photo.png" ("$scratch\many\{0:D3}.png" -f $i)}
    $settings="$scratch\settings.ini"
    $base="[functions]`npreview.image = off`npreview.pdf = off`npreview.text = off`n[view.thumbnails]`ncache_mb = 8`n"
    Set-Content $settings $base
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\thumbnail-profile.csv","$scratch\gallery","$scratch\right")
    Check 'details remain the initial view' ((Caption $pane0) -eq 'Details') (Caption $pane0)
    Check 'details create no thumbnail resources' ((Service) -eq [IntPtr]::Zero)
    Chord $pane0 @('Ctrl','Alt') 0x54
    Check 'thumbnail view toggles per pane' ((Caption $pane0) -match 'Thumbnails 160' -and (Caption $pane1) -eq 'Details')
    Check 'images and PDF load through the thumbnail worker' (Wait-Until {(Metric 'ready') -ge 5 -and (Metric 'unavailable') -eq 1} 10000) (Caption (Service))
    Check 'red image pixels are painted in the tile' ((Color-Count $pane0 'red') -gt 200)
    Check 'blue image pixels are painted in the tile' ((Color-Count $pane0 'blue') -gt 200)
    Check 'PDF first-page pixels are painted on white paper' ((Color-Count $pane0 'paper') -gt 200)
    Check 'thumbnail helpers are independent of disabled readers' (@(Get-Process folderwing-image -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 1 -and @(Get-Process folderwing-pdf -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 1)
    Shot $out '01_gallery'
    $columns=Grid;Key $pane0 0x27 20
    Check 'Right selects the next tile' ((Caption $pane0) -match 'Cursor 01_red.png') (Caption $pane0)
    Key $pane0 0x28 20
    Check 'Down moves by a grid row' ((Caption $pane0) -notmatch 'Cursor 01_red.png')
    Key $pane0 0x24 20;Chord $pane0 'Shift' 0x27 20
    Check 'Shift+Right selects a range across tiles' ((Caption $pane0) -match 'Selected 2')
    $columns=Grid;$cell=[int]((Client-Width $pane0)/$columns)
    Click $pane0 ([int]($cell*1.5)) ([int](145*$scale));Post $pane0 0x202 0 0
    Check 'mouse hit testing selects the tile by column' ((Caption $pane0) -match 'Cursor 01_red.png')
    Chord $pane0 'Ctrl' 0x41 20
    Check 'select all retains ordinary file selection' ((Caption $pane0) -match 'Selected 8')
    Key $pane0 0x24 20
    Key $pane0 0x27 20
    Key $pane0 0x71 20
    $rename=@(foreach($h in (Pane-Edits $pane0)){if([FpUi]::IsWindowVisible($h)){$h}})
    Check 'F2 creates a rename editor under the tile' ($rename.Count -eq 1)
    if($rename.Count){Send-Text $rename[0] 'renamed';Key $rename[0] 0x0D 20}
    Check 'rename preserves the thumbnail source' (Wait-Until {Test-Path "$scratch\gallery\renamed.png"})
    Check 'rename keeps the changed item selected' (Wait-Until {(Test-UiSettingsApplied) -and (Caption $pane0) -match 'Cursor renamed.png'})
    Key $pane0 0x74 20
    Check 'F5 still prepares a copy from a tile' (Wait-Until {@(Get-Process folderwing-operations -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 1})
    $side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
    $slot=[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)
    Set-Clipboard 'waiting for ready transfer'
    Check 'tile transfer shows the source before Start' (Wait-Until {
        Chord $slot 'Ctrl' 0x43 30;$snapshot=Get-Clipboard -Raw -ErrorAction SilentlyContinue
        $snapshot -match 'Ready to start' -and $snapshot.Contains("$scratch\gallery\renamed.png")
    } 4000)
    Key $slot 0x0D 20
    Check 'tile copy reaches the other folder' (Wait-Until {Test-Path "$scratch\right\renamed.png"})
    Chord $slot @('Ctrl','Shift') 0x4F 20
    Chord $pane0 @('Ctrl','Shift') 0x26 20
    Check 'larger thumbnails use 256 pixels' ((Caption $pane0) -match 'Thumbnails 256')
    Chord $pane0 @('Ctrl','Shift') 0x28 20;Chord $pane0 @('Ctrl','Shift') 0x28 20
    Check 'smaller thumbnails use 96 pixels' ((Caption $pane0) -match 'Thumbnails 96')
    Shot $out '02_small'
    $svgBefore=Metric 'decoded'
    Navigate "$scratch\svg"
    Check 'SVG tile renders through the restricted image helper' (Wait-Until {(Metric 'decoded') -gt $svgBefore -and (Color-Count $pane0 'red') -gt 200} 7000)
    Check 'SVG thumbnail does not need the full image reader' (-not [FpUi]::IsWindowVisible([FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)))
    Navigate "$scratch\red"
    Check 'a single image loads in the smaller view' (Wait-Until {(Color-Count $pane0 'red') -gt 200} 6000)
    $before=Metric 'decoded';Make-Color "$scratch\red\photo.png" ([Drawing.Color]::Blue)
    Chord $pane0 'Ctrl' 0x52 20
    Check 'changed files invalidate cached pixels' (Wait-Until {(Metric 'decoded') -gt $before -and (Color-Count $pane0 'blue') -gt 200} 7000)
    Check 'new pixels replace old pixels' ((Color-Count $pane0 'red') -lt 100)
    Navigate "$scratch\empty"
    Check 'an empty folder remains usable in tile mode' ((Caption $pane0) -match 'Thumbnails 96')
    Navigate "$scratch\many"
    Check 'visible and look-ahead thumbnails load' (Wait-Until {(Metric 'decoded') -gt ($before+6)} 7000)
    Start-Sleep -Milliseconds 400
    Check 'the folder is not eagerly decoded' ((Metric 'decoded') -lt 100) (Caption (Service))
    $top=Caption $pane0;Post $pane0 $WM_MOUSEWHEEL ((-120 -band 0xFFFF) -shl 16);Start-Sleep -Milliseconds 200
    Check 'wheel scrolling moves whole grid rows' ((Caption $pane0) -ne $top)
    Key $pane0 0x23 20
    Check 'End brings the last tile into view' ((Caption $pane0) -match 'Cursor 179.png' -and (Caption $pane0) -notmatch 'Top 0')
    Check 'the last screen loads without the intervening folder' (Wait-Until {(Metric 'ready') -ge 25} 7000)
    Check 'pixel cache respects the configured 8 MB cap' ((Metric 'bytes') -le 8MB) (Caption (Service))
    Shot $out '03_last_page'
    Navigate "$scratch\blue"
    $helpers=@(Get-Process folderwing-image -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid});$helpers|Stop-Process -Force
    Navigate "$scratch\gallery"
    Chord $pane0 @('Ctrl','Shift') 0x26 20
    Check 'a helper crash recovers on fresh thumbnail work' (Wait-Until {(Color-Count $pane0 'blue') -gt 200} 7000)
    Chord $pane1 @('Ctrl','Alt') 0x54 20
    Check 'the second pane can use thumbnail view independently' ((Caption $pane1) -match 'Thumbnails' -and (Caption $pane0) -match 'Thumbnails')
    Chord $pane0 @('Ctrl','Alt') 0x54 20
    Check 'returning one pane to details keeps the other in tiles' ((Caption $pane0) -eq 'Details' -and (Caption $pane1) -match 'Thumbnails')
    Set-UiSettings $settings ($base+"`nmode = thumbnails`nsize = large`n")
    Check 'settings can select thumbnail mode and size' (Wait-Until {(Test-UiSettingsApplied) -and (Caption $pane0) -match 'Thumbnails 256' -and (Caption $pane1) -match 'Thumbnails 256'} 5000)
    Set-UiSettings $settings ($base.Replace('preview.text = off','preview.text = off' + "`nview.thumbnails = off"))
    Check 'disabling thumbnails returns both panes to details' (Wait-Until {(Test-UiSettingsApplied) -and (Caption $pane0) -eq 'Details' -and (Caption $pane1) -eq 'Details'} 5000)
    Chord $pane0 @('Ctrl','Alt') 0x54 20
    Check 'disabling thumbnails releases its resources' ((Service) -eq [IntPtr]::Zero)
    Check 'a disabled thumbnail shortcut falls through' ((Caption $pane0) -eq 'Details')
    Set-UiSettings $settings ($base+"`nmode = thumbnails`nsize = medium`n")
    Check 'reenabling thumbnails applies the current settings' (Wait-Until {(Test-UiSettingsApplied) -and (Caption $pane0) -match 'Thumbnails 160'} 5000)
    # A reader uses separate helpers while the source pane stays in thumbnail mode.
    Navigate "$scratch\red"
    Set-UiSettings $settings ($base.Replace('preview.image = off','preview.image = on')+"`nmode = thumbnails`nsize = medium`n")
    $image=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)
    Check 'side image preview still opens beside thumbnails' (Wait-Until {
        $script:image=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)
        (Test-UiSettingsApplied) -and [FpUi]::IsWindowVisible($script:image) -and (Caption $pane0) -match 'Thumbnails'
    } 6000)
    Check 'thumbnail and reader helpers are independent' (Wait-Until {
        @(Get-Process folderwing-image -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 2
    } 6000)
    Key $image 0x1B 20
    Set-UiSettings $settings ($base+"`nmode = thumbnails`nsize = medium`n")
    Check 'closing the reader retains the thumbnail pane' (Wait-Until {(Test-UiSettingsApplied) -and -not [FpUi]::IsWindowVisible($image) -and (Caption $pane0) -match 'Thumbnails'} 5000)
    # WSL identity and reserved names use the same 9P paths as the reader.
    $dist=@(& wsl.exe -l -q|ForEach-Object {$_.Replace([string][char]0,'').Trim()}|Where-Object {$_})[0]
    if(-not $dist){throw 'WSL acceptance requires an available distro'}
    $linux="/tmp/openpilot-thumbnails-$PID"
    & wsl.exe -d $dist -- mkdir -p $linux
    if($LASTEXITCODE){throw 'WSL thumbnail fixture creation failed'}
    $src='/mnt/'+$scratch.Substring(0,1).ToLower()+$scratch.Substring(2).Replace('\','/')
    & wsl.exe -d $dist -- cp "$src/red/photo.png" "$linux/A.png"
    if($LASTEXITCODE){throw 'WSL thumbnail fixture creation failed'}
    & wsl.exe -d $dist -- cp "$src/gallery/renamed.png" "$linux/a.png"
    if($LASTEXITCODE){throw 'WSL thumbnail fixture creation failed'}
    & wsl.exe -d $dist -- cp "$src/blue/photo.png" "$linux/a:b.png"
    if($LASTEXITCODE){throw 'WSL thumbnail fixture creation failed'}
    Navigate "\\wsl.localhost\$dist$($linux.Replace('/','\'))"
    Check 'WSL case-distinct and reserved-name thumbnails load' (Wait-Until {(Color-Count $pane0 'red') -gt 200 -and (Color-Count $pane0 'blue') -gt 200} 10000)
    Shot $out '04_wsl'
    Check 'test explorer closes cleanly' (Stop-Demo $p);$p=$null
    $profile=Import-Csv "$out\thumbnail-profile.csv"
    $viol=($profile|Where-Object {$_.kind -eq 'metric' -and $_.name -eq 'fn_violations'}).value
    $ui=$profile|Where-Object {$_.kind -eq 'zone' -and $_.name -eq 'ui.dispatch'}
    Check 'thumbnail functions have no lifetime violations' ($viol -eq '0') $viol
    Check 'thumbnail UI dispatch stays within 16 ms' ([double]$ui.max_ms -lt 16) $ui.max_ms
    Check 'thumbnail helpers leave with the explorer' (@(Get-Process folderwing-image,folderwing-pdf -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 0)
}finally{
    if($p -and -not $p.HasExited){Stop-Demo $p|Out-Null}
    if($linux -and $dist){& wsl.exe -d $dist -- rm -rf $linux}
    Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
"`n$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
