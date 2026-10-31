// Owns only its off-screen parent and the Markdown child it starts.
using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;
using System.Runtime.InteropServices;
internal sealed class Probe : Form
{
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    readonly string helper, file;
    readonly int expected, expectedErrors;
    Process child;
    readonly Timer timer = new Timer();
    int seconds;
    bool ready, passed;
    Probe(string exe, string path, int count, int errors) {
        helper=exe; file=path; expected=count; expectedErrors=errors;
        StartPosition=FormStartPosition.Manual; Location=new Point(-32000,-32000);
        Size=new Size(600,600); ShowInTaskbar=false;
    }
    protected override void OnShown(EventArgs e) {
        base.OnShown(e);
        var info = new ProcessStartInfo(helper, Handle.ToInt64()+" 4194304 \""+file+"\"");
        info.UseShellExecute=false; child=Process.Start(info);
        timer.Interval=1000; timer.Tick+=delegate {
            if (++seconds>40 || child.HasExited) { Console.WriteLine("Markdown host exited or timed out"); Close(); }
        }; timer.Start();
    }
    protected override void WndProc(ref Message m) {
        if(m.Msg==0x8053) ready=true;
        else if(m.Msg==0x8054) { Console.WriteLine("Markdown host failed"); Close(); }
        else if(m.Msg==0x8055) {
            int count=m.WParam.ToInt32(), errors=m.LParam.ToInt32();
            passed=ready && count==expected && errors==expectedErrors;
            Console.WriteLine("Rendered items: "+count+"; errors: "+errors);
            Close();
        }
        base.WndProc(ref m);
    }
    protected override void OnFormClosed(FormClosedEventArgs e) {
        timer.Stop(); if(child!=null && !child.HasExited) child.Kill();
        Environment.ExitCode=passed?0:1; base.OnFormClosed(e);
    }
    [STAThread] static int Main(string[] args) {
        int count, errors=0;
        if((args.Length!=3 && args.Length!=4) || !Int32.TryParse(args[2],out count) || (args.Length==4 && !Int32.TryParse(args[3],out errors))) return 2;
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        Application.Run(new Probe(args[0],args[1],count,errors));
        return Environment.ExitCode;
    }
}
