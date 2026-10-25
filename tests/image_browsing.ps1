<# Folder image browsing through real pane/preview controls and private fixtures. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;using System.Text;
public static class BrowseProbe {
 [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")] static extern bool CloseClipboard();
 [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint f);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)] static extern uint DragQueryFileW(IntPtr h,uint i,StringBuilder b,uint n);
 public static string CopiedFile() {
  if(!OpenClipboard(IntPtr.Zero))return "";
  try {var h=GetClipboardData(15);if(h==IntPtr.Zero||DragQueryFileW(h,0xffffffff,null,0)!=1)return "";
   var b=new StringBuilder(4096);DragQueryFileW(h,0,b,4096);return b.ToString();
  } finally {CloseClipboard();}
 }
 [DllImport("ntdll.dll")] public static extern int NtSuspendProcess(IntPtr p);
 [DllImport("ntdll.dll")] public static extern int NtResumeProcess(IntPtr p);
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\image_browsing';New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0;$p=$null;$linux=$null;$pausedHelper=$null
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Image {[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)}
function Image-Title {Window-Text (Image)}
function Prop([string]$name){[FpUi]::GetPropW((Image),"Image.$name").ToInt64()}
function Ready([string]$name,[int]$position,[int]$count){Wait-Until {[FpUi]::IsWindowVisible((Image)) -and (Image-Title) -cmatch $name -and (Image-Title) -notmatch 'Loading|unavailable' -and (Prop 'Position') -eq $position -and (Prop 'Count') -eq $count} 7000}
function Navigate([string]$path,[IntPtr]$pane=$script:pane0){
    Chord $pane 'Ctrl' 0x4C 20;$edit=(Pane-Edits $pane)[0];Send-Text $edit $path;Key $edit 0x0D 20
    Wait-Until {(Window-Text $edit) -eq $path -and [FpUi]::GetPropW($pane,'Address.Loading').ToInt64() -eq 0} 7000
}
function Next {Chord (Image) 'Ctrl' 0x22 0}
function Previous {Chord (Image) 'Ctrl' 0x21 0}
function Helpers {@(Get-Process folderwing-image -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid})}
function Native([string]$command){$v=& wsl.exe --exec sh -c $command;if($LASTEXITCODE){throw "Private WSL fixture failed: $command"};($v -join "`n").TrimEnd()}
$scratch=Join-Path $env:TEMP "openpilot_image_browsing_$PID"
try {
    foreach($dir in @('main','edge','allbad','right','empty','large','rapid')){New-Item -ItemType Directory -Force "$scratch\$dir"|Out-Null}
    $fixtures="$root\tests\fixtures\images"
    Copy-Item "$fixtures\alpha.png" "$scratch\main\01-photo.png"
    Copy-Item "$fixtures\alpha.webp" "$scratch\main\03-photo.webp"
    Copy-Item "$fixtures\animated.gif" "$scratch\main\04-photo.gif"
    Copy-Item "$fixtures\shapes.svg" "$scratch\main\05-photo.svg"
    Set-Content -LiteralPath "$scratch\main\02-photo.jpg" -Value 'broken image';Set-Content -LiteralPath "$scratch\main\00-note.txt" -Value 'not an image'
    New-Item -ItemType Directory -Force "$scratch\main\00-folder.png"|Out-Null
    Copy-Item "$fixtures\alpha.png" "$scratch\edge\01-good.png";Set-Content -LiteralPath "$scratch\edge\02-bad.jpg" -Value 'broken image'
    Set-Content -LiteralPath "$scratch\allbad\01-bad.jpg" -Value 'broken';Set-Content -LiteralPath "$scratch\allbad\02-bad.png" -Value 'broken'
    Copy-Item "$fixtures\alpha.webp" "$scratch\right\01-right.webp";Copy-Item "$fixtures\shapes.svg" "$scratch\right\02-right.svg"
    Copy-Item "$fixtures\alpha.png" "$scratch\rapid\01-red.png";Copy-Item "$fixtures\alpha.webp" "$scratch\rapid\02-green.webp";Copy-Item "$fixtures\shapes.svg" "$scratch\rapid\03-final.svg"
    foreach($i in 1..2){$bmp=[Drawing.Bitmap]::new($(if($i -eq 1){2400}else{1600}),$(if($i -eq 1){1600}else{1200}));$gr=[Drawing.Graphics]::FromImage($bmp);$gr.Clear([Drawing.Color]::CornflowerBlue);$bmp.Save("$scratch\large\0$i-large.png",[Drawing.Imaging.ImageFormat]::Png);$gr.Dispose();$bmp.Dispose()}
    $settings="$scratch\settings.ini";$base="[functions]`npreview.text = off`npreview.pdf = off`n[preview.image]`nlinger = 0`n"
    Set-Content -LiteralPath $settings -Value $base
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","--profile-out=$out\browsing-profile.csv","$scratch\main","$scratch\empty")
    # The directories sort first; select the first image by filtering out the folder/note.
    Key $pane0 0x24 20;Send-Text $pane0 'photo'
    Check 'filtered image list opens with its source-pane order' (Ready '01-photo.png' 1 5) (Image-Title)
    Check 'first image disables Previous and enables Next' ((Prop 'Previous') -eq 0 -and (Prop 'Next') -eq 1)
    $frame=Image;$helper=(Helpers)[0]
    Next;Check 'Next skips a corrupt recognized image and selects the next decodable file' (Ready '03-photo.webp' 2 4) (Image-Title)
    Check 'browsing retains the same preview window and helper' ((Image) -eq $frame -and (Helpers).Count -eq 1 -and (Helpers)[0].Id -eq $helper.Id)
    Chord (Image) 'Ctrl' 0x43 20
    Check 'file commands now target the browsed selection' (Wait-Until {[BrowseProbe]::CopiedFile() -eq "$scratch\main\03-photo.webp"} 3000)
    Previous;Check 'Previous skips the known failure in reverse' (Ready '01-photo.png' 1 4)
    Click (Image) ([int](170*$scale)) ([int](65*$scale));Post (Image) 0x202 0 0
    Check 'Next image button browses a file' (Ready '03-photo.webp' 2 4)
    Click (Image) ([int](60*$scale)) ([int](65*$scale));Post (Image) 0x202 0 0
    Check 'Previous image button browses a file' (Ready '01-photo.png' 1 4)
    Next;Check 'returning to WebP is ready' (Ready '03-photo.webp' 2 4);Next
    Check 'folder browsing opens GIF with a separate frame counter' (Ready '04-photo.gif.*Frame' 3 4)
    Key (Image) 0x20 20;Check 'Space still pauses animation' (Wait-Until {(Image-Title) -match 'Paused'} 3000)
    $old=Image-Title;Chord (Image) @('Ctrl','Shift') 0x27 20
    Check 'animation frame stepping stays on the same file and image position' (Wait-Until {(Image-Title) -ne $old -and (Image-Title) -match '04-photo.gif.*Paused' -and (Prop 'Position') -eq 3} 3000)
    Next;Check 'Next after an animation browses an SVG file' (Ready '05-photo.svg' 4 4)
    Check 'last image disables Next and retains Previous' ((Prop 'Next') -eq 0 -and (Prop 'Previous') -eq 1)
    $last=Image-Title;Next;Start-Sleep -Milliseconds 150;Check 'browsing does not wrap at an edge' ((Image-Title) -eq $last)
    Post (Image) $WM_CONTEXTMENU (Image).ToInt64() -1;$menu=Open-Menu
    Check 'image context menu exposes folder navigation independently of frame commands' ($menu -and $menu.Items -contains 'Previous image' -and $menu.Items -notcontains 'Next image' -and $menu.Items -notcontains 'Next animation frame');if($menu){Key $menu.Window 0x1B 20}
    Key $pane0 0x1B 20;Key $pane0 0x24 20;Key $pane0 0x28 20;Key $pane0 0x28 20
    Check 'folders and non-image files are omitted from the unfiltered counter' (Ready '01-photo.png' 1 5)
    Click-Header $pane0 'Name'
    Check 'descending sort updates the image position while preserving the selected file' (Ready '01-photo.png' 5 5) (Image-Title)
    Next;Check 'Next is unavailable at the end of descending order' ((Prop 'Next') -eq 0)
    Previous;Check 'Previous follows descending pane order' (Ready '03-photo.webp' 3 4) (Image-Title)
    Click-Header $pane0 'Name';Check 'ascending sort restores the file position' (Ready '03-photo.webp' 3 5)
    # Narrowing and clearing a filter invalidates the old order asynchronously.
    Send-Text $pane0 '03-photo';Check 'filter restricts browsing to its visible matches' (Ready '03-photo.webp' 1 1)
    Check 'single-image filter disables both directions' ((Prop 'Previous') -eq 0 -and (Prop 'Next') -eq 0)
    Key $pane0 0x1B 20;Check 'clearing a filter restores the folder image list' (Ready '03-photo.webp' 3 5)
    Check 'edge-error folder opens' (Navigate "$scratch\edge");Check 'edge-error first image is ready' (Ready '01-good.png' 1 2)
    Next;Check 'a trailing undecodable image restores the last good image without looping' (Ready '01-good.png' 1 1)
    Check 'all-bad folder opens' (Navigate "$scratch\allbad")
    Check 'manually selected bad image remains inspectable and is excluded from browsing' (Wait-Until {(Image-Title) -match '01-bad.jpg.*unavailable' -and (Prop 'Count') -eq 1 -and (Prop 'Next') -eq 1} 7000)
    Next;Check 'all unsupported images end with a responsive empty browse list' (Wait-Until {(Image-Title) -match '02-bad.png.*unavailable' -and (Prop 'Count') -eq 0 -and (Prop 'Next') -eq 0 -and (Prop 'Previous') -eq 0} 7000)
    Check 'large-image folder opens' (Navigate "$scratch\large");Check 'large image starts in Fit' (Ready '01-large.png.*Fit' 1 2)
    Chord (Image) 'Ctrl' 0x31 20;Check 'actual size is selected' (Wait-Until {(Image-Title) -match '100%.*Zoom'} 3000)
    Next;Check '100-percent zoom is retained across file browsing' (Ready '02-large.png.*100%.*Zoom' 2 2)
    Chord (Image) 'Ctrl' 0x32 20;Previous;Check 'Fit width is retained across different image dimensions' (Ready '01-large.png.*Fit width' 1 2)
    Chord (Image) 'Ctrl' 0x30 20;Next;Check 'Fit preference remains Fit for the next image' (Ready '02-large.png.*Fit' 2 2)
    Key (Image) 0x25 20;Check 'unmodified arrows still pan without changing the image' ((Prop 'Position') -eq 2 -and (Image-Title) -match '02-large.png')
    Chord (Image) 'Ctrl' 0x31 20;Key $pane0 0x26 20;Check 'direct list selection in the same folder also retains zoom' (Ready '01-large.png.*100%.*Zoom' 1 2)
    $savedMain=$script:main;try{$script:main=Image;Shot $out '01_browse_controls'}finally{$script:main=$savedMain}
    Check 'rapid-navigation fixture opens' (Navigate "$scratch\rapid");Check 'rapid first image opens' (Ready '01-red.png' 1 3)
    $pausedHelper=(Helpers)[0];[BrowseProbe]::NtSuspendProcess($pausedHelper.Handle)|Out-Null
    Next;Next;Previous;Next;Next;Previous;Next
    [BrowseProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null;$pausedHelper=$null
    Check 'rapid actions display only the latest requested file after the helper resumes' (Ready '03-final.svg' 3 3)
    Remove-Item -LiteralPath "$scratch\rapid\02-green.webp"
    Check 'directory deletion refreshes the image count and keeps the selected file' (Ready '03-final.svg' 2 2)
    Copy-Item "$fixtures\alpha.webp" "$scratch\rapid\04-new.webp"
    Check 'a newly created image updates the list and Next state' (Wait-Until {(Prop 'Count') -eq 3 -and (Prop 'Position') -eq 2 -and (Prop 'Next') -eq 1} 7000)
    Next;Check 'newly created image is browsable' (Ready '04-new.webp' 3 3)
    Check 'settings watcher is ready' (Wait-Until {[FpUi]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Set-Content -LiteralPath $settings -Value ($base+"[functions]`npreview.image = off`n")
    Check 'disabling preview cancels browsing and hides the reader' (Wait-Until {-not [FpUi]::IsWindowVisible((Image))} 7000)
    Set-Content -LiteralPath $settings -Value $base;Check 'reenabling preview rebuilds the current image list' (Ready '04-new.webp' 3 3)
    Previous;Check 'returning to an interior image enables Next' (Ready '03-final.svg' 2 3)
    Set-Content -LiteralPath $settings -Value ($base+"[functions]`nimage.next_file = off`n")
    Check 'disabling the Next function disables its header control' (Wait-Until {(Prop 'Next') -eq 0} 5000)
    Next;Click (Image) ([int](170*$scale)) ([int](65*$scale));Post (Image) 0x202 0 0
    Check 'disabled folder navigation is inert through both keys and buttons' (Ready '03-final.svg' 2 3)
    Set-Content -LiteralPath $settings -Value ($base+"[keys]`nimage.next_file = Ctrl+Alt+N`n")
    Check 'reenabling the function restores the header control' (Wait-Until {(Prop 'Next') -eq 1} 5000)
    Next;Start-Sleep -Milliseconds 150;Check 'a customized binding replaces the default folder shortcut' (Ready '03-final.svg' 2 3)
    Chord (Image) @('Ctrl','Alt') 0x4E 20;Check 'the customized folder shortcut invokes normal function dispatch' (Ready '04-new.webp' 3 3)
    Set-Content -LiteralPath $settings -Value $base
    Check 'main browsing app closes cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\browsing-profile.csv" -Raw
    Check 'browsing actions and scanner have no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,');Check 'browsing stays within the 16-ms UI dispatch budget' ($match.Success -and [double]$match.Groups[1].Value -lt 16) $match.Value
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","$scratch\empty","$scratch\right")
    Post $pane1 7 0;Key $pane1 0x24 0
    Check 'right-pane source opens beside its image preview' (Ready '01-right.webp' 1 2)
    Next;Check 'browsing uses the right pane when it owns the preview' (Ready '02-right.svg' 2 2)
    Check 'right-pane source stays visible and left pane stays hidden' ([FpUi]::IsWindowVisible($pane1) -and -not [FpUi]::IsWindowVisible($pane0))
    Check 'right-pane app closes cleanly' (Stop-Demo $p);$p=$null
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","$scratch\empty")
    Post $pane0 $WM_CHAR 0x1C;Start-Sleep -Milliseconds 100;Navigate "$scratch\right"|Out-Null;Check 'single-pane source opens' (Ready '01-right.webp' 1 2);Next
    Check 'single-pane layout supports image browsing' (Ready '02-right.svg' 2 2)
    Key (Image) 0x1B 20;Check 'Escape closes browsing and restores the single pane' (Wait-Until {-not [FpUi]::IsWindowVisible((Image)) -and -not [FpUi]::IsWindowVisible($pane1)} 3000)
    Check 'single-pane app closes cleanly' (Stop-Demo $p);$p=$null
    $linux="/tmp/openpilot-image-browsing-$PID";$distro=Native 'echo "$WSL_DISTRO_NAME"';$unc="\\wsl.localhost\$distro$($linux.Replace('/','\'))"
    Native "mkdir -p '$linux'; cp /mnt/c/Work/openpilot/tests/fixtures/images/alpha.webp '$linux/Case.webp'; cp /mnt/c/Work/openpilot/tests/fixtures/images/animated.webp '$linux/case.webp'; cp /mnt/c/Work/openpilot/tests/fixtures/images/alpha.png '$linux/last:photo.png'; mkfifo '$linux/pipe.png'; ln -s Case.webp '$linux/link.png'"|Out-Null
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","$unc","$scratch\empty")
    Check 'Linux list excludes special nodes and symlinks without opening them' (Ready '(Case.webp|case.webp)' 1 3) (Image-Title)
    $first=Image-Title;$nextCase=if($first -cmatch '^Case.webp'){'case.webp.*12 x 6'}else{'Case.webp.*16 x 8'}
    Next;Check 'Linux case-sensitive image names remain distinct' (Ready $nextCase 2 3) (Image-Title)
    Next;Check 'Linux reserved punctuation survives file browsing' (Ready 'last:photo.png' 3 3)
    Check 'Linux app closes with its scanner and image helper' (Stop-Demo $p);$p=$null
}finally{
    if($pausedHelper){[BrowseProbe]::NtResumeProcess($pausedHelper.Handle)|Out-Null}
    if($p){Stop-Demo $p|Out-Null}
    if($linux){Native "rm -rf '$linux'"|Out-Null}
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
