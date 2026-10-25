<# Independent native-window oracle for the opt-in UI input helper. #>
$ErrorActionPreference='Stop'
. "$PSScriptRoot\uilib.ps1"
. "$PSScriptRoot\ui_input.ps1"
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class NativeInputProbe {
  public static IntPtr Window,Edit;
  public static volatile int Keys,LastKey,LastModifiers,Characters,CharacterModifiers,Modal;
  static Thread thread;static readonly ManualResetEvent ready=new ManualResetEvent(false);
  delegate IntPtr Proc(IntPtr h,uint m,IntPtr w,IntPtr l);
  static Proc windowProc=Dispatch,editProc=EditDispatch;static IntPtr original;
  [StructLayout(LayoutKind.Sequential,CharSet=CharSet.Unicode)] struct WC {public uint style;public Proc proc;public int cls,win;public IntPtr instance,icon,cursor,brush;public string menu,name;}
  [StructLayout(LayoutKind.Sequential)] struct MSG {public IntPtr h;public uint m;public IntPtr w,l;public uint time;public int x,y;public uint p;}
  [DllImport("kernel32.dll",CharSet=CharSet.Unicode)] static extern IntPtr GetModuleHandleW(string name);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern ushort RegisterClassW(ref WC wc);
  [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern IntPtr CreateWindowExW(uint ex,string cls,string title,uint style,int x,int y,int width,int height,IntPtr parent,IntPtr menu,IntPtr instance,IntPtr param);
  [DllImport("user32.dll")] static extern IntPtr SetWindowLongPtrW(IntPtr h,int index,IntPtr value);
  [DllImport("user32.dll")] static extern IntPtr CallWindowProcW(IntPtr proc,IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] static extern IntPtr DefWindowProcW(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] static extern int GetMessageW(out MSG msg,IntPtr h,uint first,uint last);
  [DllImport("user32.dll")] static extern bool TranslateMessage(ref MSG msg);
  [DllImport("user32.dll")] static extern IntPtr DispatchMessageW(ref MSG msg);
  [DllImport("user32.dll")] static extern bool DestroyWindow(IntPtr h);
  [DllImport("user32.dll")] static extern void PostQuitMessage(int code);
  [DllImport("user32.dll")] static extern short GetKeyState(int key);
  [DllImport("user32.dll")] static extern bool SetKeyboardState(byte[] state);
  public static int Modifiers(){return (GetKeyState(0x11)<0?1:0)|(GetKeyState(0x10)<0?2:0)|(GetKeyState(0x12)<0?4:0);}
  static IntPtr EditDispatch(IntPtr h,uint m,IntPtr w,IntPtr l){if(m==0x102){Characters++;CharacterModifiers|=Modifiers();}return CallWindowProcW(original,h,m,w,l);}
  static IntPtr Dispatch(IntPtr h,uint m,IntPtr w,IntPtr l){
    if(m==0x401){var s=new byte[256];int mask=w.ToInt32();foreach(var k in new[]{0x11,0xA2})if((mask&1)!=0)s[k]=0x80;foreach(var k in new[]{0x10,0xA0})if((mask&2)!=0)s[k]=0x80;foreach(var k in new[]{0x12,0xA4})if((mask&4)!=0)s[k]=0x80;if(!SetKeyboardState(s))throw new Exception("Cannot prime probe keyboard");return (IntPtr)Modifiers();}
    if(m==0x402){Modal=0;return IntPtr.Zero;}
    if(m==0x100){
      LastKey=w.ToInt32();LastModifiers=Modifiers();Keys++;
      if(LastKey==0x77){Modal=1;MSG nested;while(Modal!=0&&GetMessageW(out nested,IntPtr.Zero,0,0)>0){TranslateMessage(ref nested);DispatchMessageW(ref nested);}}
      return IntPtr.Zero;
    }
    if(m==0x10){DestroyWindow(h);return IntPtr.Zero;}
    if(m==2){PostQuitMessage(0);return IntPtr.Zero;}
    return DefWindowProcW(h,m,w,l);
  }
  public static void Start(){
    thread=new Thread(()=>{
      IntPtr instance=GetModuleHandleW(null);string cls="InputProbe"+Guid.NewGuid().ToString("N");
      var wc=new WC{proc=windowProc,instance=instance,name=cls};RegisterClassW(ref wc);
      Window=CreateWindowExW(0,cls,"",0x80000000,0,0,200,100,IntPtr.Zero,IntPtr.Zero,instance,IntPtr.Zero);
      Edit=CreateWindowExW(0,"EDIT","",0x40000080,0,0,180,40,Window,IntPtr.Zero,instance,IntPtr.Zero);
      original=SetWindowLongPtrW(Edit,-4,Marshal.GetFunctionPointerForDelegate(editProc));ready.Set();
      MSG msg;while(GetMessageW(out msg,IntPtr.Zero,0,0)>0){TranslateMessage(ref msg);DispatchMessageW(ref msg);}
    });thread.SetApartmentState(ApartmentState.STA);thread.Start();
    if(!ready.WaitOne(3000)||Window==IntPtr.Zero||Edit==IntPtr.Zero)throw new Exception("Native probe window did not initialize");
  }
  public static void Join(){if(!thread.Join(3000))throw new Exception("Native probe thread did not stop");}
}
"@
$passed=0
function Check([string]$name,[bool]$ok){if(-not $ok){throw "Failed: $name"};$script:passed++;"  ok    $name"}
$script:demoPid=[uint32]$PID
[NativeInputProbe]::Start();$h=[NativeInputProbe]::Window;$edit=[NativeInputProbe]::Edit
try {
    Check 'probe has an independently primed Shift state' ([FpUi]::SendMessageW($h,0x401,[IntPtr]2,[IntPtr]::Zero).ToInt64() -eq 2)
    Post $h $WM_KEYDOWN 0x74 (Key-LParam 0x74)
    Check 'raw posted F5 inherits the target modifier state' ((Wait-Until {[NativeInputProbe]::Keys -eq 1} 1000) -and [NativeInputProbe]::LastModifiers -eq 2)
    [FpUi]::SendMessageW($h,0x401,[IntPtr]2,[IntPtr]::Zero)|Out-Null
    Key $h 0x74 0
    Check 'plain F5 is acknowledged with no modifiers' ([NativeInputProbe]::Keys -eq 2 -and [NativeInputProbe]::LastKey -eq 0x74 -and [NativeInputProbe]::LastModifiers -eq 0)
    Chord-Acknowledged $h @('Ctrl','Shift') 0x43 1000
    Check 'explicit Ctrl Shift chord retains its modifiers' ([NativeInputProbe]::LastModifiers -eq 3)
    [FpUi]::SendMessageW($h,0x401,[IntPtr]7,[IntPtr]::Zero)|Out-Null
    Send-AcknowledgedText $edit 'Case:literal' 1000
    Check 'character input reaches the native edit exactly' ([FpUi]::ReadText($edit,1000) -ceq 'Case:literal')
    Check 'all characters are acknowledged without inherited modifiers' ([NativeInputProbe]::Characters -eq 12 -and [NativeInputProbe]::CharacterModifiers -eq 0)
    $before=[NativeInputProbe]::Keys;$rejected=$false
    try{[FpInput]::Key([IntPtr]::Zero,$script:demoPid,0x74,[IntPtr]::Zero,1000)}catch{$rejected=$true}
    Check 'missing target fails before input' ($rejected -and [NativeInputProbe]::Keys -eq $before)
    $rejected=$false
    try{[FpInput]::Key($h,($script:demoPid+1),0x74,[IntPtr]::Zero,1000)}catch{$rejected=$true}
    Check 'wrong process owner fails before input' ($rejected -and [NativeInputProbe]::Keys -eq $before)
    $rejected=$false
    try{[FpInput]::Key($h,$script:demoPid,0x74,[IntPtr]::Zero,0)}catch{$rejected=$true}
    Check 'expired deadline sends no key' ($rejected -and [NativeInputProbe]::Keys -eq $before)
    $task=Start-ModalKey $h 0x77 2000
    Check 'modal input leaves the acceptance thread available' ((Wait-Until {[NativeInputProbe]::Modal -eq 1} 1000) -and -not $task.IsCompleted)
    Post $h 0x402 0
    Complete-ModalKey $task 1500
    Check 'modal key completes after the owned response' ($task.IsCompleted -and [NativeInputProbe]::LastModifiers -eq 0)
    $task=Start-ModalKey $h 0x77 100
    $timeout=$false
    try{Complete-ModalKey $task 1000}catch{$timeout=$true}finally{Post $h 0x402 0}
    Check 'blocked input reports its deadline failure' $timeout
    Key $h 0x74 0
    Check 'input remains usable after a timeout' ([NativeInputProbe]::LastModifiers -eq 0 -and [NativeInputProbe]::LastKey -eq 0x74)
} finally {Post $h 0x402 0;Post $h 0x10 0;[NativeInputProbe]::Join()}
"$passed passed, 0 failed"
