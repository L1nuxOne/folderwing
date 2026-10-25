# Shared, opt-in Windows child-session test infrastructure. Dot-sourcing does not
# change machine settings, start services, or create a session.
function Get-ChildUiRepository {
    if((Split-Path $PSScriptRoot -Leaf) -ieq 'tools'){return (Split-Path $PSScriptRoot -Parent)}
    $parent=Split-Path $PSScriptRoot -Parent
    if((Split-Path $parent -Leaf) -ieq 'child-ui-cache' -and (Split-Path (Split-Path $parent -Parent) -Leaf) -ieq 'build'){
        return (Split-Path (Split-Path $parent -Parent) -Parent)
    }
    throw 'Child UI helpers must run from tools or their verified repository cache'
}
function Initialize-ChildUiWindows {
    if(-not $IsWindows -or $PSVersionTable.PSVersion.Major -lt 7){throw 'Child UI tests require Windows PowerShell 7'}
    $windows=[Environment]::GetFolderPath('Windows')
    $env:SystemRoot=$windows;$env:windir=$windows;$env:SystemDrive=[IO.Path]::GetPathRoot($windows).TrimEnd('\')
    $env:ComSpec=Join-Path $windows 'System32\cmd.exe';$env:PATHEXT='.COM;.EXE;.BAT;.CMD'
    $env:PATH=(Join-Path $windows 'System32')+';'+$windows+';'+$env:PATH
}
function Import-ChildUiNative {
    if('OpenPilotChildUi' -as [type]){return}
    Add-Type @'
using System;using System.Runtime.InteropServices;using System.Text.Json;using System.IO;using System.Linq;
public static class OpenPilotChildUi {
 [DllImport("wtsapi32.dll",SetLastError=true)]public static extern bool WTSIsChildSessionsEnabled(out bool enabled);
 [DllImport("wtsapi32.dll",SetLastError=true)]public static extern bool WTSGetChildSessionId(out uint id);
 [DllImport("wtsapi32.dll",SetLastError=true)]public static extern bool WTSEnableChildSessions(bool enabled);
 [DllImport("kernel32.dll")]public static extern uint WTSGetActiveConsoleSessionId();
 [StructLayout(LayoutKind.Sequential)]public struct RECT {public int L,T,R,B;}
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]public struct MONITOR {public uint Size;public RECT Bounds,Work;public uint Flags;}
 [DllImport("user32.dll")]public static extern IntPtr MonitorFromWindow(IntPtr h,uint flags);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern bool GetMonitorInfoW(IntPtr h,ref MONITOR m);
 [DllImport("user32.dll")]static extern IntPtr OpenInputDesktop(uint f,bool inherit,uint access);
 [DllImport("user32.dll")]static extern IntPtr GetThreadDesktop(uint id);
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")]static extern bool CloseDesktop(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern bool GetUserObjectInformationW(IntPtr h,int n,System.Text.StringBuilder s,uint bytes,out uint needed);
 static string DesktopName(IntPtr h){uint n;var s=new System.Text.StringBuilder(256);if(h==IntPtr.Zero||!GetUserObjectInformationW(h,2,s,512,out n))throw new Exception("Desktop identity unavailable");return s.ToString();}
 public static string InputDesktop(){var h=OpenInputDesktop(0,false,1);try{return DesktopName(h);}finally{if(h!=IntPtr.Zero)CloseDesktop(h);}}
 public static string CallerDesktop(){return DesktopName(GetThreadDesktop(GetCurrentThreadId()));}
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct OWNERSTART {public int cb;public string reserved,desktop,title;public int x,y,w,h,cx,cy,fill,flags;public short show,reserved2;public IntPtr reservedPtr,input,output,error;}
 [StructLayout(LayoutKind.Sequential)]struct OWNERSTARTEX {public OWNERSTART start;public IntPtr attributes;}
 [StructLayout(LayoutKind.Sequential)]struct OWNERPROCESS {public IntPtr process,thread;public int pid,tid;}
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool InitializeProcThreadAttributeList(IntPtr list,int count,int flags,ref IntPtr bytes);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool UpdateProcThreadAttribute(IntPtr list,uint flags,IntPtr attribute,IntPtr value,IntPtr bytes,IntPtr previous,IntPtr returned);
 [DllImport("kernel32.dll")]static extern void DeleteProcThreadAttributeList(IntPtr list);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool SetHandleInformation(IntPtr h,uint mask,uint flags);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool CreateProcessW(string image,System.Text.StringBuilder command,IntPtr ps,IntPtr ts,bool inherit,uint flags,IntPtr env,string directory,ref OWNERSTARTEX startup,out OWNERPROCESS process);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr h);
 static void Check(bool value,string operation){if(!value)throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),operation);}
 static string Quote(string value){if(value.IndexOfAny(new[]{'"','\r','\n','\0'})>=0)throw new ArgumentException("Invalid owned path");return "\""+value+"\"";}
 public static System.Diagnostics.Process StartOwner(string image,string script,string bootstrap,string root){
  IntPtr bytes=IntPtr.Zero,attributes=IntPtr.Zero,handles=IntPtr.Zero;bool initialized=false;OWNERPROCESS child=new OWNERPROCESS();
  using(var stdout=new FileStream(Path.Combine(root,"controller.log"),FileMode.CreateNew,FileAccess.Write,FileShare.ReadWrite))
  using(var stderr=new FileStream(Path.Combine(root,"controller.stderr.log"),FileMode.CreateNew,FileAccess.Write,FileShare.ReadWrite)){
   try{
    IntPtr output=stdout.SafeFileHandle.DangerousGetHandle(),error=stderr.SafeFileHandle.DangerousGetHandle();
    Check(SetHandleInformation(output,1,1)&&SetHandleInformation(error,1,1),"Owned log inheritance");
    InitializeProcThreadAttributeList(IntPtr.Zero,1,0,ref bytes);if(bytes==IntPtr.Zero)throw new Exception("Handle-list size unavailable");
    attributes=Marshal.AllocHGlobal(bytes);Check(InitializeProcThreadAttributeList(attributes,1,0,ref bytes),"Initialize owner handle list");initialized=true;
    handles=Marshal.AllocHGlobal(IntPtr.Size*2);Marshal.WriteIntPtr(handles,0,output);Marshal.WriteIntPtr(handles,IntPtr.Size,error);
    Check(UpdateProcThreadAttribute(attributes,0,(IntPtr)0x20002,handles,(IntPtr)(IntPtr.Size*2),IntPtr.Zero,IntPtr.Zero),"Restrict owner handle inheritance");
    var startup=new OWNERSTARTEX{start=new OWNERSTART{cb=Marshal.SizeOf(typeof(OWNERSTARTEX)),flags=0x100,output=output,error=error},attributes=attributes};
    var command=new System.Text.StringBuilder(Quote(image)+" -NoProfile -File "+Quote(script)+" -BootstrapPath "+Quote(bootstrap));
    Check(CreateProcessW(image,command,IntPtr.Zero,IntPtr.Zero,true,0x08080000,IntPtr.Zero,root,ref startup,out child),"Create detached ordinary owner");
    return System.Diagnostics.Process.GetProcessById(child.pid);
   }finally{
    if(child.thread!=IntPtr.Zero)CloseHandle(child.thread);if(child.process!=IntPtr.Zero)CloseHandle(child.process);
    if(handles!=IntPtr.Zero)Marshal.FreeHGlobal(handles);if(initialized)DeleteProcThreadAttributeList(attributes);if(attributes!=IntPtr.Zero)Marshal.FreeHGlobal(attributes);
   }
  }
 }
 static void Write(Utf8JsonWriter w,JsonElement e){switch(e.ValueKind){case JsonValueKind.Object:w.WriteStartObject();foreach(var p in e.EnumerateObject().OrderBy(p=>p.Name,StringComparer.Ordinal)){w.WritePropertyName(p.Name);Write(w,p.Value);}w.WriteEndObject();break;case JsonValueKind.Array:w.WriteStartArray();foreach(var a in e.EnumerateArray())Write(w,a);w.WriteEndArray();break;default:e.WriteTo(w);break;}}
 public static string Canonical(string text){using(var d=JsonDocument.Parse(text))using(var s=new MemoryStream()){using(var w=new Utf8JsonWriter(s)){Write(w,d.RootElement);}return System.Text.Encoding.UTF8.GetString(s.ToArray());}}
}
'@
}
function Get-ChildUiDigest($Value) {
    Import-ChildUiNative
    $bytes=[Text.Encoding]::UTF8.GetBytes([OpenPilotChildUi]::Canonical(($Value|ConvertTo-Json -Depth 80 -Compress)))
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}
function Write-ChildUiJson([string]$Path,$Value) {
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 80))
    $temporary=$Path+'.pending-'+[Guid]::NewGuid().ToString('N')
    try{
        $s=[IO.FileStream]::new($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        try{$s.Write($bytes);$s.Flush($true)}finally{$s.Dispose()}
        [IO.File]::Move($temporary,$Path,$false)
    }finally{if(Test-Path -LiteralPath $temporary){Remove-Item -LiteralPath $temporary}}
}
function Assert-ChildUiPath([string]$Path) {
    if(-not [IO.Path]::IsPathFullyQualified($Path) -or $Path -match '[\x00-\x1f"]'){throw 'Absolute Windows path without control characters required'}
    $full=[IO.Path]::GetFullPath($Path);$at=$full
    while($at){if(Test-Path -LiteralPath $at){if((Get-Item -LiteralPath $at -Force).Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Reparse path refused: $at"}};$at=[IO.Path]::GetDirectoryName($at)}
    $full
}
function Read-ChildUiLease([string]$Path) {
    $path=Assert-ChildUiPath $Path
    if((Get-Item -LiteralPath $path).Length -gt 32768){throw 'Oversized child UI lease'}
    $lease=Get-Content -LiteralPath $path -Raw|ConvertFrom-Json
    $repo=Get-ChildUiRepository
    if($lease.schema -cne 'openpilot-child-ui/v1' -or $lease.token -cnotmatch '^[a-f0-9]{32}$'){throw 'Unknown child UI lease identity'}
    $expected=Join-Path $repo "build\child-ui\$($lease.token)"
    if($lease.root -ine $expected -or (Split-Path $path) -ine $expected -or (Split-Path $path -Leaf) -cne 'lease.json'){throw 'Child UI lease is outside its owned root'}
    if($lease.sid -cne [Security.Principal.WindowsIdentity]::GetCurrent().User.Value){throw 'Child UI lease belongs to another account'}
    if($lease.profile -cnotin @('Smoke','Settings','Shell','All')){throw 'Unknown child UI profile'}
    $lease
}
function Assert-ChildUiProcess([int]$ProcessId,[long]$Ticks,[int]$Session,[string]$Image) {
    $p=Get-Process -Id $ProcessId -ErrorAction Stop
    if($p.StartTime.ToUniversalTime().Ticks -ne $Ticks -or $p.SessionId -ne $Session -or $p.Path -ine $Image){throw 'Child UI process PID/birth/session/image changed'}
    $p
}
function Assert-ChildUiWorker($Lease) {
    $ready=Get-Content (Join-Path $Lease.root 'host-ready.json') -Raw|ConvertFrom-Json
    $session=[Diagnostics.Process]::GetCurrentProcess().SessionId
    Import-ChildUiNative
    if($ready.token -cne $Lease.token -or -not $ready.child_mode -or $ready.redirect_clipboard -or
       $session -ne $ready.child_session -or $session -eq $Lease.parent_session -or $session -le 0 -or
       [OpenPilotChildUi]::WTSGetActiveConsoleSessionId() -ne $Lease.parent_session){throw 'Exact isolated child/parent session required'}
    $null=Assert-ChildUiProcess $ready.pid $ready.birth_ticks $Lease.parent_session $Lease.host_image
    if($Lease.session_root){$null=Assert-ChildUiReusableRun $Lease $ready}
    if($Lease.private_desktop){
        $desktop=Get-Content (Join-Path $Lease.root 'desktop-ready.json') -Raw|ConvertFrom-Json
        $name='OpenPilotUi-'+$Lease.token
        if($desktop.token -cne $Lease.token -or $desktop.session -ne $session -or $desktop.name -cne $name -or
           [OpenPilotChildUi]::InputDesktop() -cne $name -or [OpenPilotChildUi]::CallerDesktop() -cne $name){throw 'Exact private child input/caller desktop required'}
    }
    $ready
}
function Get-ChildUiSnapshot([string]$Repo) {
    $files=[ordered]@{}
    $paths=@(Get-ChildItem -LiteralPath $Repo -File -Force)
    foreach($name in @('app','include','linux','src','tests','third_party','tools','docs')){
        $paths+=@(Get-ChildItem -LiteralPath (Join-Path $Repo $name) -File -Recurse -Force|Where-Object {$_.FullName -notmatch '[\\/](__pycache__|\.zig-cache)[\\/]' -and $_.Extension -notin @('.pyc','.pyo')})
    }
    foreach($f in $paths|Sort-Object FullName){$name=[IO.Path]::GetRelativePath($Repo,$f.FullName).Replace('\','/');$files[$name]=(Get-FileHash -LiteralPath $f.FullName).Hash.ToLowerInvariant()}
    $files
}
function Assert-ChildUiInputs($Lease) {
    $inputFile=Join-Path $Lease.root 'input.json'
    $input=Get-Content $inputFile -Raw|ConvertFrom-Json
    if((Get-ChildUiDigest $input) -cne $Lease.input_key){throw 'Child UI input manifest changed'}
    $repo=Get-ChildUiRepository
    if((Get-ChildUiDigest (Get-ChildUiSnapshot $repo)) -cne (Get-ChildUiDigest $input.sources)){throw 'Child UI source set/content changed'}
    foreach($e in $input.images.PSObject.Properties){if((Get-FileHash -LiteralPath (Join-Path $repo $e.Name)).Hash.ToLowerInvariant() -cne $e.Value){throw ('Child UI image changed: '+$e.Name)}}
    if((Get-FileHash -LiteralPath $Lease.powershell).Hash.ToLowerInvariant() -cne $input.powershell_sha256){throw 'Child UI PowerShell image changed'}
    if($Lease.session_root -and ($input.session_token -cne $Lease.session_token -or
       (Get-FileHash (Join-Path $Lease.session_root 'session-ready.json')).Hash.ToLowerInvariant() -cne $input.session_ready_sha256)){throw 'Authenticated-session manifest binding changed'}
    $input
}
function Assert-ChildUiSessionBinding($Session,$Ready,$HostReady) {
    if(-not $Session.persistent -or $Ready.token -cne $Session.token -or $HostReady.token -cne $Session.token -or
       $Ready.cache_key -cne $Session.cache_key -or $Ready.sid -cne $Session.sid -or
       $Ready.parent_session -ne $Session.parent_session -or $Ready.child_session -le 0 -or $Ready.child_session -eq $Session.parent_session -or
       $HostReady.child_session -ne $Ready.child_session -or $HostReady.parent_session -ne $Session.parent_session -or
       $Ready.controller_pid -ne $Session.launcher_pid -or $Ready.controller_birth_ticks -ne $Session.launcher_birth_ticks -or
       $Ready.controller_image -ine $Session.powershell -or $Ready.host_pid -ne $HostReady.pid -or $Ready.host_birth_ticks -ne $HostReady.birth_ticks -or
       -not $HostReady.child_mode -or $HostReady.redirect_clipboard){throw 'Authenticated session binding changed; never adopt another session'}
}
function Get-ChildUiSession([string]$LeasePath,[switch]$AllowClosed) {
    $session=Read-ChildUiLease $LeasePath
    if(-not $session.persistent){throw 'Lease is not a reusable session'}
    if(Test-Path (Join-Path $session.root 'FINAL.json')){if($AllowClosed){return $session};throw 'Authenticated session already closed'}
    $ready=Get-Content (Join-Path $session.root 'session-ready.json') -Raw|ConvertFrom-Json
    $hostFacts=Get-Content (Join-Path $session.root 'host-ready.json') -Raw|ConvertFrom-Json
    Assert-ChildUiSessionBinding $session $ready $hostFacts
    if([OpenPilotChildUi]::WTSGetActiveConsoleSessionId() -ne $session.parent_session){throw 'Parent console changed'}
    $null=Assert-ChildUiProcess $ready.controller_pid $ready.controller_birth_ticks $session.parent_session $session.powershell
    $null=Assert-ChildUiProcess $ready.host_pid $ready.host_birth_ticks $session.parent_session $session.host_image
    # WTSGetChildSessionId is meaningful only in the parent. Child workers use
    # their own session ID plus the captured parent/host/controller identities.
    if([Diagnostics.Process]::GetCurrentProcess().SessionId -eq $session.parent_session){
        $child=[uint32]0
        if(-not [OpenPilotChildUi]::WTSGetChildSessionId([ref]$child) -or $child -ne $ready.child_session){throw 'Captured child session is absent or replaced'}
    }
    $session
}
function Assert-ChildUiReusableRun($Lease,$Ready) {
    $session=Get-ChildUiSession (Join-Path $Lease.session_root 'lease.json')
    $original=Get-Content (Join-Path $session.root 'host-ready.json') -Raw|ConvertFrom-Json
    if($Lease.session_token -cne $session.token -or $Lease.sid -cne $session.sid -or
       $Lease.parent_session -ne $session.parent_session -or $Lease.host_image -ine $session.host_image -or
       $Ready.session_token -cne $session.token -or $Ready.pid -ne $original.pid -or $Ready.birth_ticks -ne $original.birth_ticks -or
       $Ready.child_session -ne $original.child_session){throw 'Run is not bound to the exact authenticated session'}
    $session
}
function Assert-ChildUiTaskIdentity($Task,[string]$ExpectedXml,[string]$InstanceGuid) {
    if(-not $ExpectedXml -or $Task.Xml -cne $ExpectedXml){throw 'Owned task definition changed; leave it untouched'}
    $active=@($Task.GetInstances(0))
    foreach($one in $active){if(-not $InstanceGuid -or $one.InstanceGuid -cne $InstanceGuid){throw 'Unowned task instance; no stop/delete'}}
    $active
}
