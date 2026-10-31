using System;
using System.IO;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

// Untrusted Word packages and their browser renderer live in this on-demand child.
internal sealed class WordHost : Form
{
    const int Ready=0x8000+83,Failed=0x8000+84;
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child,IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr target,int message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SetWindowLongW")] static extern int SetWindowLong(IntPtr window,int index,int value);
    [DllImport("user32.dll",EntryPoint="GetWindowLongW")] static extern int GetWindowLong(IntPtr window,int index);
    readonly BrowserNavigation navigation;
    readonly IntPtr parent;readonly string path;readonly WebView2 browser=new WebView2();readonly Timer lifetime=new Timer();bool started;
    readonly JavaScriptSerializer serializer=new JavaScriptSerializer {MaxJsonLength=50*1024*1024};
    WordHost(IntPtr p,string f,NavigationStore marks){parent=p;path=f;navigation=new BrowserNavigation(marks);FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;StartPosition=FormStartPosition.Manual;Location=new Point(-32000,-32000);browser.Dock=DockStyle.Fill;Controls.Add(browser);lifetime.Interval=1000;lifetime.Tick+=delegate{if(!IsWindow(parent))Close();};Shown+=Start;}
    void Send(object value){if(!IsDisposed&&browser.CoreWebView2!=null)browser.CoreWebView2.PostWebMessageAsJson(serializer.Serialize(value));}
    async void Start(object sender,EventArgs args){try {
        SetWindowLong(Handle,-16,(GetWindowLong(Handle,-16)&~unchecked((int)0x80000000))|0x40000000);SetParent(Handle,parent);if(GetParent(Handle)!=parent)throw new InvalidOperationException("Could not attach preview.");lifetime.Start();
        string assets=AppDomain.CurrentDomain.BaseDirectory,profile=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Folderwing","WebView2Word");
        var env=await CoreWebView2Environment.CreateAsync(null,profile);await browser.EnsureCoreWebView2Async(env);var core=browser.CoreWebView2;
        core.Settings.AreDefaultContextMenusEnabled=false;core.Settings.AreDevToolsEnabled=false;core.Settings.IsStatusBarEnabled=false;core.Settings.AreHostObjectsAllowed=false;
        core.SetVirtualHostNameToFolderMapping("folderwing-word.invalid",assets,CoreWebView2HostResourceAccessKind.DenyCors);
        string page="https://folderwing-word.invalid/word/index.html";
        core.NavigationStarting+=delegate(object s,CoreWebView2NavigationStartingEventArgs e){if(e.Uri!=page)e.Cancel=true;};
        core.FrameNavigationStarting+=delegate(object s,CoreWebView2NavigationStartingEventArgs e){e.Cancel=true;};
        core.NewWindowRequested+=delegate(object s,CoreWebView2NewWindowRequestedEventArgs e){e.Handled=true;};
        core.PermissionRequested+=delegate(object s,CoreWebView2PermissionRequestedEventArgs e){e.State=CoreWebView2PermissionState.Deny;};
        core.DownloadStarting+=delegate(object s,CoreWebView2DownloadStartingEventArgs e){e.Cancel=true;};
        core.AddWebResourceRequestedFilter("*",CoreWebView2WebResourceContext.All);
        core.WebResourceRequested+=delegate(object s,CoreWebView2WebResourceRequestedEventArgs e){string uri=e.Request.Uri;if(!uri.StartsWith("https://folderwing-word.invalid/",StringComparison.OrdinalIgnoreCase) && !uri.StartsWith("data:",StringComparison.OrdinalIgnoreCase) && !uri.StartsWith("blob:",StringComparison.OrdinalIgnoreCase))e.Response=env.CreateWebResourceResponse(null,403,"Blocked","");};
        core.WebMessageReceived+=async delegate(object s,CoreWebView2WebMessageReceivedEventArgs e){
            if(e.Source!=page)return;
            try{string message=e.TryGetWebMessageAsString();if(await navigation.Handle(core,message))return;
                if(message.StartsWith("word:",StringComparison.Ordinal)){string[] p=message.Split(':');int n,err;if(p.Length==3 && Int32.TryParse(p[1],out n) && Int32.TryParse(p[2],out err))PostMessage(parent,0x8000+85,new IntPtr(n),new IntPtr(err));return;}
                if(message!="ready"||started)return;started=true;PostMessage(parent,Ready,Handle,IntPtr.Zero);
                try {var package=await Task.Run(delegate{return WordPackage.Prepare(WordPackage.Snapshot(path));});Send(new {kind="document",data=Convert.ToBase64String(package.Bytes),notices=package.Notices});}
                catch(Exception ex){string text=ex is OutOfMemoryException?"Document exceeds available preview memory.":ex.Message;if(text.Length>400)text=text.Substring(0,400);Send(new {kind="error",message="Could not preview this document: "+text});}
            }catch {PostMessage(parent,Failed,Handle,IntPtr.Zero);}
        };
        core.Navigate(page);
    }catch {PostMessage(parent,Failed,Handle,IntPtr.Zero);Close();}}
    protected override void WndProc(ref Message message){if(BrowserNavigation.Dispatch(browser.CoreWebView2,message.Msg))return;base.WndProc(ref message);}
    [STAThread] static int Main(string[] args){long target;int limit;if((args.Length!=3&&args.Length!=5)||!Int64.TryParse(args[0],out target)||!Int32.TryParse(args[1],out limit)||limit<1)return 2;IntPtr p=new IntPtr(target);if(!IsWindow(p))return 2;Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new WordHost(p,args[2],NavigationStore.FromArgs(args,3,args[2])));return 0;}
}
