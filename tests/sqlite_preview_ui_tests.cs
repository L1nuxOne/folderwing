// Exercises the shipped helper in an owned, off-screen parent without desktop input.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
internal sealed class SqliteUiProbe : Form
{
    delegate bool EnumProc(IntPtr hwnd,IntPtr state);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent,EnumProc callback,IntPtr state);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr hwnd,StringBuilder text,int max);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr SendMessageW(IntPtr hwnd,int message,IntPtr w,string text);
    [DllImport("user32.dll")] static extern IntPtr SendMessageW(IntPtr hwnd,int message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr hwnd,StringBuilder text,int max);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr hwnd,IntPtr after,int x,int y,int cx,int cy,uint flags);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_open_v2(byte[] path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close_v2(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_exec(IntPtr db,byte[] sql,IntPtr cb,IntPtr ctx,out IntPtr error);
    static byte[] Utf8(string text){return Encoding.UTF8.GetBytes(text+"\0");}
    readonly string helper,file;readonly bool archive;readonly Timer timer=new Timer();
    Process process;IntPtr child;int ticks,phase;bool passed;
    SqliteUiProbe(string exe,string source,bool fromArchive){helper=exe;file=source;archive=fromArchive;StartPosition=FormStartPosition.Manual;Location=new Point(-32000,-32000);Size=new Size(960,600);ShowInTaskbar=false;}
    protected override void OnShown(EventArgs e)
    {
        base.OnShown(e);var info=new ProcessStartInfo(helper,Handle.ToInt64()+" 4194304 \""+file+"\" "+(archive?"1":"0"));info.UseShellExecute=false;process=Process.Start(info);
        timer.Interval=100;timer.Tick+=Tick;timer.Start();
    }
    protected override void WndProc(ref Message m)
    {
        if(m.Msg==0x8053){child=m.WParam;if(GetParent(child)!=Handle){Console.WriteLine("Child was not parented to the owned preview");Close();}else SetWindowPos(child,IntPtr.Zero,0,0,900,500,0x0010|0x0004|0x0040);}
        if(m.Msg==0x8054){Console.WriteLine("Database helper failed");Close();}base.WndProc(ref m);
    }
    static string TextOf(IntPtr window){var b=new StringBuilder(4096);GetWindowTextW(window,b,b.Capacity);return b.ToString();}
    Dictionary<string,IntPtr> Children(){var found=new Dictionary<string,IntPtr>();EnumChildWindows(child,delegate(IntPtr h,IntPtr unused){string text=TextOf(h);if(text.Length>0)found[text]=h;var cls=new StringBuilder(256);GetClassNameW(h,cls,cls.Capacity);if(cls.ToString().Contains("EDIT") && text.Length==0)found["<search>"]=h;return true;},IntPtr.Zero);return found;}
    void Tick(object sender,EventArgs args)
    {
        if(++ticks>300 || process.HasExited){Console.WriteLine("Database helper exited or timed out in phase "+phase);Close();return;}
        if(child==IntPtr.Zero)return;var windows=Children();
        if(phase==0 && windows.ContainsKey("Page 1 · 50 rows") && windows.ContainsKey("Next")){SendMessageW(windows["Next"],0x00F5,IntPtr.Zero,IntPtr.Zero);phase=1;}
        else if(phase==1 && windows.ContainsKey("Page 2 · 5 rows") && windows.ContainsKey("<search>") && windows.ContainsKey("Search")){
            SendMessageW(windows["<search>"],0x000C,IntPtr.Zero,"needle");SendMessageW(windows["Search"],0x00F5,IntPtr.Zero,IntPtr.Zero);phase=2;
        }
        else if(phase==2 && windows.ContainsKey("Page 1 · 1 rows")){
            Console.WriteLine("Embedded helper passed: "+(archive?"archive":"ordinary")+" path, 50-row page, Next, literal search.");passed=true;Close();
        }
    }
    protected override void OnFormClosed(FormClosedEventArgs e){timer.Stop();if(process!=null && !process.HasExited){process.Kill();process.WaitForExit(3000);}base.OnFormClosed(e);}
    static void CreateFixture(string file)
    {
        IntPtr db,error;int rc=sqlite3_open_v2(Utf8(file),out db,6,IntPtr.Zero);if(rc!=0)throw new Exception("fixture open "+rc);
        try {var sql=new StringBuilder("CREATE TABLE items(id INTEGER, label TEXT); BEGIN;");for(int i=0;i<55;i++)sql.Append("INSERT INTO items VALUES(").Append(i).Append(",'").Append(i==43?"needle":"row").Append("');");sql.Append("COMMIT;");rc=sqlite3_exec(db,Utf8(sql.ToString()),IntPtr.Zero,IntPtr.Zero,out error);if(rc!=0)throw new Exception("fixture setup "+rc);}finally{sqlite3_close_v2(db);}
    }
    [STAThread] static int Main(string[] args)
    {
        if(args.Length!=2)return 2;try{string file=Path.Combine(args[1],"sample.sqlite3");CreateFixture(file);Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            var normal=new SqliteUiProbe(args[0],file,false);Application.Run(normal);if(!normal.passed)return 1;
            using(var locked=File.Open(file,FileMode.Open,FileAccess.Read,FileShare.Read)){var archived=new SqliteUiProbe(args[0],@"\\?\"+file,true);Application.Run(archived);if(!archived.passed)return 1;}
            return 0;
        }catch(Exception ex){Console.Error.WriteLine(ex);return 1;}
    }
}
