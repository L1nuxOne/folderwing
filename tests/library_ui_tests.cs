// Real controls, real layout, and a parent that acknowledges reader state.
using System;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;
internal sealed class LibraryUiTests : Form
{
    [StructLayout(LayoutKind.Sequential)] struct CopyData{public IntPtr Type;public int Size;public IntPtr Data;}
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd,int msg,IntPtr w,ref CopyData data);
    static int checks;
    string previewPath="";uint previewSession;int previews;
    static T Field<T>(object o,string name){return (T)o.GetType().GetField(name,BindingFlags.Instance|BindingFlags.NonPublic).GetValue(o);}
    static object Call(object o,string name,params object[] values){return o.GetType().GetMethod(name,BindingFlags.Instance|BindingFlags.NonPublic).Invoke(o,values);}
    static void Check(bool value,string text){if(!value)throw new Exception(text);checks++;Console.WriteLine("ok "+text);}
    static bool Wait(Func<bool> ready){var until=DateTime.UtcNow.AddSeconds(20);do{Application.DoEvents();if(ready())return true;Thread.Sleep(10);}while(DateTime.UtcNow<until);return false;}
    static void Pump(int ms){var end=DateTime.UtcNow.AddMilliseconds(ms);do{Application.DoEvents();Thread.Sleep(10);}while(DateTime.UtcNow<end);}
    void MessageTo(IntPtr target,long type,byte[] bytes){IntPtr p=Marshal.AllocHGlobal(bytes.Length);try{Marshal.Copy(bytes,0,p,bytes.Length);var data=new CopyData{Type=new IntPtr(type),Size=bytes.Length,Data=p};SendMessage(target,0x4A,Handle,ref data);}finally{Marshal.FreeHGlobal(p);}}
    void ReaderState(IntPtr target,uint session,bool open,string path){byte[] text=System.Text.Encoding.Unicode.GetBytes(path+"\0"),bytes=new byte[text.Length+8];Buffer.BlockCopy(BitConverter.GetBytes(session),0,bytes,0,4);Buffer.BlockCopy(BitConverter.GetBytes(open?1:0),0,bytes,4,4);Buffer.BlockCopy(text,0,bytes,8,text.Length);MessageTo(target,0x46574C03,bytes);}
    protected override void WndProc(ref Message m){
        if(m.Msg==0x4A){
            var d=(CopyData)Marshal.PtrToStructure(m.LParam,typeof(CopyData));
            if(d.Type.ToInt64()==0x46574C02){int action=Marshal.ReadInt32(d.Data,4);uint session=(uint)Marshal.ReadInt32(d.Data);string path=Marshal.PtrToStringUni(new IntPtr(d.Data.ToInt64()+8));IntPtr sender=m.WParam;
                if(action==1||action==5){previewSession=session;previewPath=path;previews++;}
                if(action==1||action==4||action==5)BeginInvoke(new Action(delegate{if(IsWindowTarget(sender))ReaderState(sender,session,action!=4,path);}));
            }
            m.Result=new IntPtr(1);return;
        }
        base.WndProc(ref m);
    }
    [DllImport("user32.dll",EntryPoint="IsWindow")] static extern bool IsWindowTarget(IntPtr h);
    void Switch(LibraryHost host,string root,string type,uint token){byte[] text=System.Text.Encoding.Unicode.GetBytes(root+"\0Switched\0"),bytes=new byte[text.Length+8];Buffer.BlockCopy(BitConverter.GetBytes(token),0,bytes,0,4);Buffer.BlockCopy(BitConverter.GetBytes(type=="calibre"?1:2),0,bytes,4,4);Buffer.BlockCopy(text,0,bytes,8,text.Length);MessageTo(host.Handle,0x46574C01,bytes);}
    void SwitchMany(LibraryHost host,uint token,params LibrarySource[] sources){using(var stream=new MemoryStream())using(var output=new BinaryWriter(stream)){output.Write(token);output.Write((uint)sources.Length);foreach(var source in sources){output.Write(source.Kind=="calibre"?1u:2u);output.Write(System.Text.Encoding.Unicode.GetBytes(source.Path+"\0"+source.Name+"\0"));}MessageTo(host.Handle,0x46574C04,stream.ToArray());}}
    static void Select(LibraryHost host,long id){var rows=Field<List<LibraryItem>>(host,"visible");int at=rows.FindIndex(i=>i.Id==id);if(at<0)throw new Exception("Missing fixture item "+id);var list=Field<ListView>(host,"list");list.SelectedIndices.Clear();list.Items[at].Selected=true;}
    static void Pick(LibraryHost host,long id){var item=(LibraryItem)Call(host,"Selected");int at=item.Attachments.FindIndex(a=>a.Id==id);if(at<0)throw new Exception("Missing attachment "+id);Field<ComboBox>(host,"attachmentPicker").SelectedIndex=at;}
    static bool Loaded(LibraryHost host,string kind){var c=Field<LibraryCatalog>(host,"catalog");return c!=null&&c.Kind==kind&&!Field<bool>(host,"loading");}
    static bool Contained(Control root){foreach(Control c in root.Controls){if(!c.Visible)continue;if(c.Width>0&&c.Height>0){var r=Rectangle.Intersect(root.ClientRectangle,c.Bounds);if(r.Width<c.Width||r.Height<c.Height){Console.WriteLine("CLIPPED "+c.GetType().Name+" "+c.Text+" "+c.Bounds+" in "+root.GetType().Name+" "+root.ClientRectangle);return false;}}if(!Contained(c))return false;}return true;}
    static void LayoutChecks(LibraryHost host){
        var view=Field<LibraryViewState>(host,"view");view.Details=true;Call(host,"ArrangePanes");
        foreach(int width in new[]{1180,650,450,320,260}){
            host.ClientSize=new Size(width,650);Pump(150);var list=Field<ListView>(host,"list");var picker=Field<ComboBox>(host,"attachmentPicker");var button=Field<Button>(host,"preview");
            Check(Contained(host),"all visible controls fit at "+width+" px");
            Check(list.Columns[4].DisplayIndex==1&&list.Columns[0].Width+list.Columns[4].Width<=list.ClientSize.Width,"title and PDF size stay visible at "+width+" px");
            Check(picker.Visible&&button.Visible&&picker.Width>0,"attachment selection and reading remain reachable at "+width+" px");
        }
        Check(view.Details,"automatic details collapse preserves the user preference");
        host.ClientSize=new Size(1180,650);Pump(150);Check(!Field<SplitContainer>(host,"split").Panel2Collapsed,"details return when space returns");
    }
    static bool Key(LibraryHost host,Keys key){var m=Message.Create(host.Handle,0,IntPtr.Zero,IntPtr.Zero);return (bool)Call(host,"ProcessCmdKey",m,key);}
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h,int message,IntPtr w,IntPtr l);
    static void GroupKey(Control control,Keys key){PostMessage(control.Handle,0x100,new IntPtr((int)key),new IntPtr(1));PostMessage(control.Handle,0x101,new IntPtr((int)key),new IntPtr(1));Pump(80);}
    void PolishChecks(LibraryHost host)
    {
        var group=Field<LibraryGroupingPicker>(host,"group");var list=Field<ListView>(host,"list");var format=Field<ComboBox>(host,"format");var search=Field<TextBox>(host,"search");
        Check(Field<Label>(host,"title").Text=="Owned Calibre · Calibre","provider title keeps a distinct library name");
        Key(host,Keys.Control|Keys.G);Pump(50);Check(group.IsOpen,"Ctrl+G opens grouping search");
        var query=Field<TextBox>(group,"query");var matches=Field<ListBox>(group,"matches");
        Check(matches.Items.Count==200&&Field<Label>(group,"count").Text.Contains("of 302"),"large group lists are bounded and disclose remaining matches");
        query.Text="tAg 299";Check(matches.Items.Count==1&&matches.Items[0].ToString().Contains("299"),"group search finds case-insensitive substrings beyond the result cap");
        Check(list.VirtualListSize==3&&group.SelectedKey=="","typing a group search does not change the catalogue");
        GroupKey(query,Keys.Escape);Check(!group.IsOpen&&group.SelectedKey=="","Escape cancels grouping without applying a draft");
        group.OpenPicker();query.Text="Series One";var category=Field<List<RadioButton>>(group,"kinds");category.Single(c=>c.Text=="Tags").Checked=true;
        Check(matches.Items.Count==0&&!Field<Button>(group,"apply").Enabled,"category filters distinguish tags from series and explain no matches");
        category.Single(c=>c.Text=="Series").Checked=true;query.Focus();GroupKey(query,Keys.Down);Check(matches.Focused,"Down moves from grouping search to matching results");GroupKey(matches,Keys.Enter);Pump(40);
        Check(group.SelectedKey=="s:Series One"&&list.VirtualListSize==1&&!group.IsOpen,"Enter applies the matching series group");
        Key(host,Keys.Control|Keys.Shift|Keys.F);Check(list.VirtualListSize==3&&group.SelectedKey==""&&format.Text=="All formats"&&!Field<Button>(host,"clearFilters").Enabled,"clear shortcut resets all filters together");
        search.Text="__no_match__";Call(host,"Filter");
        Check(Field<System.Windows.Forms.Panel>(host,"emptySurface").Visible&&Field<Label>(host,"surfaceTitle").Text.Contains("No matching")&&Field<Button>(host,"surfaceAction").Text=="Clear filters","no-result state is visible in the catalogue with recovery");
        foreach(int width in new[]{260,650}){host.ClientSize=new Size(width,650);Pump(60);Check(Contained(host),"no-result controls fit at "+width+" px");}
        Field<Button>(host,"surfaceAction").PerformClick();Check(list.VirtualListSize==3&&!Field<System.Windows.Forms.Panel>(host,"emptySurface").Visible,"empty-state action restores catalogue rows");
        host.ClientSize=new Size(1180,650);Key(host,Keys.Control|Keys.F);Check(search.Focused,"Ctrl+F focuses metadata search");
        search.Text="First";Key(host,Keys.Enter);Check(list.Focused&&list.VirtualListSize==1,"Enter applies search and focuses results without reading");
        Key(host,Keys.F6);Check(Field<ComboBox>(host,"attachmentPicker").Focused,"F6 moves from catalogue to attachment choice");
        Key(host,Keys.Shift|Keys.F6);Check(list.Focused,"Shift+F6 returns to catalogue");
        Key(host,Keys.Control|Keys.F);Key(host,Keys.Escape);Check(search.Text==""&&list.VirtualListSize==3,"Escape clears a focused metadata search first");
        Check(list.OwnerDraw&&Field<ListView>(host,"attachments").OwnerDraw&&format.DrawMode==DrawMode.OwnerDrawFixed&&format.BackColor==LibraryTheme.Panel,"catalogue, attachments and selectors use consistent themed drawing");
    }
    void StateChecks(string fixture)
    {
        string root=Path.Combine(fixture,"recover"),state=Path.Combine(fixture,"recovery-view.json");
        using(var host=new LibraryHost(Handle,40,"zotero",root,"Zotero",state)){
            host.Show();Check(Wait(delegate{return Field<string>(host,"loadError").Length>0;}),"missing catalogue reaches a recoverable error state");
            Check(!File.Exists(Path.Combine(root,"zotero.sqlite"))&&!Field<Button>(host,"preview").Enabled&&!Field<TextBox>(host,"search").Enabled,"failed read-only opening creates no database and disables stale actions");
            Check(Field<Label>(host,"title").Text=="Zotero"&&Field<Button>(host,"surfaceAction").Text=="Retry","provider title is deduplicated and error offers Retry");
            host.ClientSize=new Size(260,650);Pump(50);Check(Contained(host),"error state wraps in a narrow catalogue");
            Directory.CreateDirectory(root);File.Copy(Path.Combine(fixture,"empty","zotero.sqlite"),Path.Combine(root,"zotero.sqlite"));Field<Button>(host,"surfaceAction").PerformClick();
            Check(Field<bool>(host,"loading")&&Field<ProgressBar>(host,"progress").Visible&&Field<Button>(host,"surfaceAction").Text=="Cancel","loading exposes progress and cancellation before results arrive");
            Field<Button>(host,"surfaceAction").PerformClick();Pump(80);
            Check(Field<bool>(host,"loadCancelled")&&!Field<bool>(host,"loading")&&Field<LibraryCatalog>(host,"catalog")==null&&Field<Button>(host,"surfaceAction").Text=="Retry","cancellation rejects late load completion and allows Retry");
            Key(host,Keys.F5);Check(Wait(delegate{return Loaded(host,"zotero");}),"F5 retries after cancellation");
            Check(Field<Label>(host,"surfaceTitle").Text=="This catalogue is empty"&&Field<Button>(host,"surfaceAction").Text=="Refresh"&&Contained(host),"genuinely empty catalogue has its own message and refresh action");
            Check(Field<TextBox>(host,"search").Enabled&&Field<LibraryGroupingPicker>(host,"group").Enabled,"successful retry restores filtering controls");
        }
        string legacy=Path.Combine(fixture,"legacy-view.json");var states=new LibraryStates(legacy);states.Get(Path.Combine(fixture,"calibre")+"\u001fcalibre:synthetic-calibre").Group="Series: Series One";states.Save();
        using(var host=new LibraryHost(Handle,41,"calibre",Path.Combine(fixture,"calibre"),"Calibre",legacy)){
            host.Show();Check(Wait(delegate{return Loaded(host,"calibre");}),"legacy view state loads");
            Check(Field<LibraryGroupingPicker>(host,"group").SelectedKey=="s:Series One"&&Field<ListView>(host,"list").VirtualListSize==1,"legacy label selection migrates to a stable group key");
        }
    }
    void CombinedChecks(string fixture)
    {
        var books=new LibrarySource{Kind="calibre",Path=Path.Combine(fixture,"calibre"),Name="Books"};var papers=new LibrarySource{Kind="zotero",Path=Path.Combine(fixture,"zotero"),Name="Papers"};
        using(var host=new LibraryHost(Handle,100,"calibre",books.Path,books.Name,Path.Combine(fixture,"combined-state.json"))){
            host.Show();Check(Wait(delegate{return Loaded(host,"calibre");}),"single library opens before combined selection");
            Field<TextBox>(host,"search").Text="Author Two";Call(host,"Filter");Call(host,"SaveView");
            SwitchMany(host,101,books,papers);Check(Wait(delegate{return Loaded(host,"combined");}),"typed multi-root handoff opens a combined catalogue");
            var list=Field<ListView>(host,"list");var rows=Field<List<LibraryItem>>(host,"visible");
            Check(rows.Count==5&&Field<TextBox>(host,"search").Text=="","combined view has independent filters and includes both sources");
            Check(list.Columns[6].DisplayIndex==2&&Enumerable.Range(0,5).All(i=>list.Items[i].SubItems[6].Text==rows[i].SourceName),"parent Library column labels every combined row");
            host.ClientSize=new Size(1180,650);Pump(60);Check(list.Columns[6].Width>=110,"parent library names have readable width in a normal pane");
            foreach(int width in new[]{1180,650,320,260}){host.ClientSize=new Size(width,650);Pump(70);Check(Contained(host)&&list.Columns[6].Width>0&&list.Columns[0].Width+list.Columns[4].Width+list.Columns[6].Width<=list.ClientSize.Width,"title, size and parent library fit at "+width+" px");}
            host.ClientSize=new Size(1180,650);var group=Field<LibraryGroupingPicker>(host,"group");group.SelectKey("t:Research");Check(list.VirtualListSize==2,"shared tag filter spans libraries");
            var collection=Field<LibraryCatalog>(host,"catalog").Groups.Single(g=>g.Name=="Research"&&g.SourceName=="Papers"&&g.StableKey.EndsWith("\u001f10",StringComparison.Ordinal));group.SelectKey(collection.StableKey);
            Check(list.VirtualListSize==1&&((LibraryItem)Call(host,"Selected")).SourceName=="Papers","collection filtering stays with its originating library");
            Check(Field<List<LibraryGroupOption>>(group,"options").Any(g=>g.Label.StartsWith("Collection: Papers /")),"combined collection labels name their library");
            Call(host,"ClearFilters");var research=Field<List<LibraryItem>>(host,"visible").Single(i=>i.Title=="Research paper");Select(host,research.Id);
            Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"combined reference resolves its own PDF");
            var appendix=research.Attachments.Single(a=>a.Path.EndsWith("appendix.pdf"));Pick(host,appendix.Id);Field<Button>(host,"preview").PerformClick();
            Check(Wait(delegate{return Field<bool>(host,"readerOpen")&&previewPath.EndsWith("appendix.pdf");})&&Field<Label>(host,"readingLabel").Text.Contains("Papers"),"reader context identifies the selected source library");
            var pin=Field<CheckBox>(host,"pinReader");pin.Checked=true;var book=Field<List<LibraryItem>>(host,"visible").Single(i=>i.Title=="First book");Select(host,book.Id);Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"other library book resolves despite colliding original IDs");int before=previews;Pump(250);
            Check(before==previews&&Field<Label>(host,"readingLabel").Text.Contains("Papers"),"pin keeps the original source while browsing another library");pin.Checked=false;
            Check(Wait(delegate{return previewPath.EndsWith("First book.pdf");})&&Field<Label>(host,"readingLabel").Text.Contains("Books"),"unpin follows the correct attachment in another library");
            Call(host,"SendAction",4,"");Check(Wait(delegate{return !Field<bool>(host,"readerOpen");}),"combined reader closes normally");Select(host,research.Id);Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"combined reference selection returns");Pick(host,appendix.Id);Call(host,"SaveView");string stable=research.StableKey;
            string db=Path.Combine(books.Path,"metadata.db"),offline=db+".offline";File.Move(db,offline);
            try{
                Call(host,"LoadCatalog");Check(Wait(delegate{return Loaded(host,"combined");}),"combined catalogue survives an unavailable source");
                Check(Field<LibraryCatalog>(host,"catalog").Failures.Count==1&&Field<Label>(host,"sourceWarning").Visible&&Field<Label>(host,"status").Text.Contains("Incomplete"),"partial view is visibly labelled incomplete");
                Check(((LibraryItem)Call(host,"Selected")).StableKey==stable&&((LibraryAttachment)Call(host,"Attachment")).Path.EndsWith("appendix.pdf"),"selection and attachment choice survive remapped numeric IDs");
            }finally{File.Move(offline,db);}
            Call(host,"LoadCatalog");Check(Wait(delegate{return Loaded(host,"combined");})&&!Field<Label>(host,"sourceWarning").Visible&&Field<ListView>(host,"list").VirtualListSize==5,"refresh restores recovered source and clears incomplete warning");
            Check(((LibraryItem)Call(host,"Selected")).StableKey==stable&&((LibraryAttachment)Call(host,"Attachment")).Path.EndsWith("appendix.pdf"),"source-qualified choice survives source recovery");
            Switch(host,books.Path,"calibre",102);Check(Wait(delegate{return Loaded(host,"calibre");})&&Field<TextBox>(host,"search").Text=="Author Two"&&list.Columns[6].Width==0,"return to one library restores its own view and hides redundant source column");
            SwitchMany(host,103,papers,books);Check(Wait(delegate{return Loaded(host,"combined");})&&((LibraryItem)Call(host,"Selected")).StableKey==stable,"combined preferences survive reordered checkbox selection");
            MessageTo(host.Handle,0x46574C04,new byte[]{1,0,0,0,2,0,0,0});Check(Field<LibraryCatalog>(host,"catalog")!=null&&Field<uint>(host,"session")==103,"truncated root-set payload cannot replace active catalogue");
            SwitchMany(host,104,books,new LibrarySource{Kind="zotero",Path=Path.Combine(fixture,"empty"),Name="Empty"});Check(Wait(delegate{return Loaded(host,"combined");})&&Field<ListView>(host,"list").VirtualListSize==3&&Field<LibraryCatalog>(host,"catalog").Failures.Count==0,"empty selected library contributes zero rows without a failure");
            string other=Path.Combine(fixture,"zotero-other");Directory.CreateDirectory(other);foreach(string file in Directory.GetFiles(papers.Path,"*",SearchOption.AllDirectories)){string target=Path.Combine(other,file.Substring(papers.Path.Length+1));Directory.CreateDirectory(Path.GetDirectoryName(target));File.Copy(file,target);}
            SwitchMany(host,105,papers,new LibrarySource{Kind="zotero",Path=other,Name="Second papers"});Check(Wait(delegate{return Loaded(host,"combined");})&&list.VirtualListSize==4,"two Zotero libraries retain duplicate references as separate rows");
            var otherGroup=Field<LibraryCatalog>(host,"catalog").Groups.Single(g=>g.SourceName=="Second papers"&&g.StableKey.EndsWith("\u001f10",StringComparison.Ordinal));group.SelectKey(otherGroup.StableKey);
            Check(list.VirtualListSize==1&&((LibraryItem)Call(host,"Selected")).SourceName=="Second papers","colliding collection IDs in separate Zotero databases cannot cross-filter");
            Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;})&&((LibraryAttachment)Call(host,"Attachment")).Path.StartsWith(other+"\\",StringComparison.OrdinalIgnoreCase),"colliding attachment IDs still resolve inside their parent library");

        }
    }
    void Run(string fixture){
        string state=Path.Combine(fixture,"library-ui.json");
        using(var host=new LibraryHost(Handle,1,"calibre",Path.Combine(fixture,"calibre"),"Owned Calibre",state)){
            Check(!Field<Button>(host,"preview").Enabled,"reading starts disabled before catalogue readiness");host.Show();Check(Wait(delegate{return Loaded(host,"calibre");}),"Calibre surface opens");var list=Field<ListView>(host,"list");
            Check(list.VirtualMode&&list.VirtualListSize==3,"catalogue list is virtualized");PolishChecks(host);Field<ComboBox>(host,"format").SelectedItem="PDF";Check(list.VirtualListSize==2,"PDF filter selects PDF books");
            typeof(ListView).GetMethod("OnColumnClick",BindingFlags.NonPublic|BindingFlags.Instance).Invoke(list,new object[]{new ColumnClickEventArgs(4)});
            Check(Field<List<LibraryItem>>(host,"visible")[0].Id==1&&Field<LibraryViewState>(host,"view").Descending,"sort ranks PDF size rather than larger EPUB");
            Check(list.Columns[4].Text.StartsWith("PDF size"),"size heading identifies the selected format");
            Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"available PDF enables reading");
            LayoutChecks(host);Field<Button>(host,"preview").PerformClick();Check(Wait(delegate{return Field<bool>(host,"readerOpen");}),"parent acknowledgement opens the reader context");
            Check(previewSession==1&&previewPath.EndsWith("First book.pdf"),"reader action uses selected format and session");
            host.ClientSize=new Size(450,650);Pump(150);Check(Contained(host)&&Field<TableLayoutPanel>(host,"readingBar").Height<=50,"reader context remains compact and contained in a narrow catalogue");host.ClientSize=new Size(1180,650);Pump(100);
            Field<TextBox>(host,"search").Text="Author Two";Check(Wait(delegate{return list.VirtualListSize==1;}),"creator search filters catalogue");
            Field<Button>(host,"detailsToggle").PerformClick();list.Columns[0].Width=410;Call(host,"SaveView");host.Close();
        }
        using(var host=new LibraryHost(Handle,2,"calibre",Path.Combine(fixture,"calibre"),"Owned Calibre",state)){
            host.Show();Check(Wait(delegate{return Loaded(host,"calibre");}),"library reopens");
            Check(Field<TextBox>(host,"search").Text=="Author Two"&&Field<ComboBox>(host,"format").Text=="PDF"&&Field<int[]>(host,"preferredColumns")[0]==410&&Field<SplitContainer>(host,"split").Panel2Collapsed,"filter, preferred widths and manual details preference restore: "+Field<TextBox>(host,"search").Text+" | "+Field<ComboBox>(host,"format").Text+" | "+Field<int[]>(host,"preferredColumns")[0]+" | "+Field<SplitContainer>(host,"split").Panel2Collapsed);
            Switch(host,Path.Combine(fixture,"zotero"),"zotero",3);Check(Wait(delegate{return Loaded(host,"zotero");}),"same helper switches to Zotero");
            var group=Field<LibraryGroupingPicker>(host,"group");Check(Field<List<LibraryGroupOption>>(group,"options").Any(o=>o.Label=="Collection: Research / Reading"),"collection hierarchy is available");group.SelectKey("c:10");
            Check(Field<ListView>(host,"list").VirtualListSize==1,"parent collection includes descendants");Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"stored Zotero PDF resolves locally");
            var picker=Field<ComboBox>(host,"attachmentPicker");Check(picker.Items.Cast<object>().Any(o=>o.ToString().Contains("paper.pdf"))&&picker.Items.Cast<object>().Any(o=>o.ToString().Contains("appendix.pdf")),"multiple PDFs have distinct filename labels");
            Field<ComboBox>(host,"format").SelectedItem="PDF";var view=Field<LibraryViewState>(host,"view");view.Sort=4;view.Descending=true;Call(host,"Filter");
            Check(Wait(delegate{return !Field<bool>(host,"sizeBusy");}),"attachment size query finishes");
            var rows=Field<ListView>(host,"attachments");var stored=rows.Items.Cast<ListViewItem>().Single(r=>((LibraryAttachment)r.Tag).Id==2);
            Check(stored.SubItems[2].Text!="Unknown"&&stored.SubItems[2].Text==Field<ListView>(host,"list").Items[0].SubItems[4].Text,"catalogue and attachment share resolved size");
            host.ClientSize=new Size(450,650);Pump(150);Pick(host,8);Check((LibraryAttachment)Call(host,"Attachment")!=null&&((LibraryAttachment)Call(host,"Attachment")).Id==8,"collapsed details still allow alternate attachment selection");
            Field<Button>(host,"preview").PerformClick();Check(Wait(delegate{return Field<bool>(host,"readerOpen")&&previewPath.EndsWith("appendix.pdf");}),"alternate PDF opens from the compact chooser");
            Key(host,Keys.Control|Keys.G);GroupKey(Field<TextBox>(group,"query"),Keys.Escape);Check(Field<bool>(host,"readerOpen")&&!group.IsOpen,"Escape dismisses group search without closing the reader");
            Field<ComboBox>(host,"format").DroppedDown=true;Key(host,Keys.Escape);Check(Field<bool>(host,"readerOpen")&&!Field<ComboBox>(host,"format").DroppedDown,"Escape dismisses a format dropdown before closing the reader");
            var pin=Field<CheckBox>(host,"pinReader");Key(host,Keys.Alt|Keys.P);Check(pin.Checked,"Alt+P pins the open reader");group.SelectKey("");Select(host,5);Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"second reference has a readable PDF");int before=previews;Pump(350);
            Check(previews==before&&previewPath.EndsWith("appendix.pdf"),"pinned reader stays on its original PDF");
            Check(Field<Label>(host,"readingLabel").Text.Contains("Research paper")&&Field<TextBox>(host,"metadata").Text.Contains("Standalone PDF"),"reading label distinguishes pinned book from selected reference");
            Key(host,Keys.Alt|Keys.P);Check(!pin.Checked,"Alt+P unpins the reader");Check(Wait(delegate{return previewPath.EndsWith("standalone.pdf");}),"unpin follows the currently selected PDF");
            Select(host,1);Check(Wait(delegate{return previewPath.EndsWith("appendix.pdf");}),"returning to a reference remembers its chosen attachment");
            before=previews;Select(host,5);Select(host,1);Pump(350);Check(previews==before,"rapid selection changes do not open intermediate PDFs");
            ReaderState(host.Handle,1,true,@"C:\stale.pdf");Check(Field<string>(host,"readingPath").EndsWith("appendix.pdf"),"stale reader feedback is ignored");
            Call(host,"SendAction",4,"");Check(Wait(delegate{return !Field<bool>(host,"readerOpen");}),"reader close feedback clears follow state");before=previews;Select(host,5);Pump(350);Check(previews==before,"selection does not reopen a closed reader");
            Select(host,1);Check(Wait(delegate{return Field<Button>(host,"preview").Enabled;}),"remembered reference is ready");
            Switch(host,Path.Combine(fixture,"empty"),"zotero",4);Check(Wait(delegate{var c=Field<LibraryCatalog>(host,"catalog");return c!=null&&c.Root.EndsWith("empty")&&!Field<bool>(host,"loading");}),"empty catalogue opens");
            Check(Field<ListView>(host,"list").VirtualListSize==0&&!Field<Button>(host,"preview").Enabled&&Field<TextBox>(host,"metadata").Text.Contains("empty"),"empty library clears attachment actions");
            Switch(host,Path.Combine(fixture,"zotero"),"zotero",5);Check(Wait(delegate{return Loaded(host,"zotero")&&Field<Button>(host,"preview").Enabled;}),"Zotero reopens with its saved selection");
            Check(((LibraryAttachment)Call(host,"Attachment")).Id==8,"attachment choice survives root switching");
            string paper=Path.Combine(fixture,"zotero","storage","ABCDEFGH","paper.pdf");File.Move(paper,paper+".saved");
            view=Field<LibraryViewState>(host,"view");view.Attachments.Clear();Call(host,"LoadCatalog");
            Check(Wait(delegate{return Loaded(host,"zotero")&&Field<Button>(host,"preview").Enabled;}),"missing first PDF still leaves a readable attachment");
            Check(((LibraryAttachment)Call(host,"Attachment")).Id==8,"default selection prefers the available PDF");
            group.SelectKey("c:20");Check(Field<ListView>(host,"list").VirtualListSize==1&&((LibraryItem)Call(host,"Selected")).Id==5,"duplicate collection labels select by collection identity");
            Switch(host,Path.Combine(fixture,"empty"),"zotero",6);Check(Wait(delegate{return Field<LibraryCatalog>(host,"catalog")!=null&&Field<LibraryCatalog>(host,"catalog").Root.EndsWith("empty")&&!Field<bool>(host,"loading");}),"duplicate-name view can switch away");
            Switch(host,Path.Combine(fixture,"zotero"),"zotero",7);Check(Wait(delegate{return Loaded(host,"zotero");}),"duplicate-name view reopens");
            Check(group.SelectedKey=="c:20"&&((LibraryItem)Call(host,"Selected")).Id==5,"stable collection identity survives view restoration");host.Close();
        }
        StateChecks(fixture);CombinedChecks(fixture);
    }
    [STAThread] static int Main(string[] args){if(args.Length!=1)return 2;Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);Application.ThreadException+=delegate(object sender,ThreadExceptionEventArgs e){Console.Error.WriteLine(e.Exception);Environment.Exit(1);};int result=0;Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);using(var parent=new LibraryUiTests{StartPosition=FormStartPosition.Manual,Location=new Point(-32000,-32000),ShowInTaskbar=false}){parent.Shown+=delegate{parent.BeginInvoke(new Action(delegate{try{parent.Run(args[0]);Console.WriteLine(checks+" library UI checks passed");}catch(Exception e){Console.Error.WriteLine(e);result=1;}finally{parent.Close();}}));};Application.Run(parent);}return result;}
}
