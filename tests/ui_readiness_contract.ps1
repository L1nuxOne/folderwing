<# Negative and positive readiness oracles against hidden, independently owned native windows. #>
param([string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
Add-Type @"
using System;using System.Runtime.InteropServices;
public static class ReadinessProbe {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct WC {public uint style;public IntPtr proc;public int cls,win;public IntPtr instance,icon,cursor,brush;public string menu,name;}
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
 [DllImport("kernel32.dll",CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr h,string name);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern ushort RegisterClassW(ref WC wc);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr param);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool SetPropW(IntPtr h,string name,IntPtr value);
 [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
 public static IntPtr Make(string cls,string title,IntPtr parent){return CreateWindowExW(0,cls,title,parent==IntPtr.Zero?0u:0x40000000u,0,0,100,40,parent,IntPtr.Zero,GetModuleHandleW(null),IntPtr.Zero);}
 public static void Register(){var wc=new WC{proc=GetProcAddress(GetModuleHandleW("user32.dll"),"DefWindowProcW"),instance=GetModuleHandleW(null),name="OpenPilotFolderCompare"};if(RegisterClassW(ref wc)==0)throw new Exception("Cannot register owned fixture class");}
}
"@
$script:passed=0
function Check([string]$name,[bool]$ok){if(-not $ok){throw "Failed: $name"};$script:passed++;"  ok    $name"}
$script:demoPid=[uint32]$PID;$dialog=[IntPtr]::Zero;$script:main=[IntPtr]::Zero
$root=if($EvidenceRoot){$EvidenceRoot}else{Join-Path $env:TEMP ('openpilot-readiness-'+[guid]::NewGuid().ToString('N'))}
New-Item -ItemType Directory -Path $root|Out-Null
$script:readinessEvidence=Join-Path $root 'expected-rejections.jsonl'
try{
 [ReadinessProbe]::Register()
 $script:main=[ReadinessProbe]::Make('STATIC','owned readiness fixture',[IntPtr]::Zero)
 $script:pane0=[ReadinessProbe]::Make('STATIC','left',$main);$script:pane1=[ReadinessProbe]::Make('STATIC','right',$main)
 $edit=[ReadinessProbe]::Make('EDIT','C:\owned',$pane0)
 Check 'hidden native fixture is live' ($main -ne [IntPtr]::Zero -and $edit -ne [IntPtr]::Zero)
 $state=Get-UiReadinessState 'absent'
 Check 'absent dialog is distinct from a busy worker' ($state.comparison.phase -eq 'window-absent')
 $dialog=[ReadinessProbe]::Make('OpenPilotFolderCompare','Compare folders',[IntPtr]::Zero)
 Check 'dialog lookup matches owned PID and title' ((Find-OwnedWindow 'OpenPilotFolderCompare' 'Compare folders') -eq $dialog)
 Check 'wrong title cannot match a dialog' ((Find-OwnedWindow 'OpenPilotFolderCompare' 'wrong') -eq [IntPtr]::Zero)
 $script:demoPid=[uint32]($PID+1)
 Check 'another owner cannot satisfy dialog readiness' ((Find-OwnedWindow 'OpenPilotFolderCompare') -eq [IntPtr]::Zero)
 $script:demoPid=[uint32]$PID
 Check 'building window is distinct from completed worker' ((Get-UiReadinessState 'building').comparison.phase -eq 'window-building')
 [ReadinessProbe]::SetPropW($dialog,'FolderCompare.Busy',[IntPtr]1)|Out-Null
 Check 'busy worker remains explicit' ((Get-UiReadinessState 'busy').comparison.phase -eq 'worker-busy')
 [ReadinessProbe]::SetPropW($dialog,'FolderCompare.Busy',[IntPtr]::Zero)|Out-Null
 [ReadinessProbe]::SetPropW($dialog,'FolderCompare.Done',[IntPtr]1)|Out-Null
 [ReadinessProbe]::SetPropW($dialog,'FolderCompare.Error',[IntPtr]5)|Out-Null
 $state=Get-UiReadinessState 'finished error'
 Check 'finished error cannot disappear into a readiness timeout' ($state.comparison.phase -eq 'finished' -and $state.comparison.Error -eq 5)
 Set-UiSettings (Join-Path $root 'settings.ini') '[functions]'
 Check 'old revision cannot acknowledge settings write' (-not(Test-UiSettingsApplied))
 [ReadinessProbe]::SetPropW($main,'Settings.Revision',[IntPtr]1)|Out-Null
 Check 'new settings revision is acknowledged' (Test-UiSettingsApplied)
 Set-UiSettings (Join-Path $root 'settings.ini') '[functions]'
 $caught=$false;try{Wait-UiSettings 20}catch{$caught=$_.Exception.Message -match 'within 20ms'}
 Check 'settings deadline fails with state evidence' ($caught -and (Test-Path $script:readinessEvidence))
 function Navigate-Verified {throw 'injected navigation error 5'}
 $caught=$false;try{Navigate-Ready 'C:\owned' $pane0 20}catch{$caught=$_.Exception.Message -eq 'injected navigation error 5'}
 Check 'navigation failure keeps its original cause' $caught
 $rows=@(Get-Content $script:readinessEvidence|ForEach-Object{$_|ConvertFrom-Json})
 Check 'failure evidence includes owned address and request state' ($rows[-1].panes[0].address -ceq 'C:\owned' -and $rows[-1].panes[0].Request -eq 0)
 Check 'clipboard diagnostics avoid copying clipboard contents' ($rows[-1].clipboard.PSObject.Properties.Name -notcontains 'text')
 Check 'unavailable text is diagnostic evidence rather than an observer exception' ((Read-ReadinessText ([IntPtr]::Zero)) -like '<unavailable:*')
 [ReadinessProbe]::DestroyWindow($dialog)|Out-Null;$dialog=[IntPtr]::Zero
 Check 'a destroyed comparison window cannot break close polling' ((Find-OwnedWindow 'OpenPilotFolderCompare' 'Compare folders') -eq [IntPtr]::Zero)
 [ReadinessProbe]::DestroyWindow($main)|Out-Null
 Check 'closed app can be observed without touching destroyed edits' ((Get-UiReadinessState 'closed').panes.Count -eq 0)
 "$script:passed passed, 0 failed; expected rejections retained at $root"
}finally{
 if($dialog -ne [IntPtr]::Zero){[ReadinessProbe]::DestroyWindow($dialog)|Out-Null}
 if([FpReadiness]::IsWindow($main)){[ReadinessProbe]::DestroyWindow($main)|Out-Null}
}
