# Isolated experiment: owned C: fixtures, read-only volume, no desktop input.
param([Parameter(Mandatory)][string]$Probe,[Parameter(Mandatory)][string]$ResultDirectory)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $ResultDirectory){throw 'Use a fresh evidence directory'}
if([IO.Path]::GetPathRoot([IO.Path]::GetFullPath($ResultDirectory)) -ine 'C:\'){throw 'This cohort is explicitly scoped to C:'}
if(-not ([Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Elevated read-only volume access required'}
New-Item -ItemType Directory -Path $ResultDirectory|Out-Null
$out=$ResultDirectory;$cache=Join-Path $out 'model.bin';$fixtures=Join-Path $out 'fixtures';$checks=[Collections.Generic.List[object]]::new()
function Check([string]$name,[bool]$okay){$checks.Add(@{name=$name;passed=$okay});if(-not $okay){throw "Check failed: $name"}}
function Start-Owned([string[]]$arguments){$pi=[Diagnostics.ProcessStartInfo]::new($Probe);$pi.UseShellExecute=$false;$pi.CreateNoWindow=$true;$pi.RedirectStandardOutput=$true;$pi.RedirectStandardError=$true;foreach($a in $arguments){[void]$pi.ArgumentList.Add($a)};$p=[Diagnostics.Process]::Start($pi);return @{p=$p;stdout=$p.StandardOutput.ReadToEndAsync();stderr=$p.StandardError.ReadToEndAsync()}}
function Finish-Owned([string]$label,$owned,[int]$expected=0){$p=$owned.p;if(-not $p.WaitForExit(180000)){$p.Kill($true);throw "Owned $label timed out"};[IO.File]::WriteAllText("$out\$label.stdout.txt",$owned.stdout.Result);[IO.File]::WriteAllText("$out\$label.stderr.txt",$owned.stderr.Result);$code=$p.ExitCode;$p.Dispose();Check "$label.exit.$expected" ($code -eq $expected)}
function Run([string]$label,[string[]]$extra=@(),[int]$expected=0,[string]$cachePath=$cache){$arguments=@('C:',"--cache=$cachePath","--json=$out\$label.json","--csv=$out\$label.csv","--prefix=$fixtures")+$extra;$owned=Start-Owned $arguments;Finish-Owned $label $owned $expected;return Get-Content "$out\$label.json" -Raw|ConvertFrom-Json}
function Stat([string]$path){$owned=Start-Owned @('--stat',$path);if(-not $owned.p.WaitForExit(10000)){$owned.p.Kill($true);throw 'stat timeout'};$text=$owned.stdout.Result;$code=$owned.p.ExitCode;$owned.p.Dispose();if($code){throw 'stat failed'};return $text|ConvertFrom-Json}
function Verify([string]$label){$rows=@(Import-Csv "$out\$label.csv");$map=@{};foreach($r in $rows){Check "$label.unique.$($r.Path)" (-not $map.ContainsKey($r.Path));$map[$r.Path]=$r};$actual=@(Get-Item -LiteralPath $fixtures)+@(Get-ChildItem -LiteralPath $fixtures -Recurse -Force);$sizes=@{};Check "$label.path.count" ($actual.Count -eq $rows.Count);foreach($item in $actual){Check "$label.path.$($item.Name)" ($map.ContainsKey($item.FullName));$row=$map[$item.FullName];if(-not $item.PSIsContainer){$s=Stat $item.FullName;Check "$label.identity.$($item.Name)" ([uint64]$row.Reference -eq [uint64]$s.reference);Check "$label.size.$($item.Name)" ([uint64]$row.Size -eq [uint64]$s.size);$sizes[$item.FullName]=[uint64]$s.size}};foreach($dir in $actual|Where-Object PSIsContainer){$sum=0L;foreach($file in $actual|Where-Object {-not $_.PSIsContainer -and $_.FullName.StartsWith($dir.FullName+'\',[StringComparison]::OrdinalIgnoreCase)}){$sum+=$sizes[$file.FullName]};Check "$label.tree-total.$($dir.Name)" ([uint64]$map[$dir.FullName].Size -eq $sum)};return $map}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class CacheFixtureUsn {
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFile(string p,uint access,uint share,IntPtr sa,uint creation,uint flags,IntPtr template);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h,uint code,IntPtr input,uint inputSize,byte[] output,uint outputSize,out uint returned,IntPtr overlapped);
 public static long Read(string path){using(var h=CreateFile(path,0x80,7,IntPtr.Zero,3,0x02000000,IntPtr.Zero)){var b=new byte[4096];uint n;if(h.IsInvalid||!DeviceIoControl(h,0x900eb,IntPtr.Zero,0,b,(uint)b.Length,out n,IntPtr.Zero)||n<60)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());return BitConverter.ToInt64(b,24);}}
}
'@
try {
 @{schema='openpilot-cached-mft-input/v1';probe=$Probe;probe_sha256=(Get-FileHash $Probe -Algorithm SHA256).Hash.ToLowerInvariant();wsl_stopped=$false;desktop_input=$false;volume_writes=$false;workers=1;volume='C:';cpu=@(Get-CimInstance Win32_Processor|ForEach-Object LoadPercentage);free_memory_kib=(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory}|ConvertTo-Json -Depth 5|Set-Content "$out\INPUT.json"
 New-Item -ItemType Directory -Path $fixtures,"$fixtures\hot","$fixtures\tree","$fixtures\links"|Out-Null
 [IO.File]::WriteAllText("$fixtures\resident-β.txt",'abc');[IO.File]::WriteAllBytes("$fixtures\normal.bin",[byte[]]::new(65536));[IO.File]::WriteAllText("$fixtures\normal.bin:extra",'named-stream');[IO.File]::WriteAllText("$fixtures\delete.txt",'to delete');[IO.File]::WriteAllText("$fixtures\tree\child.md",'nested')
 New-Item -ItemType HardLink -Path "$fixtures\normal-link.bin" -Target "$fixtures\normal.bin"|Out-Null
 # Enough names to force extension records and an attribute list.
 foreach($i in 1..48){New-Item -ItemType HardLink -Path ("$fixtures\links\alias-{0:D2}.bin" -f $i) -Target "$fixtures\normal.bin"|Out-Null}
 $owned=Start-Owned @('--selftest');Finish-Owned 'selftest' $owned
 $base=Run 'baseline';Check 'baseline.full-read' ($base.rebuilt -and $base.mft_bytes -gt 0 -and -not $base.partial);$map=Verify 'baseline';Check 'baseline.resident-allocation' ([uint64]$map["$fixtures\resident-β.txt"].Allocated -eq 0)
 [IO.File]::WriteAllBytes("$fixtures\resident-β.txt",[byte[]]::new(131072));$f=[IO.File]::Open("$fixtures\normal.bin",[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite);$f.SetLength(16384);$f.Dispose();[IO.File]::WriteAllText("$fixtures\normal.bin:extra",('stream'*100))
 Move-Item -LiteralPath "$fixtures\tree" -Destination "$fixtures\renamed-tree";Rename-Item -LiteralPath "$fixtures\normal-link.bin" -NewName 'renamed-link.bin';Remove-Item -LiteralPath "$fixtures\delete.txt";Remove-Item -LiteralPath "$fixtures\links\alias-01.bin";New-Item -ItemType HardLink -Path "$fixtures\links\new-alias.bin" -Target "$fixtures\normal.bin"|Out-Null
 New-Item -ItemType Directory -Path "$fixtures\cold-new"|Out-Null;[IO.File]::WriteAllBytes("$fixtures\cold-new\unseen.dat",[byte[]]::new(8192))
 $r=Run 'changed';Check 'changed.no-full-read' (-not $r.rebuilt -and $r.mft_bytes -eq 0);Check 'changed.checkpoint-advanced' ($r.cursor -gt $base.cursor);$map=Verify 'changed';Check 'changed.ADS' ([uint64]$map["$fixtures\normal.bin"].AllStreamSize -eq 16984);Check 'changed.coalesced' ($r.coalesced -gt 0)
 $r=Run 'reopen';Check 'reopen.no-full-read' (-not $r.rebuilt -and $r.mft_bytes -eq 0);$null=Verify 'reopen'
 # Keep writer open through scan completion; later DATA_EXTEND writes may coalesce.
 $writerPath="$fixtures\hot\held.bin";[IO.File]::WriteAllBytes($writerPath,[byte[]]::new(8192));$writer=[IO.File]::Open($writerPath,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite);$writer.Position=$writer.Length;$bytes=[byte[]]::new(32768);$writer.Write($bytes);$writer.Flush();$before=[CacheFixtureUsn]::Read($writerPath)
 $owned=Start-Owned @('C:',"--cache=$cache","--json=$out\watch.json","--csv=$out\watch.csv","--prefix=$fixtures",'--watch=12');$history=[Collections.Generic.List[object]]::new();$history.Add(@{length=$writer.Length;usn=$before})
 foreach($i in 1..3){Start-Sleep -Seconds 2;$writer.Write($bytes);$writer.Flush();$history.Add(@{length=$writer.Length;usn=[CacheFixtureUsn]::Read($writerPath)})}
 Finish-Owned 'watch' $owned;$r=Get-Content "$out\watch.json" -Raw|ConvertFrom-Json;Check 'watch.hot-sampling' ($r.hot_samples -gt 0 -and $r.cycles -gt 2);$map=Verify 'watch';Check 'watch.open-writer-current' ([uint64]$map[$writerPath].Size -eq $writer.Length);$history|ConvertTo-Json|Set-Content "$out\OPEN_WRITER_USN.json";$writer.Dispose();$writer=$null
 $old=(Get-FileHash $cache -Algorithm SHA256).Hash;$r=Run 'interrupted-save' @('--interrupt-save') 4;Check 'interrupted-save.original-preserved' ((Get-FileHash $cache -Algorithm SHA256).Hash -eq $old);Check 'interrupted-save.candidate-retained' (@(Get-ChildItem -LiteralPath $out -Filter 'model.bin.pending-*').Count -eq 1)
 $r=Run 'after-interrupted-save';Check 'after-interrupted-save.cache-loaded' (-not $r.rebuilt)
 [IO.File]::WriteAllText("$fixtures\cold-new\unseen.dat",'failed refresh replay');$prior=(Get-Content "$out\after-interrupted-save.json" -Raw|ConvertFrom-Json);$r=Run 'refresh-failure' @('--inject=refresh') 3;Check 'refresh-failure.cursor-held' ($r.partial -and $r.failed_refreshes -gt 0 -and $r.cursor -eq $prior.cursor);$r=Run 'refresh-retry';Check 'refresh-retry.clean' (-not $r.partial -and $r.cursor -gt $prior.cursor);$null=Verify 'refresh-retry'
 # Only injected metadata is changed. The real volume and USN journal are untouched.
 foreach($fault in @('gap','journal','volume')){$r=Run "injected-$fault" @("--inject=$fault");Check "injected-$fault.rebaseline" ($r.rebuilt -and $r.reason -eq 'journal-gap-or-identity-change' -and $r.mft_bytes -gt 0)}
 $r=Run 'repeated-gap' @('--inject=unstable') 3;Check 'repeated-gap.stays-incomplete' ($r.partial -and $r.reason -eq 'injected-repeated-journal-gap');$r=Run 'after-repeated-gap';Check 'after-repeated-gap.recovers' (-not $r.partial -and -not $r.rebuilt)
 # Small malformed input: rejected file is retained before safe rebuild.
 $bad="$out\bad.bin";[IO.File]::WriteAllBytes($bad,[byte[]]@(1,2,3,4));$hash=(Get-FileHash $bad -Algorithm SHA256).Hash;$r=Run 'truncated-cache' @() 0 $bad;Check 'truncated-cache.rebuilt' ($r.rebuilt -and $r.reason -eq 'cache-invalid');$rejected=@(Get-ChildItem -LiteralPath $out -Filter 'bad.bin.rejected-*');Check 'truncated-cache.preserved' ($rejected.Count -eq 1 -and (Get-FileHash $rejected[0].FullName -Algorithm SHA256).Hash -eq $hash)
 @{outcome='passed';checks=$checks.Count;wsl_stopped=$false;release_changed=$false}|ConvertTo-Json|Set-Content "$out\RESULT.json"
} catch {$_|Out-String|Set-Content "$out\ERROR.txt";@{outcome='failed';checks=$checks.Count;error=$_.ToString()}|ConvertTo-Json|Set-Content "$out\RESULT.json";throw}
finally {if($writer){$writer.Dispose()};$checks.ToArray()|ConvertTo-Json -Depth 5|Set-Content "$out\CHECKS.json"}
