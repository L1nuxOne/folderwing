<# Image preview acceptance: actual helper, native input, scratch files only. #>
param([string] $Config='release')
$ErrorActionPreference='Stop'
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class ImageProbe {
 [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr process);
 [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr process);
 [DllImport("user32.dll")] public static extern bool IsClipboardFormatAvailable(uint format);
 [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")] static extern bool CloseClipboard();
 [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint format);
 [DllImport("kernel32.dll")] static extern IntPtr GlobalLock(IntPtr h);
 [DllImport("kernel32.dll")] static extern bool GlobalUnlock(IntPtr h);
 public static int[] Pixel(int x,int y) {
  if(!OpenClipboard(IntPtr.Zero)) return new int[0];
  try { var h=GetClipboardData(17);if(h==IntPtr.Zero)return new int[0];var p=GlobalLock(h);if(p==IntPtr.Zero)return new int[0];
   try {int w=Marshal.ReadInt32(p,4),hgt=Marshal.ReadInt32(p,8),header=Marshal.ReadInt32(p,0);int row=hgt<0?y:Math.Abs(hgt)-1-y;
    if(x<0||x>=w||y<0||y>=Math.Abs(hgt))return new int[0];int off=header+(row*w+x)*4;
    return new[]{(int)Marshal.ReadByte(p,off),(int)Marshal.ReadByte(p,off+1),(int)Marshal.ReadByte(p,off+2),(int)Marshal.ReadByte(p,off+3)};
   }finally{GlobalUnlock(h);}
  }finally{CloseClipboard();}
 }
 public static int[] Dimensions() {
  if(!OpenClipboard(IntPtr.Zero)) return new int[0];
  try { var h=GetClipboardData(17);var p=GlobalLock(h);if(p==IntPtr.Zero)return new int[0];
   try{return new[]{Marshal.ReadInt32(p,4),Math.Abs(Marshal.ReadInt32(p,8))};}finally{GlobalUnlock(h);}
  }finally{CloseClipboard();}
 }
}
"@
. "$PSScriptRoot\uilib.ps1"
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root "build\$Config\folderwing.exe"
$out=Join-Path $root 'build\images'
New-Item -ItemType Directory -Force $out | Out-Null
$script:failed=0;$script:passed=0
function Check([string]$what,[bool]$ok,[string]$detail='') {
    if($ok){"  ok    $what";$script:passed++}else{"  FAIL  $what $detail";$script:failed++}
}
function Image-Window { [FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero) }
function Image-Title { $sb=[Text.StringBuilder]::new(512);[FpUi]::GetWindowTextW((Image-Window),$sb,512)|Out-Null;$sb.ToString() }
function Navigate([string]$path,[IntPtr]$pane=$script:pane0) {
    $null=Navigate-Verified $path $pane 7000
}
function Image-Ready([string]$pattern){Wait-Until { $h=Image-Window;$title=Image-Title;[FpUi]::IsWindowVisible($h) -and $title -match $pattern -and $title -notmatch 'Loading|unavailable' } 7000}
function Copy-Pixel([int]$x=0,[int]$y=0) {
    Set-Clipboard 'waiting for frame';Chord (Image-Window) @('Ctrl','Shift') 0x43 20
    Wait-Until {($script:pixel=[ImageProbe]::Pixel($x,$y)).Count -eq 4} 3000|Out-Null
    $script:pixel
}
function Text-Window {[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotText',[IntPtr]::Zero)}
function Text-Title {$sb=[Text.StringBuilder]::new(512);[FpUi]::GetWindowTextW((Text-Window),$sb,512)|Out-Null;$sb.ToString()}
function Helpers { @(Get-Process folderwing-image -ErrorAction SilentlyContinue | Where-Object {$_.Parent.Id -eq $script:demoPid}) }
$scratch=Join-Path $env:TEMP "openpilot_images_$PID"
$p=$null;$linux=$null
try {
    foreach($name in @('png','webp','jpg','profile','animated','pdf','large','bad','empty','right','svg','viewbox','partial','doctype','disposal','gif','badsvg')) {New-Item -ItemType Directory -Force "$scratch\$name"|Out-Null}
    $fixtures=Join-Path $root 'tests\fixtures\images'
    Copy-Item "$fixtures\alpha.png" "$scratch\png\alpha.png"
    Copy-Item "$fixtures\alpha.webp" "$scratch\webp\alpha.webp"
    Copy-Item "$fixtures\oriented.jpg" "$scratch\jpg\oriented.jpg"
    Copy-Item "$fixtures\profile.png" "$scratch\profile\profile.png"
    Copy-Item "$fixtures\animated.webp" "$scratch\animated\animated.webp"
    foreach($name in @('shapes','viewbox','partial','doctype','broken')) {
        $folder=if($name -eq 'shapes'){'svg'}elseif($name -eq 'broken'){'badsvg'}else{$name}
        Copy-Item "$fixtures\$name.svg" "$scratch\$folder\$name.svg"
    }
    Copy-Item "$fixtures\animated.gif" "$scratch\gif\animated.gif"
    Copy-Item "$fixtures\disposal.gif" "$scratch\disposal\disposal.gif"
    Set-Content "$scratch\bad\broken.jpg" 'not an image'
    & "$PSScriptRoot\make_pdf.ps1" "$scratch\pdf\paper.pdf" | Out-Null
    $bmp=[Drawing.Bitmap]::new(2400,1600);$g=[Drawing.Graphics]::FromImage($bmp);$g.Clear([Drawing.Color]::CornflowerBlue)
    $g.FillRectangle([Drawing.Brushes]::Red,0,0,1200,800);$bmp.Save("$scratch\large\large.png",[Drawing.Imaging.ImageFormat]::Png);$g.Dispose();$bmp.Dispose()
    $settings="$scratch\settings.ini";Set-Content $settings "[preview.image]`nlinger = 0`n[preview.pdf]`nlinger = 0"
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\image-profile.csv","$scratch\png","$scratch\right")
    Check 'PNG opens automatically beside the list' (Image-Ready 'PNG.*16 x 8.*100%.*Fit') (Image-Title)
    $image=Image-Window
    Check 'image occupies the right pane' ([FpUi]::IsWindowVisible($image) -and -not [FpUi]::IsWindowVisible($pane1))
    Check 'isolated image helper is running' ((Helpers).Count -eq 1)
    Shot $out '01_png'
    Key $pane0 0x1B
    Check 'Escape in the list retains list behaviour' ([FpUi]::IsWindowVisible($image))
    Set-Clipboard 'waiting for image pixels'
    Chord $image @('Ctrl','Shift') 0x43
    Check 'copy pixels puts an image on the clipboard' (Wait-Until {[ImageProbe]::IsClipboardFormatAvailable(17)} 5000)
    Wait-Until { ($script:clip=[ImageProbe]::Dimensions()).Count -eq 2 } 2000 | Out-Null
    Check 'copied image has original dimensions' ($script:clip.Count -eq 2 -and $script:clip[0] -eq 16 -and $script:clip[1] -eq 8) ($script:clip -join ' x ')
    Chord $image 'Ctrl' 0x43
    Check 'Ctrl+C still copies the selected file' (Wait-Until { [ImageProbe]::IsClipboardFormatAvailable(15) } 3000)
    Chord $image @('Ctrl','Shift') 0x52
    Check 'rotate changes view dimensions' (Image-Ready '8 x 16.*Rotated') (Image-Title)
    Chord $image @('Ctrl','Shift') 0x48
    Check 'flip is reflected in view state' (Image-Ready 'Flipped') (Image-Title)
    Check 'view transforms leave original file unchanged' ((Get-FileHash "$scratch\png\alpha.png").Hash -eq (Get-FileHash "$fixtures\alpha.png").Hash)
    Chord $image 'Ctrl' 0x49
    Shot $out '02_metadata_rotated'
    Key $image 0x1B
    Check 'Escape restores both panes' (Wait-Until {-not [FpUi]::IsWindowVisible($image) -and [FpUi]::IsWindowVisible($pane1)})
    Start-Sleep -Milliseconds 250
    Check 'Escape stays closed on the same image' (-not [FpUi]::IsWindowVisible($image))
    Navigate "$scratch\webp"
    Check 'bundled WebP works' (Image-Ready 'WebP.*16 x 8') (Image-Title)
    Navigate "$scratch\jpg"
    Check 'camera orientation is applied automatically' (Image-Ready 'JPEG.*8 x 16') (Image-Title)
    Navigate "$scratch\profile"
    Check 'colour-profile PNG opens' (Image-Ready 'PNG.*16 x 8') (Image-Title)
    Shot $out '03_colour_profile'
    Navigate "$scratch\animated"
    Check 'animated WebP opens with playback controls' (Image-Ready 'WebP.*12 x 6.*Frame [12]/2.*Playing') (Image-Title)
    Key $image 0x20 20
    Check 'Space pauses animation in the viewer' (Image-Ready 'Paused') (Image-Title)
    $paused=Image-Title;Start-Sleep -Milliseconds 350
    Check 'paused frame stays unchanged' ((Image-Title) -eq $paused)
    Chord $image @('Ctrl','Shift') 0x27 20
    Check 'next frame pauses and changes the shown frame' (Wait-Until {(Image-Title) -ne $paused -and (Image-Title) -match 'Paused'}) (Image-Title)
    Chord $image @('Ctrl','Shift') 0x25 20
    Check 'previous frame returns to the paused frame' (Wait-Until {(Image-Title) -eq $paused}) (Image-Title)
    $pixel=Copy-Pixel
    $blue=$paused -match 'Frame 2/2'
    Check 'copy pixels uses the current WebP frame' ($pixel.Count -eq 4 -and $pixel[3] -eq 255 -and (($blue -and $pixel[0] -eq 255) -or (-not $blue -and $pixel[2] -eq 255))) ($pixel -join ',')
    $beforeCopyStep=Image-Title;$ownedHelper=(Helpers)[0]
    $suspended=[ImageProbe]::NtSuspendProcess($ownedHelper.Handle) -eq 0
    Check 'owned decoder can be paused for the copy/step overlap fixture' $suspended
    if($suspended) {
        try {
            Set-Clipboard 'pending frame copy';Chord $image @('Ctrl','Shift') 0x43 20
            Chord $image @('Ctrl','Shift') 0x27 20
        } finally {[ImageProbe]::NtResumeProcess($ownedHelper.Handle)|Out-Null}
    }
    Check 'frame stepping cancels a pending copy and continues rendering' (Wait-Until {(Image-Title) -ne $beforeCopyStep -and (Image-Title) -match 'Paused'} 3000) (Image-Title)
    Check 'cancelled copy does not publish pixels from the old frame' (-not [ImageProbe]::IsClipboardFormatAvailable(17))
    $ownedHelper=(Helpers)[0];$suspended=[ImageProbe]::NtSuspendProcess($ownedHelper.Handle) -eq 0
    if($suspended) {
        try {
            Set-Clipboard 'cancelled frame copy';Chord $image @('Ctrl','Shift') 0x43 20
            Chord $image @('Ctrl','Shift') 0x27 20;Chord $image @('Ctrl','Shift') 0x25 20
        } finally {[ImageProbe]::NtResumeProcess($ownedHelper.Handle)|Out-Null}
    }
    Start-Sleep -Milliseconds 350
    Check 'returning to the original frame does not revive a cancelled copy' ($suspended -and -not [ImageProbe]::IsClipboardFormatAvailable(17))
    Chord $image @('Ctrl','Shift') 0x20 20
    Check 'play command resumes animation'   (Image-Ready 'Playing') (Image-Title)
    $first=Image-Title
    Check 'playing advances the displayed frame' (Wait-Until {(Image-Title) -ne $first -and (Image-Title) -match 'Playing'} 3000)
    # Header buttons invoke the same actions without losing the list selection.
    Click $image ([int](130*$scale)) ([int](93*$scale));Post $image 0x202 0 0
    Check 'header Pause button pauses playback' (Image-Ready 'Paused')
    $paused=Image-Title;Click $image ([int](220*$scale)) ([int](93*$scale));Post $image 0x202 0 0
    Check 'header Next button steps a frame' (Wait-Until {(Image-Title) -ne $paused -and (Image-Title) -match 'Paused'})
    Click $image ([int](40*$scale)) ([int](93*$scale));Post $image 0x202 0 0
    Check 'header Previous button reverses the step' (Wait-Until {(Image-Title) -eq $paused})
    Navigate "$scratch\disposal"
    Check 'finite-loop GIF plays its logical canvas' (Image-Ready 'GIF.*6 x 4.*Frame [1-4]/4') (Image-Title)
    Check 'GIF respects its finite loop count and stops on the final frame' (Wait-Until {(Image-Title) -match 'Frame 4/4.*Paused'} 7000) (Image-Title)
    Key $image 0x20 20
    Check 'Play restarts a completed finite animation' (Image-Ready 'Playing') (Image-Title)
    Check 'restarted finite animation stops again' (Wait-Until {(Image-Title) -match 'Frame 4/4.*Paused'} 7000)
    $pixel=Copy-Pixel 3 1
    Check 'GIF disposal clears the previous rectangle to transparency' ($pixel.Count -eq 4 -and $pixel[3] -eq 0) ($pixel -join ',')
    Chord $image @('Ctrl','Shift') 0x25 20
    Check 'GIF backward stepping rebuilds the composited canvas' (Image-Ready 'Frame 3/4.*Paused')
    $pixel=Copy-Pixel 1 1
    Check 'GIF restore-previous disposal restores original pixels' ($pixel.Count -eq 4 -and $pixel[2] -eq 255 -and $pixel[1] -eq 0) ($pixel -join ',')
    Chord $image @('Ctrl','Shift') 0x52 20
    Check 'rotation works on an animation frame' (Image-Ready '4 x 6.*Rotated.*Frame 3/4.*Paused')
    Chord $image @('Ctrl','Shift') 0x48 20
    Check 'flip works on an animation frame' (Image-Ready 'Flipped.*Frame 3/4.*Paused')
    Chord $image 'Ctrl' 0x31 20
    Check 'animation supports actual size' (Image-Ready '100%.*Zoom')
    Chord $image 'Ctrl' 0x30 20
    Check 'animation supports fit' (Image-Ready 'Fit')
    Shot $out '09_animation'
    Navigate "$scratch\gif"
    Check 'ordinary animated GIF opens' (Image-Ready 'GIF.*12 x 6.*Frame [12]/2')
    Key $image 0x20 20
    Check 'GIF can be paused' (Image-Ready 'Paused')
    Key $image 0x1B 20
    Check 'closing animation clears playback state and restores the file panes' (Wait-Until {-not [FpUi]::IsWindowVisible($image) -and (Image-Title) -notmatch 'Frame|Playing|Paused'})
    Navigate "$scratch\svg"
    Check 'SVG renders in the isolated helper' (Image-Ready 'SVG.*16 x 8') (Image-Title)
    $pixel=Copy-Pixel 0 0
    Check 'SVG pixels preserve shape colours' ($pixel.Count -eq 4 -and $pixel[0] -eq 40 -and $pixel[1] -eq 20 -and $pixel[2] -eq 240 -and $pixel[3] -eq 255) ($pixel -join ',')
    $pixel=Copy-Pixel 8 0
    Check 'SVG clipboard preserves transparency' ($pixel.Count -eq 4 -and $pixel[3] -eq 128) ($pixel -join ',')
    Chord $image @('Ctrl','Shift') 0x52 20
    Check 'SVG rotation uses the normal image controls' (Image-Ready '8 x 16.*Rotated')
    Chord $image @('Ctrl','Shift') 0x48 20
    Check 'SVG flip uses the normal image controls' (Image-Ready 'Flipped')
    Chord $image 'Ctrl' 0x31 20
    Chord $image 'Ctrl' 0x30 20
    Check 'SVG fit and zoom remain available' (Image-Ready 'Fit')
    Shot $out '10_svg'
    Chord $image @('Ctrl','Shift') 0x55 20
    Check 'SVG source command switches to the text preview' (Wait-Until {[FpUi]::IsWindowVisible((Text-Window)) -and (Text-Title) -match 'shapes.svg' -and (Text-Title) -notmatch 'Loading|unavailable'}) (Text-Title)
    Click (Text-Window) 100 ([int](130*$scale));Post (Text-Window) 0x202 0 0
    Chord (Text-Window) 'Ctrl' 0x41 100;Set-Clipboard 'waiting for SVG source';Chord (Text-Window) 'Ctrl' 0x43 100
    Check 'SVG source is readable and copyable' (Wait-Until {(Get-Clipboard -Raw -ErrorAction SilentlyContinue) -match '<svg.*viewBox'})
    Key $pane0 0x24 20;Start-Sleep -Milliseconds 250
    Check 'redrawing the list retains SVG source mode' ([FpUi]::IsWindowVisible((Text-Window)) -and -not [FpUi]::IsWindowVisible($image))
    Chord (Text-Window) @('Ctrl','Shift') 0x55 20
    Check 'source command returns to the rendered SVG' ((Image-Ready 'SVG.*16 x 8') -and [FpUi]::IsWindowVisible($image) -and -not [FpUi]::IsWindowVisible((Text-Window)))
    Navigate "$scratch\viewbox"
    Check 'SVG with only a viewBox derives its size' (Image-Ready 'SVG.*24 x 12') (Image-Title)
    Navigate "$scratch\partial"
    Check 'SVG with unsupported elements still renders supported shapes' (Image-Ready 'SVG.*16 x 8')
    Chord $image @('Ctrl','Shift') 0x55 20
    Check 'unsupported SVG content remains available as source' (Wait-Until {[FpUi]::IsWindowVisible((Text-Window)) -and (Text-Title) -match 'partial.svg' -and (Text-Title) -notmatch 'Loading|unavailable'})
    Navigate "$scratch\doctype"
    Check 'SVG external entities are refused' (Wait-Until {(Image-Title) -match 'unavailable' -and [FpUi]::IsWindowVisible($image)}) (Image-Title)
    Chord $image @('Ctrl','Shift') 0x55 20
    Check 'a refused SVG can still be inspected as source' (Wait-Until {[FpUi]::IsWindowVisible((Text-Window)) -and (Text-Title) -match 'doctype.svg' -and (Text-Title) -notmatch 'Loading|unavailable'})
    Navigate "$scratch\badsvg"
    Check 'malformed SVG reports failure without blocking navigation' (Wait-Until {(Image-Title) -match 'unavailable' -and [FpUi]::IsWindowVisible($image)}) (Image-Title)
    Navigate "$scratch\large"
    Check 'large image starts fitted' (Image-Ready '2400 x 1600.*Fit') (Image-Title)
    Chord $image 'Ctrl' 0x31
    Check 'actual size shows 100 percent' (Image-Ready '100%.*Zoom') (Image-Title)
    Key $image 0x28
    Chord $image 'Ctrl' 0x32
    Check 'fit width is available' (Image-Ready 'Fit width') (Image-Title)
    Chord $image 'Ctrl' 0xBB
    Check 'zoom in works' (Image-Ready 'Zoom') (Image-Title)
    Chord $image 'Ctrl' 0xBD
    Chord $image 'Ctrl' 0x30
    Check 'reset returns to fit' (Image-Ready 'Fit') (Image-Title)
    Shot $out '04_large_fit'
    $helper=(Helpers)[0]
    $helper.Kill();$helper.WaitForExit(3000)|Out-Null
    Check 'helper crash shows a failure without closing the app' (Wait-Until {(Image-Title) -match 'unavailable' -and -not $p.HasExited})
    Navigate "$scratch\webp"
    Check 'another image restarts the helper' (Image-Ready 'WebP.*16 x 8') (Image-Title)
    Navigate "$scratch\pdf"
    Check 'switching to PDF hides the image reader' (Wait-Until {
        $script:pdf=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotPdf',[IntPtr]::Zero)
        [FpUi]::IsWindowVisible($script:pdf) -and -not [FpUi]::IsWindowVisible($image)
    })
    Navigate "$scratch\png"
    Check 'switching back to an image hides the PDF reader' ((Image-Ready 'PNG.*16 x 8') -and -not [FpUi]::IsWindowVisible($pdf))
    # The directory watcher notices a replacement under the same name.
    Copy-Item "$scratch\large\large.png" "$scratch\png\alpha.png" -Force
    Check 'changed selected file is refreshed' (Image-Ready '2400 x 1600') (Image-Title)
    Navigate "$scratch\bad"
    Check 'corrupt image reports failure' (Wait-Until {(Image-Title) -match 'unavailable'}) (Image-Title)
    Check 'navigation survives a corrupt image' (-not $p.HasExited)
    Shot $out '05_corrupt'
    Navigate "$scratch\empty"
    Check 'leaving images obeys zero linger' (Wait-Until {-not [FpUi]::IsWindowVisible($image) -and [FpUi]::IsWindowVisible($pane1)})
    Navigate "$scratch\webp"
    Check 'preview works after a decoding error' (Image-Ready 'WebP.*16 x 8')
    Set-Content $settings "[functions]`npreview.image = off"
    Check 'disabling image previews closes the reader' (Wait-Until {-not [FpUi]::IsWindowVisible($image)})
    Set-Content $settings "[preview.image]`nbackground = white`nlinger = 0"
    Check 'reenabling previews opens the current image' (Image-Ready 'WebP.*16 x 8')
    Navigate "$scratch\jpg"
    Check 'reenabling previews takes effect while running' (Image-Ready 'JPEG.*8 x 16')
    Shot $out '06_white_background'
    Check 'app closes cleanly after preview use' (Stop-Demo $p);$p=$null
    $profile=@(Import-Csv "$out\image-profile.csv")
    $viol=($profile|Where-Object name -eq 'fn_violations').value
    Check 'image actions have no lifetime violations' ($viol -eq '0') "$viol"
    $imagePaint=$profile|Where-Object name -eq 'image.paint'
    Check 'image painting stays within a UI frame' ($null -ne $imagePaint -and [double]$imagePaint.max_ms -le 16) "$($imagePaint.max_ms) ms"
    $ui=$profile|Where-Object name -eq 'ui.dispatch'
    Check 'image workflow stays within the UI message budget' ($null -ne $ui -and [double]$ui.max_ms -le 16) "$($ui.max_ms) ms"
    Check 'image helper exits with its owner'  (Wait-Until {(Helpers).Count -eq 0})
    # An image in the right pane must keep that list visible on the left.
    $p=Start-Demo $exe @("$scratch\empty","$scratch\webp")
    Post $pane1 0x0007 0;Key $pane1 0x24 0 # make right pane active without taking foreground focus
    Check 'right-pane image opens' (Image-Ready 'WebP.*16 x 8')
    Check 'right-pane list stays beside its preview' ([FpUi]::IsWindowVisible($pane1) -and -not [FpUi]::IsWindowVisible($pane0))
    Shot $out '07_right_pane'
    Check 'right-pane test closes cleanly' (Stop-Demo $p);$p=$null
    $p=Start-Demo $exe @("$scratch\empty")
    Post $pane0 $WM_CHAR 0x1C;Start-Sleep -Milliseconds 250
    Navigate "$scratch\webp"
    Check 'single-pane layout keeps the image preview' (Image-Ready 'WebP.*16 x 8')
    Key (Image-Window) 0x1B
    Check 'closing a preview restores the single pane' (Wait-Until {-not [FpUi]::IsWindowVisible((Image-Window)) -and -not [FpUi]::IsWindowVisible($pane1)})
    Check 'single-pane test closes cleanly' (Stop-Demo $p);$p=$null
    # Ordinary Linux file paths exercise the same decoder through the WSL share.
    $distro=(& wsl.exe --exec sh -c 'echo $WSL_DISTRO_NAME').Trim()
    if($LASTEXITCODE -ne 0 -or $distro -notmatch '^[\w.-]+$'){throw 'WSL is required for the image acceptance gate'}
    $linux="/tmp/openpilot-images-$PID";& wsl.exe --exec mkdir -p $linux
    if($LASTEXITCODE){throw 'WSL fixture mkdir failed'}
    $unc="\\wsl.localhost\$distro\tmp\openpilot-images-$PID"
    Copy-Item "$fixtures\alpha.webp" "$unc\linux.webp"
    $p=Start-Demo $exe @($unc,"$scratch\empty")
    Check 'Linux image previews from WSL' (Image-Ready 'WebP.*16 x 8') (Image-Title)
    Shot $out '08_wsl'
    & wsl.exe --exec mkdir -p "$linux/svg" "$linux/animation"
    Copy-Item "$fixtures\shapes.svg" "$unc\svg\linux.svg"
    Copy-Item "$fixtures\disposal.gif" "$unc\animation\linux.gif"
    Navigate "$unc\svg"
    Check 'SVG previews from a native Linux path' (Image-Ready 'SVG.*16 x 8') (Image-Title)
    Chord (Image-Window) @('Ctrl','Shift') 0x55 20
    Check 'Linux SVG source can be opened' (Wait-Until {[FpUi]::IsWindowVisible((Text-Window)) -and (Text-Title) -match 'linux.svg' -and (Text-Title) -notmatch 'Loading|unavailable'})
    Navigate "$unc\animation"
    Check 'GIF animation previews from a native Linux path' (Image-Ready 'GIF.*6 x 4.*Frame [1-4]/4') (Image-Title)
    Check 'Linux animation respects finite loop counts' (Wait-Until {(Image-Title) -match 'Frame 4/4.*Paused'} 7000)
    & wsl.exe --exec mkdir -p "$linux/case"
    & wsl.exe --exec cp /mnt/c/Work/openpilot/tests/fixtures/images/alpha.webp "$linux/case/Case.webp"
    & wsl.exe --exec cp /mnt/c/Work/openpilot/tests/fixtures/images/animated.webp "$linux/case/case.webp"
    Navigate "$unc\case"
    Check 'first case-sensitive Linux image opens' (Image-Ready '(16 x 8|12 x 6)')
    $first=Image-Title;Key $pane0 0x28 0
    Check 'case-distinct Linux image is a different preview' (Wait-Until {(Image-Title) -ne $first -and (Image-Title) -match '(16 x 8|12 x 6)' -and (Image-Title) -notmatch 'Loading'})
    & wsl.exe --exec mkdir -p "$linux/reserved"
    & wsl.exe --exec cp /mnt/c/Work/openpilot/tests/fixtures/images/alpha.webp "$linux/reserved/linux:photo.webp"
    Navigate "$unc\reserved"
    Check 'Linux image with a colon in its name previews' (Image-Ready 'WebP.*16 x 8') (Image-Title)
    Check 'WSL image test closes cleanly' (Stop-Demo $p);$p=$null
} finally {
    if($p){Stop-Demo $p|Out-Null}
    if($linux){& wsl.exe --exec rm -rf -- $linux}
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
exit $script:failed
