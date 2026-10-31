using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Drawing;
using System.ComponentModel;
using Microsoft.Win32.SafeHandles;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using System.Collections.Generic;
using System.Threading.Tasks;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

// A separate process owns the browser and the untrusted document.
internal sealed class MarkdownHost : Form
{
    const int ReadyMessage = 0x8000 + 83;
    const int FailedMessage = 0x8000 + 84;
    [DllImport("user32.dll")] static extern IntPtr SetParent(IntPtr child, IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetParent(IntPtr child);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr target, int message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll", EntryPoint="SetWindowLongW")] static extern int SetWindowLong(IntPtr window, int index, int value);
    [DllImport("user32.dll", EntryPoint="GetWindowLongW")] static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security,
                                             uint creation, uint flags, IntPtr template);
    readonly BrowserNavigation navigation;
    readonly IntPtr parent;
    readonly string file;
    readonly int limit;
    readonly bool json, jsonl, table, notebook, config, ini, xml;
    readonly string configFormat;
    char tableDelimiter;
    string tableEncoding="utf8";
    readonly WebView2 browser = new WebView2();
    readonly Timer lifetime = new Timer();
    StreamReader records;
    DelimitedReader tableReader;
    int recordNumber;
    bool recordsBusy, recordsEnded;
    sealed class Record { public int number { get; set; } public string text { get; set; } public string error { get; set; } }
    sealed class RecordBatch { public string kind { get; set; } public List<Record> records { get; set; } public bool end { get; set; } }
    sealed class TableBatch { public string kind { get; set; } public List<DelimitedReader.Row> rows { get; set; } public bool end { get; set; } }

    TableBatch NextTable()
    {
        var batch=new TableBatch { kind="rows", rows=new List<DelimitedReader.Row>() };
        int units=0;
        while(batch.rows.Count<64 && units<1024*1024) {
            var row=tableReader.Read();if(row==null)break;
            batch.rows.Add(row);
            if(row.fields!=null)foreach(string field in row.fields)units+=field.Length;
        }
        batch.end=tableReader.Ended;return batch;
    }

    RecordBatch NextRecords()
    {
        var batch = new RecordBatch { kind="records", records=new List<Record>() };
        int bytes = 0;
        while (batch.records.Count < 64 && bytes < 1024 * 1024 && !recordsEnded) {
            var line = new StringBuilder(); bool tooLong=false, hasData=false;
            for (;;) {
                int ch=records.Read();
                if (ch<0) { recordsEnded=true; break; }
                hasData=true;
                if (ch=='\n') break;
                if (line.Length < 1024 * 1024) line.Append((char)ch); else tooLong=true;
            }
            if (!hasData && line.Length==0 && recordsEnded) break;
            if (line.Length>0 && line[line.Length-1]=='\r') line.Length--;
            string value=tooLong ? null : line.ToString();
            bytes += line.Length;
            batch.records.Add(new Record { number=++recordNumber, text=value, error=tooLong ? "Record exceeds the 1,048,576 UTF-16-unit preview limit" : null });
        }
        batch.end=recordsEnded;
        return batch;
    }

    static FileStream OpenDocument(string path)
    {
        // Archive previews use \\?\ paths, which legacy .NET Framework path
        // normalization rejects. Open through Win32 and retain its long-path semantics.
        SafeFileHandle handle = CreateFileW(path, 0x80000000, 7, IntPtr.Zero, 3, 0x08000000, IntPtr.Zero);
        if (handle.IsInvalid) {
            int error = Marshal.GetLastWin32Error(); handle.Dispose(); throw new Win32Exception(error);
        }
        try { return new FileStream(handle, FileAccess.Read); }
        catch { handle.Dispose(); throw; }
    }

    static string DecodeIni(byte[] bytes, int used, string mode, out string label)
    {
        Encoding encoding=Encoding.UTF8;bool guessed=false;
        bool bom=(used>=3 && bytes[0]==0xef && bytes[1]==0xbb && bytes[2]==0xbf) ||
                 (used>=2 && ((bytes[0]==0xff && bytes[1]==0xfe) || (bytes[0]==0xfe && bytes[1]==0xff))) ||
                 (used>=4 && bytes[0]==0 && bytes[1]==0 && bytes[2]==0xfe && bytes[3]==0xff);
        if(mode=="windows1252")encoding=Encoding.GetEncoding(1252);
        else if(mode=="auto" && !bom){
            try { new UTF8Encoding(false,true).GetCharCount(bytes,0,used); }
            catch(DecoderFallbackException){encoding=Encoding.GetEncoding(1252);guessed=true;}
        }
        using(var reader=new StreamReader(new MemoryStream(bytes,0,used),encoding,true)){
            string result=reader.ReadToEnd();label=reader.CurrentEncoding.WebName+(guessed?" (guessed)":"");return result;
        }
    }

    MarkdownHost(IntPtr target, string path, int maxBytes, NavigationStore marks)
    {
        parent = target; file = path; limit = maxBytes; navigation=new BrowserNavigation(marks);
        jsonl = path.EndsWith(".jsonl",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".ndjson",StringComparison.OrdinalIgnoreCase);
        json = jsonl || path.EndsWith(".json",StringComparison.OrdinalIgnoreCase);
        table = path.EndsWith(".csv",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".tsv",StringComparison.OrdinalIgnoreCase);
        notebook = path.EndsWith(".ipynb",StringComparison.OrdinalIgnoreCase);
        ini=path.EndsWith(".ini",StringComparison.OrdinalIgnoreCase);
        configFormat=ini?"ini":path.EndsWith(".toml",StringComparison.OrdinalIgnoreCase)?"toml":"yaml";
        config = ini || path.EndsWith(".yaml",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".yml",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".toml",StringComparison.OrdinalIgnoreCase);
        xml = path.EndsWith(".xml",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".xaml",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".rss",StringComparison.OrdinalIgnoreCase) || path.EndsWith(".atom",StringComparison.OrdinalIgnoreCase);
        tableDelimiter=path.EndsWith(".tsv",StringComparison.OrdinalIgnoreCase)?'\t':',';
        FormBorderStyle = FormBorderStyle.None;
        ShowInTaskbar = false;
        StartPosition = FormStartPosition.Manual;
        Location = new Point(-32000, -32000);
        browser.Dock = DockStyle.Fill;
        Controls.Add(browser);
        lifetime.Interval = 1000;
        lifetime.Tick += delegate { if (!IsWindow(parent)) Close(); };
        Shown += Start;
    }

    async void Start(object sender, EventArgs args)
    {
        try {
            SetWindowLong(Handle, -16, (GetWindowLong(Handle, -16) & ~unchecked((int)0x80000000)) | 0x40000000);
            SetParent(Handle, parent);
            if (GetParent(Handle) != parent) throw new InvalidOperationException("Could not attach preview");
            lifetime.Start();
            string source=null,encodingLabel=null; bool limited=false;byte[] iniBytes=null;int iniUsed=0;
            XmlPreviewReader.Result xmlResult=null;
            if(xml) {
                xmlResult=await Task.Run(delegate {using(FileStream stream=OpenDocument(file))return XmlPreviewReader.Parse(stream,limit);});
            }
            else if (jsonl || table) {
                records=new StreamReader(OpenDocument(file),Encoding.UTF8,true,4096);
                if(table)tableReader=new DelimitedReader(records,tableDelimiter);
            }
            else using (FileStream stream = OpenDocument(file)) {
                limited=stream.Length>limit;
                byte[] data = new byte[Math.Min(limit, (int)Math.Min(stream.Length, Int32.MaxValue))];
                int used = 0, n;
                while (used < data.Length && (n = stream.Read(data, used, data.Length - used)) > 0) used += n;
                if(ini){iniBytes=data;iniUsed=used;source=DecodeIni(data,used,"auto",out encodingLabel);}
                else using (StreamReader reader = new StreamReader(new MemoryStream(data, 0, used), Encoding.UTF8, true)) source = reader.ReadToEnd();
            }
            string assets = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "markdown");
            string dataDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Folderwing", "WebView2");
            CoreWebView2Environment env = await CoreWebView2Environment.CreateAsync(null, dataDir);
            await browser.EnsureCoreWebView2Async(env);
            CoreWebView2 core = browser.CoreWebView2;
            core.Settings.AreDefaultContextMenusEnabled = false;
            core.Settings.AreDevToolsEnabled = false;
            core.Settings.IsStatusBarEnabled = false;
            core.Settings.AreHostObjectsAllowed = false;
            core.Settings.IsZoomControlEnabled = true;
            core.SetVirtualHostNameToFolderMapping("folderwing-preview.invalid", assets, CoreWebView2HostResourceAccessKind.DenyCors);
            string page="https://folderwing-preview.invalid/"+(xml ? "xml.html" : notebook ? "notebook.html" : table ? "table.html" : config ? "config.html" : json ? "structured.html" : "index.html");
            core.NavigationStarting += delegate(object s, CoreWebView2NavigationStartingEventArgs e) {
                if (e.Uri != page) e.Cancel = true;
            };
            core.NewWindowRequested += delegate(object s, CoreWebView2NewWindowRequestedEventArgs e) { e.Handled = true; };
            core.WebMessageReceived += async delegate(object s, CoreWebView2WebMessageReceivedEventArgs e) {
                if (e.Source != page) return;
                try {
                    string message = e.TryGetWebMessageAsString();
                    if(await navigation.Handle(core,message))return;
                    if(ini && message.StartsWith("ini-encoding:",StringComparison.Ordinal)){
                        string mode=message.Substring(13);
                        if(mode!="auto" && mode!="utf8" && mode!="windows1252")return;
                        string label,decoded=DecodeIni(iniBytes,iniUsed,mode,out label);
                        var ser=new JavaScriptSerializer();ser.MaxJsonLength=Math.Max(decoded.Length*6+1000,1024);
                        core.PostWebMessageAsJson(ser.Serialize(new {kind="config",format="ini",source=decoded,limited=limited,encoding=label}));
                        return;
                    }
                    if ((jsonl || table) && (message=="reset" || (table && (message.StartsWith("delimiter:",StringComparison.Ordinal) || message.StartsWith("encoding:",StringComparison.Ordinal)))) && !recordsBusy) {
                        if(table && message.StartsWith("delimiter:",StringComparison.Ordinal)) {
                            string name=message.Substring(10);
                            if(name=="comma")tableDelimiter=',';else if(name=="tab")tableDelimiter='\t';else if(name=="semicolon")tableDelimiter=';';else return;
                        }
                        if(table && message.StartsWith("encoding:",StringComparison.Ordinal)) {
                            string name=message.Substring(9);
                            if(name!="utf8" && name!="windows1252")return;
                            tableEncoding=name;
                        }
                        Encoding encoding=table && tableEncoding=="windows1252"?Encoding.GetEncoding(1252):Encoding.UTF8;
                        records.Dispose(); records=new StreamReader(OpenDocument(file),encoding,true,4096);
                        recordsEnded=false; recordNumber=0;
                        if(table)tableReader=new DelimitedReader(records,tableDelimiter);
                        core.PostWebMessageAsJson("{\"kind\":\"reset\"}");
                        return;
                    }
                    if ((jsonl || table) && message=="next" && !recordsBusy && !recordsEnded) {
                        recordsBusy=true;
                        try {
                            var ser = new JavaScriptSerializer(); ser.MaxJsonLength=16*1024*1024;
                            if(table) {
                                TableBatch batch=await Task.Run((Func<TableBatch>)NextTable);recordsEnded=batch.end;
                                if(!IsDisposed)core.PostWebMessageAsJson(ser.Serialize(batch));
                            } else {
                                RecordBatch batch=await Task.Run((Func<RecordBatch>)NextRecords);
                                if(!IsDisposed)core.PostWebMessageAsJson(ser.Serialize(batch));
                            }
                        } finally { recordsBusy=false; }
                        return;
                    }
                    if (message.StartsWith("rendered:", StringComparison.Ordinal) || message.StartsWith("structured:", StringComparison.Ordinal) || message.StartsWith("table:", StringComparison.Ordinal) || message.StartsWith("notebook:", StringComparison.Ordinal)) {
                        string[] parts = message.Split(':'); int count, errors;
                        if (parts.Length == 3 && Int32.TryParse(parts[1], out count) && Int32.TryParse(parts[2], out errors))
                            PostMessage(parent, 0x8000 + 85, new IntPtr(count), new IntPtr(errors));
                        return;
                    }
                    if (message != "ready") return;
                    var serializer = new JavaScriptSerializer(); serializer.MaxJsonLength = xml ? 64*1024*1024 : (jsonl || table) ? 1024 : Math.Max(source.Length * 6 + 1000, 1024);
                    if(xml) {serializer.RecursionLimit=512;core.PostWebMessageAsJson(serializer.Serialize(xmlResult));}
                    else if(config)core.PostWebMessageAsJson(serializer.Serialize(new {kind="config",format=configFormat,source=source,limited=limited,encoding=encodingLabel}));
                    else if(notebook)core.PostWebMessageAsJson(serializer.Serialize(new {kind="notebook",source=source,limited=limited}));
                    else if(table)core.PostWebMessageAsJson(serializer.Serialize(new {kind="table",delimiter=tableDelimiter=='\t'?"tab":"comma"}));
                    else if (jsonl) core.PostWebMessageAsJson(serializer.Serialize(new {kind="jsonl"}));
                    else if(json) core.PostWebMessageAsJson(serializer.Serialize(new {kind="json",source=source,limited=limited}));
                    else core.PostWebMessageAsJson(serializer.Serialize(source));
                    PostMessage(parent, ReadyMessage, Handle, IntPtr.Zero);
                } catch { PostMessage(parent, FailedMessage, Handle, IntPtr.Zero); }
            };
            core.Navigate(page);
        } catch { PostMessage(parent, FailedMessage, Handle, IntPtr.Zero); Close(); }
    }
    protected override void WndProc(ref Message message){if(BrowserNavigation.Dispatch(browser.CoreWebView2,message.Msg))return;base.WndProc(ref message);}
    [STAThread] static int Main(string[] args)
    {
        long target; int maxBytes;
        if ((args.Length != 3 && args.Length != 5) || !Int64.TryParse(args[0], out target) || !Int32.TryParse(args[1], out maxBytes) || maxBytes < 1 || maxBytes > 8 * 1024 * 1024) return 2;
        IntPtr parent = new IntPtr(target);
        if (!IsWindow(parent)) return 2;
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new MarkdownHost(parent, args[2], maxBytes, NavigationStore.FromArgs(args,3,args[2])));
        return 0;
    }
}
