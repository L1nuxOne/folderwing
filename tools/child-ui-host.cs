// Ordinary-user, loopback-only RDP child-session host. Built from the local
// Windows type library; authentication stays in Windows' credential dialog.
using System;
using System.IO;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using OpenPilotRdp;

sealed class ChildRdpControl : AxHost {
    public ChildRdpControl() : base("8B918B82-7985-4C24-89DF-C33AD2BBFBCD") { }
    public object Ocx { get { return GetOcx(); } }
}
sealed class ChildUiHost : Form {
    readonly string root;
    readonly Dictionary<string,object> lease;
    readonly JavaScriptSerializer json=new JavaScriptSerializer();
    readonly Stopwatch clock=Stopwatch.StartNew();
    readonly ChildRdpControl box=new ChildRdpControl();
    readonly Timer timer=new Timer();
    readonly int parent=Process.GetCurrentProcess().SessionId;
    IMsRdpClient9 rdp;
    IMsRdpExtendedSettings extended;
    uint created=UInt32.MaxValue;
    bool loggedIn,closing,failed,requested;

    [DllImport("wtsapi32.dll",SetLastError=true)]static extern bool WTSIsChildSessionsEnabled(out bool value);
    [DllImport("wtsapi32.dll",SetLastError=true)]static extern bool WTSGetChildSessionId(out uint id);
    [DllImport("wtsapi32.dll",SetLastError=true)]static extern bool WTSLogoffSession(IntPtr server,uint id,bool wait);
    [DllImport("kernel32.dll")]static extern uint WTSGetActiveConsoleSessionId();
    protected override bool ShowWithoutActivation { get { return true; } }
    protected override CreateParams CreateParams { get { var p=base.CreateParams;p.ExStyle|=0x08000000;return p; } }
    void Require(bool condition,string message){if(!condition)throw new InvalidOperationException(message);}
    void Log(string phase,object detail){File.AppendAllText(Path.Combine(root,"host-events.jsonl"),json.Serialize(new{utc=DateTime.UtcNow.ToString("o"),phase=phase,ms=clock.ElapsedMilliseconds,detail=detail})+Environment.NewLine);}
    void New(string name,object value){string target=Path.Combine(root,name),temporary=target+".pending";using(var w=new StreamWriter(new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.Read))){w.Write(json.Serialize(value));}File.Move(temporary,target);}
    bool LauncherAlive(){try{var p=Process.GetProcessById(Convert.ToInt32(lease["launcher_pid"]));return p.SessionId==parent&&p.StartTime.ToUniversalTime().Ticks==Convert.ToInt64(lease["launcher_birth_ticks"]);}catch(ArgumentException){return false;}}
    public ChildUiHost(string path){
        root=Path.GetDirectoryName(Path.GetFullPath(path));
        lease=json.Deserialize<Dictionary<string,object>>(File.ReadAllText(path));
        Require((string)lease["schema"]=="openpilot-child-ui/v1"&&Path.GetFileName(root)==(string)lease["token"],"Invalid run identity");
        Require(System.Security.Principal.WindowsIdentity.GetCurrent().User.Value==(string)lease["sid"],"Wrong Windows account");
        Require(!new System.Security.Principal.WindowsPrincipal(System.Security.Principal.WindowsIdentity.GetCurrent()).IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator),"Host must run with an ordinary token");
        Text="OpenPilot isolated UI tests (view only)";Width=520;Height=350;
        var a=Screen.PrimaryScreen.WorkingArea;StartPosition=FormStartPosition.Manual;Left=a.Right-Width;Top=a.Bottom-Height;
        box.Dock=DockStyle.Fill;box.Enabled=false;box.TabStop=false;Controls.Add(box);
        Shown+=delegate{try{ConnectChild();}catch(Exception e){Fail(e);}};
        timer.Interval=500;timer.Tick+=delegate{try{TickChild();}catch(Exception e){Fail(e);}};
        FormClosing+=delegate{Cleanup();};
    }
    void ConnectChild(){
        Require(parent==Convert.ToInt32(lease["parent_session"])&&WTSGetActiveConsoleSessionId()==parent&&LauncherAlive(),"Original parent session/launcher changed");
        bool enabled;Require(WTSIsChildSessionsEnabled(out enabled)&&enabled,"Child-session feature unavailable");
        uint prior;Require(!WTSGetChildSessionId(out prior)&&prior==UInt32.MaxValue,"Existing child session: never adopt");
        rdp=(IMsRdpClient9)box.Ocx;extended=(IMsRdpExtendedSettings)box.Ocx;
        object yes=true;extended.set_Property("ConnectToChildSession",ref yes);
        Require(Convert.ToBoolean(extended.get_Property("ConnectToChildSession")),"Loopback child mode readback failed");
        rdp.Server="localhost";
        string account=System.Security.Principal.WindowsIdentity.GetCurrent().Name;
        int split=account.IndexOf('\\');rdp.Domain=split<0?"":account.Substring(0,split);rdp.UserName=split<0?account:account.Substring(split+1);
        rdp.DesktopWidth=Convert.ToInt32(lease["desktop_width"]);rdp.DesktopHeight=Convert.ToInt32(lease["desktop_height"]);rdp.ColorDepth=32;
        var settings=rdp.AdvancedSettings9;
        settings.EnableCredSspSupport=true;settings.GrabFocusOnConnect=false;settings.SmartSizing=true;
        settings.RedirectClipboard=false;settings.RedirectDrives=false;settings.RedirectPrinters=false;settings.RedirectPorts=false;settings.RedirectSmartCards=false;
        Require(!settings.RedirectClipboard&&settings.EnableCredSspSupport,"Clipboard/authentication settings did not apply");
        Guid source=typeof(IMsTscAxEvents).GUID;
        ComEventsHelper.Combine(box.Ocx,source,2,new Action(delegate{CaptureChild();Log("connected",null);}));
        ComEventsHelper.Combine(box.Ocx,source,3,new Action(delegate{loggedIn=true;Log("login-complete",null);}));
        ComEventsHelper.Combine(box.Ocx,source,4,new Action<int>(delegate(int code){if(!closing)CaptureChild();Log("disconnected",new{code=code,description=rdp.GetErrorDescription((uint)code,(uint)rdp.ExtendedDisconnectReason)});if(!closing){failed=true;Close();}}));
        ComEventsHelper.Combine(box.Ocx,source,10,new Action<int>(delegate(int code){Log("fatal-error",new{code=code});failed=true;Close();}));
        ComEventsHelper.Combine(box.Ocx,source,22,new Action<int>(delegate(int code){Log("logon-event",new{code=code});}));
        // Check again immediately before the one connection request.
        Require(!WTSGetChildSessionId(out prior)&&prior==UInt32.MaxValue,"Child appeared before connect; leave untouched");
        timer.Start();requested=true;rdp.Connect();Log("connect-requested",new{parent_session=parent,clipboard_redirection=false});
    }
    void CaptureChild(){
        if(!requested)return;
        uint child;if(WTSGetChildSessionId(out child)&&child!=UInt32.MaxValue){
            Require(child!=parent,"Child cannot be the parent session");
            if(created==UInt32.MaxValue){created=child;Log("created-child",new{session=child});}
            Require(child==created,"Child session changed; never adopt replacement");
        }
    }
    void TickChild(){
        Require(WTSGetActiveConsoleSessionId()==parent&&LauncherAlive(),"Parent session/launcher ended");
        CaptureChild();
        if(loggedIn&&created!=UInt32.MaxValue&&!File.Exists(Path.Combine(root,"host-ready.json"))){
            var p=Process.GetCurrentProcess();
            New("host-ready.json",new{token=lease["token"],pid=p.Id,birth_ticks=p.StartTime.ToUniversalTime().Ticks,parent_session=parent,child_session=created,child_mode=true,redirect_clipboard=rdp.AdvancedSettings9.RedirectClipboard});
        }
        if(File.Exists(Path.Combine(root,"stop-host"))){Close();return;}
        Require(loggedIn||clock.Elapsed.TotalSeconds<Convert.ToInt32(lease["connect_seconds"]),"Child connection deadline: normal Windows sign-in required");
        Require(clock.Elapsed.TotalSeconds<Convert.ToInt32(lease["host_seconds"]),"Bounded child-session lifetime exceeded");
    }
    public void Fail(Exception e){failed=true;Log("error",new{type=e.GetType().FullName,message=e.Message});Close();}
    public void Cleanup(){
        if(closing)return;closing=true;timer.Stop();bool off=false;int error=0;
        try{
            CaptureChild();
            uint current;
            if(created!=UInt32.MaxValue){
                Require(WTSGetActiveConsoleSessionId()==parent,"Parent console changed; leave sessions untouched");
                Require(WTSGetChildSessionId(out current)&&current==created&&current!=parent,"Owned child changed or absent; no logoff of another session");
                off=WTSLogoffSession(IntPtr.Zero,current,true);if(!off)error=Marshal.GetLastWin32Error();
            }
            if(rdp!=null&&rdp.Connected!=0)rdp.Disconnect();
        }catch(Exception e){failed=true;Log("cleanup-error",new{message=e.Message});}
        if(created!=UInt32.MaxValue&&!off)failed=true;
        New("host-result.json",new{outcome=failed?"blocked":"completed",token=lease["token"],child_session=created,owned_child_logged_off=off,logoff_error=error});
    }
    [STAThread]static void Main(string[] args){
        if(args.Length!=1){Environment.ExitCode=2;return;}
        ChildUiHost host=null;
        try{Application.SetUnhandledExceptionMode(UnhandledExceptionMode.ThrowException);Application.EnableVisualStyles();host=new ChildUiHost(args[0]);Application.Run(host);}
        catch(Exception e){File.WriteAllText(Path.Combine(Path.GetDirectoryName(Path.GetFullPath(args[0])),"host-unhandled.txt"),e.ToString());Environment.ExitCode=1;}
        finally{if(host!=null)host.Cleanup();}
    }
}
