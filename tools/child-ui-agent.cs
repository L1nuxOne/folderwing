// Fixed child-only bootstrap: create a private desktop before starting PS7.
// Startup applications stay on Default; physical input remains in the parent.
using System;
using System.IO;
using System.Text;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Web.Script.Serialization;
class ChildUiAgent {
 [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)]struct STARTUP {public int cb;public string reserved,desktop,title;public int x,y,w,h,cx,cy,fill,flags;public short show,reserved2;public IntPtr reservedPtr,input,output,error;}
 [StructLayout(LayoutKind.Sequential)]struct PROCESS {public IntPtr process,thread;public int pid,tid;}
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr CreateDesktopW(string name,IntPtr device,IntPtr mode,uint flags,uint access,IntPtr security);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern IntPtr OpenDesktopW(string name,uint flags,bool inherit,uint access);
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr OpenInputDesktop(uint flags,bool inherit,uint access);
 [DllImport("user32.dll",SetLastError=true)]static extern bool SwitchDesktop(IntPtr desktop);
 [DllImport("user32.dll")]static extern bool CloseDesktop(IntPtr desktop);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool GetUserObjectInformationW(IntPtr h,int n,StringBuilder text,uint bytes,out uint needed);
 [DllImport("kernel32.dll")]static extern uint WTSGetActiveConsoleSessionId();
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool CreateProcessW(string image,StringBuilder command,IntPtr processSecurity,IntPtr threadSecurity,bool inherit,uint flags,IntPtr env,string directory,ref STARTUP startup,out PROCESS process);
 [DllImport("kernel32.dll",SetLastError=true)]static extern bool SetHandleInformation(IntPtr h,uint mask,uint flags);
 [DllImport("kernel32.dll")]static extern uint WaitForSingleObject(IntPtr handle,uint ms);
 [DllImport("kernel32.dll")]static extern bool GetExitCodeProcess(IntPtr handle,out uint code);
 [DllImport("kernel32.dll")]static extern bool CloseHandle(IntPtr handle);
 [DllImport("advapi32.dll")]static extern uint GetSecurityInfo(IntPtr h,int type,uint info,out IntPtr owner,out IntPtr group,out IntPtr dacl,out IntPtr sacl,out IntPtr descriptor);
 [DllImport("advapi32.dll")]static extern uint SetSecurityInfo(IntPtr h,int type,uint info,IntPtr owner,IntPtr group,IntPtr dacl,IntPtr sacl);
 [DllImport("advapi32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool ConvertSecurityDescriptorToStringSecurityDescriptorW(IntPtr descriptor,uint revision,uint info,out IntPtr text,out uint length);
 [DllImport("kernel32.dll")]static extern IntPtr LocalFree(IntPtr memory);
 static JavaScriptSerializer json=new JavaScriptSerializer();
 static string Name(IntPtr desktop){uint needed;var s=new StringBuilder(256);if(desktop==IntPtr.Zero||!GetUserObjectInformationW(desktop,2,s,512,out needed))throw new Exception("Desktop identity unavailable");return s.ToString();}
 static void Require(bool ok,string why){if(!ok)throw new Exception(why);}
 static void Write(string root,string name,object value){string target=Path.Combine(root,name),temporary=target+".pending";using(var w=new StreamWriter(new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.Read))){w.Write(json.Serialize(value));}File.Move(temporary,target);}
 static string Quote(string value){Require(value.IndexOf('"')<0&&value.IndexOf('\r')<0&&value.IndexOf('\n')<0,"Invalid owned path");return "\""+value+"\"";}
 static string SecurityText(IntPtr handle,uint info){
  IntPtr owner,group,dacl,sacl,sd,text=IntPtr.Zero;uint length;
  uint result=GetSecurityInfo(handle,7,info,out owner,out group,out dacl,out sacl,out sd);Require(result==0,"Desktop security read failed: "+result);
  try{Require(ConvertSecurityDescriptorToStringSecurityDescriptorW(sd,1,info,out text,out length),"Desktop security serialization failed");return Marshal.PtrToStringUni(text);}finally{if(text!=IntPtr.Zero)LocalFree(text);LocalFree(sd);}
 }
 static void CopyIntegrityLabel(IntPtr original,IntPtr owned,string root){
  string label=SecurityText(original,16),before=SecurityText(owned,16),daclBefore=SecurityText(owned,4);
  IntPtr user,group,dacl,sacl,sd;
  uint result=GetSecurityInfo(original,7,16,out user,out group,out dacl,out sacl,out sd);Require(result==0,"Default desktop label unavailable: "+result);
  try{
   Require(sacl!=IntPtr.Zero&&!String.IsNullOrEmpty(label),"Default desktop has no explicit template label; refuse invented policy");
   // Match only the new owned desktop to the existing interactive desktop's
   // integrity label. Default, its DACL, tokens and host policy are untouched.
   result=SetSecurityInfo(owned,7,16,IntPtr.Zero,IntPtr.Zero,IntPtr.Zero,sacl);Require(result==0,"Owned desktop label failed: "+result);
  }finally{LocalFree(sd);}
  string after=SecurityText(owned,16),daclAfter=SecurityText(owned,4);
  Require(after==label&&daclBefore==daclAfter,"Desktop label readback or DACL preservation differs");
  Write(root,"desktop-security.json",new{template_label=label,owned_before=before,owned_after=after,dacl_unchanged=true,template_modified=false});
 }
 static void Main(string[] args){
  if(args.Length!=1){Environment.ExitCode=2;return;}
  string root=Path.GetDirectoryName(Path.GetFullPath(args[0]));IntPtr prior=IntPtr.Zero,owned=IntPtr.Zero;bool switched=false,restored=false;string error=null;uint exit=1;PROCESS process=new PROCESS();
  try{
   var lease=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(args[0]));
   string token=(string)lease["token"],repo=Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"..","..",".."));
   Require((string)lease["schema"]=="openpilot-child-ui/v1"&&System.Text.RegularExpressions.Regex.IsMatch(token,"^[a-f0-9]{32}$")&&string.Equals(root,Path.Combine(repo,"build","child-ui",token),StringComparison.OrdinalIgnoreCase),"Owned lease root required");
   var ready=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(root,"host-ready.json")));
   int session=Process.GetCurrentProcess().SessionId,parent=Convert.ToInt32(lease["parent_session"]);
   Require(System.Security.Principal.WindowsIdentity.GetCurrent().User.Value==(string)lease["sid"]&&session>0&&session!=parent&&session==Convert.ToInt32(ready["child_session"])&&WTSGetActiveConsoleSessionId()==parent,"Exact child account/session required before desktop mutation");
   Require((string)ready["token"]==token&&Convert.ToBoolean(ready["child_mode"])&&!Convert.ToBoolean(ready["redirect_clipboard"]),"Isolated authenticated host required");
   using(var host=Process.GetProcessById(Convert.ToInt32(ready["pid"]))){Require(host.SessionId==parent&&host.StartTime.ToUniversalTime().Ticks==Convert.ToInt64(ready["birth_ticks"])&&string.Equals(host.MainModule.FileName,(string)lease["host_image"],StringComparison.OrdinalIgnoreCase),"Host identity changed");}
   if(lease.ContainsKey("session_root")){
    string sessionToken=(string)lease["session_token"],sessionRoot=(string)lease["session_root"];
    Require(System.Text.RegularExpressions.Regex.IsMatch(sessionToken,"^[a-f0-9]{32}$")&&string.Equals(sessionRoot,Path.Combine(repo,"build","child-ui",sessionToken),StringComparison.OrdinalIgnoreCase)&&!File.Exists(Path.Combine(sessionRoot,"FINAL.json")),"Owned live reusable session required");
    var owner=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(sessionRoot,"lease.json")));
    var original=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(sessionRoot,"host-ready.json")));
    Require((string)owner["token"]==sessionToken&&(string)original["token"]==sessionToken&&(string)ready["session_token"]==sessionToken&&(string)owner["sid"]==(string)lease["sid"]&&Convert.ToInt32(owner["parent_session"])==parent&&Convert.ToInt32(original["pid"])==Convert.ToInt32(ready["pid"])&&Convert.ToInt64(original["birth_ticks"])==Convert.ToInt64(ready["birth_ticks"])&&Convert.ToInt32(original["child_session"])==session,"Reusable host binding changed");
    using(var controller=Process.GetProcessById(Convert.ToInt32(owner["launcher_pid"]))){Require(controller.SessionId==parent&&controller.StartTime.ToUniversalTime().Ticks==Convert.ToInt64(owner["launcher_birth_ticks"])&&string.Equals(controller.MainModule.FileName,(string)owner["powershell"],StringComparison.OrdinalIgnoreCase),"Reusable controller identity changed");}
   }
   prior=OpenInputDesktop(0,false,0x20101);Require(Name(prior)=="Default","Child input desktop is not Default; do not replace it");
   string desktop="OpenPilotUi-"+token;
   IntPtr existing=OpenDesktopW(desktop,0,false,1);if(existing!=IntPtr.Zero){CloseDesktop(existing);throw new Exception("Private desktop already exists; never adopt");}
   owned=CreateDesktopW(desktop,IntPtr.Zero,IntPtr.Zero,0,0xF01FF,IntPtr.Zero);Require(owned!=IntPtr.Zero,"CreateDesktop failed: "+Marshal.GetLastWin32Error());
   CopyIntegrityLabel(prior,owned,root);
   Require(SwitchDesktop(owned),"SwitchDesktop failed: "+Marshal.GetLastWin32Error());switched=true;
   Write(root,"desktop-ready.json",new{token=token,session=session,name=desktop,prior="Default",pid=Process.GetCurrentProcess().Id});
   using(var stdout=new FileStream(Path.Combine(root,"worker.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read))
   using(var stderr=new FileStream(Path.Combine(root,"worker.stderr.log"),FileMode.CreateNew,FileAccess.Write,FileShare.Read)){
    IntPtr output=stdout.SafeFileHandle.DangerousGetHandle(),err=stderr.SafeFileHandle.DangerousGetHandle();
    Require(SetHandleInformation(output,1,1)&&SetHandleInformation(err,1,1),"Owned output handle inheritance failed");
    var startup=new STARTUP{cb=Marshal.SizeOf(typeof(STARTUP)),desktop="WinSta0\\"+desktop,flags=0x101,show=0,output=output,error=err};
    string powershell=(string)lease["powershell"];
    var command=new StringBuilder(Quote(powershell)+" -NoProfile -STA -File "+Quote(Path.Combine(repo,"tools","child-ui-worker.ps1"))+" -LeasePath "+Quote(Path.GetFullPath(args[0])));
    Require(CreateProcessW(powershell,command,IntPtr.Zero,IntPtr.Zero,true,0x08000000,IntPtr.Zero,repo,ref startup,out process),"CreateProcess failed: "+Marshal.GetLastWin32Error());
    Write(root,"worker-process.json",new{pid=process.pid,session=session,desktop=desktop});
    Require(WaitForSingleObject(process.process,1500000)==0,"Worker outer containment deadline; owned child cleanup required");
    Require(GetExitCodeProcess(process.process,out exit),"Worker exit unavailable");
   }
  }catch(Exception e){error=e.ToString();exit=1;}
  finally{
   if(process.thread!=IntPtr.Zero)CloseHandle(process.thread);if(process.process!=IntPtr.Zero)CloseHandle(process.process);
   if(switched){try{IntPtr current=OpenInputDesktop(0,false,1);try{Require(Name(current)==Name(owned),"Private input desktop changed; no restoration of another desktop");}finally{if(current!=IntPtr.Zero)CloseDesktop(current);}restored=SwitchDesktop(prior);Require(restored,"Original child desktop restoration failed");}catch(Exception e){error=(error??"")+e.ToString();exit=1;}}
   if(owned!=IntPtr.Zero)CloseDesktop(owned);if(prior!=IntPtr.Zero)CloseDesktop(prior);
   Write(root,"agent-result.json",new{outcome=error==null?"completed":"blocked",error=error,worker_exit=exit,desktop_switched=switched,desktop_restored=restored});
  }
  Environment.ExitCode=exit==0?0:1;
 }
}
