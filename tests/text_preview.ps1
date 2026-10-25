<# Text preview acceptance: private files, real helper, native UI and WSL paths. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class TextClipboardProbe {
 [DllImport("user32.dll")] public static extern bool OpenClipboard(IntPtr owner);
 [DllImport("user32.dll")] public static extern bool CloseClipboard();
}
"@
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root "build\$Config\folderwing.exe"
$out=Join-Path $root 'build\text';New-Item -ItemType Directory -Force $out|Out-Null
$script:passed=0;$script:failed=0
$script:readinessEvidence=Join-Path $out "readiness-$PID.jsonl"
function Check([string]$what,[bool]$ok,[string]$detail=''){if($ok){"  ok    $what";$script:passed++}else{Write-UiReadinessFailure $what;"  FAIL  $what $detail";$script:failed++}}
function Viewer { [FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotText',[IntPtr]::Zero) }
function Reader-Title { Window-Text (Viewer) }
function Ready([string]$pattern){Wait-Until { (Test-UiSettingsApplied) -and (Reader-Title) -match $pattern -and (Reader-Title) -notmatch 'Loading|unavailable' } 7000}
function Helpers {@(Get-ReadinessHelpers 'folderwing-text')}
function Navigate([string]$path){Navigate-Ready $path $script:pane0 7000}
function Focus-Reader { Click (Viewer) 100 ([int](130*$scale)); }
function Copy-All { Focus-Reader;Chord (Viewer) 'Ctrl' 0x41;Set-Clipboard 'waiting';Chord (Viewer) 'Ctrl' 0x43;Wait-Until {$script:copiedText=Get-Clipboard -Raw -ErrorAction SilentlyContinue; -not [string]::IsNullOrEmpty($script:copiedText) -and $script:copiedText -ne 'waiting'} 5000 }
function Edit-Find { [FpUi]::FindWindowExW((Viewer),[IntPtr]::Zero,'Edit',[IntPtr]::Zero) }
$scratch=Join-Path $env:TEMP "openpilot_text_$PID";$p=$null;$linux=$null
try {
    foreach($folder in @('code','utf16','utf32','legacy','emptyfile','long','binary','json','image','pdf','other','empty','unicode','many')){New-Item -ItemType Directory -Force "$scratch\$folder"|Out-Null}
    $code="// source preview`nint answer = 42;`n// Answer again`nreturn answer;`n"
    [IO.File]::WriteAllText("$scratch\code\sample.c",$code,[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$scratch\utf16\wide.txt","Romanian: ăîșț`r`nLine two",[Text.UnicodeEncoding]::new($false,$true))
    [IO.File]::WriteAllText("$scratch\utf32\wide.txt","Unicode: ă 😀",[Text.UTF32Encoding]::new($false,$true))
    [IO.File]::WriteAllBytes("$scratch\legacy\legacy.txt",[byte[]](99,97,102,233,32,128))
    [IO.File]::WriteAllBytes("$scratch\emptyfile\empty.txt",[byte[]]@())
    [IO.File]::WriteAllBytes("$scratch\binary\binary.txt",[byte[]](97,0,3,0,1,2,0,9,0,99))
    [IO.File]::WriteAllText("$scratch\unicode\unicode.txt","Romanian ăîșț and emoji 😀`nWide: 日本語",[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$scratch\json\data.json",'{"name":"OpenPilot","enabled":true,"number":42}',[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$scratch\many\many.py",(('def demo():  # code' + "`n" + '    return "answer"' + "`n")*8000),[Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText("$scratch\long\long.log",('x'*2200000),[Text.UTF8Encoding]::new($false))
    Copy-Item "$root\tests\fixtures\images\alpha.png" "$scratch\image\alpha.png"
    & "$PSScriptRoot\make_pdf.ps1" "$scratch\pdf\paper.pdf"|Out-Null
    $settings="$scratch\settings.ini";Set-Content $settings "[preview.text]`nlinger = 0`nlimit_mb = 1`n[preview.image]`nlinger = 0`n[preview.pdf]`nlinger = 0"
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\text-profile.csv","$scratch\code","$scratch\other")
    Check 'code previews automatically beside the list' (Ready 'Code.*UTF-8.*5 lines.*Wrap') (Reader-Title)
    Check 'text occupies the right pane' ([FpUi]::IsWindowVisible((Viewer)) -and -not [FpUi]::IsWindowVisible($pane1))
    Check 'text helper is isolated from the app' ((Helpers).Count -eq 1)
    Check 'copying all preview text completes' (Copy-All)
    Check 'copied text preserves source lines' ($script:copiedText -eq $code.Replace("`n","`r`n"))
    Set-Clipboard 'waiting';Check 'test can hold clipboard briefly' (Wait-Until {[TextClipboardProbe]::OpenClipboard([IntPtr]::Zero)} 2000)
    try {Chord (Viewer) 'Ctrl' 0x43;Start-Sleep -Milliseconds 150}finally{[TextClipboardProbe]::CloseClipboard()|Out-Null}
    Check 'text copying recovers from a transient clipboard lock' (Wait-Until {(Get-Clipboard -Raw) -eq $code.Replace("`n","`r`n")} 3000)
    Key (Viewer) 0x1B # clear selection
    Chord (Viewer) 'Ctrl' 0x24
    Chord (Viewer) 'Shift' 0x27;Chord (Viewer) 'Shift' 0x27
    Set-Clipboard 'waiting';Chord (Viewer) 'Ctrl' 0x43
    Check 'Shift+arrow selects text for copying' (Wait-Until {(Get-Clipboard -Raw) -eq '//'} 3000)
    Key (Viewer) 0x1B
    Set-Clipboard 'waiting';Chord (Viewer) 'Ctrl' 0x43
    Check 'Ctrl+C without selection copies the file' (Wait-Until {(Get-Clipboard -Raw) -ne 'waiting'})
    Chord (Viewer) 'Alt' 0x5A
    Check 'word wrap can be toggled' (Ready 'No wrap') (Reader-Title)
    Chord (Viewer) 'Alt' 0x5A
    Check 'word wrap can be restored' (Ready '\| Wrap')
    Shot $out '01_code'
    Chord $pane0 'Ctrl' 0x46
    Check 'Ctrl+F searches the text preview' (Wait-Until {(Edit-Find) -ne [IntPtr]::Zero})
    Send-Text (Edit-Find) 'answer'
    Check 'search finds all case-insensitive matches' (Ready 'Find 1/3') (Reader-Title)
    Key (Edit-Find) 0x0D
    Check 'Enter selects the next match' (Ready 'Find 2/3')
    Chord (Edit-Find) 'Shift' 0x0D
    Check 'Shift+Enter selects the previous match' (Ready 'Find 1/3')
    Shot $out '02_search'
    Send-AcknowledgedKey (Edit-Find) 0x1B 3000 # acknowledge find closure before copy
    Set-Clipboard 'waiting';Chord (Viewer) 'Ctrl' 0x43
    Check 'copy selected match copies only the match' (Wait-Until {(Get-Clipboard -Raw) -eq 'answer'} 3000)
    Key (Viewer) 0x1B # clear selection
    Key (Viewer) 0x1B # close preview
    Check 'Escape restores the file panes' (Wait-Until {-not [FpUi]::IsWindowVisible((Viewer)) -and [FpUi]::IsWindowVisible($pane1)})
    Start-Sleep -Milliseconds 200
    Check 'Escape keeps the same text preview closed' (-not [FpUi]::IsWindowVisible((Viewer)))
    Navigate "$scratch\utf16"
    Check 'UTF-16 BOM is detected' (Ready 'UTF-16 LE') (Reader-Title)
    Check 'UTF-16 text can be copied' (Copy-All)
    Check 'Unicode text is decoded correctly' ($script:copiedText -eq "Romanian: ăîșț`r`nLine two")
    Navigate "$scratch\utf32"
    Check 'UTF-32 BOM is detected' (Ready 'UTF-32 LE') (Reader-Title)
    $copied=Copy-All;$actual=$script:copiedText
    Check 'UTF-32 supplementary characters copy correctly' ($copied -and $actual -eq 'Unicode: ă 😀') ("Got: "+$actual)
    Navigate "$scratch\legacy"
    Check 'legacy fallback encoding is visible' (Ready 'Windows-1252') (Reader-Title)
    $copied=Copy-All;$actual=$script:copiedText
    Check 'Windows-1252 text copies correctly' ($copied -and $actual -eq 'café €') ("Got: "+$actual)
    Navigate "$scratch\unicode"
    Check 'UTF-8 supplementary and wide text previews' (Ready 'UTF-8')
    $copied=Copy-All;$actual=$script:copiedText
    Check 'UTF-8 text copies correctly' ($copied -and $actual -eq "Romanian ăîșț and emoji 😀`r`nWide: 日本語") ("Got: "+$actual)
    Shot $out '03_unicode'
    Navigate "$scratch\emptyfile"
    Check 'empty text file previews as one empty line' (Ready '1 lines')
    Navigate "$scratch\binary"
    Check 'binary contents are rejected despite a text extension' (Wait-Until {(Reader-Title) -match 'unavailable'}) (Reader-Title)
    Check 'binary contents do not close the explorer' (-not $p.HasExited)
    Navigate "$scratch\json"
    Check 'JSON preview recovers after binary data' (Ready 'JSON.*UTF-8') (Reader-Title)
    Shot $out '04_json'
    Navigate "$scratch\long"
    Check 'large file is visibly marked partial' (Ready 'Partial preview') (Reader-Title)
    Focus-Reader;Key (Viewer) 0x23
    Check 'large file remains navigable' (-not $p.HasExited)
    Chord (Viewer) 'Alt' 0x5A
    Check 'very long line can be viewed without wrapping' (Ready 'No wrap')
    Chord (Viewer) 'Ctrl' 0x23
    Chord (Viewer) 'Ctrl' 0x24
    Key (Viewer) 0x27
    Shot $out '05_long_line'
    Navigate "$scratch\many"
    Check 'large code file previews with line numbers' (Ready 'Python/Ruby.*16001 lines') (Reader-Title)
    Focus-Reader;Key (Viewer) 0x23;Key (Viewer) 0x24
    Shot $out '06_many_lines'
    # Copy operation remains available while a preview covers the destination pane.
    Navigate "$scratch\code";Check 'code preview reopens for Operations' (Ready 'sample.c')
    Key $pane0 0x74
    Set-Clipboard 'waiting for Operations draft'
    Check 'F5 still opens Operations while text is previewed' (Wait-Until {
        if (@(Get-Process folderwing-operations -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -ne 1) { return $false }
        $side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
        $script:operationSlot=[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)
        if ($script:operationSlot -eq [IntPtr]::Zero) { return $false }
        Chord $script:operationSlot 'Ctrl' 0x43 0
        (Get-Clipboard -Raw) -match 'Ready to start'
    })
    $slot=$script:operationSlot;Key $slot 0x0D
    Check 'previewed text copies to the other folder' (Wait-Until {Test-Path "$scratch\other\sample.c"})
    Chord $slot @('Ctrl','Shift') 0x4F
    Navigate "$scratch\image"
    Check 'switching to an image hides the text reader' (Wait-Until {
        $img=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotImage',[IntPtr]::Zero)
        [FpUi]::IsWindowVisible($img) -and -not [FpUi]::IsWindowVisible((Viewer))
    })
    Navigate "$scratch\pdf"
    Check 'switching to PDF hides the text reader' (Wait-Until {
        $pdf=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotPdf',[IntPtr]::Zero)
        [FpUi]::IsWindowVisible($pdf) -and -not [FpUi]::IsWindowVisible((Viewer))
    })
    Navigate "$scratch\code";Check 'text returns after other preview formats' (Ready 'sample.c')
    [IO.File]::WriteAllText("$scratch\code\sample.c","// changed`nint count = 1;",[Text.UTF8Encoding]::new($false))
    Check 'changed selected text file refreshes' (Ready '2 lines') (Reader-Title)
    $copied=Copy-All;$actual=$script:copiedText
    Check 'refreshed file copies the new contents' ($copied -and $actual -eq "// changed`r`nint count = 1;") ("Got: "+$actual)
    $helper=(Helpers)[0];$helper.Kill();$helper.WaitForExit(3000)|Out-Null
    Check 'helper crash leaves the app usable' (Wait-Until {(Reader-Title) -match 'unavailable' -and -not $p.HasExited})
    Navigate "$scratch\json";Check 'next text file restarts the helper' (Ready 'JSON.*UTF-8')
    Set-UiSettings $settings "[functions]`npreview.text = off"
    Check 'text preview can be disabled while running' (Wait-Until {(Test-UiSettingsApplied) -and -not [FpUi]::IsWindowVisible((Viewer))})
    Set-UiSettings $settings "[preview.text]`nwrap = off`nlinger = 0"
    Check 'enabling text preview opens the current file' (Ready 'JSON.*No wrap')
    Navigate "$scratch\empty"
    Check 'zero linger restores both panes' (Wait-Until {-not [FpUi]::IsWindowVisible((Viewer)) -and [FpUi]::IsWindowVisible($pane1)})
    Check 'text app closes cleanly' (Stop-Demo $p);$p=$null
    $profile=@(Import-Csv "$out\text-profile.csv")
    Check 'text actions have no lifetime violations' (($profile|Where-Object name -eq fn_violations).value -eq '0')
    $paint=$profile|Where-Object name -eq 'text.paint';$ui=$profile|Where-Object name -eq 'ui.dispatch'
    Check 'text paints stay within the UI frame budget' ($null -ne $paint -and [double]$paint.max_ms -le 16) "$($paint.max_ms) ms"
    Check 'text workload stays within the UI message budget' ($null -ne $ui -and [double]$ui.max_ms -le 16) "$($ui.max_ms) ms"
    Check 'text helper exits with its owner' (Wait-Until {(Helpers).Count -eq 0})
    $p=Start-Demo $exe @("$scratch\empty","$scratch\code");Post $pane1 0x0007 0;Key $pane1 0x24
    Check 'right-pane text previews while retaining its source list' ((Ready 'sample.c') -and [FpUi]::IsWindowVisible($pane1) -and -not [FpUi]::IsWindowVisible($pane0))
    Check 'right-pane text closes cleanly' (Stop-Demo $p);$p=$null
    $p=Start-Demo $exe @("$scratch\empty");Post $pane0 $WM_CHAR 0x1C;Start-Sleep -Milliseconds 200;Navigate "$scratch\code"
    Check 'text preview works in single-pane mode' (Ready 'sample.c')
    Focus-Reader;Key (Viewer) 0x1B
    Check 'closing text restores single-pane mode' (Wait-Until {-not [FpUi]::IsWindowVisible((Viewer)) -and -not [FpUi]::IsWindowVisible($pane1)})
    Check 'single-pane text closes cleanly' (Stop-Demo $p);$p=$null
    $distro=(& wsl.exe --exec sh -c 'echo $WSL_DISTRO_NAME').Trim();if($LASTEXITCODE -or $distro -notmatch '^[\w.-]+$'){throw 'WSL required for text acceptance'}
    $linux="/tmp/openpilot-text-$PID";& wsl.exe --exec mkdir -p $linux
    $unc="\\wsl.localhost\$distro\tmp\openpilot-text-$PID"
    [IO.File]::WriteAllText("$unc\linux.txt","WSL text ă 😀",[Text.UTF8Encoding]::new($false))
    $p=Start-Demo $exe @($unc,"$scratch\empty")
    Check 'Linux files preview through WSL' (Ready 'linux.txt.*UTF-8') (Reader-Title)
    $copied=Copy-All;$actual=$script:copiedText
    Check 'Linux Unicode text copies correctly' ($copied -and $actual -eq 'WSL text ă 😀') ("Got: "+$actual)
    Shot $out '07_wsl'
    Check 'WSL text closes cleanly' (Stop-Demo $p);$p=$null
} finally {
    if($p){Stop-Demo $p|Out-Null}
    if($linux){& wsl.exe --exec rm -rf -- $linux}
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
exit $script:failed
