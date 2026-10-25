<# Focused ordinary-host Explorer-menu/Properties/first-Close acceptance.
   Can be dot-sourced with -DefinitionsOnly by the standing smoke. #>
param([string]$Config='release',[string]$EvidenceRoot='',[switch]$DefinitionsOnly)
$ErrorActionPreference='Stop'
if(-not (Get-Command Start-Demo -CommandType Function -ErrorAction SilentlyContinue)){. "$PSScriptRoot\uilib.ps1"}
if(-not ('OwnedSettingsInput' -as [type])){Add-Type -Path "$PSScriptRoot\settings_ui_native.cs"}
if(-not ('ShellUiFacts' -as [type])) { Add-Type @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class ShellUiFacts {
 delegate bool Visitor(IntPtr h,IntPtr data);
 [DllImport("user32.dll")] static extern bool EnumWindows(Visitor v,IntPtr data);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr h,StringBuilder text,int size);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h,StringBuilder text,int size);
 [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr h,uint flags);
 [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr h,uint relation);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 public sealed class Window {public long Hwnd,Owner,Root;public uint Pid,Thread;public bool Visible;public string Class,Title;}
 public static Window[] Windows(uint host,uint[] helpers) {
  var a=new List<Window>();EnumWindows((h,data)=>{uint pid;uint tid=GetWindowThreadProcessId(h,out pid);var c=new StringBuilder(128);GetClassNameW(h,c,128);
   // Global menu identities are useful for discovery; never read foreign titles.
   if(pid==host||Array.IndexOf(helpers,pid)>=0||c.ToString()=="#32768") {var title=new StringBuilder(256);if(pid==host||Array.IndexOf(helpers,pid)>=0)GetWindowTextW(h,title,256);a.Add(new Window{Hwnd=h.ToInt64(),Owner=GetWindow(h,4).ToInt64(),Root=GetAncestor(h,3).ToInt64(),Pid=pid,Thread=tid,Visible=IsWindowVisible(h),Class=c.ToString(),Title=title.ToString()});}return true;},IntPtr.Zero);return a.ToArray();
 }
}
'@ }
function Write-ShellPhase([string]$Phase) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$responsive=$false
    try{[FpUi]::OwnedWindow($script:main,$script:demoPid);$null=[FpUi]::Input($script:main,0,0,[IntPtr]::Zero,150);$responsive=$true}catch{}
    $helpers=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$script:demoPid"|ForEach-Object{@{pid=$_.ProcessId;name=$_.Name;started=$_.CreationDate.ToUniversalTime().ToString('o')}})
    $facts=@{phase=$Phase;utc=[DateTime]::UtcNow.ToString('o');host=$script:demoPid;main=$script:main.ToInt64();responsive=$responsive;probe_ms=$clock.Elapsed.TotalMilliseconds;foreground=[ShellUiFacts]::GetForegroundWindow().ToInt64();windows=[ShellUiFacts]::Windows($script:demoPid,[uint32[]]@($helpers|ForEach-Object{$_.pid}));helpers=$helpers}
    $facts|ConvertTo-Json -Depth 8 -Compress|Add-Content -LiteralPath $script:shellPhaseFile
}
function Open-ShellOwnedMenu([string]$Class,[int]$Deadline) {
    $clock=[Diagnostics.Stopwatch]::StartNew();$h=[IntPtr]::Zero
    do {
        $h=Find-DemoMenu $Class
        if($h -ne [IntPtr]::Zero){break};Start-Sleep -Milliseconds 25
    }while($clock.ElapsedMilliseconds -lt $Deadline)
    $remaining=$Deadline-[int]$clock.ElapsedMilliseconds
    if($h -eq [IntPtr]::Zero -or $remaining -le 0){return $null}
    [FpUi]::OwnedWindow($h,$script:demoPid)
    $hm=[IntPtr][int64][FpUi]::Input($h,0x1E1,0,[IntPtr]::Zero,[uint32]$remaining)
    $items=@(for($i=0;$i -lt [FpUi]::GetMenuItemCount($hm);$i++){$text=[Text.StringBuilder]::new(256);$null=[FpUi]::GetMenuStringW($hm,[uint32]$i,$text,256,0x400);if([FpUi]::GetMenuState($hm,[uint32]$i,0x400)-band 0x800){'-'}else{($text.ToString()-split "`t")[0]-replace '&',''}})
    if($clock.ElapsedMilliseconds -ge $Deadline){return $null}
    @{phase='menu-discovered';hwnd=$h.ToInt64();pid=$script:demoPid;hmenu=$hm.ToInt64();class=$Class;visible=[FpUi]::IsWindowVisible($h);elapsed_ms=$clock.Elapsed.TotalMilliseconds;items=$items}|ConvertTo-Json -Depth 6 -Compress|Add-Content -LiteralPath $script:shellPhaseFile
    [pscustomobject]@{Window=$h;Items=$items;Class=$Class;Menu=$hm}
}
function Close-ShellOwnedMenu($Menu) {
    if(-not $Menu){throw 'No owned menu was observed; Escape cannot be credited'}
    [FpUi]::OwnedWindow($Menu.Window,$script:demoPid)
    Post $Menu.Window 0x100 0x1B (Key-LParam 0x1B)
    $closed=Wait-Until {-not [FpUi]::IsWindowVisible($Menu.Window)} 2000
    Check 'observed native menu closes after actual Escape' $closed
    if(-not $closed){Write-ShellPhase 'escape-failed';throw 'Dependent inputs stopped: observed menu did not close'}
}
function Invoke-ShellExplorerChecks {
    Write-ShellPhase 'before-app-menu'
    Post $script:pane0 0x7B $script:pane0 -1
    $menu=Open-ShellOwnedMenu 'OpenPilotMenu' 3000
    Check 'owned app menu opens' ($null -ne $menu)
    if(-not $menu){Write-ShellPhase 'app-menu-missing';throw 'Dependent inputs stopped: app menu absent'}
    $more="More from Windows$([char]0x2026)"
    $picked=Pick-Menu $menu $more
    Check 'More from Windows is picked on the owned item' $picked
    if(-not $picked){Write-ShellPhase 'more-not-picked';throw 'Dependent inputs stopped: More was not picked'}
    $native=Open-ShellOwnedMenu '#32768' 15000
    $valid=$null -ne $native -and $native.Items -contains 'Properties' -and -not($native.Items -contains 'Copy path')
    Check 'More from Windows exposes the owned native Explorer menu' $valid
    Write-ShellPhase $(if($valid){'native-menu-ready'}else{'native-menu-missing'})
    if(-not $valid){throw 'Dependent inputs stopped: native menu unavailable within15000ms; no Escape/next-menu/Properties queued'}
    Close-ShellOwnedMenu $native
    Post $script:pane0 0x7B $script:pane0 -1
    $next=Open-ShellOwnedMenu 'OpenPilotMenu' 3000
    Check 'next app menu opens after native dismissal' ($null -ne $next)
    if(-not $next){Write-ShellPhase 'next-menu-missing';throw 'Dependent inputs stopped: next menu absent'}
    Close-ShellOwnedMenu $next
    Write-ShellPhase 'menus-completed'
}
function Invoke-ShellPropertiesCheck([string]$ExpectedTitle,[string]$ExpectedPath) {
    if(-not $ExpectedPath){throw 'Properties requires an explicit owned intended item before any key admission'}
    $clock=[Diagnostics.Stopwatch]::StartNew();$custody=$null
    Write-ShellPhase 'before-independent-properties'
    try{
    [OwnedSettingsInput]::InputFactsFile=$script:shellPhaseFile
    $activation=[OwnedSettingsInput]::ActivatePaneFooter($script:main,$script:pane0,$script:demoPid,($script:shellPhaseFile+'.properties-before.bmp'),$script:shellPhaseFile)
    if($activation.PointerRestore -ne 'restored'){throw 'Properties activation did not restore its own pointer; no keyboard command submitted'}
    $custody=[OwnedSettingsInput+ClipboardCustody]::new($script:main)
    [OwnedSettingsInput]::Seed($script:main,'owned-properties-selection-sentinel')
    $before=[OwnedSettingsInput]::ObserveFiles($script:main,[string[]]@($ExpectedPath))
    $copy=[OwnedSettingsInput]::Key($script:main,$script:pane0,$script:demoPid,67,[ushort[]]@(17))
    $selected=Wait-Until {$script:propertiesSelection=[OwnedSettingsInput]::ObserveFiles($script:main,[string[]]@($ExpectedPath));$v=$script:propertiesSelection;$v.Opened -and $v.Exact -and $v.Stable -and $v.Sequence -ne $before.Sequence -and $v.Owner -eq $script:main.ToInt64() -and $clock.ElapsedMilliseconds -lt 5000} ([Math]::Max(0,5000-[int]$clock.ElapsedMilliseconds))
    @{phase='properties-item-admission';accepted=$selected;expected_owned_path=$ExpectedPath;clipboard=$script:propertiesSelection;copy_transaction=$copy;elapsed_ms=$clock.Elapsed.TotalMilliseconds}|ConvertTo-Json -Depth 7 -Compress|Add-Content $script:shellPhaseFile
    if(-not $selected){throw 'Intended Properties item was not acknowledged by ordinary Copy; no Alt+Enter submitted'}
    if($clock.ElapsedMilliseconds -ge 5000){throw 'Properties admission consumed original5000ms deadline; no Alt+Enter submitted'}
    $key=[OwnedSettingsInput]::Key($script:main,$script:pane0,$script:demoPid,13,[ushort[]]@(18))
    @{phase='properties-key-submitted';transaction=$key;elapsed_ms=$clock.Elapsed.TotalMilliseconds;foreground=[OwnedSettingsInput]::GetForegroundWindow().ToInt64();dispatch='ordinary OS input through normal host message loop; invocation still requires dialog/profile evidence'}|ConvertTo-Json -Depth 5 -Compress|Add-Content $script:shellPhaseFile
    $script:ownedProperties=[IntPtr]::Zero
    $found=Wait-Until {
        $childIds=[uint32[]]@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$script:demoPid"|ForEach-Object{$_.ProcessId})
        $matches=@([ShellUiFacts]::Windows($script:demoPid,$childIds)|Where-Object{($_.Pid -eq $script:demoPid -or $childIds -contains $_.Pid) -and $_.Class -eq '#32770' -and $_.Visible -and $_.Title -ceq $ExpectedTitle})
        if($matches.Count -eq 1){$script:ownedProperties=[IntPtr]$matches[0].Hwnd;return $true};$false
    } ([Math]::Max(0,5000-[int]$clock.ElapsedMilliseconds))
    $found=$found -and $clock.ElapsedMilliseconds -lt 5000
    Check 'independent Alt+Enter opens Properties for the owned item' $found
    Write-ShellPhase $(if($found){'properties-ready'}else{'properties-missing'})
    if(-not $found){throw 'Properties was not observed; no unowned window will be closed'}
    $propsPid=[uint32]0;$null=[FpUi]::GetWindowThreadProcessId($script:ownedProperties,[ref]$propsPid)
    if($propsPid -ne $script:demoPid -and -not(@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$script:demoPid"|ForEach-Object{$_.ProcessId}) -contains $propsPid)){throw 'Properties helper ownership changed before close'}
    [FpUi]::OwnedWindow($script:ownedProperties,$propsPid)
    Post $script:ownedProperties 0x10 0
    $gone=Wait-Until {-not [FpUi]::IsWindowVisible($script:ownedProperties)} 2000
    Check 'owned Properties dialog closes' $gone
    if(-not $gone){throw 'Owned Properties dialog did not close'}
    }finally{if($custody){$custody.Dispose()}}
}
if($DefinitionsOnly){return}
$root=Split-Path $PSScriptRoot -Parent
if(-not $EvidenceRoot){$EvidenceRoot=Join-Path $root "build\shell_ui\$([Guid]::NewGuid().ToString('N'))"}
New-Item -ItemType Directory -Path $EvidenceRoot -ErrorAction Stop|Out-Null
$script:shellPhaseFile=Join-Path $EvidenceRoot 'phases.jsonl'
$left=Join-Path $EvidenceRoot 'left';$right=Join-Path $EvidenceRoot 'right'
New-Item -ItemType Directory -Path "$left\made",$right|Out-Null
$script:passed=0;$script:failed=0
function Check([string]$what,[bool]$ok,[string]$detail=''){if($ok){"  ok    $what";$script:passed++}else{"  FAIL  $what $detail";$script:failed++}}
foreach($case in @('native-menu','independent-properties')) {
    $p=$null;$started=$null
    try {
        $p=Start-Demo "$root\build\$Config\folderwing.exe" @("--profile-out=$EvidenceRoot\$case.csv",$left,$right)
        $started=$p.StartTime
        @{case=$case;pid=$p.Id;started=$started.ToUniversalTime().ToString('o');exe=$p.Path;main=$script:main.ToInt64()}|ConvertTo-Json -Compress|Add-Content "$EvidenceRoot\processes.jsonl"
        if(-not(Wait-Until {[FpUi]::GetPropW($script:pane0,'Address.Loading') -eq [IntPtr]::Zero} 3000)){throw 'Owned folder listing was not ready'}
        if($case -eq 'native-menu'){Invoke-ShellExplorerChecks}else{Invoke-ShellPropertiesCheck 'made Properties' "$left\made"}
    }catch{
        $cause=$_.Exception.GetBaseException();$code=$null;if($cause -is [ComponentModel.Win32Exception]){$code=$cause.NativeErrorCode}
        @{phase='caught-exception';case=$case;type=$cause.GetType().FullName;message=$cause.Message;hresult=$cause.HResult;win32_error=$code}|ConvertTo-Json -Compress|Add-Content $script:shellPhaseFile
        Check "$case stops at its first failing phase" $false $_.Exception.Message
        if($p){Write-ShellPhase "$case-error"}
    }finally{
        if($p){
            $p.Refresh();$clock=[Diagnostics.Stopwatch]::StartNew()
            if(-not $p.HasExited){$null=$p.CloseMainWindow()}
            $closed=$p.HasExited -or $p.WaitForExit([Math]::Max(0,3000-[int]$clock.ElapsedMilliseconds))
            Check "$case first Close completes within3000ms" $closed
            @{phase='first-close';case=$case;pid=$p.Id;started=$started.ToUniversalTime().ToString('o');completed=$closed;elapsed_ms=$clock.Elapsed.TotalMilliseconds;budget_ms=3000}|ConvertTo-Json -Compress|Add-Content -LiteralPath $script:shellPhaseFile
            if(-not $closed){
                Write-ShellPhase "$case-close-timeout"
                $actual=Get-Process -Id $p.Id -ErrorAction SilentlyContinue
                if(-not $actual -or $actual.StartTime -ne $started -or $actual.Path -cne "$root\build\$Config\folderwing.exe"){throw 'Owned process cleanup identity mismatch'}
                $actual.Kill();$actual.WaitForExit();@{case=$case;pid=$actual.Id;started=$started.ToUniversalTime().ToString('o');reason='Only exact owned host killed after failed first Close3000ms; fixture/profile retained'}|ConvertTo-Json -Compress|Add-Content "$EvidenceRoot\cleanup.jsonl"
            }
        }
    }
}
"$script:passed passed, $script:failed failed"
if($script:failed){exit 1}
