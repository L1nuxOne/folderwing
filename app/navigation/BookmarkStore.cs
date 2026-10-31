using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

// Same bounded, checksummed format as docmarks.c. The document key is supplied
// by the explorer, never by document script or a browser message.
internal sealed class NavigationMark
{
    public string id {get;set;}
    public uint page {get;set;}
    public float y {get;set;}
    public string title {get;set;}
}
internal sealed class NavigationStore
{
    sealed class Row { public ulong Id; public uint Page; public float Y; public string Path,Title; }
    const int MaxRows=4096,PerFile=512,MaxBytes=8*1024*1024;
    readonly string store,identity;
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFileW(string path,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool CreateDirectoryW(string path,IntPtr security);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool DeleteFileW(string path);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool MoveFileExW(string from,string to,uint flags);
    public NavigationStore(string file,string key){store=file;identity=key;}
    public static NavigationStore FromArgs(string[] args,int at,string document){
        string file=args.Length==at+2?args[at]:Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Folderwing","preview-bookmarks.fwb");
        if(String.IsNullOrEmpty(file))file=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"Folderwing","preview-bookmarks.fwb");
        string key=args.Length==at+2?args[at+1]:document;
        return new NavigationStore(file,key);
    }
    static FileStream Open(string path,uint access,uint share,uint creation){var h=CreateFileW(path,access,share,IntPtr.Zero,creation,0,IntPtr.Zero);if(h.IsInvalid){int e=Marshal.GetLastWin32Error();h.Dispose();throw new System.ComponentModel.Win32Exception(e);}return new FileStream(h,access==0x80000000?FileAccess.Read:FileAccess.ReadWrite);}
    static byte[] Units(string text){var bytes=new byte[text.Length*2];for(int i=0;i<text.Length;i++){bytes[i*2]=(byte)text[i];bytes[i*2+1]=(byte)(text[i]>>8);}return bytes;}
    static string Text(byte[] bytes){var chars=new char[bytes.Length/2];for(int i=0;i<chars.Length;i++)chars[i]=(char)(bytes[i*2]|bytes[i*2+1]<<8);return new String(chars);}
    static ulong Hash(byte[] data,int start,int count){ulong h=14695981039346656037UL;unchecked{for(int i=start;i<start+count;i++){h^=data[i];h*=1099511628211UL;}}return h;}
    static int SensitiveAt(string s){int prefix=s.StartsWith(@"\\wsl.localhost\",StringComparison.OrdinalIgnoreCase)?16:s.StartsWith(@"\\wsl$\",StringComparison.OrdinalIgnoreCase)?7:0;if(prefix==0)return s.Length;int end=s.IndexOf('\\',prefix);return end<0?s.Length:end;}
    static bool Same(string a,string b){int x=SensitiveAt(a),y=SensitiveAt(b);return x==y&&String.Equals(a.Substring(0,x),b.Substring(0,y),StringComparison.OrdinalIgnoreCase)&&String.Equals(a.Substring(x),b.Substring(y),StringComparison.Ordinal);}
    string Key(string scope){if(String.IsNullOrEmpty(identity)||identity.Length>30000)throw new InvalidDataException("Bookmarks are unavailable for this preview.");if(scope==null)scope="";if(scope.Length>1024)throw new InvalidDataException("Reading context is too long.");var key=new StringBuilder(identity);key.Append('\x1f');foreach(char c in scope)key.Append(((int)c).ToString("X4"));if(key.Length>32767)throw new InvalidDataException("Reading context is too long.");return key.ToString();}
    List<Row> Read(){
        byte[] data;
        try {using(var f=Open(store,0x80000000,7,3)){if(f.Length<24||f.Length>MaxBytes)throw new InvalidDataException("Bookmark storage needs review; saved data was kept.");data=new byte[(int)f.Length];int used=0,n;while(used<data.Length&&(n=f.Read(data,used,data.Length-used))>0)used+=n;if(used!=data.Length)throw new IOException("Bookmark storage could not be read.");}}
        catch(System.ComponentModel.Win32Exception e){if(e.NativeErrorCode==2||e.NativeErrorCode==3)return new List<Row>();throw;}
        using(var reader=new BinaryReader(new MemoryStream(data),Encoding.Unicode)){
            if(Encoding.ASCII.GetString(reader.ReadBytes(8))!="FWMARK01")throw new InvalidDataException("Bookmark storage needs review; saved data was kept.");
            uint count=reader.ReadUInt32(),bytes=reader.ReadUInt32();ulong sum=reader.ReadUInt64();if(count>MaxRows||bytes!=data.Length-24||sum!=Hash(data,24,data.Length-24))throw new InvalidDataException("Bookmark storage needs review; saved data was kept.");
            var result=new List<Row>();var ids=new HashSet<ulong>();
            for(uint i=0;i<count;i++){
                if(reader.BaseStream.Length-reader.BaseStream.Position<24)throw new InvalidDataException("Invalid bookmark storage.");
                var row=new Row{Id=reader.ReadUInt64(),Page=reader.ReadUInt32(),Y=reader.ReadSingle()};uint pathLength=reader.ReadUInt32(),titleLength=reader.ReadUInt32();
                if(row.Id==0||!ids.Add(row.Id)||Single.IsNaN(row.Y)||Single.IsInfinity(row.Y)||row.Y<0||row.Y>1000000||pathLength<1||pathLength>32767||titleLength>159||(long)(pathLength+titleLength)*2>reader.BaseStream.Length-reader.BaseStream.Position)throw new InvalidDataException("Invalid bookmark storage.");
                row.Path=Text(reader.ReadBytes((int)pathLength*2));row.Title=Text(reader.ReadBytes((int)titleLength*2));if(row.Path.IndexOf('\0')>=0||row.Title.IndexOf('\0')>=0)throw new InvalidDataException("Invalid bookmark storage.");result.Add(row);
            }
            if(reader.BaseStream.Position!=reader.BaseStream.Length)throw new InvalidDataException("Invalid bookmark storage.");return result;
        }
    }
    void Write(List<Row> rows){
        byte[] payload;
        using(var memory=new MemoryStream()){using(var writer=new BinaryWriter(memory,Encoding.Unicode)){foreach(var row in rows){writer.Write(row.Id);writer.Write(row.Page);writer.Write(row.Y);writer.Write((uint)row.Path.Length);writer.Write((uint)row.Title.Length);writer.Write(Units(row.Path));writer.Write(Units(row.Title));}writer.Flush();payload=memory.ToArray();}}
        if(payload.Length>MaxBytes-24)throw new InvalidDataException("Bookmark storage limit reached.");
        string temp=store+"."+Guid.NewGuid().ToString("N")+".tmp";
        bool created=false;
        try {using(var file=Open(temp,0xC0000000,0,1)){created=true;using(var writer=new BinaryWriter(file,Encoding.Unicode)){writer.Write(Encoding.ASCII.GetBytes("FWMARK01"));writer.Write((uint)rows.Count);writer.Write((uint)payload.Length);writer.Write(Hash(payload,0,payload.Length));writer.Write(payload);writer.Flush();file.Flush(true);}}
            if(!MoveFileExW(temp,store,9))throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());created=false;
        }finally{if(created)DeleteFileW(temp);}
    }
    public List<NavigationMark> Load(string scope){string key=Key(scope);var result=new List<NavigationMark>();foreach(var row in Read())if(Same(row.Path,key)){if(result.Count==PerFile)throw new InvalidDataException("Bookmark limit reached.");result.Add(new NavigationMark{id=row.Id.ToString("X16"),page=row.Page,y=row.Y,title=row.Title});}return result;}
    public void Add(string scope,uint page,float y,string title){
        if(Single.IsNaN(y)||Single.IsInfinity(y)||y<0||y>1000000)throw new InvalidDataException("Invalid reading position.");
        string key=Key(scope);title=title??"Reading position";if(title.Length>159)title=title.Substring(0,159);if(title.IndexOf('\0')>=0)throw new InvalidDataException("Invalid bookmark title.");
        int slash=store.LastIndexOf('\\');if(slash>0){string directory=store.Substring(0,slash);if(!CreateDirectoryW(directory,IntPtr.Zero)){int error=Marshal.GetLastWin32Error();if(error!=183)throw new System.ComponentModel.Win32Exception(error);}}
        using(Open(store+".lock",0xC0000000,0,4)){var rows=Read();int count=0;var ids=new HashSet<ulong>();foreach(var row in rows){ids.Add(row.Id);if(Same(row.Path,key)){count++;if(row.Page==page&&Math.Abs(row.Y-y)<0.0001f)return;}}
            if(rows.Count==MaxRows||count==PerFile)throw new InvalidDataException("Bookmark limit reached.");ulong id=(ulong)DateTime.UtcNow.Ticks;while(id==0||ids.Contains(id))id++;
            rows.Add(new Row{Id=id,Page=page,Y=y,Path=key,Title=title});Write(rows);
        }
    }
    public void Remove(string scope,string idText){ulong id;if(!UInt64.TryParse(idText,System.Globalization.NumberStyles.HexNumber,System.Globalization.CultureInfo.InvariantCulture,out id))throw new InvalidDataException("Invalid bookmark.");string key=Key(scope);
        using(Open(store+".lock",0xC0000000,0,4)){var rows=Read();int removed=rows.RemoveAll(delegate(Row row){return row.Id==id&&Same(row.Path,key);});if(removed>0)Write(rows);}
    }
    public static string Error(Exception e){var native=e as System.ComponentModel.Win32Exception;if(native!=null&&native.NativeErrorCode==32)return "Bookmarks are in use. Try again.";if(e is InvalidDataException)return e.Message;return "Bookmark storage is unavailable; saved data was kept.";}
}
