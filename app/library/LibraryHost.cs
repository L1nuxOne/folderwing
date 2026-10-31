using System;
using System.Collections.Generic;
using System.Linq;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Timer=System.Windows.Forms.Timer;

internal sealed class LibraryHost : Form
{
    static readonly string[] Headers={"Title","Author / creator","Year / date","Formats","Size","Added","Library"};
    const long RootCommand=0x46574C01,ActionCommand=0x46574C02,ReaderCommand=0x46574C03,SelectionCommand=0x46574C04;
    const int Ready=0x8000+110,FocusCatalogueMessage=0x8000+114;
    [StructLayout(LayoutKind.Sequential)] struct CopyData {public IntPtr Type;public int Size;public IntPtr Data;}
    [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] static extern IntPtr Cue(IntPtr hwnd,uint message,IntPtr w,string text);
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child,IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd,int message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SetWindowLongW")] static extern int SetWindowLong(IntPtr hwnd,int index,int value);
    [DllImport("user32.dll",EntryPoint="GetWindowLongW")] static extern int GetWindowLong(IntPtr hwnd,int index);
    [DllImport("user32.dll")] static extern IntPtr SendMessageTimeout(IntPtr hwnd,int message,IntPtr w,ref CopyData data,uint flags,uint timeout,out IntPtr result);
    static readonly Color Background=LibraryTheme.Background,Panel=LibraryTheme.Panel,TextColor=LibraryTheme.Text;
    readonly IntPtr parent;
    readonly LibraryStates states;
    readonly Timer lifetime=new Timer(),searchTimer=new Timer(),saveTimer=new Timer(),refreshTimer=new Timer(),followTimer=new Timer();
    readonly ToolTip tips=new ToolTip();
    readonly TextBox search=new TextBox(),metadata=new TextBox();
    readonly ComboBox format=new ComboBox(),attachmentPicker=new ComboBox();
    readonly LibraryGroupingPicker group=new LibraryGroupingPicker();
    readonly ListView list=new LibraryListView(),attachments=new LibraryListView();
    readonly Label title=new Label(),status=new Label(),readingLabel=new Label(),sourceWarning=new Label();
    readonly PictureBox cover=new PictureBox();
    readonly SplitContainer split=new SplitContainer();
    readonly Button preview=new Button(),detailsToggle=new Button(),closeReader=new Button(),clearFilters=new Button(),refresh=new Button(),surfaceAction=new Button();
    readonly System.Windows.Forms.Panel emptySurface=new System.Windows.Forms.Panel();
    readonly Label surfaceTitle=new Label(),surfaceMessage=new Label();
    readonly ProgressBar progress=new ProgressBar();
    readonly CheckBox pinReader=new CheckBox();
    readonly TableLayoutPanel readingBar=new TableLayoutPanel();
    readonly Dictionary<string,long> sizes=new Dictionary<string,long>(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<long,string> availability=new Dictionary<long,string>();
    readonly int[] preferredColumns={320,200,100,100,100,110,140};
    List<LibrarySource> sources=new List<LibrarySource>();
    readonly Dictionary<string,DateTime> databaseStamps=new Dictionary<string,DateTime>(StringComparer.OrdinalIgnoreCase);
    bool selectionStarted;string readingKey="",pendingReadingKey="";
    bool Combined {get{return sources.Count>1;}}
    LibraryCatalog catalog;
    List<LibraryItem> visible=new List<LibraryItem>();
    LibraryItem detailItem;
    LibraryViewState view;
    CancellationTokenSource request;
    int generation,detailGeneration;
    uint session;
    string root,kind,name,detailFormat="",readingPath="",loadError="",sizeError="";
    bool loadCancelled;int sizePending;
    bool restoring,loading,sizeBusy,viewDirty,restorePosition,arranging,arrangingColumns,bindingAttachments,manualAttachment,readerOpen;

    sealed class AttachmentOption
    {
        public LibraryAttachment Attachment;public string Text;
        public override string ToString(){return Text;}
    }

    public LibraryHost(IntPtr target,uint token,string type,string path,string label,string stateFile,bool deferInitial=false)
    {
        parent=target;session=token;root=path;kind=type;name=label;sources.Add(new LibrarySource{Kind=type,Path=path,Name=label});
        try{states=new LibraryStates(stateFile);}catch{states=null;}
        FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;StartPosition=FormStartPosition.Manual;
        Location=new Point(-32000,-32000);ClientSize=new Size(900,650);
        BackColor=Background;ForeColor=TextColor;Font=new Font("Segoe UI",9);KeyPreview=true;

        var body=Table(1,6);body.Padding=new Padding(6);
        body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        body.RowStyles.Add(new RowStyle(SizeType.Absolute,66));
        body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        body.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        body.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        body.RowStyles.Add(new RowStyle(SizeType.Absolute,36));Controls.Add(body);
        var bar=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=true,AutoSize=true,AutoSizeMode=AutoSizeMode.GrowAndShrink,Margin=Padding.Empty};
        title.AutoSize=true;title.MaximumSize=new Size(220,0);title.Margin=new Padding(5,7,15,5);
        bar.Controls.Add(title);bar.Controls.Add(Button("Files",delegate{SendAction(0,"");}));
        refresh.Text="Refresh";Style(refresh);refresh.Click+=delegate{LoadCatalog();};bar.Controls.Add(refresh);
        tips.SetToolTip(refresh,"Reload this catalogue (F5)");
        detailsToggle.Text="Hide details";Style(detailsToggle);
        detailsToggle.Click+=delegate{if(view==null)return;view.Details=!view.Details;ArrangePanes();QueueSave();};
        bar.Controls.Add(detailsToggle);
        closeReader.Text="Close reader";Style(closeReader);closeReader.Visible=false;
        closeReader.Click+=delegate{SendAction(4,"");};bar.Controls.Add(closeReader);body.Controls.Add(bar,0,0);

        var filters=Table(3,2);filters.ColumnStyles.Clear();
        filters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,50));filters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,50));filters.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,86));
        filters.RowStyles.Add(new RowStyle(SizeType.Percent,50));filters.RowStyles.Add(new RowStyle(SizeType.Percent,50));
        search.HandleCreated+=delegate{Cue(search.Handle,0x1501,new IntPtr(1),"Search titles, authors, tags…");};
        search.Dock=DockStyle.Fill;search.MaxLength=256;search.BackColor=Panel;search.ForeColor=TextColor;
        search.AccessibleName="Search library title, creator or metadata";
        search.TextChanged+=delegate{if(!restoring){searchTimer.Stop();searchTimer.Start();}};
        filters.Controls.Add(search,0,0);filters.SetColumnSpan(search,2);
        clearFilters.Text="Clear filters";Style(clearFilters);clearFilters.Dock=DockStyle.Fill;clearFilters.Enabled=false;
        clearFilters.Click+=delegate{ClearFilters();};tips.SetToolTip(clearFilters,"Clear search, format and grouping (Ctrl+Shift+F)");filters.Controls.Add(clearFilters,2,0);
        ConfigureCombo(format);format.SelectedIndexChanged+=delegate{if(!restoring)Filter();};
        group.SelectionChanged+=delegate{tips.SetToolTip(group,group.SelectedLabel+"\nSearch groups (Ctrl+G)");if(!restoring)Filter();};
        format.AccessibleName="Book format";
        filters.Controls.Add(format,0,1);filters.Controls.Add(group,1,1);filters.SetColumnSpan(group,2);body.Controls.Add(filters,0,1);
        search.Enabled=format.Enabled=group.Enabled=false;

        sourceWarning.AutoSize=true;sourceWarning.ForeColor=LibraryTheme.Contrast?SystemColors.WindowText:Color.FromArgb(242,194,113);sourceWarning.Margin=new Padding(3,4,3,8);sourceWarning.Visible=false;
        body.Controls.Add(sourceWarning,0,2);body.SizeChanged+=delegate{sourceWarning.MaximumSize=new Size(Math.Max(40,body.ClientSize.Width-18),0);};
        readingBar.Dock=DockStyle.Fill;readingBar.ColumnCount=2;readingBar.RowCount=1;
        readingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));readingBar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,110));
        readingBar.RowStyles.Add(new RowStyle(SizeType.Percent,100));readingBar.AutoSize=true;readingBar.AutoSizeMode=AutoSizeMode.GrowAndShrink;
        readingBar.MinimumSize=new Size(0,46);readingBar.Visible=false;readingBar.Margin=Padding.Empty;
        readingLabel.Dock=DockStyle.Fill;readingLabel.AutoEllipsis=true;readingLabel.TextAlign=ContentAlignment.MiddleLeft;
        readingLabel.AccessibleName="Currently reading";
        pinReader.Appearance=Appearance.Button;pinReader.Text="Pin reader";pinReader.Dock=DockStyle.Fill;
        pinReader.FlatStyle=FlatStyle.Flat;pinReader.BackColor=Panel;pinReader.ForeColor=TextColor;pinReader.FlatAppearance.CheckedBackColor=LibraryTheme.Selection;
        pinReader.TextAlign=ContentAlignment.MiddleCenter;
        pinReader.CheckedChanged+=delegate{if(restoring)return;if(view!=null)view.ReaderPinned=pinReader.Checked;UpdateReaderLabel();QueueFollow();QueueSave();};
        tips.SetToolTip(pinReader,"Pin this PDF while browsing other books. Unpin to follow selection.");
        readingBar.Controls.Add(readingLabel,0,0);readingBar.Controls.Add(pinReader,1,0);body.Controls.Add(readingBar,0,3);

        split.Size=new Size(900,500);split.SplitterDistance=600;split.Dock=DockStyle.Fill;
        split.TabStop=false;split.FixedPanel=FixedPanel.Panel2;split.Panel1MinSize=0;split.Panel2MinSize=0;split.BackColor=Panel;
        split.SplitterMoving+=delegate(object sender,SplitterCancelEventArgs e){if(!split.Panel2Collapsed&&split.ClientSize.Width>=720)e.SplitX=Math.Max(380,Math.Min(e.SplitX,split.ClientSize.Width-224));};
        split.SplitterMoved+=delegate{if(!arranging&&!restoring&&view!=null&&!split.Panel2Collapsed){view.DetailWidth=split.Panel2.Width;QueueSave();}};
        split.SizeChanged+=delegate{ArrangePanes();};body.Controls.Add(split,0,4);
        var catalogueArea=Table(1,2);catalogueArea.RowStyles.Add(new RowStyle(SizeType.Percent,100));catalogueArea.RowStyles.Add(new RowStyle(SizeType.Absolute,38));
        split.Panel1.Controls.Add(catalogueArea);
        var listWell=new System.Windows.Forms.Panel{Dock=DockStyle.Fill,Margin=Padding.Empty};catalogueArea.Controls.Add(listWell,0,0);
        list.Dock=DockStyle.Fill;list.View=View.Details;list.FullRowSelect=true;list.HideSelection=false;
        list.MultiSelect=false;list.VirtualMode=true;list.ShowItemToolTips=true;list.BackColor=Background;list.ForeColor=TextColor;list.BorderStyle=BorderStyle.None;
        for(int i=0;i<Headers.Length;i++)list.Columns.Add(Headers[i],preferredColumns[i]);
        int[] displayOrder={0,4,6,1,2,3,5};for(int i=0;i<displayOrder.Length;i++)list.Columns[displayOrder[i]].DisplayIndex=i;
        list.Columns[4].TextAlign=HorizontalAlignment.Right;
        list.RetrieveVirtualItem+=delegate(object s,RetrieveVirtualItemEventArgs e){
            if(e.ItemIndex<0||e.ItemIndex>=visible.Count){e.Item=new ListViewItem("");return;}
            var item=visible[e.ItemIndex];e.Item=new ListViewItem(new[]{item.Title,item.Creator,item.Year,String.Join(", ",item.Attachments.Select(a=>a.Format).Distinct().ToArray()),SizeText(ItemSize(item)),item.Added,item.SourceName});
            e.Item.ToolTipText=item.Title+"\n"+item.Creator+"\nLibrary: "+item.SourceName+"\n"+item.SourcePath;
        };
        list.SelectedIndexChanged+=delegate{if(!restoring)ShowItem();};
        list.ColumnClick+=delegate(object s,ColumnClickEventArgs e){
            if(view==null)return;if(view.Sort==e.Column)view.Descending=!view.Descending;else{view.Sort=e.Column;view.Descending=e.Column==4;}
            view.Selected=0;view.SelectedKey="";view.Top=0;restoring=true;list.SelectedIndices.Clear();restoring=false;Filter();
        };
        list.ColumnWidthChanging+=delegate(object s,ColumnWidthChangingEventArgs e){
            if(arrangingColumns)return;
            if(e.ColumnIndex==0)e.NewWidth=Math.Max(60,Math.Min(e.NewWidth,list.ClientSize.Width-88));
            if(e.ColumnIndex==4)e.NewWidth=Math.Max(72,Math.Min(e.NewWidth,list.ClientSize.Width-100));
        };
        list.ColumnWidthChanged+=delegate(object s,ColumnWidthChangedEventArgs e){
            if(arrangingColumns||restoring||list.Columns[e.ColumnIndex].Width<=0||(e.ColumnIndex==6&&!Combined))return;preferredColumns[e.ColumnIndex]=Math.Max(50,Math.Min(1200,list.Columns[e.ColumnIndex].Width));
            if(view!=null&&e.ColumnIndex==0)view.TitleFixed=true;QueueSave();
        };
        list.Resize+=delegate{ArrangeColumns();};list.ItemActivate+=delegate{Preview();};listWell.Controls.Add(list);BuildSurface(listWell);LibraryTheme.List(list);

        var compact=Table(3,1);compact.ColumnStyles.Clear();
        compact.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));compact.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,86));compact.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,36));
        ConfigureCombo(attachmentPicker);attachmentPicker.AccessibleName="Selected book attachment";
        attachmentPicker.DropDown+=delegate{attachmentPicker.DropDownWidth=Math.Max(attachmentPicker.Width,550);};
        attachmentPicker.SelectedIndexChanged+=delegate{if(!bindingAttachments){var option=attachmentPicker.SelectedItem as AttachmentOption;ChooseAttachment(option==null?null:option.Attachment,true);}};
        preview.Text="Read PDF";preview.Enabled=false;Style(preview);preview.Dock=DockStyle.Fill;preview.Click+=delegate{Preview();};
        var more=Button("…",delegate{});more.AccessibleName="Attachment actions";more.Dock=DockStyle.Fill;
        more.Click+=delegate{
            var menu=new ContextMenuStrip();LibraryTheme.Menu(menu);menu.Closed+=delegate{menu.Dispose();};var a=Attachment();
            menu.Items.Add("Reveal containing folder",null,delegate{FileAction(2);}).Enabled=a!=null&&a.Path.Length>0;
            menu.Items.Add("Copy path",null,delegate{FileAction(3);}).Enabled=a!=null&&a.Path.Length>0;
            menu.Show(more,new Point(0,more.Height));
        };
        compact.Controls.Add(attachmentPicker,0,0);compact.Controls.Add(preview,1,0);compact.Controls.Add(more,2,0);catalogueArea.Controls.Add(compact,0,1);

        var detail=Table(1,3);detail.RowStyles.Add(new RowStyle(SizeType.Absolute,130));
        detail.RowStyles.Add(new RowStyle(SizeType.Percent,60));detail.RowStyles.Add(new RowStyle(SizeType.Percent,40));split.Panel2.Controls.Add(detail);
        cover.Dock=DockStyle.Fill;cover.SizeMode=PictureBoxSizeMode.Zoom;cover.AccessibleName="Book cover";detail.Controls.Add(cover,0,0);
        metadata.Dock=DockStyle.Fill;metadata.Multiline=true;metadata.ReadOnly=true;metadata.ScrollBars=ScrollBars.Vertical;
        metadata.BorderStyle=BorderStyle.None;metadata.BackColor=Background;metadata.ForeColor=TextColor;detail.Controls.Add(metadata,0,1);
        attachments.Dock=DockStyle.Fill;attachments.View=View.Details;attachments.FullRowSelect=true;attachments.MultiSelect=false;
        attachments.ShowItemToolTips=true;attachments.HideSelection=false;attachments.BackColor=Background;attachments.ForeColor=TextColor;
        attachments.Columns.Add("Attachment",150);attachments.Columns.Add("Availability",100);attachments.Columns.Add("Size",80);attachments.Columns[2].TextAlign=HorizontalAlignment.Right;
        attachments.SelectedIndexChanged+=delegate{if(!bindingAttachments){var a=attachments.SelectedItems.Count==0?null:attachments.SelectedItems[0].Tag as LibraryAttachment;ChooseAttachment(a,true);}};
        attachments.ItemActivate+=delegate{Preview();};attachments.Resize+=delegate{ArrangeAttachmentColumns();};detail.Controls.Add(attachments,0,2);LibraryTheme.List(attachments);
        status.Dock=DockStyle.Fill;status.AutoEllipsis=true;status.TextAlign=ContentAlignment.MiddleLeft;body.Controls.Add(status,0,5);

        lifetime.Interval=1000;lifetime.Tick+=delegate{if(!IsWindow(parent))Close();};
        searchTimer.Interval=180;searchTimer.Tick+=delegate{searchTimer.Stop();Filter();};
        saveTimer.Interval=350;saveTimer.Tick+=delegate{saveTimer.Stop();SaveView();};
        followTimer.Interval=180;followTimer.Tick+=delegate{
            followTimer.Stop();var a=Attachment();
            if(readerOpen&&!pinReader.Checked&&Readable(a)&&!String.Equals(a.Path,readingPath,StringComparison.OrdinalIgnoreCase)){pendingReadingKey=detailItem==null?"":detailItem.StableKey;SendAction(5,a.Path);}
        };
        refreshTimer.Interval=2000;refreshTimer.Tick+=delegate{
            if(IsDisposed||Disposing||!IsWindowVisible(Handle)||loading||catalog==null)return;
            if(SourcesChanged()){viewDirty=true;UpdateStatus();}
        };
        Shown+=delegate{
            try{
                SetWindowLong(Handle,-16,(GetWindowLong(Handle,-16)&~unchecked((int)0x80000000))|0x40000000);SetParent(Handle,parent);
                if(GetParent(Handle)!=parent)throw new IOException("Could not attach library view.");
                lifetime.Start();refreshTimer.Start();PostMessage(parent,Ready,Handle,IntPtr.Zero);if(!deferInitial)LoadCatalog();else{loading=true;status.Text="Waiting for library selection…";}
            }catch(Exception e){loadError=e.Message;loading=false;UpdateSurface();UpdateStatus();}
        };
        ApplyTabOrder(body);cover.TabStop=false;metadata.AccessibleName="Selected book or reference details";
        tips.SetToolTip(search,"Search catalogue metadata (Ctrl+F). Enter focuses results; Escape clears the search.");
        tips.SetToolTip(pinReader,"Pin this PDF while browsing other books (Alt+P). Unpin to follow selection.");
        FormClosing+=delegate{SaveView();followTimer.Stop();if(request!=null)request.Cancel();};

    }

    protected override void Dispose(bool disposing)
    {
        if(disposing){lifetime.Dispose();refreshTimer.Dispose();searchTimer.Dispose();saveTimer.Dispose();followTimer.Dispose();if(request!=null){request.Cancel();request.Dispose();request=null;}tips.Dispose();if(cover.Image!=null){cover.Image.Dispose();cover.Image=null;}}
        base.Dispose(disposing);
    }
    static TableLayoutPanel Table(int columns,int rows){var table=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=columns,RowCount=rows,Margin=Padding.Empty};table.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));if(rows==1)table.RowStyles.Add(new RowStyle(SizeType.Percent,100));return table;}
    static void ConfigureCombo(ComboBox c){c.Dock=DockStyle.Fill;c.DropDownStyle=ComboBoxStyle.DropDownList;LibraryTheme.Combo(c);}
    static void Style(Button b){b.AutoSize=true;b.FlatStyle=FlatStyle.Flat;b.BackColor=Panel;b.ForeColor=TextColor;}
    static Button Button(string text,Action action){var b=new Button{Text=text};Style(b);b.Click+=delegate{action();};return b;}
    void SnapshotSources(){databaseStamps.Clear();foreach(var source in sources)foreach(string file in new[]{source.Database,source.Database+"-wal"})databaseStamps[file]=File.GetLastWriteTimeUtc(file);}
    bool SourcesChanged(){return databaseStamps.Any(pair=>File.GetLastWriteTimeUtc(pair.Key)!=pair.Value);}
    LibraryItem Selected(){return list.SelectedIndices.Count==0||list.SelectedIndices[0]>=visible.Count?null:visible[list.SelectedIndices[0]];}
    string SelectedFormat {get{return format.SelectedItem as string??"All formats";}}
    LibraryAttachment Attachment(){var option=attachmentPicker.SelectedItem as AttachmentOption;return option==null?null:option.Attachment;}
    long? AttachmentSize(LibraryAttachment a){long actual;if(a.Path.Length>0&&sizes.TryGetValue(a.Path,out actual))return actual>=0?(long?)actual:null;return a.Size;}
    long? ItemSize(LibraryItem item){long? size=null;foreach(var a in item.Attachments.Where(a=>SelectedFormat=="All formats"||a.Format==SelectedFormat)){var n=AttachmentSize(a);if(n.HasValue)size=Math.Max(size??0,n.Value);}return size;}
    static string SizeText(long? size){if(!size.HasValue)return "Unknown";double n=size.Value;string[] units={"B","KiB","MiB","GiB"};int at=0;while(n>=1024&&at<3){n/=1024;at++;}return n.ToString(at==0?"0":"0.0")+" "+units[at];}
    string Availability(LibraryAttachment a){string value;if(availability.TryGetValue(a.Id,out value))return value;return a.Path.Length>0?"Checking…":a.Status.Length>0?a.Status:"Unavailable";}
    bool Readable(LibraryAttachment a){return a!=null&&a.Format=="PDF"&&Availability(a)=="Available";}
    string AttachmentName(LibraryAttachment a){if(a.Path.Length>0){try{return Path.GetFileName(a.Path);}catch{}}return a.Name.Length>0?a.Name:a.Format;}
    string AttachmentText(LibraryAttachment a){return a.Format+" · "+AttachmentName(a)+" · "+Availability(a)+" · "+SizeText(AttachmentSize(a));}

    void ArrangePanes()
    {
        if(arranging||split.IsDisposed)return;arranging=true;
        try{
            bool wanted=view==null||view.Details;bool room=split.ClientSize.Width>=720&&catalog!=null&&visible.Count>0;
            bool collapse=!wanted||!room;if(split.Panel2Collapsed!=collapse)split.Panel2Collapsed=collapse;
            detailsToggle.Text=wanted?(room?"Hide details":"Details (auto)"):"Show details";
            tips.SetToolTip(detailsToggle,wanted&&!room?"Details will return when there is room. Attachment selection remains below the catalogue.":"Show or hide book details");
            if(!collapse){int desired=view==null?300:Math.Max(220,Math.Min(500,view.DetailWidth));int distance=Math.Max(380,split.ClientSize.Width-desired-split.SplitterWidth);distance=Math.Min(distance,split.ClientSize.Width-split.Panel2MinSize-split.SplitterWidth);if(split.SplitterDistance!=distance)split.SplitterDistance=distance;}
        }finally{arranging=false;}
        ArrangeColumns();ArrangeAttachmentColumns();
    }
    void ArrangeColumns()
    {
        if(arrangingColumns||list.Columns.Count!=Headers.Length)return;int width=list.ClientSize.Width;if(width<80)return;
        arrangingColumns=true;
        try{
            bool author=width>=(Combined?650:480),year=width>=(Combined?820:650),formats=width>=(Combined?980:820)&&SelectedFormat=="All formats",added=width>=(Combined?1180:1040);
            int[] widths=new int[Headers.Length];widths[6]=Combined?Math.Max(55,Math.Min(Math.Max(110,preferredColumns[6]),width/4)):0;widths[4]=Math.Min(Math.Max(60,width-62),Math.Min(110,Math.Max(80,preferredColumns[4])));
            if(author)widths[1]=Math.Min(240,Math.Max(120,preferredColumns[1]));
            if(year)widths[2]=Math.Min(110,Math.Max(78,preferredColumns[2]));
            if(formats)widths[3]=Math.Min(120,Math.Max(80,preferredColumns[3]));
            if(added)widths[5]=Math.Min(140,Math.Max(100,preferredColumns[5]));
            int room=Math.Max(50,width-2-widths.Skip(1).Sum());
            widths[0]=view!=null&&view.TitleFixed?Math.Min(Math.Max(100,preferredColumns[0]),room):room;
            for(int i=0;i<Headers.Length;i++)if(list.Columns[i].Width!=widths[i])list.Columns[i].Width=widths[i];
        }finally{arrangingColumns=false;}
    }
    void ArrangeAttachmentColumns(){if(attachments.Columns.Count!=3)return;int w=attachments.ClientSize.Width;if(w<100)return;attachments.Columns[2].Width=72;attachments.Columns[1].Width=Math.Max(60,Math.Min(100,w/3));attachments.Columns[0].Width=Math.Max(60,w-attachments.Columns[1].Width-74);}

    async void LoadCatalog()
    {
        SaveView();selectionStarted=true;searchTimer.Stop();followTimer.Stop();group.ClosePicker();if(request!=null)request.Cancel();request=new CancellationTokenSource();
        var token=request.Token;int id=++generation;detailGeneration++;sizeBusy=false;loading=true;loadCancelled=false;loadError=sizeError="";
        catalog=null;detailItem=null;availability.Clear();visible.Clear();list.VirtualListSize=0;ClearAttachments();metadata.Clear();
        if(cover.Image!=null){cover.Image.Dispose();cover.Image=null;}
        search.Enabled=format.Enabled=group.Enabled=clearFilters.Enabled=false;refresh.Text="Refresh";detailsToggle.Enabled=false;
        sourceWarning.Visible=false;string provider=kind=="calibre"?"Calibre":"Zotero";title.Text=Combined?"Combined libraries ("+sources.Count+")":String.Equals(name,provider,StringComparison.OrdinalIgnoreCase)?name:name+" · "+provider;
        UpdateSurface();UpdateStatus();var inputs=sources.ToArray();
        try{
            var loaded=await Task.Run(delegate{return LibraryCatalog.OpenSelection(inputs,token);},token);
            if(IsDisposed||id!=generation)return;catalog=loaded;sizes.Clear();visible.Clear();list.VirtualListSize=0;
            view=states==null?new LibraryViewState():states.Get(root+"\x1f"+loaded.Identity);restoring=true;
            format.Items.Clear();format.Items.Add("All formats");foreach(var f in loaded.Items.SelectMany(b=>b.Attachments).Select(a=>a.Format).Distinct().OrderBy(f=>f))format.Items.Add(f);
            format.SelectedItem=format.Items.Contains(view.Format)?view.Format:"All formats";
            group.SetOptions(Groups(),view.GroupKey,view.Group);tips.SetToolTip(group,group.SelectedLabel+"\nSearch groups (Ctrl+G)");
            search.Text=view.Search??"";if(search.Text.Length>256)search.Text=search.Text.Substring(0,256);pinReader.Checked=view.ReaderPinned;
            if(view.Columns!=null)for(int i=0;i<Math.Min(view.Columns.Length,Headers.Length);i++)if(view.Columns[i]>0)preferredColumns[i]=Math.Max(50,Math.Min(1200,view.Columns[i]));
            ArrangePanes();restoring=false;SnapshotSources();UpdateSourceWarning();
            loading=false;viewDirty=false;search.Enabled=format.Enabled=group.Enabled=detailsToggle.Enabled=true;restorePosition=true;Filter();
        }catch(OperationCanceledException){}
        catch(Exception e){if(!IsDisposed&&id==generation){catalog=null;visible.Clear();list.VirtualListSize=0;metadata.Text="";ClearAttachments();loadError=e.Message;loading=false;restoring=false;UpdateSurface();UpdateStatus();}}
    }
    IEnumerable<LibraryGroupOption> Groups()
    {
        var byId=catalog.Groups.ToDictionary(g=>g.Id);
        if(catalog.Groups.Count>0)foreach(var g in catalog.Groups.OrderBy(g=>g.Name,StringComparer.OrdinalIgnoreCase).ThenBy(g=>g.Id)){
            var chain=new List<string>();var seen=new HashSet<long>();var at=g;
            while(at!=null&&seen.Add(at.Id)&&chain.Count<32){chain.Add(at.Name);LibraryGroup parent;at=byId.TryGetValue(at.Parent,out parent)?parent:null;}
            chain.Reverse();string path=String.Join(" / ",chain);
            yield return new LibraryGroupOption{Key=Combined?g.StableKey:"c:"+g.Id,Id=g.Id,Kind="Collections",Name=path,Label="Collection: "+(Combined?g.SourceName+" / ":"")+path};
        }
        foreach(var t in catalog.Items.SelectMany(b=>b.Tags).Distinct().OrderBy(t=>t,StringComparer.OrdinalIgnoreCase))yield return new LibraryGroupOption{Key="t:"+t,Kind="Tags",Name=t,Label="Tag: "+t};
        if(catalog.Kind!="zotero")foreach(var t in catalog.Items.Select(b=>b.Series).Where(t=>t.Length>0).Distinct().OrderBy(t=>t,StringComparer.OrdinalIgnoreCase))yield return new LibraryGroupOption{Key="s:"+t,Kind="Series",Name=t,Label="Series: "+t};
    }

    void Filter()
    {
        if(catalog==null||restoring)return;var selected=Selected();long preserve=selected==null?(view==null?0:view.Selected):selected.Id;string preserveKey=selected==null?(view==null?"":view.SelectedKey):selected.StableKey;
        string text=search.Text.Trim(),f=SelectedFormat;var selectedGroup=group.SelectedOption;
        IEnumerable<LibraryItem> found=catalog.Items.Where(b=>(f=="All formats"||b.Attachments.Any(a=>a.Format==f))&&(text.Length==0||b.SearchText.IndexOf(text,StringComparison.OrdinalIgnoreCase)>=0));
        if(selectedGroup!=null){
            if(selectedGroup.Kind=="Tags")found=found.Where(b=>b.Tags.Contains(selectedGroup.Name));
            else if(selectedGroup.Kind=="Series")found=found.Where(b=>b.Series==selectedGroup.Name);
            else if(selectedGroup.Kind=="Collections"){
                var ids=new HashSet<long>{selectedGroup.Id};bool again=true;
                while(again){again=false;foreach(var c in catalog.Groups)if(ids.Contains(c.Parent)&&ids.Add(c.Id))again=true;}found=found.Where(b=>b.Collections.Overlaps(ids));
            }
        }
        var rows=found.ToList();int sort=view==null?0:Math.Max(0,Math.Min(Headers.Length-1,view.Sort)),direction=view!=null&&view.Descending?-1:1;
        for(int i=0;i<Headers.Length;i++)list.Columns[i].Text=(i==4?(f=="All formats"?"Largest size":f+" size"):Headers[i])+(i==sort?(direction<0?" ▼":" ▲"):"");
        rows.Sort(delegate(LibraryItem a,LibraryItem b){
            int c;if(sort==4){var x=ItemSize(a);var y=ItemSize(b);if(!x.HasValue||!y.HasValue)return x.HasValue?-1:y.HasValue?1:a.Id.CompareTo(b.Id);c=x.Value.CompareTo(y.Value);}
            else{string x=sort==1?a.Creator:sort==2?a.Year:sort==3?String.Join(",",a.Attachments.Select(v=>v.Format)):sort==5?a.Added:sort==6?a.SourceName:a.Title,y=sort==1?b.Creator:sort==2?b.Year:sort==3?String.Join(",",b.Attachments.Select(v=>v.Format)):sort==5?b.Added:sort==6?b.SourceName:b.Title;c=StringComparer.OrdinalIgnoreCase.Compare(x,y);}
            return c==0?a.Id.CompareTo(b.Id):c*direction;
        });
        restoring=true;list.SelectedIndices.Clear();visible=rows;list.VirtualListSize=rows.Count;list.Invalidate();
        int at=rows.FindIndex(b=>Combined?b.StableKey==preserveKey:b.Id==preserve);if(at>=0){list.Items[at].Selected=true;list.EnsureVisible(at);}else if(rows.Count>0){list.Items[0].Selected=true;list.EnsureVisible(0);}
        if(restorePosition&&view!=null&&view.Top>0&&at>=0){try{list.TopItem=list.Items[Math.Min(view.Top,rows.Count-1)];}catch{}}
        restoring=false;restorePosition=false;ArrangeColumns();ShowItem();
        clearFilters.Enabled=FiltersActive();UpdateSurface();UpdateStatus();
        QueueSave();if(sort==4&&catalog.Kind!="calibre")LoadSizes();
    }
    async void LoadSizes()
    {
        if(sizeBusy||catalog==null)return;
        var paths=visible.SelectMany(b=>b.Attachments).Where(a=>!a.CatalogSize&&a.Path.Length>0&&(SelectedFormat=="All formats"||a.Format==SelectedFormat)).Select(a=>a.Path).Distinct(StringComparer.OrdinalIgnoreCase).Where(p=>!sizes.ContainsKey(p)).ToArray();
        if(paths.Length==0)return;sizeBusy=true;sizePending=paths.Length;sizeError="";int id=generation;var token=request.Token;UpdateStatus();
        try{
            var collected=await Task.Run(delegate{var result=new Dictionary<string,long>(StringComparer.OrdinalIgnoreCase);foreach(var p in paths){token.ThrowIfCancellationRequested();try{result[p]=new FileInfo(p).Length;}catch{result[p]=-1;}}return result;},token);
            if(IsDisposed||id!=generation)return;foreach(var pair in collected)sizes[pair.Key]=pair.Value;sizeBusy=false;Filter();
        }catch(OperationCanceledException){}catch(Exception e){if(!IsDisposed&&id==generation){sizeBusy=false;sizeError=e.Message;UpdateStatus();}}
    }

    void ClearAttachments(){bindingAttachments=true;try{attachments.Items.Clear();attachmentPicker.Items.Clear();attachmentPicker.Enabled=false;}finally{bindingAttachments=false;}preview.Enabled=false;}
    long RememberedAttachment(LibraryItem item){if(view==null||view.Attachments==null)return 0;var choice=view.Attachments.Find(c=>Combined?c.ItemKey==item.StableKey:c.Item==item.Id);if(choice==null)return 0;return Combined?item.Attachments.Where(a=>a.StableKey==choice.AttachmentKey).Select(a=>a.Id).FirstOrDefault():choice.Attachment;}
    void RememberAttachment(){if(view==null||detailItem==null)return;var a=Attachment();if(a==null)return;if(view.Attachments==null)view.Attachments=new List<LibraryAttachmentChoice>();view.Attachments.RemoveAll(c=>Combined?c.ItemKey==detailItem.StableKey:c.Item==detailItem.Id);view.Attachments.Insert(0,new LibraryAttachmentChoice{Item=detailItem.Id,Attachment=a.Id,ItemKey=detailItem.StableKey,AttachmentKey=a.StableKey});if(view.Attachments.Count>256)view.Attachments.RemoveRange(256,view.Attachments.Count-256);QueueSave();}
    LibraryAttachment PreferredAttachment(LibraryItem item)
    {
        var matches=item.Attachments.Where(a=>SelectedFormat=="All formats"||a.Format==SelectedFormat).ToList();if(matches.Count==0)matches=item.Attachments.ToList();
        long saved=RememberedAttachment(item);var remembered=matches.Find(a=>a.Id==saved);
        if(remembered!=null&&Availability(remembered)=="Available")return remembered;
        return matches.Find(Readable)??matches.Find(a=>Availability(a)=="Available")??remembered??matches.Find(a=>a.Format=="PDF")??matches.FirstOrDefault();
    }
    void RefreshAttachmentRows(LibraryAttachment selected)
    {
        if(detailItem==null){ClearAttachments();return;}bindingAttachments=true;
        try{
            attachments.BeginUpdate();attachments.Items.Clear();attachmentPicker.Items.Clear();int at=-1;
            foreach(var a in detailItem.Attachments){
                int index=attachments.Items.Count;var row=new ListViewItem(new[]{AttachmentName(a),Availability(a),SizeText(AttachmentSize(a))});row.Tag=a;row.ToolTipText=a.Format+" · "+a.Name+"\n"+(a.Path.Length>0?a.Path:Availability(a));attachments.Items.Add(row);
                attachmentPicker.Items.Add(new AttachmentOption{Attachment=a,Text=AttachmentText(a)});if(selected!=null&&a.Id==selected.Id)at=index;
            }
            attachmentPicker.Enabled=attachmentPicker.Items.Count>0;if(at>=0){attachments.Items[at].Selected=true;attachmentPicker.SelectedIndex=at;}
        }finally{attachments.EndUpdate();bindingAttachments=false;}
        ArrangeAttachmentColumns();UpdatePreview();
    }
    void ChooseAttachment(LibraryAttachment choice,bool manual)
    {
        if(bindingAttachments)return;bindingAttachments=true;
        try{
            attachments.SelectedIndices.Clear();int at=detailItem==null||choice==null?-1:detailItem.Attachments.FindIndex(a=>a.Id==choice.Id);
            attachmentPicker.SelectedIndex=at;if(at>=0)attachments.Items[at].Selected=true;
        }finally{bindingAttachments=false;}
        if(manual){manualAttachment=true;RememberAttachment();}UpdatePreview();QueueFollow();
    }
    async void ShowItem()
    {
        var item=Selected();followTimer.Stop();
        if(item!=null&&ReferenceEquals(item,detailItem)){
            var choice=Attachment();if(detailFormat!=SelectedFormat){detailFormat=SelectedFormat;manualAttachment=false;choice=PreferredAttachment(item);}RefreshAttachmentRows(choice);QueueFollow();return;
        }
        detailItem=item;detailFormat=SelectedFormat;manualAttachment=false;availability.Clear();int id=++detailGeneration;
        var detail=cover.Parent as TableLayoutPanel;if(detail!=null)detail.RowStyles[0].Height=item!=null&&item.Cover.Length>0?130:0;
        if(cover.Image!=null){cover.Image.Dispose();cover.Image=null;}ClearAttachments();
        if(item==null){metadata.Text=catalog!=null&&catalog.Items.Count==0?"This catalogue is empty.":"Select a book or reference.";UpdateReaderLabel();return;}
        var lines=new List<string>{item.Title,"",item.Creator};if(Combined)lines.Add("Library: "+item.SourceName);
        if(item.Year.Length>0)lines.Add("Published: "+item.Year);
        if(item.Type.Length>0)lines.Add("Type: "+System.Text.RegularExpressions.Regex.Replace(item.Type,"([a-z])([A-Z])","$1 $2"));
        if(item.Publication.Length>0)lines.Add("Publication: "+item.Publication);
        if(item.Series.Length>0)lines.Add("Series: "+item.Series+(item.SeriesIndex.Length>0?" #"+item.SeriesIndex:""));
        if(item.Tags.Count>0){lines.Add("");lines.Add("Tags: "+String.Join(", ",item.Tags));}metadata.Text=String.Join("\r\n",lines);

        var set=item.Attachments.ToArray();RefreshAttachmentRows(PreferredAttachment(item));QueueSave();UpdateReaderLabel();
        try{
            var checks=await Task.Run(delegate{
                var result=new Dictionary<long,Tuple<string,long?>>();foreach(var a in set){
                    string state=a.Path.Length==0?a.Status:"Missing file";long? size=null;
                    if(a.Path.Length>0)try{var file=new FileInfo(a.Path);if(file.Exists){state="Available";size=file.Length;}}catch{}
                    result[a.Id]=Tuple.Create(state,size);
                }return result;
            });
            if(IsDisposed||id!=detailGeneration)return;
            foreach(var a in set){availability[a.Id]=checks[a.Id].Item1;if(a.Path.Length>0)sizes[a.Path]=checks[a.Id].Item2??-1;}
            var chosen=manualAttachment?Attachment():PreferredAttachment(item);RefreshAttachmentRows(chosen);list.Invalidate();QueueFollow();
            if(item.Cover.Length>0){
                var picture=await Task.Run(delegate{
                    var info=new FileInfo(item.Cover);if(!info.Exists||info.Length>8*1024*1024)return null;
                    using(var f=new FileStream(item.Cover,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete))using(var image=Image.FromStream(f)){
                        if((long)image.Width*image.Height>20000000)return null;var thumbnail=new Bitmap(100,120);
                        using(var dc=Graphics.FromImage(thumbnail)){dc.Clear(Background);double scale=Math.Min(100.0/image.Width,120.0/image.Height);int w=(int)(image.Width*scale),h=(int)(image.Height*scale);dc.DrawImage(image,(100-w)/2,(120-h)/2,w,h);}return thumbnail;
                    }
                });
                if(IsDisposed||id!=detailGeneration){if(picture!=null)picture.Dispose();}else cover.Image=picture;
            }
        }catch{if(!IsDisposed&&id==detailGeneration)UpdatePreview();}
    }
    void UpdatePreview(){var a=Attachment();preview.Enabled=Readable(a);tips.SetToolTip(preview,a==null?"Select an attachment":a.Format!="PDF"?"Embedded reading supports PDF attachments":Availability(a));UpdateReaderLabel();}
    void QueueFollow(){followTimer.Stop();if(readerOpen&&!pinReader.Checked&&detailItem!=null&&Readable(Attachment())&&String.Equals(Attachment().Path,readingPath,StringComparison.OrdinalIgnoreCase))readingKey=detailItem.StableKey;UpdateReaderLabel();if(readerOpen&&!pinReader.Checked&&!loading&&Readable(Attachment()))followTimer.Start();}
    void UpdateReaderLabel()
    {
        readingBar.Visible=readerOpen;closeReader.Visible=readerOpen;pinReader.Enabled=readerOpen;pinReader.Text=pinReader.Checked?"Reader pinned":"Pin reader";
        if(!readerOpen)return;var book=catalog==null?null:catalog.Items.Find(b=>b.StableKey==readingKey)??catalog.Items.Find(b=>b.Attachments.Any(a=>String.Equals(a.Path,readingPath,StringComparison.OrdinalIgnoreCase)));
        string text="Reading: "+(book==null?Path.GetFileName(readingPath):book.Title)+(Combined&&book!=null?" · "+book.SourceName:"")+"\n"+Path.GetFileName(readingPath);
        if(!pinReader.Checked&&!Readable(Attachment()))text+=" · No readable PDF selected";
        readingLabel.Text=text;tips.SetToolTip(readingLabel,text+(pinReader.Checked?"\nPinned while you browse":"\nFollows the selected readable PDF"));
    }
    void SetReaderState(bool open,string path){readerOpen=open;readingPath=open?path:"";if(open){var pending=catalog==null?null:catalog.Items.Find(b=>b.StableKey==pendingReadingKey&&b.Attachments.Any(a=>String.Equals(a.Path,path,StringComparison.OrdinalIgnoreCase)));if(pending!=null)readingKey=pending.StableKey;QueueFollow();}else{readingKey=pendingReadingKey="";followTimer.Stop();UpdateReaderLabel();}}
    void Preview(){var a=Attachment();if(!Readable(a)||!File.Exists(a.Path)){status.Text=a==null?"Select an attachment.":a.Format!="PDF"?"Embedded reading in v0 supports PDF attachments.":"The selected PDF is missing or unavailable.";return;}RememberAttachment();pendingReadingKey=detailItem==null?"":detailItem.StableKey;SendAction(1,a.Path);}
    void FileAction(int action){var a=Attachment();if(a!=null&&a.Path.Length>0)SendAction(action,a.Path);}
    void SendAction(int action,string path)
    {
        if(!String.IsNullOrEmpty(path)&&!LibraryCatalog.LocalPath(path)){status.Text="This attachment is not a local Windows file.";return;}
        byte[] text=System.Text.Encoding.Unicode.GetBytes(path+"\0"),bytes=new byte[text.Length+8];
        Buffer.BlockCopy(BitConverter.GetBytes(session),0,bytes,0,4);Buffer.BlockCopy(BitConverter.GetBytes(action),0,bytes,4,4);Buffer.BlockCopy(text,0,bytes,8,text.Length);
        IntPtr memory=Marshal.AllocHGlobal(bytes.Length);
        try{Marshal.Copy(bytes,0,memory,bytes.Length);var data=new CopyData{Type=new IntPtr(ActionCommand),Size=bytes.Length,Data=memory};IntPtr result;if(SendMessageTimeout(parent,0x4A,Handle,ref data,2|8,1000,out result)==IntPtr.Zero)status.Text="Folderwing is busy; try again.";}
        finally{Marshal.FreeHGlobal(memory);}
    }
    void QueueSave(){if(restoring||loading||view==null)return;saveTimer.Stop();saveTimer.Start();}
    void SaveView()
    {
        if(view==null||states==null||restoring||loading||loadCancelled||loadError.Length>0)return;
        try{
            view.Search=search.Text;view.Format=SelectedFormat;view.Group=group.SelectedLabel;view.GroupKey=group.SelectedKey;view.ReaderPinned=pinReader.Checked;
            var item=Selected();view.Selected=item==null?0:item.Id;view.SelectedKey=item==null?"":item.StableKey;try{view.Top=list.TopItem==null?0:list.TopItem.Index;}catch{}
            view.Columns=(int[])preferredColumns.Clone();states.Save();
        }catch{status.Text="Library view state could not be saved; catalogue is unchanged.";}
    }
    bool FiltersActive(){return search.Text.Trim().Length>0||SelectedFormat!="All formats"||group.SelectedOption!=null;}
    void ClearFilters()
    {
        if(catalog==null||loading)return;searchTimer.Stop();restoring=true;search.Clear();format.SelectedItem="All formats";group.SelectKey("");restoring=false;Filter();
    }
    void FocusCatalogue(){if(catalog!=null&&visible.Count>0)list.Focus();else if(emptySurface.Visible)surfaceAction.Focus();else if(search.Enabled)search.Focus();}
    void CancelLoad()
    {
        if(!loading)return;SaveView();if(request!=null)request.Cancel();generation++;detailGeneration++;loading=false;loadCancelled=true;UpdateSurface();UpdateStatus();
    }
    void UpdateStatus()
    {
        if(loading){status.Text="Opening catalogue… · Read-only";return;}
        if(loadCancelled){status.Text="Opening cancelled · F5 to retry";return;}
        if(loadError.Length>0){status.Text="Catalogue unavailable · F5 to retry";return;}
        if(catalog==null)return;
        string f=SelectedFormat;status.Text=visible.Count+" of "+catalog.Items.Count+" items"+(Combined?" · "+sources.Count+" libraries":"")+" · Read-only"+(catalog.Failures.Count>0?" · Incomplete ("+catalog.Failures.Count+" unavailable)":"")+(view!=null&&view.Sort==4?" · "+(f=="All formats"?"Largest available format":"Size of "+f):"")+(sizeBusy?" · Reading "+sizePending+" attachment sizes; ranking incomplete":"")+(sizeError.Length>0?" · Size lookup failed; Refresh to retry":"")+(viewDirty?" · Catalogue changed; Refresh":"")+(states==null?" · View memory unavailable":"");
        tips.SetToolTip(status,status.Text+(sizeError.Length>0?"\n"+sizeError:""));
    }
    void UpdateSourceWarning()
    {
        sourceWarning.Visible=catalog!=null&&catalog.Failures.Count>0;if(!sourceWarning.Visible)return;
        sourceWarning.Text="Incomplete view: "+catalog.Failures.Count+" selected librar"+(catalog.Failures.Count==1?"y is":"ies are")+" unavailable ("+String.Join(", ",catalog.Failures.Take(2).Select(f=>f.Source.Name).ToArray())+"). Refresh to retry.";
        tips.SetToolTip(sourceWarning,String.Join("\n",catalog.Failures.Select(f=>f.Source.Name+": "+f.Error).ToArray()));
    }
    static void ApplyTabOrder(Control parent){int at=0;foreach(Control c in parent.Controls){c.TabIndex=at++;ApplyTabOrder(c);}}
    void BuildSurface(Control well)
    {
        emptySurface.Dock=DockStyle.Fill;emptySurface.BackColor=Background;emptySurface.Padding=new Padding(12);emptySurface.Visible=false;well.Controls.Add(emptySurface);emptySurface.BringToFront();
        var body=Table(1,5);body.RowStyles.Add(new RowStyle(SizeType.Percent,50));body.RowStyles.Add(new RowStyle(SizeType.AutoSize));body.RowStyles.Add(new RowStyle(SizeType.AutoSize));body.RowStyles.Add(new RowStyle(SizeType.AutoSize));body.RowStyles.Add(new RowStyle(SizeType.Percent,50));emptySurface.Controls.Add(body);
        surfaceTitle.AutoSize=true;surfaceTitle.Font=new Font(Font.FontFamily,12,FontStyle.Bold);surfaceTitle.ForeColor=TextColor;surfaceTitle.Margin=new Padding(0,4,0,8);body.Controls.Add(surfaceTitle,0,1);
        surfaceMessage.AutoSize=true;surfaceMessage.ForeColor=LibraryTheme.Muted;surfaceMessage.Margin=new Padding(0,0,0,12);body.Controls.Add(surfaceMessage,0,2);
        var actions=new FlowLayoutPanel{Dock=DockStyle.Fill,AutoSize=true,WrapContents=true,Margin=Padding.Empty};Style(surfaceAction);surfaceAction.Click+=delegate{if(loading)CancelLoad();else if(catalog!=null&&catalog.Items.Count>0)ClearFilters();else LoadCatalog();};actions.Controls.Add(surfaceAction);body.Controls.Add(actions,0,3);
        body.SizeChanged+=delegate{int width=Math.Max(40,body.ClientSize.Width);surfaceTitle.MaximumSize=surfaceMessage.MaximumSize=new Size(width,0);};
        progress.Dock=DockStyle.Top;progress.Height=5;progress.Style=ProgressBarStyle.Marquee;progress.MarqueeAnimationSpeed=35;progress.Visible=false;emptySurface.Controls.Add(progress);progress.BringToFront();
    }
    void UpdateSurface()
    {
        bool show=loading||loadCancelled||loadError.Length>0||catalog==null||visible.Count==0;detailsToggle.Visible=detailsToggle.Enabled=!show;ArrangePanes();emptySurface.Visible=show;list.Visible=!show;progress.Visible=loading;
        if(!show)return;emptySurface.BringToFront();
        if(loading){surfaceTitle.Text="Opening "+name+"…";surfaceMessage.Text="Reading catalogue metadata. Your library stays read-only.";surfaceAction.Text="Cancel";}
        else if(loadCancelled){surfaceTitle.Text="Opening cancelled";surfaceMessage.Text="Retry when you are ready, or return to Files.";surfaceAction.Text="Retry";}
        else if(loadError.Length>0){surfaceTitle.Text="Couldn't open catalogue";surfaceMessage.Text=loadError+"\nCheck that the library is available, then retry.";surfaceAction.Text="Retry";}
        else if(catalog!=null&&catalog.Items.Count==0){surfaceTitle.Text=Combined?"No items in the available libraries":"This catalogue is empty";surfaceMessage.Text=catalog.Failures.Count>0?"Some selected libraries could not be opened. Refresh to retry them.":"No books or references are recorded here. Add them in Calibre or Zotero, then refresh.";surfaceAction.Text="Refresh";}
        else {surfaceTitle.Text="No matching books or references";surfaceMessage.Text="Try a different search, format or group, or clear all filters.";surfaceAction.Text="Clear filters";}
        emptySurface.AccessibleName=surfaceTitle.Text;
    }
    protected override bool ProcessCmdKey(ref Message m,Keys key)
    {
        if(key==(Keys.Control|Keys.F)){if(search.Enabled){search.Focus();search.SelectAll();}return true;}
        if(key==(Keys.Control|Keys.G)){group.OpenPicker();return true;}
        if(key==(Keys.Control|Keys.Shift|Keys.F)){ClearFilters();return true;}
        if(key==Keys.F5){LoadCatalog();return true;}
        if(key==Keys.F6||key==(Keys.Shift|Keys.F6)){if(list.ContainsFocus&&attachmentPicker.Enabled)attachmentPicker.Focus();else FocusCatalogue();return true;}
        if(key==(Keys.Alt|Keys.P)&&readerOpen){pinReader.Checked=!pinReader.Checked;return true;}
        if(key==Keys.Enter&&search.Focused){searchTimer.Stop();Filter();FocusCatalogue();return true;}
        if(key==Keys.Escape){
            if(group.IsOpen){group.ClosePicker();return true;}
            if(format.DroppedDown||attachmentPicker.DroppedDown){format.DroppedDown=attachmentPicker.DroppedDown=false;return true;}
            if(search.Focused&&search.Text.Length>0){searchTimer.Stop();search.Clear();searchTimer.Stop();Filter();return true;}
            if(readerOpen)SendAction(4,"");else FocusCatalogue();return true;
        }
        return base.ProcessCmdKey(ref m,key);
    }

    static string ReadSelectionString(BinaryReader input,int limit){var value=new System.Text.StringBuilder();for(int i=0;i<=limit;i++){ushort c=input.ReadUInt16();if(c==0)return value.ToString();if(i==limit)break;value.Append((char)c);}throw new IOException("Invalid library selection.");}
    void SetSelection(uint next,IEnumerable<LibrarySource> values)
    {
        var chosen=values.GroupBy(v=>v.Key,StringComparer.OrdinalIgnoreCase).Select(v=>v.First()).OrderBy(v=>v.Key,StringComparer.Ordinal).ToList();
        if(selectionStarted&&next==session&&chosen.Select(v=>v.Key).SequenceEqual(sources.OrderBy(v=>v.Key,StringComparer.Ordinal).Select(v=>v.Key)))return;
        SaveView();SetReaderState(false,"");session=next;sources=chosen;root=chosen.Count==1?chosen[0].Path:"";name=chosen.Count==1?chosen[0].Name:"selected libraries";kind=chosen.Count==1?chosen[0].Kind:"combined";view=null;LoadCatalog();
    }
    protected override void WndProc(ref Message message)
    {
        if(message.Msg==FocusCatalogueMessage&&message.WParam==parent){FocusCatalogue();return;}
        if(message.Msg==0x4A&&message.WParam==parent){
            var data=(CopyData)Marshal.PtrToStructure(message.LParam,typeof(CopyData));long command=data.Type.ToInt64();
            if(command==SelectionCommand&&data.Size>=8&&data.Size<=8*1024*1024&&(data.Size&1)==0){
                try{byte[] bytes=new byte[data.Size];Marshal.Copy(data.Data,bytes,0,bytes.Length);using(var input=new BinaryReader(new MemoryStream(bytes),System.Text.Encoding.Unicode)){uint next=input.ReadUInt32(),count=input.ReadUInt32();if(count<1||count>64)return;var chosen=new List<LibrarySource>();for(int i=0;i<count;i++){uint type=input.ReadUInt32();string path=ReadSelectionString(input,32767),label=ReadSelectionString(input,127);if((type!=1&&type!=2)||!LibraryCatalog.LocalPath(path))return;chosen.Add(new LibrarySource{Kind=type==1?"calibre":"zotero",Path=path,Name=label});}if(input.BaseStream.Position!=input.BaseStream.Length)return;SetSelection(next,chosen);message.Result=new IntPtr(1);return;}}catch{ return; }
            }
            if((command==RootCommand||command==ReaderCommand)&&data.Size>=10&&data.Size<=131080&&(data.Size&1)==0){
                byte[] bytes=new byte[data.Size];Marshal.Copy(data.Data,bytes,0,bytes.Length);uint next=BitConverter.ToUInt32(bytes,0);int flag=BitConverter.ToInt32(bytes,4);
                string value=System.Text.Encoding.Unicode.GetString(bytes,8,bytes.Length-8);
                if(command==ReaderCommand&&next==session&&(flag==0||flag==1)&&value.EndsWith("\0",StringComparison.Ordinal)&&value.IndexOf('\0')==value.Length-1){SetReaderState(flag==1,value.Substring(0,value.Length-1));message.Result=new IntPtr(1);return;}
                if(command==RootCommand){var fields=value.Split('\0');if(fields.Length==3&&fields[2]==""&&(flag==1||flag==2)){
                    if(fields[0].Length>32767||fields[1].Length>127||!LibraryCatalog.LocalPath(fields[0]))return;try{SetSelection(next,new[]{new LibrarySource{Path=fields[0],Name=fields[1],Kind=flag==1?"calibre":"zotero"}});}catch{return;}message.Result=new IntPtr(1);return;
                }}
            }
        }
        base.WndProc(ref message);
    }
    [STAThread] static int Main(string[] args)
    {
        long target;uint token;if((args.Length!=6&&!(args.Length==7&&args[6]=="wait"))||!Int64.TryParse(args[0],out target)||!UInt32.TryParse(args[1],out token)||(args[2]!="calibre"&&args[2]!="zotero"))return 2;
        IntPtr parent=new IntPtr(target);if(!IsWindow(parent)||!LibraryCatalog.LocalPath(args[3]))return 2;
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);Application.Run(new LibraryHost(parent,token,args[2],args[3],args[4],args[5],args.Length==7));return 0;
    }
}

// Shared paint rules keep native controls legible without changing OS-wide theming.
internal static class LibraryTheme
{
    internal static readonly bool Contrast=SystemInformation.HighContrast;
    internal static readonly Color Background=Contrast?SystemColors.Window:Color.FromArgb(28,31,38),Panel=Contrast?SystemColors.Control:Color.FromArgb(37,41,49),Text=Contrast?SystemColors.WindowText:Color.FromArgb(225,229,235),Muted=Contrast?SystemColors.GrayText:Color.FromArgb(166,177,192),Selection=Contrast?SystemColors.Highlight:Color.FromArgb(49,83,120),SelectionText=Contrast?SystemColors.HighlightText:Color.White;
    internal static void Combo(ComboBox c)
    {
        c.BackColor=Panel;c.ForeColor=Text;c.FlatStyle=FlatStyle.Flat;c.DrawMode=DrawMode.OwnerDrawFixed;
        c.DrawItem+=delegate(object sender,DrawItemEventArgs e){
            if(e.Bounds.Width<=0)return;bool selected=(e.State&DrawItemState.Selected)!=0;
            using(var brush=new SolidBrush(selected?Selection:Panel))e.Graphics.FillRectangle(brush,e.Bounds);
            string value=e.Index>=0&&e.Index<c.Items.Count?c.GetItemText(c.Items[e.Index]):c.Text;
            TextRenderer.DrawText(e.Graphics,value,c.Font,Rectangle.Inflate(e.Bounds,-3,0),c.Enabled?(selected?SelectionText:Text):Muted,TextFormatFlags.Left|TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix);
            e.DrawFocusRectangle();
        };
    }
    internal static void List(ListView c)
    {
        c.BackColor=Background;c.ForeColor=Text;c.OwnerDraw=true;
        c.DrawColumnHeader+=delegate(object sender,DrawListViewColumnHeaderEventArgs e){
            using(var brush=new SolidBrush(Panel))e.Graphics.FillRectangle(brush,e.Bounds);
            var bounds=Rectangle.Inflate(e.Bounds,-6,0);var flags=TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix;
            flags|=e.Header.TextAlign==HorizontalAlignment.Right?TextFormatFlags.Right:TextFormatFlags.Left;
            TextRenderer.DrawText(e.Graphics,e.Header.Text,c.Font,bounds,Text,flags);
            using(var pen=new Pen(Muted))e.Graphics.DrawLine(pen,e.Bounds.Left,e.Bounds.Bottom-1,e.Bounds.Right,e.Bounds.Bottom-1);
        };
        c.DrawItem+=delegate(object sender,DrawListViewItemEventArgs e){if(c.View!=View.Details)e.DrawDefault=true;};
        c.DrawSubItem+=delegate(object sender,DrawListViewSubItemEventArgs e){
            bool selected=e.Item.Selected;Color back=selected?Selection:Background,fore=selected?SelectionText:Text;
            using(var brush=new SolidBrush(back))e.Graphics.FillRectangle(brush,e.Bounds);
            var flags=TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix;
            flags|=c.Columns[e.ColumnIndex].TextAlign==HorizontalAlignment.Right?TextFormatFlags.Right:TextFormatFlags.Left;
            TextRenderer.DrawText(e.Graphics,e.SubItem.Text,c.Font,Rectangle.Inflate(e.Bounds,-5,0),fore,flags);
            if(e.ColumnIndex==0&&e.Item.Focused&&c.Focused)ControlPaint.DrawFocusRectangle(e.Graphics,e.Bounds,fore,back);
        };
        c.GotFocus+=delegate{c.Invalidate();};c.LostFocus+=delegate{c.Invalidate();};
    }
    sealed class MenuColors : ProfessionalColorTable
    {
        public override Color ToolStripDropDownBackground {get{return Panel;}}
        public override Color ImageMarginGradientBegin {get{return Panel;}}
        public override Color ImageMarginGradientMiddle {get{return Panel;}}
        public override Color ImageMarginGradientEnd {get{return Panel;}}
        public override Color MenuItemSelected {get{return Selection;}}
        public override Color MenuItemBorder {get{return Muted;}}
        public override Color MenuBorder {get{return Muted;}}
    }
    internal static void Menu(ToolStrip menu){menu.BackColor=Panel;menu.ForeColor=Text;menu.Renderer=new ToolStripProfessionalRenderer(new MenuColors());}
}
internal sealed class LibraryGroupOption
{
    internal string Key,Kind,Name,Label;internal long Id;
    public override string ToString(){return Label;}
}
internal sealed class LibraryGroupingPicker : Button
{
    const int ResultLimit=200;
    [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] static extern IntPtr Cue(IntPtr hwnd,uint message,IntPtr w,string text);
    readonly GroupingPopup popup;
    readonly TextBox query=new TextBox();
    readonly ListBox matches=new ListBox();
    readonly Label count=new Label();
    readonly Button apply=new Button();
    readonly List<RadioButton> kinds=new List<RadioButton>();
    List<LibraryGroupOption> options=new List<LibraryGroupOption>();
    LibraryGroupOption selected;
    string category="All";
    internal event EventHandler SelectionChanged;
    internal LibraryGroupOption SelectedOption {get{return selected;}}
    internal string SelectedKey {get{return selected==null?"":selected.Key;}}
    internal string SelectedLabel {get{return selected==null?"All items":selected.Label;}}
    internal bool IsOpen {get{return popup.Visible;}}
    internal LibraryGroupingPicker()
    {
        popup=new GroupingPopup(this);
        Dock=DockStyle.Fill;FlatStyle=FlatStyle.Flat;BackColor=LibraryTheme.Panel;ForeColor=LibraryTheme.Text;
        TextAlign=ContentAlignment.MiddleLeft;AutoEllipsis=true;Text="All items ▾";AccessibleName="Search library groups";
        var surface=new TableLayoutPanel{Size=new Size(440,360),Padding=new Padding(8),ColumnCount=1,RowCount=5,BackColor=LibraryTheme.Panel,ForeColor=LibraryTheme.Text};
        surface.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));
        foreach(int height in new[]{28,34,0,30,32})surface.RowStyles.Add(new RowStyle(height==0?SizeType.Percent:SizeType.Absolute,height==0?100:height));
        query.Dock=DockStyle.Fill;query.BackColor=LibraryTheme.Background;query.ForeColor=LibraryTheme.Text;query.MaxLength=256;query.AccessibleName="Find a tag, series or collection";
        query.HandleCreated+=delegate{Cue(query.Handle,0x1501,new IntPtr(1),"Find a tag, series or collection…");};query.TextChanged+=delegate{SearchOptions();};surface.Controls.Add(query,0,0);
        var categories=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=false,Margin=Padding.Empty};
        foreach(string value in new[]{"All","Tags","Series","Collections"}){
            var choice=new RadioButton{Text=value,Appearance=Appearance.Button,AutoSize=true,FlatStyle=FlatStyle.Flat,BackColor=LibraryTheme.Panel,ForeColor=LibraryTheme.Text,TabStop=true};
            choice.FlatAppearance.CheckedBackColor=LibraryTheme.Selection;choice.CheckedChanged+=delegate{if(choice.Checked){category=choice.Text;SearchOptions();}};kinds.Add(choice);categories.Controls.Add(choice);
        }
        kinds[0].Checked=true;surface.Controls.Add(categories,0,1);
        matches.Dock=DockStyle.Fill;matches.BackColor=LibraryTheme.Background;matches.ForeColor=LibraryTheme.Text;matches.BorderStyle=BorderStyle.None;matches.IntegralHeight=false;matches.DrawMode=DrawMode.OwnerDrawFixed;matches.ItemHeight=23;
        matches.AccessibleName="Matching library groups";
        matches.DrawItem+=delegate(object sender,DrawItemEventArgs e){bool focus=(e.State&DrawItemState.Selected)!=0;using(var brush=new SolidBrush(focus?LibraryTheme.Selection:LibraryTheme.Background))e.Graphics.FillRectangle(brush,e.Bounds);if(e.Index>=0)TextRenderer.DrawText(e.Graphics,matches.Items[e.Index].ToString(),Font,Rectangle.Inflate(e.Bounds,-4,0),focus?LibraryTheme.SelectionText:LibraryTheme.Text,TextFormatFlags.VerticalCenter|TextFormatFlags.EndEllipsis|TextFormatFlags.NoPrefix);e.DrawFocusRectangle();};
        matches.SelectedIndexChanged+=delegate{apply.Enabled=matches.SelectedItem!=null;};matches.DoubleClick+=delegate{Commit();};surface.Controls.Add(matches,0,2);
        count.Dock=DockStyle.Fill;count.ForeColor=LibraryTheme.Muted;count.TextAlign=ContentAlignment.MiddleLeft;surface.Controls.Add(count,0,3);
        var actions=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=false,Margin=Padding.Empty};
        apply.Text="Use group";PaintButton(apply);apply.Click+=delegate{Commit();};actions.Controls.Add(apply);
        var clear=new Button{Text="Clear group"};PaintButton(clear);clear.Click+=delegate{SelectKey("");ClosePicker();};actions.Controls.Add(clear);surface.Controls.Add(actions,0,4);
        query.KeyDown+=delegate(object sender,KeyEventArgs e){if(e.KeyCode==Keys.Down){if(matches.Items.Count>0)matches.Focus();e.SuppressKeyPress=true;}else if(e.KeyCode==Keys.Enter){Commit();e.SuppressKeyPress=true;}else if(e.KeyCode==Keys.Escape){ClosePicker();e.SuppressKeyPress=true;}};
        matches.KeyDown+=delegate(object sender,KeyEventArgs e){if(e.KeyCode==Keys.Enter){Commit();e.SuppressKeyPress=true;}else if(e.KeyCode==Keys.Escape){ClosePicker();e.SuppressKeyPress=true;}else if(e.KeyCode==Keys.Up&&matches.SelectedIndex==0){query.Focus();e.SuppressKeyPress=true;}};
        var host=new ToolStripControlHost(surface){AutoSize=false,Size=surface.Size,Margin=Padding.Empty,Padding=Padding.Empty};
        popup.Items.Add(host);popup.Padding=Padding.Empty;popup.Margin=Padding.Empty;LibraryTheme.Menu(popup);
        popup.Opened+=delegate{query.Focus();};popup.Closed+=delegate{if(!IsDisposed&&Enabled)Focus();};Click+=delegate{OpenPicker();};
        EnabledChanged+=delegate{if(!Enabled)popup.Close();};
    }
    static void PaintButton(Button b){b.AutoSize=true;b.FlatStyle=FlatStyle.Flat;b.BackColor=LibraryTheme.Panel;b.ForeColor=LibraryTheme.Text;}
    internal void SetOptions(IEnumerable<LibraryGroupOption> values,string key,string legacy)
    {
        popup.Close();options=values.ToList();selected=options.Find(o=>o.Key==key);
        if(selected==null&&String.IsNullOrEmpty(key)&&legacy!="All items"){var old=options.Where(o=>o.Label==legacy).ToArray();if(old.Length==1)selected=old[0];}
        UpdateCaption();
    }
    internal void SelectKey(string key){var next=options.Find(o=>o.Key==key);if(ReferenceEquals(selected,next))return;selected=next;UpdateCaption();if(SelectionChanged!=null)SelectionChanged(this,EventArgs.Empty);}
    void UpdateCaption(){Text=(selected==null?"All groups":SelectedLabel)+" ▾";AccessibleName="Library group: "+SelectedLabel;}
    internal void OpenPicker(){if(!Enabled||popup.Visible)return;query.Text="";category="All";kinds[0].Checked=true;SearchOptions();popup.Show(this,new Point(0,Height));}
    internal void ClosePicker(){popup.Close();}
    void SearchOptions()
    {
        string text=query.Text.Trim();var found=options.Where(o=>(category=="All"||o.Kind==category)&&(text.Length==0||o.Label.IndexOf(text,StringComparison.OrdinalIgnoreCase)>=0)).ToList();
        matches.BeginUpdate();matches.Items.Clear();foreach(var item in found.Take(ResultLimit))matches.Items.Add(item);matches.EndUpdate();
        int at=matches.Items.Cast<LibraryGroupOption>().ToList().FindIndex(o=>o.Key==SelectedKey);matches.SelectedIndex=at>=0?at:matches.Items.Count>0?0:-1;apply.Enabled=matches.SelectedItem!=null;
        count.Text=found.Count==0?"No groups match. Try another name or category.":found.Count>ResultLimit?ResultLimit+" of "+found.Count+" groups · Type to narrow results":found.Count+" matching groups · Enter to apply";
    }
    void Commit(){var value=matches.SelectedItem as LibraryGroupOption;if(value==null)return;SelectKey(value.Key);ClosePicker();}
    sealed class GroupingPopup : ToolStripDropDown
    {
        readonly LibraryGroupingPicker owner;
        internal GroupingPopup(LibraryGroupingPicker value){owner=value;}
        protected override bool ProcessCmdKey(ref Message message,Keys key)
        {
            if(key==Keys.Escape){owner.ClosePicker();return true;}
            if(key==(Keys.Control|Keys.F)){owner.query.Focus();owner.query.SelectAll();return true;}
            if(key==Keys.Enter&&(owner.query.Focused||owner.matches.Focused)){owner.Commit();return true;}
            if(key==Keys.Down&&owner.query.Focused){if(owner.matches.Items.Count>0)owner.matches.Focus();return true;}
            if(key==Keys.Up&&owner.matches.Focused&&owner.matches.SelectedIndex==0){owner.query.Focus();return true;}
            return base.ProcessCmdKey(ref message,key);
        }
    }
    protected override void Dispose(bool disposing){if(disposing)popup.Dispose();base.Dispose(disposing);}
}

// Header owner-draw covers cells; the native trailing space needs the same paint.
internal sealed class LibraryListView : ListView
{
    [StructLayout(LayoutKind.Sequential)] struct Rect {public int Left,Top,Right,Bottom;}
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr hwnd,int msg,IntPtr w,IntPtr l);
    [DllImport("user32.dll",EntryPoint="SendMessageW")] static extern IntPtr HeaderRect(IntPtr hwnd,int msg,IntPtr w,ref Rect rect);
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr hwnd,out Rect rect);
    readonly HeaderPaint header;
    internal LibraryListView(){header=new HeaderPaint(this);}
    protected override void OnHandleCreated(EventArgs e){base.OnHandleCreated(e);IntPtr h=SendMessage(Handle,0x101F,IntPtr.Zero,IntPtr.Zero);if(h!=IntPtr.Zero)header.AssignHandle(h);}
    protected override void OnHandleDestroyed(EventArgs e){if(header.Handle!=IntPtr.Zero)header.ReleaseHandle();base.OnHandleDestroyed(e);}
    sealed class HeaderPaint : NativeWindow
    {
        readonly LibraryListView owner;
        internal HeaderPaint(LibraryListView value){owner=value;}
            protected override void WndProc(ref Message message)
        {
            base.WndProc(ref message);
            if(message.Msg!=0x000F&&message.Msg!=0x0317&&message.Msg!=0x0318)return;
            Rect client;if(Handle==IntPtr.Zero||!GetClientRect(Handle,out client))return;int right=0;
            for(int i=0;i<owner.Columns.Count;i++){Rect cell=new Rect();if(HeaderRect(Handle,0x1207,new IntPtr(i),ref cell)!=IntPtr.Zero)right=Math.Max(right,cell.Right);}
            right=Math.Max(0,right);if(right>=client.Right)return;
            bool print=message.Msg!=0x000F;if(print&&message.WParam==IntPtr.Zero)return;
            using(var dc=print?Graphics.FromHdc(message.WParam):Graphics.FromHwnd(Handle))using(var brush=new SolidBrush(LibraryTheme.Panel)){
                dc.FillRectangle(brush,Rectangle.FromLTRB(right,0,client.Right,client.Bottom));
                using(var pen=new Pen(LibraryTheme.Muted))dc.DrawLine(pen,right,client.Bottom-1,client.Right,client.Bottom-1);
            }
        }
    }
}
