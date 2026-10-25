<# Opt-in keyboard isolation for UI acceptance. Dot-source after uilib.ps1.
   Plain keys and characters must not inherit the desktop's modifier state.
   Chords retain their explicit modifiers. All targets belong to this demo. #>
if (-not ('FpInput' -as [type])) {
    Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Diagnostics;
using System.Threading;
public static class FpInput {
  public static double LastKeyMs;
  public static int KeySamples;
  [DllImport("user32.dll",SetLastError=true)] static extern bool AttachThreadInput(uint a,uint b,bool attach);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] static extern bool GetKeyboardState(byte[] state);
  [DllImport("user32.dll")] static extern bool SetKeyboardState(byte[] state);
  [StructLayout(LayoutKind.Sequential)] struct MSG {public IntPtr h;public uint m;public IntPtr w,l;public uint time;public int x,y;public uint p;}
  [DllImport("user32.dll")] static extern bool PeekMessageW(out MSG m,IntPtr h,uint first,uint last,uint remove);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint pid);
  [DllImport("user32.dll",SetLastError=true)] static extern IntPtr SendMessageTimeoutW(IntPtr h,uint msg,UIntPtr key,IntPtr data,uint flags,uint timeout,out UIntPtr result);
  static uint Owned(IntPtr h,uint owner) {
    uint pid,thread=GetWindowThreadProcessId(h,out pid);
    if(h==IntPtr.Zero||thread==0||pid!=owner)throw new InvalidOperationException("Input target is not owned by the current demo");
    return thread;
  }
  static uint Remaining(Stopwatch clock,int timeout) {
    long remaining=timeout-clock.ElapsedMilliseconds;
    if(remaining<=0)throw new TimeoutException("Input exceeded its deadline");return (uint)remaining;
  }
  static void Input(IntPtr h,uint owner,uint message,uint key,IntPtr data,Stopwatch clock,int timeout) {
    Owned(h,owner);UIntPtr result;
    if(SendMessageTimeoutW(h,message,(UIntPtr)key,data,0x23,Remaining(clock,timeout),out result)==IntPtr.Zero)
      throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Input was not acknowledged within its deadline");
  }
  static void Isolated(IntPtr h,uint owner,int timeout,Action<Stopwatch> send) {
    var clock=Stopwatch.StartNew();
    uint me=GetCurrentThreadId(),them=Owned(h,owner);
    MSG pending;PeekMessageW(out pending,IntPtr.Zero,0,0,0);
    bool attached=false;byte[] original=null;
    try {
      if(me!=them) {
        if(!AttachThreadInput(me,them,true))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Could not attach input");
        attached=true;
      }
      var state=new byte[256];
      if(!GetKeyboardState(state))throw new InvalidOperationException("Could not read keyboard state");
      original=(byte[])state.Clone();
      foreach(var key in new[]{0x10,0x11,0x12,0xA0,0xA1,0xA2,0xA3,0xA4,0xA5})state[key]&=0x7f;
      if(!SetKeyboardState(state))throw new InvalidOperationException("Could not isolate input modifiers");
      Remaining(clock,timeout);send(clock);
    } finally {
      try {if(original!=null&&!SetKeyboardState(original))throw new InvalidOperationException("Could not restore keyboard state");}
      finally {if(attached&&!AttachThreadInput(me,them,false))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(),"Could not detach input");}
    }
  }
  public static void Text(IntPtr h,string text,uint owner,int timeout) {
    Isolated(h,owner,timeout,clock=>{foreach(char c in text)Input(h,owner,0x102,c,IntPtr.Zero,clock,timeout);});
  }
  public static void Key(IntPtr h,uint owner,int key,IntPtr data,int timeout) {
    Isolated(h,owner,timeout,clock=>{
      var dispatch=Stopwatch.StartNew();Input(h,owner,0x100,(uint)key,data,clock,timeout);
      LastKeyMs=dispatch.Elapsed.TotalMilliseconds;Interlocked.Increment(ref KeySamples);
    });
  }
  // A key may enter a modal loop. Keep its modifier scope on a worker while
  // the acceptance thread observes and responds to that owned dialog.
  public static Task BeginKey(IntPtr h,uint owner,int key,IntPtr data,int timeout) {
    Owned(h,owner);return Task.Run(()=>Key(h,owner,key,data,timeout));
  }
}
"@
}
function Key($h,[int]$vk,[int]$ms=200) {
    Send-AcknowledgedKey $h $vk 3000
    if($ms -gt 0){Start-Sleep -Milliseconds $ms}
}
function Send-AcknowledgedKey($h,[int]$vk,[int]$ms=3000) {
    [FpInput]::Key($h,$script:demoPid,$vk,[IntPtr](Key-LParam $vk),$ms)
}
function Send-AcknowledgedText($h,[string]$text,[int]$ms=3000) {
    [FpInput]::Text($h,$text,$script:demoPid,$ms)
}
function Send-Text($h,[string]$text) {
    Send-AcknowledgedText $h $text 3000
    Start-Sleep -Milliseconds 200
}
function Start-ModalKey($h,[int]$vk,[int]$ms) {
    [FpInput]::BeginKey($h,$script:demoPid,$vk,[IntPtr](Key-LParam $vk),[uint32]$ms)
}
function Complete-ModalKey($task,[int]$ms) {
    if(-not $task.Wait($ms)){throw "Modal key did not complete within${ms}ms"}
    $task.GetAwaiter().GetResult()
}
