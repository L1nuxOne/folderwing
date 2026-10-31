// Real WebView helper and bookmark bridge, including parent keyboard forwarding.
using System;
using System.Diagnostics;
using System.Drawing;
using System.Windows.Forms;
using System.Runtime.InteropServices;
internal sealed class NavigationBridgeTests : Form
{
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr target,int message,IntPtr w,IntPtr l);
    readonly string helper,file,store,identity;readonly Timer timer=new Timer();Process child;IntPtr preview;int phase,ticks,readyAt;bool rendered,passed;
    NavigationBridgeTests(string[] args){helper=args[0];file=args[1];store=args[2];identity=args[3];ShowInTaskbar=false;StartPosition=FormStartPosition.Manual;Location=new Point(-32000,-32000);Size=new Size(650,600);}
    protected override void OnShown(EventArgs e){base.OnShown(e);child=Process.Start(new ProcessStartInfo(helper,Handle.ToInt64()+" 4194304 \""+file+"\" \""+store+"\" \""+identity+"\""){UseShellExecute=false});timer.Interval=100;timer.Tick+=Tick;timer.Start();}
    protected override void WndProc(ref Message m){if(m.Msg==0x8053)preview=m.WParam;if(m.Msg==0x8055){rendered=true;readyAt=ticks;}if(m.Msg==0x8054){Console.Error.WriteLine("Real Markdown helper failed");Close();}base.WndProc(ref m);}
    void Tick(object s,EventArgs e){try{if(++ticks>400||child.HasExited)throw new Exception("Bridge timeout at phase "+phase);if(preview==IntPtr.Zero||!rendered||ticks<readyAt+8)return;var marks=new NavigationStore(store,identity).Load("markdown");
        if(phase==0){if(marks.Count!=0)throw new Exception("Fixture store not empty");PostMessage(preview,0x8000+90,IntPtr.Zero,IntPtr.Zero);phase=1;}
        else if(phase==1&&marks.Count==1){Console.WriteLine("PASS real WebView -> checksummed store, supplied archive identity, parent Ctrl+D dispatch");PostMessage(preview,0x8000+90,IntPtr.Zero,IntPtr.Zero);phase=2;}
        else if(phase==2&&marks.Count==0){Console.WriteLine("PASS real WebView bookmark removal");passed=true;Close();}
    }catch(Exception ex){Console.Error.WriteLine(ex);Close();}}
    protected override void OnFormClosed(FormClosedEventArgs e){timer.Stop();if(child!=null&&!child.HasExited){child.Kill();child.WaitForExit(3000);}Environment.ExitCode=passed?0:1;base.OnFormClosed(e);}
    [STAThread] static int Main(string[] args){if(args.Length!=4)return 2;Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new NavigationBridgeTests(args));return Environment.ExitCode;}
}
