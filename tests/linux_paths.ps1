<# Linux address entry acceptance. Private paths; actual WSL resolver and native controls. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class AddressProbe {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr GetPropW(IntPtr h,string name);
}
"@
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\linux_paths'
New-Item -ItemType Directory -Force $out|Out-Null
$exe=Join-Path $root "build\$Config\folderwing.exe";$script:passed=0;$script:failed=0
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command){$value=& wsl.exe --exec sh -c $command;if($LASTEXITCODE){throw "WSL fixture failed: $command"};($value -join "`n").TrimEnd()}
function Address($pane=$script:pane0){(Pane-Edits $pane)[0]}
function Current($pane=$script:pane0){Window-Text (Address $pane)}
function Distro-Label($pane=$script:pane0){[FpUi]::FindWindowExW($pane,[IntPtr]::Zero,'Static',[IntPtr]::Zero)}
function Error-Code($pane=$script:pane0){[AddressProbe]::GetPropW($pane,'Address.Error').ToInt64()}
function Resolving($pane=$script:pane0){[AddressProbe]::GetPropW($pane,'Address.Resolving').ToInt64() -ne 0}
function Navigate([string]$typed,$pane=$script:pane0){Chord $pane 'Ctrl' 0x4C 20;Send-Text (Address $pane) $typed;Key (Address $pane) 0x0D 20}
function Ready([string]$canonical,$pane=$script:pane0){Wait-Until {(Current $pane) -ceq $canonical -and -not (Resolving $pane) -and [AddressProbe]::GetPropW($pane,'Address.Loading').ToInt64() -eq 0 -and (Error-Code $pane) -eq 0} 7000}
$scratch=Join-Path $env:TEMP "openpilot_linux_paths_$PID";$linux="/tmp/openpilot-paths-$PID";$p=$null
try {
    $distro=Native 'echo "$WSL_DISTRO_NAME"'
    if($distro -notmatch '^[\w.-]+$'){throw 'A default WSL distro is required'}
    $linuxHome=Native 'getent passwd "$(id -u)" | cut -d: -f6'
    Native "mkdir -p '$linux/A/child' '$linux/a' '$linux/space β ' '$linux/literal%USERPROFILE%`$(nothing);' '$linux/back\slash:star*'"|Out-Null
    Native "ln -s A '$linux/link'; printf data > '$linux/file'"|Out-Null
    $rawHex=[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($linux))
    $fixture="import os;b=bytes.fromhex('$rawHex');[(os.mkdir(b+b'/'+n.encode())) for n in ['pair:', 'pair'+chr(0xf03a), 'pair'+chr(0xf080)+chr(0xf03a)]]"
    $fixtureHex=[Convert]::ToHexString([Text.Encoding]::UTF8.GetBytes($fixture))
    Native "python3 -c 'import sys;exec(bytes.fromhex(sys.argv[1]))' $fixtureHex"|Out-Null
    $unc="\\wsl.localhost\$distro";$nativeRoot="$unc$($linux.Replace('/','\'))"
    foreach($dir in @('left','right')){New-Item -ItemType Directory -Force "$scratch\$dir"|Out-Null}
    $settings="$scratch\settings.ini"
    $base="[functions]`npreview.image = off`npreview.text = off`npreview.pdf = off`n"
    Set-Content -LiteralPath $settings -Value $base
    $p=Start-Demo $exe @("--settings=$settings","--profile-out=$out\address-profile.csv","$scratch\left","$scratch\right")
    Check 'a Windows folder has no WSL badge' (-not [FpUi]::IsWindowVisible((Distro-Label)))
    Navigate "$linux/A"
    Check 'an absolute Linux path switches a Windows pane to the default distro' (Ready "$nativeRoot\A") (Current)
    Check 'the distro remains visible beside the path' ([FpUi]::IsWindowVisible((Distro-Label)) -and (Window-Text (Distro-Label)) -eq "WSL: $distro")
    Chord $pane0 'Ctrl' 0x4C 20
    Check 'editing the address shows its Linux spelling' ((Current) -ceq "$linux/A") (Current)
    Key (Address) 0x1B 20
    Check 'Escape restores the current path and breadcrumbs' ((Current) -ceq "$nativeRoot\A" -and -not [FpUi]::IsWindowVisible((Address)))
    Navigate "$linux/a"
    Check 'Linux paths preserve case-sensitive folder identity' (Ready "$nativeRoot\a") (Current)
    Navigate "$linux/space β "
    Check 'Linux path entry preserves Unicode and trailing spaces' (Ready "$nativeRoot\space β ") (Current)
    Navigate "$linux/literal%USERPROFILE%`$(nothing);"
    Check 'Linux filenames are literal without environment or shell expansion' (Ready "$nativeRoot\literal%USERPROFILE%`$(nothing);") (Current)
    Navigate "$linux/back\slash:star*"
    $reserved="$nativeRoot\back$([char]0xF05C)slash$([char]0xF03A)star$([char]0xF02A)"
    Check 'Linux backslashes, colons and wildcards are literal filename characters' (Ready $reserved) (Current)
    Chord $pane0 'Ctrl' 0x4C 20
    Check 'editing preserves the original reserved filename characters' ((Current) -ceq "$linux/back\slash:star*")
    Key (Address) 0x1B 20
    Navigate "$linux/pair:"
    Check 'an ASCII colon folder keeps its own canonical identity' (Ready "$nativeRoot\pair$([char]0xf03a)") (Current)
    $puaTyped="$linux/pair$([char]0xf03a)";$puaCanonical="$nativeRoot\pair$([char]0xf080)$([char]0xf03a)"
    Navigate $puaTyped
    Check 'a genuine private-use sibling opens through a distinct identity' (Ready $puaCanonical) (Current)
    Chord $pane0 'Ctrl' 0x4C 20
    Check 'editing restores genuine private-use text without converting it to colon' ((Current) -ceq $puaTyped)
    Navigate (Current)
    Check 'resubmitting a private-use path retains its exact identity' (Ready $puaCanonical) (Current)
    Navigate "$linux/pair$([char]0xf080)$([char]0xf03a)"
    Check 'a literal escape prefix and private-use character retain a third identity' (Ready "$nativeRoot\pair$([char]0xf080)$([char]0xf080)$([char]0xf080)$([char]0xf03a)") (Current)
    Navigate "$linux/A/./child/.."
    Check 'dot segments resolve inside Linux' (Ready "$nativeRoot\A")
    Navigate "$linux/link/child/.."
    Check 'symlink traversal resolves inside Linux' (Ready "$nativeRoot\A") (Current)
    Navigate '~'
    Check 'tilde expands to the default Linux user home in the current distro' (Ready "$unc$($linuxHome.Replace('/','\'))") (Current)
    Navigate '~/.'
    Check 'tilde slash syntax resolves home-relative paths' (Ready "$unc$($linuxHome.Replace('/','\'))")
    Navigate '/'
    Check 'a lone slash opens the distro root' (Ready "$unc") (Current)
    Navigate "$linux/A"
    Check 'the error fixture starts from its requested folder' (Ready "$nativeRoot\A")
    $before=Current
    Navigate "$linux/missing"
    Check 'a missing Linux directory reports its error' (Wait-Until {(Error-Code) -eq 3 -and -not (Resolving)} 5000)
    Check 'a missing path retains the previous usable folder' ((Current) -ceq $before)
    Navigate "$linux/file"
    Check 'entering a file path reports that a directory is required' (Wait-Until {(Error-Code) -eq 267} 5000)
    Check 'a file path leaves the folder intact' ((Current) -ceq $before)
    Navigate '~somebody'
    Check 'unsupported named-user tilde syntax reports a clear error' ((Error-Code) -eq 123 -and (Current) -ceq $before)
    Navigate "$linux/a"
    Check 'valid navigation recovers after path errors' (Ready "$nativeRoot\a")
    Navigate "$scratch\right"
    Check 'Windows path entry still works after Linux browsing' (Ready "$scratch\right")
    Check 'returning to Windows hides the distro badge' (-not [FpUi]::IsWindowVisible((Distro-Label)))
    Navigate '%TEMP%'
    Check 'Windows environment-variable expansion still works' (Ready ($env:TEMP.TrimEnd('\'))) (Current)
    # Superseding navigation must discard any result, including one already queued.
    Navigate "$linux/A"
    Navigate "$scratch\left"
    Check 'a newer Windows navigation wins over Linux resolution' (Ready "$scratch\left")
    Start-Sleep -Milliseconds 300
    Check 'an obsolete Linux result cannot redirect the pane' ((Current) -ceq "$scratch\left")
    Navigate "$linux/A"
    Navigate "$linux/a"
    Check 'the latest Linux request wins' (Ready "$nativeRoot\a")
    Navigate "$linux/A" $pane1
    Check 'the second pane resolves independently' (Ready "$nativeRoot\A" $pane1)
    Check 'panes retain independent paths and distro badges' ((Current $pane0) -ceq "$nativeRoot\a" -and (Window-Text (Distro-Label $pane1)) -eq "WSL: $distro")
    Shot $out '01_linux_addresses'
    Check 'the settings watcher is ready before live updates' (Wait-Until {[AddressProbe]::GetPropW($main,'Settings.Watching').ToInt64() -eq 1} 5000)
    Set-Content -LiteralPath $settings -Value ($base+"view.breadcrumbs = off`n")
    Check 'plain address mode shows a Linux path alongside its distro' (Wait-Until {[FpUi]::IsWindowVisible((Address $pane0)) -and (Current $pane0) -ceq "$linux/a"} 5000)
    Set-Content -LiteralPath $settings -Value $base
    Check 'restoring breadcrumbs keeps the canonical current folder' (Wait-Until {-not [FpUi]::IsWindowVisible((Address $pane0)) -and (Current $pane0) -ceq "$nativeRoot\a"} 5000)
    Navigate "$scratch\left"
    Set-Content -LiteralPath $settings -Value ($base+"wsl.helper = off`n")
    Start-Sleep -Milliseconds 500
    Navigate "$linux/A"
    Check 'disabled WSL integration refuses Linux syntax without opening a Windows lookalike' (Wait-Until {(Error-Code) -eq 50 -and (Current) -ceq "$scratch\left"} 3000)
    Set-Content -LiteralPath $settings -Value $base
    Start-Sleep -Milliseconds 500
    Navigate "$linux/A"
    Check 'reenabled integration resumes Linux path entry' (Ready "$nativeRoot\A")
    Navigate "$linux/a"
    Check 'closing while resolution is pending completes cleanly' (Stop-Demo $p);$p=$null
    $report=Get-Content "$out\address-profile.csv" -Raw
    Check 'Linux address navigation has no function lifetime violations' ($report -match 'metric,fn_violations,,,,0,')
    $match=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,')
    Check 'address workflow stays inside the UI message budget' ($match.Success -and [double]$match.Groups[1].Value -lt 16) $match.Groups[1].Value
} finally {
    if($p){Stop-Demo $p|Out-Null}
    & wsl.exe --exec rm -rf -- $linux
    Remove-Item -LiteralPath $scratch -Recurse -Force -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
exit $script:failed
