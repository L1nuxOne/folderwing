<# N01/N14 acceptance: owned literal NTFS siblings, actual CLI and real UI copy paths. #>
param([string]$Config='release',[string]$EvidenceRoot='')
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_readiness.ps1"
. "$PSScriptRoot\operations_contract.ps1"
$root=Split-Path $PSScriptRoot -Parent;$release=Join-Path $root "build\$Config"
$token=[guid]::NewGuid().ToString('N');$scratch=Join-Path $env:TEMP "openpilot-operands-$token"
$evidence=if($EvidenceRoot){$EvidenceRoot}else{Join-Path $root "build\operand_identity\$token"}
New-Item -ItemType Directory $scratch,$evidence|Out-Null
$script:readinessEvidence=Join-Path $evidence 'readiness.jsonl'
$passed=0;$p=$null;$success=$false
Add-Type @'
using System;using System.IO;using System.Text;using System.Runtime.InteropServices;
public static class LiteralOperand {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr CreateFileW(string p,uint access,uint share,IntPtr sa,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteFile(IntPtr h,byte[] b,uint n,out uint done,IntPtr ov);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool ReadFile(IntPtr h,byte[] b,uint n,out uint done,IntPtr ov);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("user32.dll")] static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll")] static extern bool CloseClipboard();
 [DllImport("user32.dll")] static extern IntPtr GetClipboardData(uint format);
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)] static extern uint DragQueryFileW(IntPtr h,uint i,StringBuilder text,uint chars);
 static IntPtr Open(string path,uint access,uint creation) {
  var h=CreateFileW("\\\\?\\"+path,access,7,IntPtr.Zero,creation,0,IntPtr.Zero);
  if(h==new IntPtr(-1))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),path);return h;
 }
 public static void Put(string path,string text) {
  var h=Open(path,0x40000000,1);try {var b=Encoding.UTF8.GetBytes(text);uint n;if(!WriteFile(h,b,(uint)b.Length,out n,IntPtr.Zero)||n!=b.Length)throw new Exception("Short fixture write");}finally{CloseHandle(h);}
 }
 public static string Read(string path) {
  var h=Open(path,0x80000000,3);try {var b=new byte[512];uint n;if(!ReadFile(h,b,512,out n,IntPtr.Zero))throw new Exception("Fixture read failed");return Encoding.UTF8.GetString(b,0,(int)n);}finally{CloseHandle(h);}
 }
 public static string DropPath() {
  if(!OpenClipboard(IntPtr.Zero))return null;try {var h=GetClipboardData(15);if(h==IntPtr.Zero||DragQueryFileW(h,0xffffffff,null,0)!=1)return null;var s=new StringBuilder(4096);return DragQueryFileW(h,0,s,4096)>0?s.ToString():null;}finally{CloseClipboard();}
 }
}
'@
function Check([string]$label,[bool]$ok){if(-not $ok){Write-UiReadinessFailure $label;throw "FAIL $label"};$script:passed++;Write-Output "PASS $label"}
function Run-Cli([string]$tag,[string[]]$arguments){
 $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $release 'folderwing-cli.exe'));$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
 foreach($a in $arguments){$start.ArgumentList.Add($a)}
 $process=[Diagnostics.Process]::Start($start);$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
 if(-not $process.WaitForExit(30000)){$process.Kill();$process.WaitForExit();throw 'Owned CLI exceeded30000ms'}
 $output=$stdout.GetAwaiter().GetResult();$errorText=$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
 [IO.File]::WriteAllText((Join-Path $evidence "$tag.log"),$output);[IO.File]::WriteAllText((Join-Path $evidence "$tag.stderr.log"),$errorText)
 @{arguments=$arguments;exit=$code}|ConvertTo-Json -Depth 3|Set-Content (Join-Path $evidence "$tag.json")
 [pscustomobject]@{Exit=$code;Text=$output}
}
function Slot {
 $side=[FpUi]::FindWindowExW($script:main,[IntPtr]::Zero,'OpenPilotSide',[IntPtr]::Zero)
 [FpUi]::FindWindowExW($side,[IntPtr]::Zero,'OpenPilotSideSlot',[IntPtr]::Zero)
}
function Operations-Result {
 $slot=Slot;if($slot -eq [IntPtr]::Zero){return ''}
 $sentinel="operand-result-$([guid]::NewGuid().ToString('N'))";Set-Clipboard $sentinel
 Chord-Acknowledged $slot 'Ctrl' 0x43 1000
 if(-not(Wait-Until {Test-FreshClipboard (Get-Clipboard -Raw) $sentinel} 1000)){return ''}
 Get-Clipboard -Raw
}
try {
 $left=Join-Path $scratch 'cli-source';$dest=Join-Path $scratch 'cli-dest';New-Item -ItemType Directory $left,$dest|Out-Null
 [LiteralOperand]::Put("$left\report",'plain');[LiteralOperand]::Put("$left\report.",'dot');[LiteralOperand]::Put("$left\report ",'space')
 foreach($suffix in @('.',' ')){
  foreach($prefix in @('','\\?\')){
   $tag=if($suffix -eq '.'){'dot'}else{'space'};if($prefix){$tag+='-extended'}
   $r=Run-Cli "cli-$tag" @('cp',"$prefix$left\report$suffix",$dest)
   Check "$tag CLI refuses before submission" ($r.Exit -eq 2 -and $r.Text -match 'operation not submitted' -and $r.Text -match '\(123\)')
   Check "$tag leaves both sibling identities readable with exact bytes" (([LiteralOperand]::Read("$left\report")) -ceq 'plain' -and ([LiteralOperand]::Read("$left\report.")) -ceq 'dot' -and ([LiteralOperand]::Read("$left\report ")) -ceq 'space')
   Check "$tag creates no destination entries" (@([IO.Directory]::EnumerateFileSystemEntries($dest)).Count -eq 0)
  }
 }
 $sourcePaths=@();for($i=0;$i -lt 255;$i++){$file="$left\capacity-$i";[LiteralOperand]::Put($file,"source-$i");$sourcePaths+=,$file}
 $wrong=Join-Path $scratch 'source-256';New-Item -ItemType Directory $wrong|Out-Null;[LiteralOperand]::Put("$wrong\sentinel",'keep')
 foreach($op in @('cp','mv','rm','recycle')){
  $items=if($op -in @('cp','mv')){@($sourcePaths)+@($wrong,$dest)}else{@($sourcePaths)+@("$left\report",$wrong)}
  $r=Run-Cli "overflow-$op" (@($op,'--skip')+$items+@('--fail'))
  Check "$op overflow refuses explicitly" ($r.Exit -eq 2 -and $r.Text -match 'too many operands.*no operation submitted')
  Check "$op overflow preserves sources and intended/wrong destination namespaces" (@([IO.Directory]::EnumerateFileSystemEntries($dest)).Count -eq 0 -and @([IO.Directory]::EnumerateFileSystemEntries($wrong)).Count -eq 1 -and ([LiteralOperand]::Read("$wrong\sentinel")) -ceq 'keep')
  for($i=0;$i -lt 255;$i++){if(([LiteralOperand]::Read($sourcePaths[$i])) -cne "source-$i"){throw 'Overflow changed a source'}}
 }
 $r=Run-Cli 'capacity-copy' (@('cp','--fail')+$sourcePaths+@($dest))
 Check 'exact capacity uses the actual final destination' ($r.Exit -eq 0 -and @([IO.Directory]::EnumerateFiles($dest)).Count -eq 255 -and @([IO.Directory]::EnumerateFiles($wrong)).Count -eq 1)
 for($i=0;$i -lt 255;$i++){Check "capacity copy preserves source and copies payload $i" (([LiteralOperand]::Read($sourcePaths[$i])) -ceq "source-$i" -and ([LiteralOperand]::Read("$dest\capacity-$i")) -ceq "source-$i")}
 foreach($enabled in @($true,$false)){
  foreach($suffix in @('','.',' ')){
   $id=([guid]::NewGuid().ToString('N'));$source=Join-Path $scratch "ui-$id";$target=Join-Path $scratch "out-$id";New-Item -ItemType Directory $source,$target|Out-Null
   [LiteralOperand]::Put("$source\report",'plain');if($suffix){[LiteralOperand]::Put("$source\report$suffix",'literal')}
   $settings=Join-Path $scratch "$id.ini";$flag=if($enabled){'on'}else{'off'}
   Set-Content $settings "[functions]`noperations.show = $flag`npreview.text = off`npreview.pdf = off`npreview.image = off"
   $p=Start-Demo (Join-Path $release 'folderwing.exe') @("--settings=$settings",$source,$target)
   Key $script:pane0 0x23 0 # End: literal sibling follows its unsuffixed control.
   Chord-Acknowledged $script:pane0 'Ctrl' 0x43 1000
   Check "UI $flag captured exact selected operand" (Wait-Until {([LiteralOperand]::DropPath()) -ceq "$source\report$suffix"} 1500)
   if($enabled){
    Key $script:pane0 0x74 0
    Check 'owned Operations target opens' (Wait-Until {(Slot) -ne [IntPtr]::Zero -and @(Get-Process folderwing-operations -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 1} 3000)
    $script:ready='';Check 'exact operand awaits reviewed Start' (Wait-Until {$script:ready=Operations-Result;$script:ready -match 'Ready to start' -and $script:ready.Contains("$source\report$suffix")} 3000)
    $match=[regex]::Match($script:ready,'Operation #(\d+)');Check 'ready result has an operation identity' $match.Success;$jobToken=$match.Groups[1].Value
    Key (Slot) 13 0;$script:terminal=''
    $expected=if($suffix){'Could not start'}else{'Completed'}
    Check "UI $flag reports $expected for this operation" (Wait-Until {$script:terminal=Operations-Result;$script:terminal.Contains("Operation #$jobToken`r`n") -and $script:terminal -match $expected} 10000)
    $script:terminal|Set-Content (Join-Path $evidence "$id-result.txt")
   }else{
    $clock=[Diagnostics.Stopwatch]::StartNew();$task=Start-ModalKey $script:pane0 0x74 8000
    try {
     Check 'direct copy has its owned confirmation' (Wait-Until {($script:dialog=Find-OwnedWindow '#32770' 'Copy') -ne [IntPtr]::Zero} 3000)
     Post $script:dialog $WM_COMMAND 1
    }finally{Complete-ModalKey $task (Input-Remaining $clock 8000)}
    Check 'disabled Operations creates no helper' (@(Get-Process folderwing-operations -ErrorAction SilentlyContinue|Where-Object {$_.Parent.Id -eq $script:demoPid}).Count -eq 0)
    if(-not $suffix){Check 'direct ordinary copy finishes' (Wait-Until {[IO.File]::Exists("$target\report")} 10000)}
   }
   Check 'owned UI closes and drains before namespace inspection' (Stop-Demo $p);$p=$null
   Check 'plain source bytes survive UI copy/refusal' (([LiteralOperand]::Read("$source\report")) -ceq 'plain')
   if($suffix){
    Check 'literal source bytes survive UI refusal' (([LiteralOperand]::Read("$source\report$suffix")) -ceq 'literal')
    Check 'UI refusal creates no destination or staging entries' (@([IO.Directory]::EnumerateFileSystemEntries($target)).Count -eq 0)
   }else{Check 'ordinary UI copy has exact destination bytes' (([LiteralOperand]::Read("$target\report")) -ceq 'plain')}
  }
 }
 $success=$true
}finally{
 if($p){Stop-Demo $p|Out-Null}
 @{passed=$passed;success=$success;fixture=$scratch;owner_token=$token;cleanup='retained for review';automatic_retries=0}|ConvertTo-Json|Set-Content (Join-Path $evidence 'result.json')
 Write-Output "FIXTURE retained for review: $scratch"
}
"$passed checks passed; no skips, no retries"
