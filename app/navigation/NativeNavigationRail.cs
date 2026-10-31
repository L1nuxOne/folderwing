using System;
using System.Collections.Generic;
using System.Drawing;
using System.Threading.Tasks;
using System.Windows.Forms;

// No work runs while idle. Position changes repaint; context changes reload the
// bounded local store. The caller owns the mapping to rows or source lines.
internal sealed class NativeNavigationRail : Control
{
    readonly NavigationStore store;
    List<NavigationMark> marks=new List<NavigationMark>();
    string context="",error="";bool busy,hover,drag;int mouseY,revision;
    public Func<string> Context;
    public Func<uint> Minimum,Maximum,Position;
    public Func<bool> Available;
    public Func<uint,string> Label;
    public Action<uint> Jump;
    public NativeNavigationRail(NavigationStore source){store=source;Dock=DockStyle.Right;Width=28;BackColor=Color.FromArgb(28,31,38);ForeColor=Color.FromArgb(225,229,235);TabStop=true;AccessibleName="Reading positions and bookmarks";SetStyle(ControlStyles.AllPaintingInWmPaint|ControlStyles.OptimizedDoubleBuffer|ControlStyles.UserPaint|ControlStyles.ResizeRedraw,true);}
    bool Ready {get{return Available!=null&&Available();}}
    uint Min {get{return Minimum==null?0:Minimum();}}
    uint Max {get{return Math.Max(Min,Maximum==null?0:Maximum());}}
    uint At {get{return Position==null?0:Position();}}
    float Tick(uint value){return (float)(42+(Height-54)*(Max>Min?Math.Max(0,Math.Min(1,((double)value-Min)/(Max-Min))):0));}
    uint Value(int y){return (uint)Math.Round(Min+(Max-Min)*Math.Max(0,Math.Min(1,(double)(y-42)/Math.Max(1,Height-54))));}
    NavigationMark Current(){return marks.Find(delegate(NavigationMark mark){return mark.page==At;});}
    public void Changed(){string next=Context==null?"":Context();if(next!=context){context=next;marks.Clear();error="";++revision;Load();}Invalidate();}
    async void Load(){if(String.IsNullOrEmpty(context)||busy)return;int version=revision;string key=context;try{var loaded=await Task.Run(delegate{return store.Load(key);});if(IsDisposed||version!=revision)return;marks=loaded;error="";}catch(Exception e){if(!IsDisposed&&version==revision)error=NavigationStore.Error(e);}if(!IsDisposed)Invalidate();}
    public async void Toggle(){if(!Ready||busy)return;string key=context;int version=++revision;uint at=At;string title=Label==null?"Position "+at:Label(at);busy=true;
        try{var updated=await Task.Run(delegate{var latest=store.Load(key);var found=latest.Find(delegate(NavigationMark m){return m.page==at;});if(found!=null)store.Remove(key,found.id);else store.Add(key,at,0,title);return store.Load(key);});if(!IsDisposed&&version==revision){marks=updated;error="";}}
        catch(Exception e){if(!IsDisposed&&version==revision)error=NavigationStore.Error(e);}finally{busy=false;if(!IsDisposed)Invalidate();}
    }
    async void Remove(NavigationMark mark){if(busy)return;string key=context;int version=++revision;busy=true;try{var loaded=await Task.Run(delegate{store.Remove(key,mark.id);return store.Load(key);});if(!IsDisposed&&version==revision){marks=loaded;error="";}}catch(Exception e){if(!IsDisposed&&version==revision)error=NavigationStore.Error(e);}finally{busy=false;if(!IsDisposed)Invalidate();}}
    public void ShowMarks(){var menu=new ContextMenuStrip();menu.Closed+=delegate{menu.Dispose();};if(error.Length>0)menu.Items.Add(error).Enabled=false;if(marks.Count==0)menu.Items.Add("No saved positions here yet").Enabled=false;
        foreach(var item in marks){var mark=item;var entry=new ToolStripMenuItem("★ "+mark.title);entry.Click+=delegate{if(Ready&&Jump!=null)Jump(mark.page);};entry.DropDownItems.Add("Remove bookmark",null,delegate{Remove(mark);});menu.Items.Add(entry);}menu.Items.Add(new ToolStripSeparator());var toggle=menu.Items.Add(Current()==null?"Bookmark this position":"Remove bookmark here",null,delegate{Toggle();});toggle.Enabled=Ready&&!busy;menu.Show(this,new Point(0,30));Load();
    }
    protected override void OnPaint(PaintEventArgs e){base.OnPaint(e);var g=e.Graphics;uint min=Min,max=Max;for(int i=0;i<=20;i++)DrawTick(g,Tick((uint)(min+(max-min)*(double)i/20)),false);
        foreach(var mark in marks)if(mark.page>=min&&mark.page<=max)DrawTick(g,Tick(mark.page),true);
        if(Ready)using(var b=new SolidBrush(Color.FromArgb(110,180,255)))g.FillRectangle(b,Width-3,Tick(At)-3,3,7);
        var points=new[]{new Point(8,5),new Point(20,5),new Point(20,25),new Point(14,20),new Point(8,25)};Color c=!Ready?Color.Gray:error.Length>0?Color.Salmon:Current()!=null?Color.FromArgb(232,190,90):ForeColor;
        if(Current()!=null)using(var b=new SolidBrush(c))g.FillPolygon(b,points);using(var p=new Pen(c))g.DrawPolygon(p,points);
        if(hover){uint value=Value(mouseY);string text=mouseY<30?(error.Length>0?error:Current()==null?"Bookmark this position (Ctrl+D)":"Remove bookmark (Ctrl+D)"):(Label==null?value.ToString():Label(value));var nearest=marks.Find(delegate(NavigationMark m){return m.page>=min&&m.page<=max&&Math.Abs(Tick(m.page)-mouseY)<7;});if(mouseY>=30&&nearest!=null)text="★ "+nearest.title;AccessibleDescription=text;tip.SetToolTip(this,text+"\nRight-click for personal bookmarks");}
    }
    readonly ToolTip tip=new ToolTip();
    void DrawTick(Graphics g,float y,bool personal){float length=personal?14:10;if(hover)length+=(float)Math.Max(0,42-Math.Abs(y-mouseY))*10/42;using(var brush=new SolidBrush(personal?Color.FromArgb(232,190,90):Color.FromArgb(165,173,185)))g.FillRectangle(brush,Width-4-length,y,length,personal?3:1);}
    void Seek(int y){if(!Ready||Jump==null)return;uint value=Value(y);var nearest=marks.Find(delegate(NavigationMark m){return m.page>=Min&&m.page<=Max&&Math.Abs(Tick(m.page)-y)<7;});Jump(nearest==null?value:nearest.page);}
    protected override void OnMouseMove(MouseEventArgs e){base.OnMouseMove(e);hover=true;mouseY=e.Y;if(drag)Seek(e.Y);Invalidate();}
    protected override void OnMouseLeave(EventArgs e){base.OnMouseLeave(e);hover=false;Invalidate();}
    protected override void OnMouseDown(MouseEventArgs e){base.OnMouseDown(e);Focus();if(e.Button==MouseButtons.Right){ShowMarks();return;}if(e.Button!=MouseButtons.Left)return;if(e.Y<30)Toggle();else{drag=true;Capture=true;Seek(e.Y);}}
    protected override void OnMouseUp(MouseEventArgs e){base.OnMouseUp(e);drag=false;Capture=false;}
    protected override void OnMouseCaptureChanged(EventArgs e){base.OnMouseCaptureChanged(e);if(!Capture)drag=false;}
    protected override bool IsInputKey(Keys key){return key==Keys.Up||key==Keys.Down||key==Keys.Home||key==Keys.End||base.IsInputKey(key);}
    protected override void OnKeyDown(KeyEventArgs e){base.OnKeyDown(e);if(!Ready||Jump==null)return;if(e.KeyCode==Keys.Home)Jump(Min);else if(e.KeyCode==Keys.End)Jump(Max);else if(e.KeyCode==Keys.Up)Jump(At>Min?At-1:Min);else if(e.KeyCode==Keys.Down)Jump(At<Max?At+1:Max);else return;e.Handled=true;}
    protected override void Dispose(bool disposing){if(disposing)tip.Dispose();base.Dispose(disposing);}
}
