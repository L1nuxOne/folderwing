# Standalone comparison. No app publication or physical desktop input.
param([Parameter(Mandatory)][string]$Probe,[Parameter(Mandatory)][string]$ResultDirectory,[string]$WizTree='C:\Program Files\WizTree\WizTree64.exe',[string]$Volume='C:')
$ErrorActionPreference='Stop'
if($Volume -notmatch '^[A-Z]:$'){throw 'Use one local drive letter'}
if(Test-Path -LiteralPath $ResultDirectory){throw 'Use a fresh evidence directory'}
if([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($ResultDirectory)).TrimEnd('\') -ine $Volume){throw 'Owned fixtures and output must be on the measured volume'}
if(-not ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'This read-only raw-volume comparison requires an elevated session'}
New-Item -ItemType Directory $ResultDirectory|Out-Null
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
[ComImport,Guid("618736e0-3c3d-11cf-810c-00aa00389b71"),InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
public interface ProbeAccessible {
 [DispId(-5001)] int accChildCount {get;}
 [DispId(-5003)] string get_accName([MarshalAs(UnmanagedType.Struct)] object child);
 [DispId(-5004)] string get_accValue([MarshalAs(UnmanagedType.Struct)] object child);
}
public static class MftBenchNative {
 public delegate bool EnumWindow(IntPtr h,IntPtr user);
 [DllImport("user32.dll")] static extern bool EnumWindows(EnumWindow callback,IntPtr user);
 [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr h,EnumWindow callback,IntPtr user);
 [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageTimeout(IntPtr h,uint message,IntPtr w,StringBuilder text,uint flags,uint timeout,out IntPtr result);
 [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr h,uint objectId,ref Guid riid,[MarshalAs(UnmanagedType.Interface)] out object accessible);
 [StructLayout(LayoutKind.Sequential)] public struct Io { public ulong ReadOperations,WriteOperations,OtherOperations,ReadBytes,WriteBytes,OtherBytes; }
 [DllImport("kernel32.dll")] public static extern bool GetProcessIoCounters(IntPtr h,out Io counters);
 public static string[] WindowText(int ownedPid){var lines=new List<string>();EnumWindow child=(h,u)=>{uint pid;GetWindowThreadProcessId(h,out pid);if(pid!=ownedPid)return true;var cls=new StringBuilder(128);GetClassName(h,cls,128);var text=new StringBuilder(1024);IntPtr result;SendMessageTimeout(h,13,(IntPtr)1024,text,2,30,out result);if(text.Length>0)lines.Add(cls+" | "+text);
  if(cls.ToString().IndexOf("Status",StringComparison.OrdinalIgnoreCase)>=0){try{Guid iid=new Guid("618736e0-3c3d-11cf-810c-00aa00389b71");object raw;if(AccessibleObjectFromWindow(h,0xfffffffc,ref iid,out raw)==0){var a=(ProbeAccessible)raw;int n=Math.Min(a.accChildCount,32);for(int i=0;i<=n;i++){string name=a.get_accName(i);if(!String.IsNullOrEmpty(name))lines.Add("status["+i+"] | "+name);}Marshal.ReleaseComObject(raw);}}catch(Exception e){lines.Add("status-read | "+e.GetType().Name);}}
  return true;};EnumWindows((h,u)=>{uint pid;GetWindowThreadProcessId(h,out pid);if(pid==ownedPid){child(h,u);EnumChildWindows(h,child,u);}return true;},IntPtr.Zero);return lines.ToArray();}
}
'@
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$sources=[ordered]@{};foreach($name in @('main.c','mft.c','mft.h','benchmark.ps1','build.py','compare.py','verify_fixtures.py','resident-allocation.c')){$sources['tools/ntfs-prototype/'+$name]=(Get-FileHash (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()}
$os=Get-CimInstance Win32_OperatingSystem
@{schema='openpilot-mft-comparison-input/v1';probe=$Probe;probe_sha256=(Get-FileHash $Probe -Algorithm SHA256).Hash.ToLowerInvariant();wiztree=$WizTree;wiztree_sha256=(Get-FileHash $WizTree -Algorithm SHA256).Hash.ToLowerInvariant();wiztree_version=[Diagnostics.FileVersionInfo]::GetVersionInfo($WizTree).FileVersion;volume=$Volume;admin=$true;sources=$sources;workers=1;live_volume=$true;snapshot=$false;free_memory_kib=$os.FreePhysicalMemory;cpu_before=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);physical_input=$false;wsl_stopped=$false;cache='uncontrolled; serialized WizTree full export, prototype full export, WizTree type summary, prototype scan only'}|ConvertTo-Json -Depth 6|Set-Content (Join-Path $ResultDirectory 'INPUT.json')
function Invoke-Owned([string]$Label,[string]$Image,[string[]]$Arguments,[switch]$TraceWindows){
 $pi=[Diagnostics.ProcessStartInfo]::new($Image);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($arg in $Arguments){[void]$pi.ArgumentList.Add($arg)}
 $trace=[Collections.Generic.List[object]]::new();$watch=[Diagnostics.Stopwatch]::StartNew();$p=[Diagnostics.Process]::Start($pi);$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();$next=0;$peak=0L
 while(-not $p.WaitForExit(250)){
  if($watch.Elapsed.TotalSeconds -gt 240){$p.Kill($true);throw ('Owned '+$Label+' exceeded 240 seconds')}
  if($watch.Elapsed.TotalMilliseconds -ge $next){$p.Refresh();$peak=[Math]::Max($peak,$p.PeakWorkingSet64);$io=[MftBenchNative+Io]::new();[void][MftBenchNative]::GetProcessIoCounters($p.Handle,[ref]$io)
   $trace.Add(@{ms=$watch.Elapsed.TotalMilliseconds;cpu_ms=$p.TotalProcessorTime.TotalMilliseconds;read_bytes=$io.ReadBytes;write_bytes=$io.WriteBytes;peak_bytes=$peak;windows=$(if($TraceWindows){@([MftBenchNative]::WindowText($p.Id))}else{@()})});$next=$watch.Elapsed.TotalMilliseconds+2000
  }
 }
 $watch.Stop();$code=$p.ExitCode;$ownedId=$p.Id;$p.Dispose()
 [IO.File]::WriteAllText((Join-Path $ResultDirectory ($Label+'.stdout.txt')),$stdout.Result);[IO.File]::WriteAllText((Join-Path $ResultDirectory ($Label+'.stderr.txt')),$stderr.Result)
 @{label=$Label;image=$Image;arguments=$Arguments;pid=$ownedId;exit_code=$code;wall_ms=$watch.Elapsed.TotalMilliseconds;peak_working_set_sampled=$peak;trace=@($trace.ToArray())}|ConvertTo-Json -Depth 8|Set-Content (Join-Path $ResultDirectory ($Label+'.process.json'))
 if($code){throw ('Owned '+$Label+' failed with '+$code)}
 Write-Output ($Label+' completed in '+$watch.Elapsed.TotalSeconds.ToString('F3')+' seconds')
}
$fixtures=Join-Path $ResultDirectory 'fixtures';New-Item -ItemType Directory $fixtures|Out-Null
[IO.File]::WriteAllText((Join-Path $fixtures 'resident-β.txt'),'abc');[IO.File]::WriteAllBytes((Join-Path $fixtures 'normal.bin'),[byte[]]::new(65536));[IO.File]::WriteAllBytes((Join-Path $fixtures 'compressed.bin'),[byte[]]::new(1024*1024))
New-Item -ItemType HardLink -Path (Join-Path $fixtures 'hardlink.bin') -Target (Join-Path $fixtures 'normal.bin')|Out-Null
[IO.File]::WriteAllText((Join-Path $fixtures 'normal.bin:extra'),('stream'*2048))
[IO.File]::WriteAllBytes((Join-Path $fixtures 'sparse.bin'),[byte[]]::new(0))
Invoke-Owned 'fixture-sparse' (Join-Path $env:windir 'System32\fsutil.exe') @('sparse','setflag',(Join-Path $fixtures 'sparse.bin'))
$f=[IO.File]::Open((Join-Path $fixtures 'sparse.bin'),[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite);$f.SetLength(1073741824);$f.Position=1073741824-4;$f.Write([byte[]]@(1,2,3,4));$f.Flush($true);$f.Dispose()
Invoke-Owned 'fixture-compress' (Join-Path $env:windir 'System32\compact.exe') @('/c','/i',(Join-Path $fixtures 'compressed.bin'))
$expected=@();foreach($file in Get-ChildItem -LiteralPath $fixtures -File){$pi=[Diagnostics.ProcessStartInfo]::new($Probe);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;foreach($arg in @('--stat',$file.FullName)){[void]$pi.ArgumentList.Add($arg)};$p=[Diagnostics.Process]::Start($pi);$text=$p.StandardOutput.ReadToEnd();$p.WaitForExit();if($p.ExitCode){throw 'Fixture metadata stat failed'};$p.Dispose();$value=$text|ConvertFrom-Json;$value|Add-Member -NotePropertyName path -NotePropertyValue $file.FullName;$expected+=,$value};$expected|ConvertTo-Json -Depth 5|Set-Content (Join-Path $ResultDirectory 'FIXTURES.json')
Invoke-Owned 'resident-allocation' (Join-Path (Split-Path $Probe -Parent) 'resident-allocation.exe') @((Join-Path $fixtures 'resident-β.txt'))
[IO.File]::Copy((Join-Path $ResultDirectory 'resident-allocation.stdout.txt'),(Join-Path $ResultDirectory 'RESIDENT_ALLOCATION.json'))
Invoke-Owned 'parser' $Probe @('--selftest')
Invoke-Owned 'wiztree' $WizTree @($Volume,('/export='+$ResultDirectory+'\wiztree.csv'),'/admin=1','/exportmftrecno=1','/exportUTCTime=1','/sortby=0') -TraceWindows
Invoke-Owned 'own' $Probe @($Volume,('--csv='+$ResultDirectory+'\own.csv'),('--json='+$ResultDirectory+'\own.json'))
Invoke-Owned 'wiztree-types' $WizTree @($Volume,('/exportfiletypes='+$ResultDirectory+'\wiztree-types.csv'),'/admin=1') -TraceWindows
Invoke-Owned 'own-scan-only' $Probe @($Volume,('--json='+$ResultDirectory+'\own-scan-only.json'))
@{outcome='completed';wsl_stopped=$false;production_changed=$false;cpu_after=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage)}|ConvertTo-Json|Set-Content (Join-Path $ResultDirectory 'RESULT.json')
