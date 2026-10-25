// Fixture-only ordinary desktop input and native GDI capture. No forced focus.
using System;using System.IO;using System.Text;using System.Runtime.InteropServices;using System.Collections.Generic;
public static class OwnedSettingsInput {
 public static string InputFactsFile;
 static string DownKeys(){var a=new List<int>();for(int k=1;k<256;k++)if((GetAsyncKeyState(k)&0x8000)!=0)a.Add(k);return string.Join(",",a);}
 static void InputFacts(IntPtr main,IntPtr slot,uint owner,string phase){if(InputFactsFile!=null)File.AppendAllText(InputFactsFile,System.Text.Json.JsonSerializer.Serialize(new{Stage=phase,DownKeys=DownKeys(),Facts=Probe(main,slot,owner,8,80)},new System.Text.Json.JsonSerializerOptions{IncludeFields=true})+Environment.NewLine);}
 public static void OwnRelease(){var w=System.Diagnostics.Stopwatch.StartNew();while(Busy()&&w.ElapsedMilliseconds<100)System.Threading.Thread.Sleep(1);}

 [StructLayout(LayoutKind.Sequential)] public struct POINT {public int X,Y;}
 [StructLayout(LayoutKind.Sequential)] public struct RECT {public int L,T,R,B;}
 [StructLayout(LayoutKind.Sequential)] struct GUI {public uint Size,Flags;public IntPtr Active,Focus,Capture,Menu,Move,Caret;public RECT CaretRect;}
 [StructLayout(LayoutKind.Sequential)] struct MOUSE {public int X,Y;public uint Data,Flags,Time;public UIntPtr Extra;}
 [StructLayout(LayoutKind.Sequential)] struct KEY {public ushort Code,Scan;public uint Flags,Time;public UIntPtr Extra;}
 [StructLayout(LayoutKind.Explicit,Size=40)] struct INPUT {[FieldOffset(0)]public uint Type;[FieldOffset(8)]public MOUSE Mouse;[FieldOffset(8)]public KEY Key;}

 // A single transaction owns normalization, packet construction and the count
 // consumed by the remote helper acknowledgment. Pure contracts use a local
 // fake sender; no mutable sender override or global input fault switch exists.
 public sealed class InputTransaction {
  public int PacketCount,InsertedPackets,KeyDownPackets,CleanupPackets,CleanupInserted,Error,CleanupError;
  public bool Complete;
  public bool Acknowledged(long before,long observed){return Complete&&observed>=before+KeyDownPackets;}
 }
 delegate uint PacketSender(INPUT[] packets);
 static INPUT[] KeyPackets(ushort key,ushort[] modifiers){
  var mods=new List<ushort>();if(modifiers!=null)foreach(ushort m in modifiers){if(m!=16&&m!=17&&m!=18)throw new Exception("Unsupported fixture modifier");if(!mods.Contains(m))mods.Add(m);}
  var a=new List<INPUT>();foreach(ushort m in mods)a.Add(new INPUT{Type=1,Key=new KEY{Code=m}});
  a.Add(new INPUT{Type=1,Key=new KEY{Code=key}});a.Add(new INPUT{Type=1,Key=new KEY{Code=key,Flags=2}});
  for(int i=mods.Count-1;i>=0;i--)a.Add(new INPUT{Type=1,Key=new KEY{Code=mods[i],Flags=2}});return a.ToArray();
 }
 static int KeyDownCount(INPUT[] packets){int n=0;foreach(var p in packets)if(p.Type==1&&(p.Key.Flags&2)==0)n++;return n;}
 static bool SameRelease(INPUT a,INPUT b){return a.Type==b.Type&&(a.Type==1?(a.Key.Code==b.Key.Code&&a.Key.Scan==b.Key.Scan&&(a.Key.Flags&5)==(b.Key.Flags&5)):a.Mouse.Flags==b.Mouse.Flags);}
 static INPUT[] PrefixReleases(INPUT[] packets,int inserted){
  var held=new List<INPUT>();for(int i=0;i<Math.Min(inserted,packets.Length);i++){
   var p=packets[i];if(p.Type==1){var up=p;up.Key.Flags|=2;if((p.Key.Flags&2)==0)held.Add(up);else for(int q=held.Count-1;q>=0;q--)if(SameRelease(held[q],p)){held.RemoveAt(q);break;}}
   else if((p.Mouse.Flags&2)!=0)held.Add(new INPUT{Mouse=new MOUSE{Flags=4}});
   else if((p.Mouse.Flags&4)!=0)for(int q=held.Count-1;q>=0;q--)if(held[q].Type==0&&held[q].Mouse.Flags==4){held.RemoveAt(q);break;}
  }held.Reverse();return held.ToArray();
 }
 static InputTransaction Transact(INPUT[] packets,PacketSender sender,Func<int> lastError=null){
  var t=new InputTransaction{PacketCount=packets.Length};uint inserted=sender(packets);t.InsertedPackets=(int)Math.Min(inserted,(uint)packets.Length);t.Complete=inserted==packets.Length;if(!t.Complete&&lastError!=null)t.Error=lastError();
  var insertedPrefix=new INPUT[t.InsertedPackets];Array.Copy(packets,insertedPrefix,t.InsertedPackets);t.KeyDownPackets=KeyDownCount(insertedPrefix);
  if(!t.Complete){var cleanup=PrefixReleases(packets,t.InsertedPackets);t.CleanupPackets=cleanup.Length;if(cleanup.Length>0){t.CleanupInserted=(int)sender(cleanup);if(t.CleanupInserted!=cleanup.Length&&lastError!=null)t.CleanupError=lastError();}}
  return t;
 }
 static uint RealSend(INPUT[] packets){return SendInput((uint)packets.Length,packets,40);}
 static InputTransaction SendOwned(INPUT[] packets){var t=Transact(packets,RealSend,()=>Marshal.GetLastWin32Error());if(!t.Complete)throw new Exception("Owned input insertion failed: "+System.Text.Json.JsonSerializer.Serialize(t,new System.Text.Json.JsonSerializerOptions{IncludeFields=true}));return t;}
 public static InputTransaction DescribeKey(ushort key,ushort[] modifiers){var p=KeyPackets(key,modifiers);return new InputTransaction{PacketCount=p.Length,InsertedPackets=p.Length,KeyDownPackets=KeyDownCount(p),Complete=true};}
 static bool PointerOwned(POINT now,POINT expected,bool exactHit,bool idle){return exactHit&&idle&&now.X==expected.X&&now.Y==expected.Y;}
 public sealed class Contract {public string Name;public int Expected,Actual;public bool Passed;}
 public static Contract[] InputContracts(){
  var a=new List<Contract>();Action<string,int,int> check=(name,expected,actual)=>a.Add(new Contract{Name=name,Expected=expected,Actual=actual,Passed=expected==actual});
  ushort[][] mods={null,new ushort[0],new ushort[]{16},new ushort[]{17},new ushort[]{17,16}};int[] counts={1,1,2,2,3};string[] names={"null","empty","Shift","Ctrl","Ctrl+Shift"};
  for(int i=0;i<mods.Length;i++){var t=DescribeKey(65,mods[i]);check(names[i]+" down-count",counts[i],t.KeyDownPackets);check(names[i]+" packets",counts[i]*2,t.PacketCount);check(names[i]+" early-ack",0,t.Acknowledged(10,10+counts[i]-1)?1:0);check(names[i]+" exact-ack",1,t.Acknowledged(10,10+counts[i])?1:0);}
  var keys=KeyPackets(65,new ushort[]{17,16});
  for(int prefix=0;prefix<keys.Length;prefix++){int call=0;INPUT[] observed=null;int n=prefix;var t=Transact(keys,p=>{if(call++==0)return (uint)n;observed=p;return (uint)p.Length;});var expected=PrefixReleases(keys,prefix);check("partial-key-prefix"+prefix+" incomplete",0,t.Complete?1:0);check("partial-key-prefix"+prefix+" cleanup-count",new[]{0,1,2,3,2,1}[prefix],t.CleanupInserted);check("partial-key-prefix"+prefix+" cannot-ack",0,t.Acknowledged(0,100)?1:0);if(observed!=null){int unknown=0;foreach(var p in observed)if(p.Type!=1||(p.Key.Flags&2)==0||(p.Key.Code!=17&&p.Key.Code!=16&&p.Key.Code!=65))unknown++;check("partial-key-prefix"+prefix+" releases-only-owned",0,unknown);}}
  var mouse=new[]{new INPUT{Mouse=new MOUSE{Flags=0xc001}},new INPUT{Mouse=new MOUSE{Flags=2}},new INPUT{Mouse=new MOUSE{Flags=4}}};
  for(int prefix=0;prefix<3;prefix++){int call=0,n=prefix;INPUT[] observed=null;var t=Transact(mouse,p=>{if(call++==0)return (uint)n;observed=p;return (uint)p.Length;});check("partial-mouse-prefix"+prefix+" cleanup",prefix==2?1:0,t.CleanupInserted);if(observed!=null)check("partial-mouse-prefix"+prefix+" own-left-up",1,observed.Length==1&&observed[0].Type==0&&observed[0].Mouse.Flags==4?1:0);}
  int calls=0;var failedCleanup=Transact(keys,p=>calls++==0?3u:0u,()=>123);check("failed-cleanup-observable",0,failedCleanup.CleanupInserted);check("failed-cleanup-attempted",3,failedCleanup.CleanupPackets);check("partial-error-retained",123,failedCleanup.Error);check("cleanup-error-retained",123,failedCleanup.CleanupError);
  var target=new POINT{X=20,Y=30};check("pointer-owned-restoration",1,PointerOwned(target,target,true,true)?1:0);check("pointer-user-moved-preserved",0,PointerOwned(new POINT{X=21,Y=30},target,true,true)?1:0);check("pointer-foreign-hit-preserved",0,PointerOwned(target,target,false,true)?1:0);check("pointer-busy-preserved",0,PointerOwned(target,target,true,false)?1:0);
  return a.ToArray();
 }
 [StructLayout(LayoutKind.Sequential)] struct BMI {public uint Size;public int Width,Height;public ushort Planes,Bits;public uint Compression,ImageSize;public int XPels,YPels;public uint Used,Important;}
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")]static extern IntPtr GetThreadDpiAwarenessContext();
 [DllImport("user32.dll")]static extern int GetAwarenessFromDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")]static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]static extern bool GetClientRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")]static extern bool ClientToScreen(IntPtr h,ref POINT p);
 [DllImport("user32.dll")]static extern bool GetCursorPos(out POINT p);
 [DllImport("user32.dll")]static extern IntPtr WindowFromPoint(POINT p);
 [DllImport("user32.dll")]static extern IntPtr GetAncestor(IntPtr h,uint flags);
 [DllImport("user32.dll")]static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern int GetClassNameW(IntPtr h,StringBuilder b,int n);
 [DllImport("user32.dll")]static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]static extern bool GetGUIThreadInfo(uint thread,ref GUI g);
 [DllImport("user32.dll")]static extern IntPtr GetWindowLongPtrW(IntPtr h,int index);
 [DllImport("user32.dll",SetLastError=true)]static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]static extern int GetSystemMetrics(int n);
 [DllImport("user32.dll")]static extern short GetAsyncKeyState(int key);
 [DllImport("user32.dll",SetLastError=true)]static extern uint SendInput(uint n,INPUT[] a,int size);
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr GetDC(IntPtr h);
 [DllImport("user32.dll")]static extern int ReleaseDC(IntPtr h,IntPtr dc);
 [DllImport("gdi32.dll")]static extern IntPtr CreateCompatibleDC(IntPtr dc);
 [DllImport("gdi32.dll")]static extern IntPtr CreateDIBSection(IntPtr dc,ref BMI bmi,uint usage,out IntPtr pixels,IntPtr section,uint offset);
 [DllImport("gdi32.dll")]static extern IntPtr SelectObject(IntPtr dc,IntPtr obj);
 [DllImport("gdi32.dll")]static extern bool DeleteObject(IntPtr obj);
 [DllImport("gdi32.dll")]static extern bool DeleteDC(IntPtr dc);
 [DllImport("gdi32.dll",SetLastError=true)]static extern bool BitBlt(IntPtr dest,int x,int y,int w,int h,IntPtr source,int sx,int sy,uint rop);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("kernel32.dll")]static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")]static extern IntPtr GetProcessWindowStation();
 [DllImport("user32.dll")]static extern IntPtr GetThreadDesktop(uint thread);
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr OpenInputDesktop(uint flags,bool inherit,uint access);
 [DllImport("user32.dll")]static extern bool CloseDesktop(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]static extern bool GetUserObjectInformationW(IntPtr h,int which,StringBuilder b,uint bytes,out uint needed);
 [DllImport("user32.dll")]static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")]static extern IntPtr GetWindowDpiAwarenessContext(IntPtr h);
 [DllImport("dwmapi.dll")]static extern int DwmGetWindowAttribute(IntPtr h,uint attribute,out int value,uint size);
 static string Name(IntPtr h){uint n;var b=new StringBuilder(128);if(h==IntPtr.Zero||!GetUserObjectInformationW(h,2,b,256,out n))return "unavailable:"+Marshal.GetLastWin32Error();return b.ToString();}
 public sealed class DesktopFacts {public string Station,Caller,Window,Input;public bool InputOpened,Usable;public int InputOpenError;}
 static DesktopFacts Desktop(uint thread){var input=OpenInputDesktop(0,false,1);int error=input==IntPtr.Zero?Marshal.GetLastWin32Error():0;try{var a=new DesktopFacts{Station=Name(GetProcessWindowStation()),Caller=Name(GetThreadDesktop(GetCurrentThreadId())),Window=thread!=0?Name(GetThreadDesktop(thread)):"thread-unavailable",Input=Name(input),InputOpened=input!=IntPtr.Zero,InputOpenError=error};a.Usable=a.InputOpened&&!a.Input.StartsWith("unavailable:")&&a.Station=="WinSta0"&&a.Caller==a.Input&&a.Window==a.Input;return a;}finally{if(input!=IntPtr.Zero)CloseDesktop(input);}}
 static void Emit(string file,string stage,Facts fact,Exposure exposure=null,Facts[] samples=null){File.AppendAllText(file,System.Text.Json.JsonSerializer.Serialize(new{Stage=stage,Facts=fact,Exposure=exposure==null?(object)new{SetWindowPosAttempted=false,SetWindowPosSuccess=false,SetWindowPosError=0,ShowWindowAttempted=false,ShowWindowError=0,ShowWindowResult="not attempted before safety guard"}:exposure,FrameSamples=samples},new System.Text.Json.JsonSerializerOptions{IncludeFields=true})+Environment.NewLine);}
 public static int[] Gear(IntPtr main,uint owner){using(new Dpi()){Require(main,owner);RECT r;if(!GetClientRect(main,out r))throw new Exception("Owned client rectangle unavailable");double d=GetDpiForWindow(main)/96.0;return new[]{r.R-(int)Math.Round(14*d),r.B-(int)Math.Round(12*d)};}}
 sealed class Dpi:IDisposable {IntPtr prior;public Dpi(){prior=SetThreadDpiAwarenessContext((IntPtr)(-4));if(prior==IntPtr.Zero)throw new Exception("Per-thread PMv2 DPI scope unavailable");}public void Dispose(){if(SetThreadDpiAwarenessContext(prior)==IntPtr.Zero)throw new Exception("DPI context restoration failed");}}
 sealed class Exposure:IDisposable {
  IntPtr main;uint owner;bool top;string file;
  public bool SetWindowPosAttempted,SetWindowPosSuccess,RestoreAttempted,RestoreSuccess,ShowWindowAttempted=false;
  public int SetWindowPosError,RestoreError,ShowWindowError=0;public string ShowWindowResult="not attempted: coherent visible owned window";
  public Exposure(IntPtr h,uint owner,string factsFile=null){Require(h,owner);main=h;this.owner=owner;file=factsFile;top=(GetWindowLongPtrW(h,-20).ToInt64()&8)!=0;SetWindowPosSuccess=top;if(!top){SetWindowPosAttempted=true;SetWindowPosSuccess=SetWindowPos(h,(IntPtr)(-1),0,0,0,0,0x13);SetWindowPosError=SetWindowPosSuccess?0:Marshal.GetLastWin32Error();}}
  public void Dispose(){if(!top&&SetWindowPosSuccess){Require(main,owner);RestoreAttempted=true;RestoreSuccess=SetWindowPos(main,(IntPtr)(-2),0,0,0,0,0x13);RestoreError=RestoreSuccess?0:Marshal.GetLastWin32Error();}if(file!=null)File.AppendAllText(file,System.Text.Json.JsonSerializer.Serialize(new{Stage="exposure-restoration",Exposure=this},new System.Text.Json.JsonSerializerOptions{IncludeFields=true})+Environment.NewLine);if(RestoreAttempted&&!RestoreSuccess)throw new Exception("Owned host z-order restoration failed");}
 }
 static uint Require(IntPtr h,uint owner){uint pid;uint t=GetWindowThreadProcessId(h,out pid);if(h==IntPtr.Zero||t==0||pid!=owner||!IsWindowVisible(h))throw new Exception("Visible owned window identity unavailable");return t;}
 static bool Busy(){for(int k=1;k<256;k++)if((GetAsyncKeyState(k)&0x8000)!=0)return true;return false;}
 static INPUT Move(POINT p){int x=GetSystemMetrics(76),y=GetSystemMetrics(77),w=GetSystemMetrics(78),h=GetSystemMetrics(79);if(w<=1||h<=1||p.X<x||p.X>=x+w||p.Y<y||p.Y>=y+h)throw new Exception("Point outside virtual desktop");return new INPUT{Type=0,Mouse=new MOUSE{X=(int)Math.Round((p.X-x)*65535.0/(w-1)),Y=(int)Math.Round((p.Y-y)*65535.0/(h-1)),Flags=0xc001}};}
 public sealed class Facts {
  public long Main,Slot,Hit,HitRoot,HitRootOwner,Foreground,ForegroundRoot,Focus,Active,CallerDpiContext,MainDpiContext,SlotDpiContext;
  public uint ExpectedPid,HostPid,SlotPid,HitPid,HostThread,HitThread,ForegroundPid,ForegroundThread,MainDpi,SlotDpi;
  public int Awareness,Cloaked,CloakResult,GuiError;public string HitClass,ForegroundClass;
  public DesktopFacts Desktop;public POINT Client,Screen,SlotOrigin;public RECT MainRect,MainClient,SlotRect,HitRect,SlotClient;
  public bool ExactHit,CoordinatesValid,MainVisible,SlotVisible,MainIconic,SlotIconic,GuiValid;
 }
 public static Facts Probe(IntPtr main,IntPtr slot,uint owner,int x,int y){using(new Dpi()){
  uint mainPid,slotPid;uint thread=GetWindowThreadProcessId(main,out mainPid);GetWindowThreadProcessId(slot,out slotPid);var screen=new POINT{X=x,Y=y};var origin=new POINT();RECT mr,sr,hr,sc,mc;
  bool valid=ClientToScreen(slot,ref screen)&ClientToScreen(slot,ref origin)&GetWindowRect(main,out mr)&GetWindowRect(slot,out sr)&GetClientRect(slot,out sc)&GetClientRect(main,out mc);
  IntPtr hit=WindowFromPoint(screen);uint hp;uint ht=GetWindowThreadProcessId(hit,out hp);GetWindowRect(hit,out hr);var cls=new StringBuilder(128);GetClassNameW(hit,cls,cls.Capacity);var gui=new GUI{Size=(uint)Marshal.SizeOf(typeof(GUI))};bool guiValid=thread!=0&&GetGUIThreadInfo(thread,ref gui);int guiError=guiValid?0:Marshal.GetLastWin32Error();
  IntPtr foreground=GetForegroundWindow();uint fp;uint ft=GetWindowThreadProcessId(foreground,out fp);var fc=new StringBuilder(128);GetClassNameW(foreground,fc,fc.Capacity);int cloak;int cloakResult=DwmGetWindowAttribute(main,14,out cloak,4);
  return new Facts{Main=main.ToInt64(),Slot=slot.ToInt64(),Hit=hit.ToInt64(),HitRoot=GetAncestor(hit,2).ToInt64(),HitRootOwner=GetAncestor(hit,3).ToInt64(),Foreground=foreground.ToInt64(),ForegroundRoot=GetAncestor(foreground,2).ToInt64(),ForegroundPid=fp,ForegroundThread=ft,ForegroundClass=fc.ToString(),Focus=gui.Focus.ToInt64(),Active=gui.Active.ToInt64(),ExpectedPid=owner,HostPid=mainPid,SlotPid=slotPid,HitPid=hp,HostThread=thread,HitThread=ht,MainDpi=GetDpiForWindow(main),SlotDpi=GetDpiForWindow(slot),Awareness=GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()),CallerDpiContext=GetThreadDpiAwarenessContext().ToInt64(),MainDpiContext=GetWindowDpiAwarenessContext(main).ToInt64(),SlotDpiContext=GetWindowDpiAwarenessContext(slot).ToInt64(),HitClass=cls.ToString(),Desktop=Desktop(thread),Cloaked=cloak,CloakResult=cloakResult,MainVisible=IsWindowVisible(main),SlotVisible=IsWindowVisible(slot),MainIconic=IsIconic(main),SlotIconic=IsIconic(slot),GuiValid=guiValid,GuiError=guiError,CoordinatesValid=valid,Client=new POINT{X=x,Y=y},Screen=screen,SlotOrigin=origin,MainRect=mr,MainClient=mc,SlotRect=sr,HitRect=hr,SlotClient=sc,ExactHit=valid&&hit==slot&&hp==owner&&GetAncestor(slot,2)==main};
 }}
 static Facts[] FrameFacts(IntPtr main,IntPtr slot,uint owner){RECT r;GetClientRect(main,out r);var p=new POINT();var sp=new POINT();ClientToScreen(main,ref p);ClientToScreen(slot,ref sp);return new[]{Probe(main,slot,owner,p.X+2-sp.X,p.Y+2-sp.Y),Probe(main,slot,owner,p.X+r.R-3-sp.X,p.Y+r.B-3-sp.Y),Probe(main,slot,owner,p.X+r.R/2-sp.X,p.Y+r.B/2-sp.Y)};}
 static void AdmitDesktop(Facts f){if(!f.Desktop.Usable||f.CloakResult!=0||f.Cloaked!=0||f.MainIconic||!f.MainVisible||!f.SlotVisible||!f.CoordinatesValid||f.HostPid!=f.ExpectedPid||f.SlotPid!=f.ExpectedPid)throw new Exception("Typed prerequisite blocked: owned visibility/cloaking/current input desktop contract unavailable; no OS input");}
 public static long Focus(IntPtr main,uint owner){using(new Dpi()){uint t=Require(main,owner);var g=new GUI{Size=(uint)Marshal.SizeOf(typeof(GUI))};if(!GetGUIThreadInfo(t,ref g))throw new Exception("GUI focus unavailable");return g.Focus.ToInt64();}}
 public sealed class Frame {public int Width,Height,SlotX,SlotY,SlotWidth,SlotHeight,Background,NonBackground;public int[] Pixels;public bool Recognizable;}
 static Frame CaptureRaw(IntPtr main,IntPtr slot,uint owner,string file){Require(main,owner);Require(slot,owner);RECT r,s;GetClientRect(main,out r);GetClientRect(slot,out s);var p=new POINT();var sp=new POINT();if(!ClientToScreen(main,ref p)||!ClientToScreen(slot,ref sp)||r.R<=0||r.B<=0||r.R>10000||r.B>10000)throw new Exception("Capture coordinates unavailable");
  foreach(var q in new[]{new POINT{X=p.X+2,Y=p.Y+2},new POINT{X=p.X+r.R-3,Y=p.Y+r.B-3},new POINT{X=p.X+r.R/2,Y=p.Y+r.B/2}}){uint pid;GetWindowThreadProcessId(WindowFromPoint(q),out pid);if(pid!=owner)throw new Exception("Owned frame not exposed; no capture admitted");}
  IntPtr screen=GetDC(IntPtr.Zero),dc=CreateCompatibleDC(screen),bits;var bi=new BMI{Size=40,Width=r.R,Height=-r.B,Planes=1,Bits=32};IntPtr bmp=CreateDIBSection(screen,ref bi,0,out bits,IntPtr.Zero,0),previous=IntPtr.Zero;
  try{if(screen==IntPtr.Zero||dc==IntPtr.Zero||bmp==IntPtr.Zero||bits==IntPtr.Zero)throw new Exception("Native capture allocation unavailable");previous=SelectObject(dc,bmp);if(!BitBlt(dc,0,0,r.R,r.B,screen,p.X,p.Y,0x00cc0020))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Native GDI capture failed");var f=new Frame{Width=r.R,Height=r.B,SlotX=sp.X-p.X,SlotY=sp.Y-p.Y,SlotWidth=s.R,SlotHeight=s.B,Pixels=new int[r.R*r.B]};Marshal.Copy(bits,f.Pixels,0,f.Pixels.Length);
   if(f.SlotX<0||f.SlotY<0||f.SlotX+s.R>r.R||f.SlotY+s.B>r.B)throw new Exception("Slot client outside owned frame");for(int y=0;y<s.B;y++)for(int x=0;x<s.R;x++){if((f.Pixels[(y+f.SlotY)*f.Width+x+f.SlotX]&0xffffff)==0x1e1e1e)f.Background++;else f.NonBackground++;}f.Recognizable=f.Background>s.R*s.B/10&&f.NonBackground>1000;
   using(var output=new BinaryWriter(File.Create(file))){int size=f.Pixels.Length*4;output.Write((ushort)0x4d42);output.Write(54+size);output.Write(0);output.Write(54);output.Write(40);output.Write(f.Width);output.Write(-f.Height);output.Write((ushort)1);output.Write((ushort)32);for(int i=0;i<6;i++)output.Write(i==1?size:0);foreach(int v in f.Pixels)output.Write(v);}return f;
  }finally{if(previous!=IntPtr.Zero)SelectObject(dc,previous);if(bmp!=IntPtr.Zero)DeleteObject(bmp);if(dc!=IntPtr.Zero)DeleteDC(dc);if(screen!=IntPtr.Zero)ReleaseDC(IntPtr.Zero,screen);}
 }
 public static Frame Capture(IntPtr main,IntPtr slot,uint owner,string file){using(new Dpi())using(new Exposure(main,owner))return CaptureRaw(main,slot,owner,file);}
 // Observe delivery of our queued move/up before restoring once. Seeing the
 // original point before a queued prefix drains does not prove completion.
 static string RestoreOwnPointer(IntPtr main,IntPtr slot,uint owner,int x,int y,POINT prior,POINT target,string factsFile){
  string result="preserved-current-pointer";POINT now;var w=System.Diagnostics.Stopwatch.StartNew();
  while(w.ElapsedMilliseconds<100){
   if(!GetCursorPos(out now))break;
   if(PointerOwned(now,target,Probe(main,slot,owner,x,y).ExactHit,!Busy())){
    try{SendOwned(new[]{Move(prior)});var delivery=System.Diagnostics.Stopwatch.StartNew();do{if(GetCursorPos(out now)&&now.X==prior.X&&now.Y==prior.Y){result="restored";break;}System.Threading.Thread.Sleep(1);}while(delivery.ElapsedMilliseconds<100);}
    catch(Exception error){result="restore-failed:"+error.Message;}break;
   }
   if((now.X!=prior.X||now.Y!=prior.Y)&&(now.X!=target.X||now.Y!=target.Y))break;
   System.Threading.Thread.Sleep(1);
  }
  if(factsFile!=null)File.AppendAllText(factsFile,System.Text.Json.JsonSerializer.Serialize(new{Stage="owned-pointer-restoration",Result=result,Original=prior,Target=target},new System.Text.Json.JsonSerializerOptions{IncludeFields=true})+Environment.NewLine);
  return result;
 }
 public sealed class Activation {public Facts Before,After;public int Background,NonBackground;public string PointerRestore,DownBeforeRestore;public POINT PointerBeforeRestore;public bool ClickSent;}
 public static Activation Activate(IntPtr main,IntPtr slot,uint owner,string file,string factsFile){return ActivateCore(main,slot,owner,file,factsFile,false);}
 public static Activation ActivatePaneFooter(IntPtr main,IntPtr pane,uint owner,string file,string factsFile){return ActivateCore(main,pane,owner,file,factsFile,true);}
 static bool Tile(Frame f,int x,int y,int color){if(x<2||y<2||x+2>=f.SlotWidth||y+2>=f.SlotHeight)return false;for(int yy=y-2;yy<=y+2;yy++)for(int xx=x-2;xx<=x+2;xx++)if((f.Pixels[(yy+f.SlotY)*f.Width+xx+f.SlotX]&0xffffff)!=color)return false;return true;}
 static Activation ActivateCore(IntPtr main,IntPtr slot,uint owner,string file,string factsFile,bool paneFooter){using(new Dpi()){
  var activationClock=System.Diagnostics.Stopwatch.StartNew();var preliminary=Probe(main,slot,owner,8,80);Emit(factsFile,"before-exposure-guard",preliminary,samples:FrameFacts(main,slot,owner));AdmitDesktop(preliminary);
  using(var exposure=new Exposure(main,owner,factsFile)){
  var exposed=Probe(main,slot,owner,8,80);var samples=FrameFacts(main,slot,owner);Emit(factsFile,"after-exposure-before-capture-guard",exposed,exposure,samples);AdmitDesktop(exposed);
  if(!exposure.SetWindowPosSuccess)throw new Exception("Typed prerequisite blocked: owned exposure API failed; no OS input");
  var frame=CaptureRaw(main,slot,owner,file);while(!frame.Recognizable&&activationClock.ElapsedMilliseconds<2000){System.Threading.Thread.Sleep(5);frame=CaptureRaw(main,slot,owner,file);}if(!frame.Recognizable)throw new Exception("Owned Settings frame is not recognizable");int sx=-1,sy=-1;int x=(int)Math.Round(8*GetDpiForWindow(slot)/96.0);
  if(paneFooter){
   var cls=new StringBuilder(64);GetClassNameW(slot,cls,cls.Capacity);if(cls.ToString()!="OpenPilotPane")throw new Exception("Footer activation requires an owned file pane");
   // The last24 logical pixels are the pane status footer. Its ordinary click
   // focuses the pane without touching its file cursor or selection. Pixel
   // admission verifies C_STATUS rather than treating icon padding as empty.
   int y=frame.SlotHeight-(int)Math.Round(6*GetDpiForWindow(slot)/96.0);
   if(Tile(frame,x,y,0x252528)&&Probe(main,slot,owner,x,y).ExactHit){sx=x;sy=y;}
  }else{
  // Known blank left padding: Settings controls begin at16 logical pixels;
  // avoid the0..3px changed-record marker and verify an entirely BG tile.
  for(int y=(int)(80*GetDpiForWindow(slot)/96);y<frame.SlotHeight-10&&sy<0;y+=8){bool blank=true;for(int yy=y-2;yy<=y+2&&blank;yy++)for(int xx=x-2;xx<=x+2;xx++)if((frame.Pixels[(yy+frame.SlotY)*frame.Width+xx+frame.SlotX]&0xffffff)!=0x1e1e1e){blank=false;break;}if(blank){var probe=Probe(main,slot,owner,x,y);if(probe.ExactHit){sx=x;sy=y;}}}
  }
  if(sy<0)throw new Exception("No exposed known blank slot padding; no activation click delivered");var a=new Activation{Before=Probe(main,slot,owner,sx,sy),Background=frame.Background,NonBackground=frame.NonBackground};
  Emit(factsFile,"before-ordinary-input",a.Before,exposure);AdmitDesktop(a.Before);
  POINT prior;if(!GetCursorPos(out prior)||Busy()||!a.Before.ExactHit)throw new Exception("Pointer/input/owned hit prerequisite failed");bool touched=false;
  try{var again=Probe(main,slot,owner,sx,sy);if(!again.ExactHit||Busy())throw new Exception("Owned blank hit changed before input");touched=true;var sent=SendOwned(new[]{Move(again.Screen),new INPUT{Mouse=new MOUSE{Flags=2}},new INPUT{Mouse=new MOUSE{Flags=4}}});a.ClickSent=sent.Complete;
   // Keep owned exposure until the inserted click and focus are handled.
   while(activationClock.ElapsedMilliseconds<2000&&(GetForegroundWindow()!=main||Focus(main,owner)!=slot.ToInt64()))System.Threading.Thread.Sleep(2);
   while(Busy()&&activationClock.ElapsedMilliseconds<2000)System.Threading.Thread.Sleep(1);a.After=Probe(main,slot,owner,sx,sy);
  }
  finally{OwnRelease();GetCursorPos(out a.PointerBeforeRestore);a.DownBeforeRestore=DownKeys();a.PointerRestore=touched?RestoreOwnPointer(main,slot,owner,sx,sy,prior,a.Before.Screen,factsFile):"preserved-current-pointer";}

  Emit(factsFile,"after-ordinary-input",a.After,exposure);if(activationClock.ElapsedMilliseconds>=2000)throw new Exception("Complete ordinary activation exceeded existing2000ms budget");return a;
 }}}
 public static InputTransaction Key(IntPtr main,IntPtr slot,uint owner,ushort key,ushort[] modifiers){using(new Dpi()){
  InputFacts(main,slot,owner,"before-os-key");Require(main,owner);Require(slot,owner);if(GetForegroundWindow()!=main||Focus(main,owner)!=slot.ToInt64()||Busy())throw new Exception("OS key requires owned foreground/focus and idle modifiers; down="+DownKeys());
  var t=SendOwned(KeyPackets(key,modifiers));OwnRelease();return t;
 }}
 public static void Text(IntPtr main,IntPtr slot,uint owner,string text){using(new Dpi()){InputFacts(main,slot,owner,"before-os-text");Require(main,owner);Require(slot,owner);if(GetForegroundWindow()!=main||Focus(main,owner)!=slot.ToInt64()||Busy())throw new Exception("OS text requires owned foreground/focus and idle modifiers; down="+DownKeys());var a=new List<INPUT>();foreach(char c in text){a.Add(new INPUT{Type=1,Key=new KEY{Scan=c,Flags=4}});a.Add(new INPUT{Type=1,Key=new KEY{Scan=c,Flags=6}});}SendOwned(a.ToArray());OwnRelease();}}
 public static int[] Ring(Frame f){
  const int c=0x6eb4ff;int ox=f.SlotX,oy=f.SlotY;
  for(int y=80;y<f.SlotHeight-10;y++)for(int x=f.SlotWidth/2;x<f.SlotWidth-15;x++){
   if((f.Pixels[(y+oy)*f.Width+x+ox]&0xffffff)!=c)continue;int end=x;while(end<f.SlotWidth&&(f.Pixels[(y+oy)*f.Width+end+ox]&0xffffff)==c)end++;
   if(end-x>=30&&end-x<=220)for(int b=y+10;b<Math.Min(f.SlotHeight,y+70);b++){bool good=true;for(int q=x;q<end&&good;q++)good=(f.Pixels[(b+oy)*f.Width+q+ox]&0xffffff)==c;for(int q=y;q<=b&&good;q++)good=(f.Pixels[(q+oy)*f.Width+x+ox]&0xffffff)==c&&(f.Pixels[(q+oy)*f.Width+end-1+ox]&0xffffff)==c;if(good)return new[]{(x+end)/2,(y+b)/2,x,y,end,b};}x=end;
  }throw new Exception("Focused exact choice/field border not found in owned native frame");
 }
 public static string Click(IntPtr main,IntPtr slot,uint owner,int x,int y,string factsFile){using(new Dpi()){
  var f=Probe(main,slot,owner,x,y);Emit(factsFile,"before-feature-click",f);AdmitDesktop(f);POINT prior;if(!f.ExactHit||GetForegroundWindow()!=main||Focus(main,owner)!=slot.ToInt64()||Busy()||!GetCursorPos(out prior))throw new Exception("Owned feature click admission failed");
  string restored="preserved-current-pointer";try{SendOwned(new[]{Move(f.Screen),new INPUT{Mouse=new MOUSE{Flags=2}},new INPUT{Mouse=new MOUSE{Flags=4}}});OwnRelease();}
  finally{OwnRelease();restored=RestoreOwnPointer(main,slot,owner,x,y,prior,f.Screen,factsFile);}

  return restored;
 }}
 [DllImport("user32.dll",SetLastError=true)]static extern bool OpenClipboard(IntPtr h);
 [DllImport("user32.dll",SetLastError=true)]static extern bool CloseClipboard();
 [DllImport("user32.dll",SetLastError=true)]static extern bool EmptyClipboard();
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr GetClipboardData(uint format);
 [DllImport("user32.dll",SetLastError=true)]static extern IntPtr SetClipboardData(uint format,IntPtr h);
 [DllImport("user32.dll",SetLastError=true)]static extern uint EnumClipboardFormats(uint format);
 [DllImport("user32.dll")]static extern uint GetClipboardSequenceNumber();
 [DllImport("user32.dll")]static extern IntPtr GetClipboardOwner();
 [DllImport("user32.dll")]static extern IntPtr GetOpenClipboardWindow();
 [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr GlobalAlloc(uint flags,UIntPtr size);
 [DllImport("kernel32.dll",SetLastError=true)]static extern IntPtr GlobalLock(IntPtr h);
 [DllImport("kernel32.dll")]static extern bool GlobalUnlock(IntPtr h);
 [DllImport("kernel32.dll")]static extern UIntPtr GlobalSize(IntPtr h);
 [DllImport("kernel32.dll")]static extern IntPtr GlobalFree(IntPtr h);
 [DllImport("kernel32.dll")]static extern void SetLastError(uint code);
 [DllImport("ole32.dll")]static extern IntPtr OleDuplicateData(IntPtr h,ushort format,uint flags);
 [DllImport("gdi32.dll")]static extern bool DeleteEnhMetaFile(IntPtr h);
 [DllImport("gdi32.dll")]static extern bool DeleteMetaFile(IntPtr h);
 static void OpenFixture(IntPtr owner){var w=System.Diagnostics.Stopwatch.StartNew();do{if(OpenClipboard(owner))return;System.Threading.Thread.Sleep(2);}while(w.ElapsedMilliseconds<1500);throw new Exception("Clipboard fixture acquisition unavailable: "+Marshal.GetLastWin32Error());}
 static void FreeFormat(uint format,IntPtr h){if(h==IntPtr.Zero)return;if(format==2||format==9)DeleteObject(h);else if(format==14)DeleteEnhMetaFile(h);else if(format==3){IntPtr p=GlobalLock(h);if(p!=IntPtr.Zero){DeleteMetaFile(Marshal.ReadIntPtr(p,16));GlobalUnlock(h);}GlobalFree(h);}else GlobalFree(h);}
 public sealed class ClipboardCustody:IDisposable {
  List<uint> formats=new List<uint>();List<IntPtr> data=new List<IntPtr>();IntPtr owner;public bool Restored;public int Formats;
  public ClipboardCustody(IntPtr owner){this.owner=owner;OpenFixture(owner);try{uint f=0;while(true){SetLastError(0);f=EnumClipboardFormats(f);if(f==0){if(Marshal.GetLastWin32Error()!=0)throw new Exception("Clipboard enumeration failed");break;}if(formats.Count==256||f==0x80||(f>=0x200&&f<=0x2ff)||(f>=0x300&&f<=0x3ff))throw new Exception("Unsupported external clipboard custody; mutation refused");IntPtr original=GetClipboardData(f),copy=original!=IntPtr.Zero?OleDuplicateData(original,(ushort)f,2):IntPtr.Zero;if(copy==IntPtr.Zero)throw new Exception("External clipboard format could not be duplicated; mutation refused");formats.Add(f);data.Add(copy);}Formats=formats.Count;}catch{for(int i=0;i<data.Count;i++)FreeFormat(formats[i],data[i]);throw;}finally{CloseClipboard();}}
  public void Dispose(){bool opened=false;try{OpenFixture(owner);opened=true;if(!EmptyClipboard())throw new Exception("Clipboard restoration empty failed");for(int i=0;i<data.Count;i++){if(SetClipboardData(formats[i],data[i])==IntPtr.Zero)throw new Exception("Clipboard restoration publication failed");data[i]=IntPtr.Zero;}Restored=true;}finally{if(opened)CloseClipboard();for(int i=0;i<data.Count;i++)FreeFormat(formats[i],data[i]);}}
 }
 public static void Seed(IntPtr owner,string text){byte[] bytes=Encoding.Unicode.GetBytes(text+"\0");IntPtr h=GlobalAlloc(2,(UIntPtr)bytes.Length),p=h==IntPtr.Zero?IntPtr.Zero:GlobalLock(h);if(p==IntPtr.Zero){if(h!=IntPtr.Zero)GlobalFree(h);throw new Exception("Fixture clipboard allocation failed");}Marshal.Copy(bytes,0,p,bytes.Length);GlobalUnlock(h);try{OpenFixture(owner);try{if(!EmptyClipboard()||SetClipboardData(13,h)==IntPtr.Zero)throw new Exception("Fixture clipboard seed failed");h=IntPtr.Zero;}finally{CloseClipboard();}}finally{if(h!=IntPtr.Zero)GlobalFree(h);}}
 [DllImport("shell32.dll",CharSet=CharSet.Unicode)]static extern uint DragQueryFileW(IntPtr h,uint index,StringBuilder text,uint size);
 public sealed class FileDropObservation {public bool Opened,Exact,Stable;public uint Sequence,After,Count;public long Owner,Holder;public int Error;}
 public static FileDropObservation ObserveFiles(IntPtr owner,string[] expected){
  var o=new FileDropObservation{Sequence=GetClipboardSequenceNumber(),Owner=GetClipboardOwner().ToInt64(),Holder=GetOpenClipboardWindow().ToInt64()};if(!OpenClipboard(owner)){o.Error=Marshal.GetLastWin32Error();return o;}o.Opened=true;
  try{IntPtr h=GetClipboardData(15);o.Count=h!=IntPtr.Zero?DragQueryFileW(h,uint.MaxValue,null,0):0;o.Exact=h!=IntPtr.Zero&&o.Count==expected.Length;for(uint i=0;i<o.Count&&o.Exact;i++){uint n=DragQueryFileW(h,i,null,0);var b=new StringBuilder((int)n+1);o.Exact=DragQueryFileW(h,i,b,n+1)==n&&string.Equals(b.ToString(),expected[i],StringComparison.Ordinal);}}finally{CloseClipboard();}o.After=GetClipboardSequenceNumber();o.Stable=o.Sequence==o.After;return o;
 }
 public sealed class ClipboardObservation {
  public bool Exact,Opened;public uint Sequence,SequenceAfter;public long Owner,Holder;public int Error;
  public bool Accepted(long publisher,uint baseline,long elapsed,int deadline,bool changed){return publisher!=0&&Opened&&Exact&&Owner==publisher&&Sequence==SequenceAfter&&(!changed||Sequence!=baseline)&&elapsed>=0&&elapsed<deadline;}
 }
 public static ClipboardObservation Observe(IntPtr opener,string expected){
  var o=new ClipboardObservation{Sequence=GetClipboardSequenceNumber(),Owner=GetClipboardOwner().ToInt64(),Holder=GetOpenClipboardWindow().ToInt64()};
  // Observation never waits for ownership; fixture preparation/custody may wait.
  if(!OpenClipboard(opener)){o.Error=Marshal.GetLastWin32Error();return o;}o.Opened=true;
  try{o.Sequence=GetClipboardSequenceNumber();o.Owner=GetClipboardOwner().ToInt64();IntPtr h=GetClipboardData(13),p=h!=IntPtr.Zero?GlobalLock(h):IntPtr.Zero;if(p!=IntPtr.Zero){try{if(GlobalSize(h).ToUInt64()>=(ulong)(expected.Length+1)*2)o.Exact=Marshal.PtrToStringUni(p,expected.Length+1)==expected+"\0";}finally{GlobalUnlock(h);}}else o.Error=Marshal.GetLastWin32Error();}finally{CloseClipboard();}
  o.SequenceAfter=GetClipboardSequenceNumber();return o;
 }
 public static Contract[] ClipboardBoundaryContracts(){
  var a=new List<Contract>();Action<string,bool,bool> check=(name,expected,actual)=>a.Add(new Contract{Name=name,Expected=expected?1:0,Actual=actual?1:0,Passed=expected==actual});
  var o=new ClipboardObservation{Opened=true,Exact=true,Owner=100,Sequence=2,SequenceAfter=2};
  check("owned-before-deadline",true,o.Accepted(100,1,2999,3000,true));
  check("exact-deadline-rejected",false,o.Accepted(100,1,3000,3000,true));check("late-match-rejected",false,o.Accepted(100,1,3100,3000,true));
  check("foreign-publisher-rejected",false,o.Accepted(101,1,10,3000,true));check("null-publisher-rejected",false,o.Accepted(0,1,10,3000,true));
  check("stale-sequence-rejected",false,o.Accepted(100,2,10,3000,true));o.SequenceAfter=3;check("changing-sequence-rejected",false,o.Accepted(100,1,10,3000,true));o.SequenceAfter=2;
  o.Opened=false;check("busy-observation-rejected",false,o.Accepted(100,1,10,3000,true));o.Opened=true;o.Exact=false;check("wrong-text-rejected",false,o.Accepted(100,1,10,3000,true));o.Exact=true;
  check("seed-owned-unchanged-accepted",true,o.Accepted(100,2,10,3000,false));check("seed-foreign-rejected",false,o.Accepted(101,2,10,3000,false));
  return a.ToArray();
 }

 public sealed class ClipboardHold:IDisposable {
  System.Threading.ManualResetEvent ready=new System.Threading.ManualResetEvent(false),release=new System.Threading.ManualResetEvent(false);System.Threading.Thread thread;public bool Held,Released;public int Error;
  public ClipboardHold(){thread=new System.Threading.Thread(()=>{try{OpenFixture(IntPtr.Zero);Held=true;ready.Set();release.WaitOne(5000);CloseClipboard();Released=true;}catch{Error=Marshal.GetLastWin32Error();ready.Set();}});thread.IsBackground=true;thread.Start();if(!ready.WaitOne(2000)||!Held)throw new Exception("Owned clipboard holder unavailable");}
  public void Dispose(){release.Set();if(!thread.Join(2000)||!Released)throw new Exception("Owned clipboard holder release failed");ready.Dispose();release.Dispose();}
 }
}
