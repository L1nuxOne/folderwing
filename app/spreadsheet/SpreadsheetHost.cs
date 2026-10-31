using System;
using System.IO;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

// Workbook parsing and WebView2 live in this on-demand process, outside the explorer.
internal sealed class SpreadsheetHost : Form
{
    const int Ready=0x8000+83, Failed=0x8000+84;
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child,IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr target,int message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SetWindowLongW")] static extern int SetWindowLong(IntPtr window,int index,int value);
    [DllImport("user32.dll",EntryPoint="GetWindowLongW")] static extern int GetWindowLong(IntPtr window,int index);
    readonly BrowserNavigation navigation;
    readonly IntPtr parent;readonly string path;readonly WebView2 browser=new WebView2();readonly Timer lifetime=new Timer();
    SpreadsheetReader workbook;bool busy,started;
    readonly JavaScriptSerializer serializer=new JavaScriptSerializer {MaxJsonLength=24*1024*1024};
    SpreadsheetHost(IntPtr p,string f,NavigationStore marks){parent=p;path=f;navigation=new BrowserNavigation(marks);FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;StartPosition=FormStartPosition.Manual;Location=new Point(-32000,-32000);browser.Dock=DockStyle.Fill;Controls.Add(browser);lifetime.Interval=1000;lifetime.Tick+=delegate{if(!IsWindow(parent))Close();};Shown+=Start;}
    void Send(object value){if(!IsDisposed && browser.CoreWebView2!=null)browser.CoreWebView2.PostWebMessageAsJson(serializer.Serialize(value));}
    async void Start(object s,EventArgs a){
        try {
            SetWindowLong(Handle,-16,(GetWindowLong(Handle,-16)&~unchecked((int)0x80000000))|0x40000000);SetParent(Handle,parent);
            if(GetParent(Handle)!=parent)throw new InvalidOperationException("Cannot attach preview.");lifetime.Start();
            string assets=Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"spreadsheet");
            string profile=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Folderwing","WebView2Spreadsheet");
            var env=await CoreWebView2Environment.CreateAsync(null,profile);await browser.EnsureCoreWebView2Async(env);
            var core=browser.CoreWebView2;core.Settings.AreDefaultContextMenusEnabled=false;core.Settings.AreDevToolsEnabled=false;core.Settings.IsStatusBarEnabled=false;core.Settings.AreHostObjectsAllowed=false;
            core.SetVirtualHostNameToFolderMapping("folderwing-spreadsheet.invalid",assets,CoreWebView2HostResourceAccessKind.DenyCors);
            string page="https://folderwing-spreadsheet.invalid/index.html";
            core.NavigationStarting+=delegate(object sender,CoreWebView2NavigationStartingEventArgs e){if(e.Uri!=page)e.Cancel=true;};
            core.NewWindowRequested+=delegate(object sender,CoreWebView2NewWindowRequestedEventArgs e){e.Handled=true;};
            core.WebMessageReceived+=async delegate(object sender,CoreWebView2WebMessageReceivedEventArgs e){
                if(e.Source!=page)return;
                try {
                    string msg=e.TryGetWebMessageAsString();if(await navigation.Handle(core,msg))return;
                    if(msg.StartsWith("table:",StringComparison.Ordinal)){string[] p=msg.Split(':');int n,err;if(p.Length==3 && Int32.TryParse(p[1],out n) && Int32.TryParse(p[2],out err))PostMessage(parent,0x8000+85,new IntPtr(n),new IntPtr(err));return;}
                    if(msg=="ready" && !started){
                        started=true;PostMessage(parent,Ready,Handle,IntPtr.Zero);busy=true;
                        try {var loaded=await Task.Run(delegate {return new SpreadsheetReader(WorkbookPackage.Snapshot(path));});if(IsDisposed){loaded.Dispose();return;}workbook=loaded;Send(new {kind="workbook",sheets=workbook.Package.Sheets});}
                        catch(Exception ex){Send(new {kind="error",message=Message(ex)});}finally{busy=false;}return;
                    }
                    if(busy || workbook==null)return;
                    if(msg=="next" || msg.StartsWith("sheet:",StringComparison.Ordinal)){
                        int sheet=-1;if(msg!="next" && (!Int32.TryParse(msg.Substring(6),out sheet)||sheet<0||sheet>=workbook.Package.Sheets.Count))return;
                        busy=true;try {var batch=await Task.Run(delegate {if(sheet>=0)workbook.Select(sheet);return workbook.Next();});Send(batch);}
                        catch(Exception ex){Send(new {kind="error",message=Message(ex)});}finally{busy=false;}return;
                    }
                }catch(Exception ex){Send(new {kind="error",message=Message(ex)});}
            };
            core.Navigate(page);
        } catch {PostMessage(parent,Failed,Handle,IntPtr.Zero);Close();}
    }
    static string Message(Exception ex){string message=ex.Message;if(ex is OutOfMemoryException)message="Workbook exceeds available preview memory.";if(message.Length>400)message=message.Substring(0,400);return "Could not preview this workbook: "+message;}
    protected override void OnFormClosed(FormClosedEventArgs e){if(!busy && workbook!=null)workbook.Dispose();base.OnFormClosed(e);}
    protected override void WndProc(ref Message message){if(BrowserNavigation.Dispatch(browser.CoreWebView2,message.Msg))return;base.WndProc(ref message);}
    [STAThread] static int Main(string[] args){long target;int limit;if((args.Length!=3&&args.Length!=5) || !Int64.TryParse(args[0],out target) || !Int32.TryParse(args[1],out limit) || limit<1)return 2;IntPtr p=new IntPtr(target);if(!IsWindow(p))return 2;Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new SpreadsheetHost(p,args[2],NavigationStore.FromArgs(args,3,args[2])));return 0;}
}
