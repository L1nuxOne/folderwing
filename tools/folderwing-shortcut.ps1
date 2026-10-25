<# Create/update only a Folderwing shortcut targeting the exact supplied build. #>
param([string]$BuildDirectory=(Join-Path (Split-Path $PSScriptRoot -Parent) 'build\release'),[string]$Directory=[Environment]::GetFolderPath('Desktop'),[Parameter(Mandatory)][string]$BackupDirectory)
$ErrorActionPreference='Stop';$exe=[IO.Path]::GetFullPath((Join-Path $BuildDirectory 'folderwing.exe'));if(-not(Test-Path -LiteralPath $exe -PathType Leaf)){throw 'Missing Folderwing executable'}
New-Item -ItemType Directory -Force $Directory,$BackupDirectory|Out-Null
if(-not('FolderwingShortcutProperties' -as [type])){Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FolderwingShortcutProperties {
 [StructLayout(LayoutKind.Sequential)] struct Key { public Guid fmtid; public uint pid; }
 [StructLayout(LayoutKind.Explicit,Size=24)] struct Value { [FieldOffset(0)] public ushort vt; [FieldOffset(8)] public IntPtr text; }
 [ComImport,Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),InterfaceType(ComInterfaceType.InterfaceIsIUnknown)] interface Store {
  [PreserveSig] int GetCount(out uint c); [PreserveSig] int GetAt(uint i,out Key k); [PreserveSig] int GetValue(ref Key k,out Value v); [PreserveSig] int SetValue(ref Key k,ref Value v); [PreserveSig] int Commit();
 }
 [DllImport("shell32.dll",CharSet=CharSet.Unicode,PreserveSig=true)] static extern int SHGetPropertyStoreFromParsingName(string path,IntPtr bind,uint flags,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out Store store);
 [DllImport("ole32.dll")] static extern int PropVariantClear(ref Value value);
 public static void Set(string path,string id) { Guid iid=new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"); Store store;Marshal.ThrowExceptionForHR(SHGetPropertyStoreFromParsingName(path,IntPtr.Zero,2,ref iid,out store));var key=new Key{fmtid=new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"),pid=5};var v=new Value{vt=31,text=Marshal.StringToCoTaskMemUni(id)};try{Marshal.ThrowExceptionForHR(store.SetValue(ref key,ref v));Marshal.ThrowExceptionForHR(store.Commit());}finally{PropVariantClear(ref v);Marshal.ReleaseComObject(store);} }
 public static string Get(string path) { Guid iid=new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"); Store store;Marshal.ThrowExceptionForHR(SHGetPropertyStoreFromParsingName(path,IntPtr.Zero,0,ref iid,out store));var key=new Key{fmtid=new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"),pid=5};Value v;try{Marshal.ThrowExceptionForHR(store.GetValue(ref key,out v));try{return v.vt==31?Marshal.PtrToStringUni(v.text):null;}finally{PropVariantClear(ref v);}}finally{Marshal.ReleaseComObject(store);} }
}
'@}
$shell=New-Object -ComObject WScript.Shell;$target=Join-Path $Directory 'Folderwing.lnk';$old=Join-Path $Directory 'OpenPilot.lnk';$source=$null;$legacyExe=[IO.Path]::GetFullPath((Join-Path $BuildDirectory 'fpdemo.exe'))
foreach($file in @($target,$old)){if(Test-Path -LiteralPath $file){$link=$shell.CreateShortcut($file);if([IO.Path]::GetFullPath($link.TargetPath) -ne $exe -and [IO.Path]::GetFullPath($link.TargetPath) -ne $legacyExe){if($file -eq $target){throw 'Existing Folderwing shortcut targets another app/build; retained'};continue};if(-not $source){$source=$file};Copy-Item -LiteralPath $file -Destination (Join-Path $BackupDirectory ([IO.Path]::GetFileName($file))) -ErrorAction Stop}}
$stage=Join-Path $Directory ('Folderwing-'+[guid]::NewGuid().ToString('N')+'.lnk')
try{
 if($source){Copy-Item -LiteralPath $source -Destination $stage};$link=$shell.CreateShortcut($stage);$link.TargetPath=$exe;if(-not $source){$link.WorkingDirectory=$BuildDirectory};$link.Description='Folderwing - File manager for Windows and WSL';$link.IconLocation=$exe+',0';$link.Save();[FolderwingShortcutProperties]::Set($stage,'Folderwing.FileManager')
 $verify=$shell.CreateShortcut($stage);if([IO.Path]::GetFullPath($verify.TargetPath) -ne $exe -or [FolderwingShortcutProperties]::Get($stage) -ne 'Folderwing.FileManager'){throw 'Shortcut verification failed'}
 [IO.File]::Move($stage,$target,$true)
 if($source -eq $old){Remove-Item -LiteralPath $old}
 @{name='Folderwing';path=$target;target=$verify.TargetPath;arguments=$verify.Arguments;working_directory=$verify.WorkingDirectory;icon=$verify.IconLocation;app_id=[FolderwingShortcutProperties]::Get($target);original_backup=$source}|ConvertTo-Json
}finally{if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage}}
