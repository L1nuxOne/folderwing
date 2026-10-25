<# Real cross-distro CLI and Operations acceptance. Requires an owned temporary
   distro named by build/cross_distro/test_distro.json; no user distro changes. #>
param([string]$Config='release')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
Add-Type -AssemblyName System.Windows.Forms
$root=Split-Path $PSScriptRoot -Parent;$out=Join-Path $root 'build\cross_distro';$record=Get-Content "$out\test_distro.json" -Raw|ConvertFrom-Json
if(-not $record.created -or $record.name -notmatch '^OpenPilotCrossTest-[0-9a-f]{12}$'){throw 'The private cross-distro fixture is unavailable'}
$targetDistro=$record.name;$script:passed=0;$script:failed=0;$p=$null;$script:lastToken=0
function Check([string]$name,[bool]$ok,[string]$detail=''){if($ok){"  ok    $name";$script:passed++}else{"  FAIL  $name $detail";$script:failed++}}
function Native([string]$command,[string]$distro=$script:sourceDistro,[switch]$RootUser){
    $nativeArgs=@('--distribution',$distro);if($RootUser){$nativeArgs+=@('--user','root')};$nativeArgs+=@('--exec','/bin/sh','-c',$command)
    $v=& wsl.exe @nativeArgs;if($LASTEXITCODE){throw "Private native fixture failed: $command"};($v -join "`n").TrimEnd()
}
function Run-TransferCli([string]$op,[string[]]$sources,[string]$dest,[string]$policy=''){
    $nativeArgs=@($op)+$sources+@($dest);if($policy){$nativeArgs+=@($policy)}
    $script:cliOutput=(& "$root\build\$Config\folderwing-cli.exe" @nativeArgs) -join "`n";$script:cliCode=$LASTEXITCODE
}
# Single clipboard transaction per observation; no external data is logged.
Add-Type @'
using System;using System.Runtime.InteropServices;
public static class CrossCutAdmission {
 [DllImport("user32.dll",SetLastError=true)]static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")]static extern bool CloseClipboard();
 [DllImport("user32.dll")]static extern IntPtr GetClipboardData(uint format);
 [DllImport("user32.dll")]static extern uint GetClipboardSequenceNumber();
 [DllImport("user32.dll")]static extern IntPtr GetClipboardOwner();
 [DllImport("user32.dll")]static extern IntPtr GetOpenClipboardWindow();
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern uint RegisterClipboardFormatW(string name);
 [DllImport("kernel32.dll")]static extern IntPtr GlobalLock(IntPtr h);
 [DllImport("kernel32.dll")]static extern bool GlobalUnlock(IntPtr h);
 [DllImport("kernel32.dll")]static extern UIntPtr GlobalSize(IntPtr h);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)]static extern uint DragQueryFileW(IntPtr h,uint index,System.Text.StringBuilder text,uint size);
 public sealed class Observation {public bool Opened,PathsExact,Move,Stable;public uint Sequence,After,Count;public int Error;public long Owner,Holder;}
 public static Observation Read(IntPtr owner,string[] expected){var o=new Observation{Sequence=GetClipboardSequenceNumber(),Owner=GetClipboardOwner().ToInt64(),Holder=GetOpenClipboardWindow().ToInt64()};if(!OpenClipboard(owner)){o.Error=Marshal.GetLastWin32Error();return o;}o.Opened=true;
  try{var h=GetClipboardData(15);o.Count=h!=IntPtr.Zero?DragQueryFileW(h,uint.MaxValue,null,0):0;o.PathsExact=h!=IntPtr.Zero&&o.Count==expected.Length;
   for(uint i=0;i<o.Count&&o.PathsExact;i++){uint n=DragQueryFileW(h,i,null,0);var b=new System.Text.StringBuilder((int)n+1);o.PathsExact=DragQueryFileW(h,i,b,n+1)==n&&string.Equals(b.ToString(),expected[i],StringComparison.Ordinal);}
   var effect=GetClipboardData(RegisterClipboardFormatW("Preferred DropEffect"));var p=effect!=IntPtr.Zero&&GlobalSize(effect).ToUInt64()>=4?GlobalLock(effect):IntPtr.Zero;if(p!=IntPtr.Zero){o.Move=Marshal.ReadInt32(p)==2;GlobalUnlock(effect);}
  }finally{CloseClipboard();}o.After=GetClipboardSequenceNumber();o.Stable=o.Sequence==o.After;return o;
 }
}
'@
function Apps {@(Get-Process folderwing-operations -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid})}
function Slot {$side=[FpUi]::FindWindowExW($main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero);[FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)}
function Result([int]$Ms=1000) {
    $slot=Slot
    if($slot -eq [IntPtr]::Zero){return ''}
    $clock=[Diagnostics.Stopwatch]::StartNew()
    $sentinel="openpilot-cross-result-$([guid]::NewGuid().ToString('N'))"
    $reset=$false
    for($attempt=0;$attempt -lt 5 -and -not $reset;$attempt++){
        Set-Clipboard $sentinel
        $reset=Wait-Until {(Get-Clipboard -Raw) -ceq $sentinel} ([Math]::Min(100,(Input-Remaining $clock $Ms)))
    }
    if(-not $reset){return ''}
    Chord-Acknowledged $slot 'Ctrl' 0x43 (Input-Remaining $clock $Ms)
    if(-not(Wait-Until {
        $text=Get-Clipboard -Raw
        -not[string]::IsNullOrEmpty($text) -and $text -cne $sentinel
    } (Input-Remaining $clock $Ms))){return ''}
    Get-Clipboard -Raw
}
function Prepare([bool]$move=$false,[int]$policy=0){
    Key $pane0 $(if($move){0x75}else{0x74}) 0
    $opened=Wait-Until {(Apps).Count -eq 1 -and (Slot) -ne [IntPtr]::Zero} 5000
    Check 'cross-distro transfer opens its normal Operations draft' $opened
    if(-not $opened){throw 'Cross-distro Operations input target did not open'}
    $ready=Wait-Until {$snapshot=Result;$script:lastDraftSnapshot=$snapshot;$match=[regex]::Match($snapshot,'Operation #(\d+)');if($snapshot -match 'Ready to start' -and $match.Success -and [uint64]$match.Groups[1].Value -gt $script:lastToken){$script:token=[uint64]$match.Groups[1].Value;return $true};$false} 5000
    Check 'transfer waits for an explicit Start' $ready
    if(-not $ready){throw "Cross-distro draft did not receive a new token after #$script:lastToken; last result: $script:lastDraftSnapshot"}
    $script:lastToken=$script:token;for($i=0;$i -lt $policy;$i++){Key (Slot) 0x27 20}
}
function Finish {
    $script:result=''
    Key (Slot) 0x0D 0
    Check 'cross-distro transfer reaches a retained terminal result' (Wait-Until {$snapshot=Result;if($snapshot.Contains("Operation #$script:token`r`n") -and $snapshot -match 'Completed|Finished with|Cancelled|Could not start'){$script:result=$snapshot;return $true};$false} 10000)
    $script:result
}
function Navigate([string]$path,[IntPtr]$pane=$script:pane0){Navigate-Verified $path $pane 7000}
$sourceDistro=(& wsl.exe --exec sh -c 'printf "%s" "$WSL_DISTRO_NAME"') -join '';if($LASTEXITCODE -or -not $sourceDistro){throw 'Default WSL distro is required'}
$linux="/tmp/openpilot-cross-ui-$PID";$target="/tmp/openpilot-cross-ui-$PID";$srcUnc="\\wsl.localhost\$sourceDistro$($linux.Replace('/','\'))";$dstUnc="\\wsl.localhost\$targetDistro$($target.Replace('/','\'))"
$scratch=Join-Path $env:TEMP "openpilot_cross_distro_$PID";New-Item -ItemType Directory -Force $scratch|Out-Null
try {
    Native "mkdir -p '$linux/main/tree/sub' '$linux/basic' '$linux/move' '$linux/merge/tree' '$linux/failure/tree' '$linux/cancel/many' '$linux/clipboard' '$linux/root'; printf payload >'$linux/main/tree/script'; chmod 754 '$linux/main/tree/script'; TZ=UTC touch -t 202311142213.20 '$linux/main/tree/script'; chmod 750 '$linux/main/tree/sub'; printf upper >'$linux/main/tree/Case'; printf lower >'$linux/main/tree/case'; printf literal >'$linux/main/tree/colon:name.txt'; ln -s missing '$linux/main/tree/broken'; ln -s /outside-do-not-follow '$linux/main/tree/outside'; printf new >'$linux/basic/only.txt'; printf move-data >'$linux/move/moved.bin'; printf new >'$linux/merge/tree/same'; printf extra >'$linux/merge/tree/extra'; printf good >'$linux/failure/tree/good'; mkfifo '$linux/failure/tree/pipe'; printf clip >'$linux/clipboard/clip.bin'; printf root-readable >'$linux/root/id.bin'; printf root-private >'$linux/root/denied.bin'"|Out-Null
    Native "chown 23456:23456 '$linux/root/id.bin'; chmod 4754 '$linux/root/id.bin'; chown 0:0 '$linux/root/denied.bin'; chmod 400 '$linux/root/denied.bin'" -RootUser|Out-Null
    Native "mkdir -p '$target/cli' '$target/back' '$target/mixed' '$target/ui' '$target/conflicts' '$target/move' '$target/merge/tree' '$target/failure' '$target/cancel' '$target/clipboard' '$target/root'; printf old >'$target/conflicts/only.txt'; printf old >'$target/merge/tree/same'" $targetDistro|Out-Null
    $loop='i=0; while [ "$i" -lt 250 ]; do dd if=/dev/zero of=''{0}/cancel/many/''$i.bin bs=65536 count=1 status=none; i=$((i+1)); done' -f $linux
    Native $loop|Out-Null
    Run-TransferCli cp @("$srcUnc\main\tree") "$dstUnc\cli"
    Check 'CLI copies a directory between actual WSL distros' ($cliCode -eq 0 -and (Native "cat '$target/cli/tree/script'" $targetDistro) -eq 'payload') $cliOutput
    Check 'native permissions and modification time survive the transfer' ((Native "stat -c '%a:%Y' '$target/cli/tree/script'" $targetDistro) -eq '754:1700000000')
    Check 'native directory permissions survive the transfer' ((Native "stat -c %a '$target/cli/tree/sub'" $targetDistro) -eq '750')
    Check 'case-distinct and Windows-reserved names remain literal Linux names' ((Native "cat '$target/cli/tree/Case'; cat '$target/cli/tree/case'; cat '$target/cli/tree/colon:name.txt'" $targetDistro) -eq 'upperlowerliteral')
    Check 'broken and absolute symlinks preserve literal targets without traversal' ((Native "readlink '$target/cli/tree/broken'; readlink '$target/cli/tree/outside'" $targetDistro) -eq "missing`n/outside-do-not-follow")
    Native "mkdir -p '$linux/back'"|Out-Null;Run-TransferCli cp @("$dstUnc\cli\tree") "$srcUnc\back"
    Check 'the same route works in the reverse direction' ($cliCode -eq 0 -and (Native "cat '$linux/back/tree/script'") -eq 'payload') $cliOutput
    Native "printf same-distro >'$target/local.bin'" $targetDistro|Out-Null
    Run-TransferCli cp @("$srcUnc\basic\only.txt","$dstUnc\local.bin") "$dstUnc\mixed"
    Check 'one job can mix remote and destination-distro sources' ($cliCode -eq 0 -and (Native "cat '$target/mixed/only.txt'; cat '$target/mixed/local.bin'" $targetDistro) -eq 'newsame-distro') $cliOutput
    Check 'a mixed job publishes a single final completion with combined totals' (([regex]::Matches($cliOutput,'(?m)^done\s')).Count -eq 1 -and $cliOutput -match 'done\s+2/2 items') $cliOutput
    Native "mkdir -p '$target/partial'" $targetDistro|Out-Null
    Run-TransferCli cp @("$srcUnc\basic\only.txt","$dstUnc\missing.bin") "$dstUnc\partial"
    Check 'a mixed-source failure keeps successful copies and reports the missing item' ($cliCode -ne 0 -and (Native "cat '$target/partial/only.txt'" $targetDistro) -eq 'new' -and $cliOutput -match 'errors 1') $cliOutput
    # The two distro mounts report different st_dev values for C:, but share filesystem ID and inode.
    $shared="$out\shared-$PID";New-Item -ItemType Directory -Force "$shared\tree\sub"|Out-Null;Set-Content "$shared\tree\keep.txt" 'keep'
    $sharedLinux="/mnt/c/Work/openpilot/build/cross_distro/shared-$PID"
    Run-TransferCli cp @("\\wsl.localhost\$sourceDistro$($sharedLinux.Replace('/','\'))\tree") "\\wsl.localhost\$targetDistro$($sharedLinux.Replace('/','\'))\tree\sub"
    Check 'shared Windows-drive aliases cannot copy a directory into itself across distros' ($cliCode -ne 0 -and @(Get-ChildItem "$shared\tree\sub").Count -eq 0) $cliOutput
    Remove-Item -Recurse -Force $shared
    Native "mkdir -p '$linux/engine'; printf new >'$linux/engine/before.bin'"|Out-Null
    Native "mkdir -p '$target/engine'; printf old >'$target/engine/before.bin'" $targetDistro|Out-Null
    & "$root\build\$Config\cross_tests.exe" "$srcUnc\engine\before.bin" "$dstUnc\engine" "\\wsl.localhost\$targetDistro-missing\tmp"
    Check 'native engine cancellation, unavailable endpoint and verified completion contracts pass' ($LASTEXITCODE -eq 0)
    $settings="$scratch\settings.ini";$base="[functions]`npreview.text = off`npreview.pdf = off`npreview.image = off`n"
    Set-Content -LiteralPath $settings -Value $base
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","--profile-out=$out\cross-profile.csv","$srcUnc\main","$dstUnc\ui")
    Check 'both distro panes load without starting Operations' (Wait-Until {(Window-Text (Pane-Edits $pane0)[0]) -eq "$srcUnc\main" -and (Window-Text (Pane-Edits $pane1)[0]) -eq "$dstUnc\ui" -and (Apps).Count -eq 0} 7000)
    Prepare;$result=Finish
    Check 'F5 copies through the normal Operations workflow' ($result -match 'Completed' -and (Native "cat '$target/ui/tree/script'" $targetDistro) -eq 'payload') $result
    Check 'cross-distro copy retains the original tree' ((Native "cat '$linux/main/tree/script'") -eq 'payload')
    Shot $out '01_cross_distro_completed'
    Check 'source navigates to the conflict fixture' (Navigate "$srcUnc\basic");Check 'destination navigates to the conflict fixture' (Navigate "$dstUnc\conflicts" $pane1)
    Prepare $false 1;$result=Finish
    Check 'Skip existing preserves destination data and reports the source item' ((Native "cat '$target/conflicts/only.txt'" $targetDistro) -eq 'old' -and $result -match '1 skipped' -and $result -match 'only.txt') $result
    Prepare;$result=Finish
    Check 'Keep both creates a numbered Linux sibling' ((Native "cat '$target/conflicts/only (2).txt'" $targetDistro) -eq 'new' -and $result -match 'Completed')
    Prepare $false 2;$result=Finish
    Check 'Replace commits the verified copy through the chosen policy' ((Native "cat '$target/conflicts/only.txt'" $targetDistro) -eq 'new' -and $result -match 'Completed')
    Check 'source navigates to the move fixture' (Navigate "$srcUnc\move");Check 'destination navigates to the move fixture' (Navigate "$dstUnc\move" $pane1)
    Prepare $true;$result=Finish
    Check 'F6 removes the source only after a verified destination copy' ((Native "test ! -e '$linux/move/moved.bin' && printf retained-destination") -eq 'retained-destination' -and (Native "cat '$target/move/moved.bin'" $targetDistro) -eq 'move-data' -and $result -match 'Completed') $result
    Check 'source navigates to the merge fixture' (Navigate "$srcUnc\merge");Check 'destination navigates to the merge fixture' (Navigate "$dstUnc\merge" $pane1)
    Prepare $true 1;$result=Finish
    Check 'merge move retains skipped children while completing other children' ((Native "cat '$linux/merge/tree/same'; test ! -e '$linux/merge/tree/extra' && printf moved-extra") -eq 'newmoved-extra' -and (Native "cat '$target/merge/tree/same'; cat '$target/merge/tree/extra'" $targetDistro) -eq 'oldextra' -and $result -match '1 skipped') $result
    Check 'source navigates to the partial-failure fixture' (Navigate "$srcUnc\failure");Check 'destination navigates to the partial-failure fixture' (Navigate "$dstUnc\failure" $pane1)
    Prepare $true;$result=Finish
    Check 'special nodes fail individually and successful moved files remain available' ($result -match 'Finished with errors' -and $result -match 'pipe' -and (Native "test -p '$linux/failure/tree/pipe' && printf preserved") -eq 'preserved' -and (Native "cat '$target/failure/tree/good'" $targetDistro) -eq 'good') $result
    Check 'source navigates to the cancellation fixture' (Navigate "$srcUnc\cancel");Check 'destination navigates to the cancellation fixture' (Navigate "$dstUnc\cancel" $pane1)
    Prepare $true;Key (Slot) 0x0D 0
    Check 'cross-distro cancellation fixture reaches running state' (Wait-Until {(Result) -match 'Running|Counting items'} 7000)
    Key (Slot) 0x0D 0
    Check 'Cancel produces a retained cancelled result' (Wait-Until {(Result) -match 'Cancelled'} 10000)
    $remaining=[int](Native "find '$linux/cancel/many' -type f | wc -l")
    $completed=[int](Native "if [ -d '$target/cancel/many' ]; then find '$target/cancel/many' -type f ! -name '.openpilot-xfer-*' | /bin/busybox wc -l; else printf 0; fi" $targetDistro)
    Check 'cancelled move loses no files and stops before completing the whole tree' ($remaining+$completed -ge 250 -and $remaining -gt 0) "remaining=$remaining completed=$completed"
    $remainingNames=@((Native "find '$linux/cancel/many' -type f -printf '%f\n'") -split "`n"|Where-Object {$_})
    $completedNames=@((Native "if [ -d '$target/cancel/many' ]; then find '$target/cancel/many' -type f ! -name '.openpilot-xfer-*' -exec /bin/busybox basename {} \; ; fi" $targetDistro) -split "`n"|Where-Object {$_})
    $union=@(($remainingNames+$completedNames)|Sort-Object -Unique);$expected=@(0..249|ForEach-Object {"$_.bin"}|Sort-Object)
    Check 'every original filename survives in the source or its verified destination' (($union -join ',') -eq ($expected -join ','))
    Check 'cooperative cancellation removes unfinished destination siblings' ((Native "find '$target/cancel' -name '.openpilot-xfer-*' | /bin/busybox wc -l" $targetDistro) -eq '0')
    Check 'source navigates to the clipboard fixture' (Navigate "$srcUnc\clipboard");Check 'destination navigates to the clipboard fixture' (Navigate "$dstUnc\clipboard" $pane1)
    Chord (Slot) @('Ctrl','Shift') 0x4F 20
    $cutClock=[Diagnostics.Stopwatch]::StartNew();$expectedCut=[string[]]@("$srcUnc\clipboard\clip.bin")
    $beforeCut=[CrossCutAdmission]::Read($main,$expectedCut);$oldToken=$script:lastToken
    Chord-Acknowledged $pane0 'Ctrl' 0x58 (Input-Remaining $cutClock 5000)
    $published=Wait-Until {$script:cutAccepted=[CrossCutAdmission]::Read($main,$expectedCut);$o=$script:cutAccepted;$o.Opened -and $o.PathsExact -and $o.Move -and $o.Stable -and $o.Sequence -ne $beforeCut.Sequence -and $o.Owner -eq $main.ToInt64()} (Input-Remaining $cutClock 5000)
    @{phase='cut-publication';accepted=$published;before=$beforeCut;after=$script:cutAccepted;old_operation_token=$oldToken;elapsed_ms=$cutClock.Elapsed.TotalMilliseconds}|ConvertTo-Json -Depth 5 -Compress|Add-Content "$out\cut-admission-$PID.jsonl"
    if(-not $published){throw 'Exact owned CF_HDROP/MOVE publication absent; Paste not submitted'}
    $atPaste=[CrossCutAdmission]::Read($main,$expectedCut)
    if(-not $atPaste.Opened -or -not $atPaste.PathsExact -or -not $atPaste.Move -or -not $atPaste.Stable -or $atPaste.Sequence -ne $script:cutAccepted.Sequence){throw 'Accepted Cut clipboard changed before Paste; no Paste submitted'}
    Chord-Acknowledged $pane1 'Ctrl' 0x56 (Input-Remaining $cutClock 5000)
    $ready=Wait-Until {
        $apps=Apps;$slot=Slot;if($apps.Count -ne 1 -or $slot -eq [IntPtr]::Zero -or -not [FpUi]::IsWindowVisible($slot)){return $false}
        [FpUi]::OwnedWindow($slot,$script:demoPid)
        $snapshot=Result ([Math]::Min(1000,(Input-Remaining $cutClock 5000)));$script:lastDraftSnapshot=$snapshot;$match=[regex]::Match($snapshot,'Operation #(\d+)')
        if($match.Success -and [uint64]$match.Groups[1].Value -gt $oldToken -and $snapshot -match 'Ready to start' -and $snapshot -match '(?m)^Move: Ready to start' -and $snapshot.Contains("Destination: $dstUnc\clipboard`r`n") -and $snapshot.Contains("Source: $($expectedCut[0])`r`n")){
            if($cutClock.ElapsedMilliseconds -ge 5000){return $false};$script:token=[uint64]$match.Groups[1].Value;$script:cutDraftHelper=@{pid=$apps[0].Id;started=$apps[0].StartTime;path=$apps[0].Path;slot=$slot.ToInt64()};return $true
        };$false
    } (Input-Remaining $cutClock 5000)
    Check 'cut/paste between distros opens a fresh exact move draft' $ready
    @{phase='move-draft';accepted=$ready;token=$script:token;old_token=$oldToken;helper=$script:cutDraftHelper;elapsed_ms=$cutClock.Elapsed.TotalMilliseconds;clipboard=$atPaste}|ConvertTo-Json -Depth 5 -Compress|Add-Content "$out\cut-admission-$PID.jsonl"
    if(-not $ready){throw 'New owned Move draft/path/token not ready within shared original5000ms; no Start submitted'}
    $script:lastToken=$script:token;$result=Finish
    Check 'cut/paste uses the verified cross-distro move route' ((Native "test ! -e '$linux/clipboard/clip.bin' && printf moved") -eq 'moved' -and (Native "cat '$target/clipboard/clip.bin'" $targetDistro) -eq 'clip') $result
    Check 'source navigates to the role fixture' (Navigate "$srcUnc\root");Check 'destination navigates to the role fixture' (Navigate "$dstUnc\root" $pane1)
    Key $pane0 0x24 20 # denied.bin first
    Prepare;$result=Finish
    Check 'default-user denial reports an error without automatic elevation' ($result -match 'Finished with errors' -and $result -match 'denied.bin' -and (Native "test ! -e '$target/root/denied.bin' && printf untouched" $targetDistro) -eq 'untouched') $result
    Check 'main cross-distro app closes cleanly' (Stop-Demo $p);$p=$null
    Set-Content -LiteralPath $settings -Value ($base+"[wsl.helper]`nuser = root`n")
    $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--settings=$settings","--profile-out=$out\cross-root-profile.csv","$srcUnc\root","$dstUnc\root")
    $script:lastToken=0;$script:token=0;$script:result=''
    Set-Clipboard 'waiting for root fixture selection'
    Send-Text $pane0 'id.bin'
    Check 'root fixture loads and id.bin is selected before requesting a transfer' (Wait-Until {
        Chord $pane0 'Ctrl' 0x43 0
        $selected=[Windows.Forms.Clipboard]::GetFileDropList();$selected.Count -eq 1 -and $selected[0] -eq "$srcUnc\root\id.bin"
    } 7000)
    Prepare;$result=Finish
    Check 'explicit root on both endpoints preserves numeric ownership before mode bits' ((Native "stat -c '%u:%g:%a' '$target/root/id.bin'" $targetDistro -RootUser) -eq '23456:23456:4754' -and $result -match 'Completed') $result
    Check 'root transfer app closes cleanly' (Stop-Demo $p);$p=$null
    foreach($name in @('cross-profile.csv','cross-root-profile.csv')){
        $report=Get-Content "$out\$name" -Raw
        Check "$name has no function lifetime violations" ($report -match 'metric,fn_violations,,,,0,')
        $m=[regex]::Match($report,'zone,ui.dispatch,\d+,[\d.]+,([\d.]+),,');Check "$name keeps UI dispatch below 16 ms" ($m.Success -and [double]$m.Groups[1].Value -lt 16) $m.Value
    }
} finally {
    if($p){Stop-Demo $p|Out-Null}
    Native "rm -rf '$linux'" -RootUser|Out-Null
    Native "rm -rf '$target'" $targetDistro -RootUser|Out-Null
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
