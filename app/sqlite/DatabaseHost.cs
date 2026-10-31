using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

// A private on-demand process; it never accepts SQL from the UI.
internal sealed class DatabaseHost : Form
{
    const int ReadyMessage=0x8000+83, FailedMessage=0x8000+84, PageSize=50;
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child,IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr target,int message,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll",EntryPoint="SetWindowLongW")] static extern int SetWindowLong(IntPtr window,int index,int value);
    [DllImport("user32.dll",EntryPoint="GetWindowLongW")] static extern int GetWindowLong(IntPtr window,int index);
    static readonly Color Background=Color.FromArgb(28,31,38), PanelColor=Color.FromArgb(35,39,48), Foreground=Color.FromArgb(225,229,235), Accent=Color.FromArgb(78,153,211), Muted=Color.FromArgb(165,173,185);
    readonly IntPtr parent; readonly string path; readonly bool archived;
    readonly System.Windows.Forms.Timer lifetime=new System.Windows.Forms.Timer();
    readonly ListBox tables=new ListBox();
    readonly TextBox search=new TextBox();
    readonly Button searchButton=new Button(), refresh=new Button(), prev=new Button(), next=new Button();
    readonly Label status=new Label(), pageLabel=new Label(), error=new Label();
    readonly TabControl tabs=new TabControl();
    readonly NativeNavigationRail rowRail,schemaRail;
    uint? pendingRow;
    bool rowsReady;
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr window,int message,IntPtr w,IntPtr l);
    uint SchemaTop(){return (uint)SendMessage(schema.Handle,0xCE,IntPtr.Zero,IntPtr.Zero).ToInt64();}
    void JumpSchema(uint line){SendMessage(schema.Handle,0xB6,IntPtr.Zero,new IntPtr((long)line-SchemaTop()));schemaRail.Invalidate();}
    void JumpRow(uint row){if(row/50>20000000){status.Text="Saved row is outside the preview limit.";return;}if(row/PageSize!=(uint)page){pendingRow=row;page=(int)(row/PageSize);LoadPage();return;}int local=(int)(row%PageSize);if(local>=grid.Rows.Count){status.Text="Saved row is no longer available in this query.";return;}grid.FirstDisplayedScrollingRowIndex=local;rowRail.Invalidate();}
    protected override bool ProcessCmdKey(ref Message message,Keys key){if(key==(Keys.Control|Keys.D)||key==(Keys.Control|Keys.Shift|Keys.D)){var rail=tabs.SelectedIndex==1?schemaRail:rowRail;if((key&Keys.Shift)!=0)rail.ShowMarks();else rail.Toggle();return true;}return base.ProcessCmdKey(ref message,key);}
    protected override void WndProc(ref Message message){if(message.Msg==0x8000+90||message.Msg==0x8000+91){var rail=tabs.SelectedIndex==1?schemaRail:rowRail;if(rail!=null){if(message.Msg==0x8000+91)rail.ShowMarks();else rail.Toggle();}return;}base.WndProc(ref message);}
    readonly DataGridView grid=new DataGridView();
    readonly TextBox schema=new TextBox();
    CancellationTokenSource current; int generation, page, sort=-1; bool descending, loadingList;
    string filter="";
    SqliteReader.Table selected;

    static void ConfigureButton(Button target,string text)
    {
        target.Text=text;target.AutoSize=true;target.Height=29;target.FlatStyle=FlatStyle.Flat;target.ForeColor=Foreground;target.BackColor=PanelColor;target.Margin=new Padding(3,2,3,2);
    }
    public DatabaseHost(IntPtr target,string file,bool fromArchive,NavigationStore marks)
    {
        parent=target;path=file;archived=fromArchive;
        rowRail=new NativeNavigationRail(marks);schemaRail=new NativeNavigationRail(marks);
        rowRail.Context=delegate{return selected==null?"":"sqlite:rows:"+selected.Name+":"+sort+":"+descending+":"+filter;};
        rowRail.Minimum=delegate{return (uint)(page*PageSize);};rowRail.Maximum=delegate{return (uint)(page*PageSize+Math.Max(0,grid.Rows.Count-Math.Max(1,grid.DisplayedRowCount(false))));};
        rowRail.Position=delegate{return (uint)(page*PageSize+Math.Max(0,grid.FirstDisplayedScrollingRowIndex));};rowRail.Available=delegate{return rowsReady&&grid.Rows.Count>0;};rowRail.Label=delegate(uint row){return "Row "+(row+1)+" · Loaded page";};rowRail.Jump=JumpRow;
        schemaRail.Context=delegate{return selected==null?"":"sqlite:schema:"+selected.Name;};schemaRail.Minimum=delegate{return 0;};schemaRail.Maximum=delegate{return (uint)Math.Max(0,schema.Lines.Length-Math.Max(1,schema.ClientSize.Height/schema.Font.Height));};schemaRail.Position=SchemaTop;schemaRail.Available=delegate{return rowsReady&&schema.TextLength>0;};schemaRail.Label=delegate(uint line){return "Schema line "+(line+1);};schemaRail.Jump=JumpSchema;
        grid.Scroll+=delegate{rowRail.Changed();};grid.Resize+=delegate{rowRail.Invalidate();};schema.MouseWheel+=delegate{schemaRail.Invalidate();};schema.KeyUp+=delegate{schemaRail.Invalidate();};
        KeyPreview=true;
        FormBorderStyle=FormBorderStyle.None;ShowInTaskbar=false;StartPosition=FormStartPosition.Manual;Location=new Point(-32000,-32000);ClientSize=new Size(800,500);
        BackColor=Background;ForeColor=Foreground;Font=new Font("Segoe UI",9f);
        var main=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2,RowCount=1,Padding=new Padding(6),BackColor=Background};
        main.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,145));main.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));Controls.Add(main);
        var left=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=1,RowCount=2,Margin=new Padding(0,0,6,0)};
        left.RowStyles.Add(new RowStyle(SizeType.Absolute,27));left.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        var title=new Label{Text="TABLES & VIEWS",Dock=DockStyle.Fill,ForeColor=Muted,TextAlign=ContentAlignment.MiddleLeft};
        tables.Dock=DockStyle.Fill;tables.BackColor=PanelColor;tables.ForeColor=Foreground;tables.BorderStyle=BorderStyle.FixedSingle;tables.IntegralHeight=false;
        tables.SelectedIndexChanged+=delegate {if(loadingList)return;selected=tables.SelectedItem as SqliteReader.Table;page=0;sort=-1;descending=false;LoadPage();};
        left.Controls.Add(title,0,0);left.Controls.Add(tables,0,1);main.Controls.Add(left,0,0);
        var right=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=1,RowCount=4,Margin=Padding.Empty};
        right.RowStyles.Add(new RowStyle(SizeType.Absolute,38));right.RowStyles.Add(new RowStyle(SizeType.Percent,100));right.RowStyles.Add(new RowStyle(SizeType.Absolute,37));right.RowStyles.Add(new RowStyle(SizeType.Absolute,40));main.Controls.Add(right,1,0);
        var toolbar=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=3,RowCount=1,Margin=Padding.Empty};
        toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,70));toolbar.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,75));
        search.Dock=DockStyle.Fill;search.MaxLength=256;search.BackColor=PanelColor;search.ForeColor=Foreground;search.BorderStyle=BorderStyle.FixedSingle;search.Margin=new Padding(3,6,3,0);search.AccessibleName="Search rows";
        search.KeyDown+=delegate(object s,KeyEventArgs e){if(e.KeyCode==Keys.Enter){e.SuppressKeyPress=true;ApplySearch();}};
        ConfigureButton(searchButton,"Search");searchButton.Click+=delegate{ApplySearch();};
        ConfigureButton(refresh,"Refresh");refresh.Click+=delegate{LoadCatalog();};
        searchButton.Dock=DockStyle.Fill;refresh.Dock=DockStyle.Fill;
        toolbar.Controls.Add(search,0,0);toolbar.Controls.Add(searchButton,1,0);toolbar.Controls.Add(refresh,2,0);right.Controls.Add(toolbar,0,0);
        tabs.Dock=DockStyle.Fill;tabs.Margin=Padding.Empty;
        var rowsTab=new TabPage("Rows"){BackColor=Background};var schemaTab=new TabPage("Schema"){BackColor=Background};tabs.TabPages.Add(rowsTab);tabs.TabPages.Add(schemaTab);right.Controls.Add(tabs,0,1);
        grid.Dock=DockStyle.Fill;grid.BackgroundColor=Background;grid.ForeColor=Foreground;grid.BorderStyle=BorderStyle.None;grid.ReadOnly=true;grid.AllowUserToAddRows=false;grid.AllowUserToDeleteRows=false;grid.AllowUserToResizeRows=false;grid.RowHeadersVisible=false;grid.SelectionMode=DataGridViewSelectionMode.CellSelect;grid.MultiSelect=true;grid.ClipboardCopyMode=DataGridViewClipboardCopyMode.EnableWithoutHeaderText;grid.AutoSizeColumnsMode=DataGridViewAutoSizeColumnsMode.None;grid.EnableHeadersVisualStyles=false;
        grid.ColumnHeadersDefaultCellStyle.BackColor=PanelColor;grid.ColumnHeadersDefaultCellStyle.ForeColor=Foreground;grid.DefaultCellStyle.BackColor=Background;grid.DefaultCellStyle.ForeColor=Foreground;grid.DefaultCellStyle.SelectionBackColor=Color.FromArgb(48,89,119);grid.DefaultCellStyle.SelectionForeColor=Color.White;grid.GridColor=Color.FromArgb(58,63,72);
        grid.ColumnHeaderMouseClick+=delegate(object s,DataGridViewCellMouseEventArgs e){if(selected==null)return;if(sort==e.ColumnIndex)descending=!descending;else{sort=e.ColumnIndex;descending=false;}page=0;LoadPage();};
        var copyMenu=new ContextMenuStrip();copyMenu.Items.Add("Copy selected cells",null,delegate{try{var data=grid.GetClipboardContent();if(data!=null)Clipboard.SetDataObject(data,true);}catch(ExternalException){status.Text="Clipboard is currently unavailable.";}});grid.ContextMenuStrip=copyMenu;
        grid.CellMouseDown+=delegate(object s,DataGridViewCellMouseEventArgs e){if(e.Button==MouseButtons.Right && e.RowIndex>=0 && e.ColumnIndex>=0 && !grid.Rows[e.RowIndex].Cells[e.ColumnIndex].Selected){grid.ClearSelection();grid.Rows[e.RowIndex].Cells[e.ColumnIndex].Selected=true;}};
        error.Dock=DockStyle.Fill;error.ForeColor=Muted;error.TextAlign=ContentAlignment.MiddleCenter;error.Padding=new Padding(20);error.Visible=false;
        grid.ScrollBars=ScrollBars.Horizontal;rowsTab.Controls.Add(grid);rowsTab.Controls.Add(rowRail);rowsTab.Controls.Add(error);error.BringToFront();
        schema.Dock=DockStyle.Fill;schema.Multiline=true;schema.ScrollBars=ScrollBars.Horizontal;schema.WordWrap=false;schema.ReadOnly=true;schema.BackColor=Background;schema.ForeColor=Foreground;schema.BorderStyle=BorderStyle.None;schema.Font=new Font("Consolas",9f);schemaTab.Controls.Add(schema);schemaTab.Controls.Add(schemaRail);
        var navigation=new FlowLayoutPanel{Dock=DockStyle.Fill,WrapContents=false,Margin=Padding.Empty};
        ConfigureButton(prev,"Previous");prev.Click+=delegate{if(page>0){page--;LoadPage();}};
        ConfigureButton(next,"Next");next.Click+=delegate{if(page<20000000){page++;LoadPage();}};
        pageLabel.AutoSize=true;pageLabel.ForeColor=Muted;pageLabel.Margin=new Padding(8,8,0,0);
        navigation.Controls.Add(prev);navigation.Controls.Add(next);navigation.Controls.Add(pageLabel);right.Controls.Add(navigation,0,2);
        status.Dock=DockStyle.Fill;status.ForeColor=Muted;status.TextAlign=ContentAlignment.TopLeft;status.AutoEllipsis=true;right.Controls.Add(status,0,3);
        lifetime.Interval=1000;lifetime.Tick+=delegate{if(!IsWindow(parent))Close();};
        Shown+=delegate {try{SetWindowLong(Handle,-16,(GetWindowLong(Handle,-16)&~unchecked((int)0x80000000))|0x40000000);SetParent(Handle,parent);if(GetParent(Handle)!=parent)throw new InvalidOperationException("Could not attach preview");lifetime.Start();PostMessage(parent,ReadyMessage,Handle,IntPtr.Zero);LoadCatalog();}catch{PostMessage(parent,FailedMessage,Handle,IntPtr.Zero);Close();}};
        FormClosed+=delegate {if(current!=null)current.Cancel();lifetime.Stop();};
    }
    CancellationToken BeginRequest(out int request)
    {
        if(current!=null)current.Cancel();current=new CancellationTokenSource();request=++generation;return current.Token;
    }
    void ShowError(string text)
    {
        rowsReady=false;rowRail.Changed();grid.Visible=false;error.Text=text;error.Visible=true;prev.Enabled=next.Enabled=false;pageLabel.Text="";status.Text="Read-only preview";
    }
    async void LoadCatalog()
    {
        int request;CancellationToken token=BeginRequest(out request);status.Text="Opening database…";prev.Enabled=next.Enabled=false;
        try {
            var catalog=await Task.Run(delegate {using(var reader=new SqliteReader(path,archived,token))return reader.ReadCatalog();},token);
            if(IsDisposed || request!=generation)return;
            string old=selected==null?null:selected.Name;loadingList=true;tables.Items.Clear();int choose=-1;
            for(int i=0;i<catalog.Tables.Count;i++){tables.Items.Add(catalog.Tables[i]);if(catalog.Tables[i].Name==old)choose=i;}
            loadingList=false;selected=null;
            if(tables.Items.Count==0){schema.Clear();ShowError("This database has no tables or views.");return;}
            if(choose<0)choose=0;tables.SelectedIndex=choose;
            if(catalog.Truncated)status.Text="The table list is limited to 10,000 objects.";
        } catch(OperationCanceledException) {} catch(Exception ex) {if(!IsDisposed && request==generation)ShowError(ex.Message);}
    }
    void ApplySearch(){filter=search.Text;page=0;LoadPage();}
    async void LoadPage()
    {
        if(selected==null)return;
        uint? restoreRow=pendingRow;pendingRow=null;
        int request;CancellationToken token=BeginRequest(out request);var table=selected;long offset=(long)page*PageSize;int ordering=sort;bool reverse=descending;string query=filter;
        rowsReady=false;rowRail.Changed();schemaRail.Changed();status.Text="Loading rows…";prev.Enabled=next.Enabled=false;
        try {
            var data=await Task.Run(delegate {using(var reader=new SqliteReader(path,archived,token))return reader.ReadPage(table,offset,PageSize,ordering,reverse,query);},token);
            if(IsDisposed || request!=generation)return;
            grid.Visible=false;error.Visible=false;grid.Columns.Clear();grid.Rows.Clear();
            foreach(var col in data.Columns){var c=new DataGridViewTextBoxColumn();c.HeaderText=col.Name;c.Name="column"+grid.Columns.Count;c.Width=160;c.SortMode=DataGridViewColumnSortMode.Programmatic;grid.Columns.Add(c);}
            if(ordering>=0 && ordering<grid.Columns.Count)grid.Columns[ordering].HeaderCell.SortGlyphDirection=reverse?SortOrder.Descending:SortOrder.Ascending;
            foreach(var row in data.Rows){object[] values=new object[row.Length];for(int i=0;i<row.Length;i++)values[i]=row[i].Text;int at=grid.Rows.Add(values);for(int i=0;i<row.Length;i++){if(row[i].IsNull || row[i].IsBlob)grid.Rows[at].Cells[i].Style.ForeColor=Muted;if(row[i].Truncated)grid.Rows[at].Cells[i].ToolTipText="Value shortened to 2,048 characters for preview.";}}
            var description=new StringBuilder();description.AppendLine(table.Type.ToUpperInvariant()+"  "+table.Name).AppendLine().AppendLine("COLUMNS");
            foreach(var col in data.Columns)description.Append("  ").Append(col.Name).Append(col.Type.Length>0?"  "+col.Type:"").AppendLine();
            description.AppendLine().AppendLine("CREATE STATEMENT").AppendLine(table.Schema.Length>0?table.Schema:"(not available)");schema.Text=description.ToString();
            if(data.Rows.Count==0){error.Text=query.Length>0?"No rows match this search.":"This table or view has no rows.";error.Visible=true;}else grid.Visible=true;
            rowsReady=true;rowRail.Changed();schemaRail.Changed();if(restoreRow.HasValue)JumpRow(restoreRow.Value);
            prev.Enabled=page>0;next.Enabled=data.HasMore;pageLabel.Text="Page "+(page+1)+" · "+data.Rows.Count+" rows";
            status.Text=(archived?"Archive preview: companion files are not included. ":"")+"Read-only · Click a column heading to sort · Search ignores BLOBs; ASCII case-insensitive";
        } catch(OperationCanceledException) {} catch(Exception ex) {if(!IsDisposed && request==generation)ShowError(ex.Message);}
    }
    [STAThread] static int Main(string[] args)
    {
        long target;int maxBytes,archive;
        if((args.Length!=4 && args.Length!=6) || !Int64.TryParse(args[0],out target) || !Int32.TryParse(args[1],out maxBytes) || !Int32.TryParse(args[3],out archive) || (archive!=0 && archive!=1))return 2;
        IntPtr parent=new IntPtr(target);if(!IsWindow(parent))return 2;
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new DatabaseHost(parent,args[2],archive==1,NavigationStore.FromArgs(args,4,args[2])));return 0;
    }
}
