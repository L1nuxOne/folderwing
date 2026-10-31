// Managed/native store interoperability and real read-only SQLite rail controls.
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
internal static class NavigationManagedTests
{
    static int count;
    static void Check(bool value,string text){if(!value)throw new Exception(text);count++;Console.WriteLine("ok "+text);}
    static bool Wait(Func<bool> ready){var watch=Stopwatch.StartNew();do{Application.DoEvents();if(ready())return true;Thread.Sleep(10);}while(watch.ElapsedMilliseconds<20000);return false;}
    static T Field<T>(object target,string name){return (T)target.GetType().GetField(name,BindingFlags.Instance|BindingFlags.NonPublic).GetValue(target);}
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_open_v2(byte[] path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close_v2(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_exec(IntPtr db,byte[] sql,IntPtr cb,IntPtr ctx,out IntPtr error);
    static byte[] Utf8(string value){return Encoding.UTF8.GetBytes(value+"\0");}
    static void Fixture(string path){IntPtr db,err;Check(sqlite3_open_v2(Utf8(path),out db,6,IntPtr.Zero)==0,"owned SQLite fixture opens");try{var sql=new StringBuilder("CREATE TABLE items(id INTEGER,label TEXT);BEGIN;");for(int i=0;i<105;i++)sql.Append("INSERT INTO items VALUES(").Append(i).Append(",'item").Append(i).Append("');");sql.Append("COMMIT;");Check(sqlite3_exec(db,Utf8(sql.ToString()),IntPtr.Zero,IntPtr.Zero,out err)==0,"owned SQLite fixture populated");}finally{sqlite3_close_v2(db);}}
    static void Before(string scratch){
        string file=Path.Combine(scratch,"store.fwb");var a=new NavigationStore(file,@"C:\Folder\doc.md");Check(a.Load("markdown").Count==0,"missing store empty");a.Add("markdown",700000,.25f,"Équations 日本語");var b=new NavigationStore(file,@"c:\FOLDER\DOC.MD");Check(b.Load("markdown").Count==1,"Windows paths ignore case");b.Add("markdown",800000,.5f,"Second");Check(a.Load("markdown").Count==2,"separate clients preserve changes");a.Add("markdown",700000,.25f,"Duplicate");Check(a.Load("markdown").Count==2,"duplicate position not duplicated");a.Add("source",0,0,"Source");Check(a.Load("source").Count==1&&a.Load("markdown").Count==2,"rendered and source contexts independent");
        var linux=new NavigationStore(file,@"\\wsl.localhost\Ubuntu\home\A.md");linux.Add("markdown",1,0,"Linux");Check(new NavigationStore(file,@"\\WSL.LOCALHOST\UBUNTU\home\A.md").Load("markdown").Count==1,"WSL host and distro ignore case");Check(new NavigationStore(file,@"\\wsl.localhost\Ubuntu\home\a.md").Load("markdown").Count==0,"Linux path case preserved");
        var archive=new NavigationStore(file,"C:\\archive.zip\u001f0041002E006D0064");archive.Add("markdown",1,0,"Archive");Check(new NavigationStore(file,"C:\\archive.zip\u001f0061002E006D0064").Load("markdown").Count==0,"archive members remain case distinct");
        string id=a.Load("markdown")[0].id;Check(id.Length==16,"bookmark ID avoids JS number rounding");a.Remove("source",id);Check(a.Load("markdown").Count==2,"wrong scope cannot remove bookmark");a.Remove("markdown",id);Check(a.Load("markdown").Count==1,"remove scoped bookmark");
        byte[] prior=File.ReadAllBytes(file);using(File.Open(file+".lock",FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None)){bool denied=false;try{a.Add("markdown",4,0,"Busy");}catch(System.ComponentModel.Win32Exception e){denied=e.NativeErrorCode==32;}Check(denied,"exclusive native-compatible lock");}Check(Convert.ToBase64String(prior)==Convert.ToBase64String(File.ReadAllBytes(file)),"busy store unchanged");
        File.WriteAllText(file,"preserve corrupt store");bool corrupt=false;try{a.Add("markdown",1,0,"Invalid");}catch(InvalidDataException){corrupt=true;}Check(corrupt&&File.ReadAllText(file)=="preserve corrupt store","corrupt store never overwritten");
        new NavigationStore(Path.Combine(scratch,"interop.fwb"),@"C:\interop.md").Add("markdown",700000,.25f,"Équations 日本語");
    }
    static void After(string scratch){var store=new NavigationStore(Path.Combine(scratch,"interop.fwb"),@"c:\INTEROP.md");var rows=store.Load("markdown");Check(rows.Count==2&&rows[1].page==1234567&&rows[1].title=="Native mark","managed reads native addition with unchanged checksum contract");
        string db=Path.Combine(scratch,"navigation.sqlite3"),marks=Path.Combine(scratch,"sqlite.fwb");Fixture(db);byte[] before=File.ReadAllBytes(db);var saved=new NavigationStore(marks,db);
        using(var parent=new Form{ShowInTaskbar=false,StartPosition=FormStartPosition.Manual,Location=new Point(-32000,-32000)}){parent.Show();using(var host=new DatabaseHost(parent.Handle,db,false,saved)){host.Show();var grid=Field<DataGridView>(host,"grid");var rail=Field<NativeNavigationRail>(host,"rowRail");Check(Wait(delegate{return Field<bool>(host,"rowsReady")&&grid.Rows.Count==50;}),"SQLite initial page ready");Check(grid.ScrollBars==ScrollBars.Horizontal,"SQLite native vertical scrollbar replaced");rail.Jump(20);uint at=rail.Position();rail.Toggle();Check(Wait(delegate{return saved.Load(rail.Context()).Count==1;}),"SQLite row mark saved");Check(saved.Load(rail.Context())[0].page==at,"SQLite records actual first visible row");
            rail.Jump(70);Check(Wait(delegate{return Field<bool>(host,"rowsReady")&&Field<int>(host,"page")==1;}),"SQLite bookmark navigation can load another page");rail.Jump(at);Check(Wait(delegate{return Field<bool>(host,"rowsReady")&&Field<int>(host,"page")==0&&rail.Position()==at;}),"SQLite row restored across pages");
            Field<TextBox>(host,"search").Text="item100";Field<Button>(host,"searchButton").PerformClick();Check(Wait(delegate{return Field<bool>(host,"rowsReady")&&grid.Rows.Count==1;}),"SQLite filter changes context");Check(saved.Load(rail.Context()).Count==0,"filtered query does not inherit row marks");var schemaRail=Field<NativeNavigationRail>(host,"schemaRail");schemaRail.Toggle();Check(Wait(delegate{return saved.Load(schemaRail.Context()).Count==1;}),"SQLite schema mark saved separately");host.Close();}parent.Close();}
        Check(Convert.ToBase64String(before)==Convert.ToBase64String(File.ReadAllBytes(db)),"database bytes unchanged by navigation and bookmarks");
    }
    [STAThread] static int Main(string[] args){try{if(args.Length!=2)return 2;Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);if(args[1]=="before")Before(args[0]);else After(args[0]);Console.WriteLine(count+" managed checks passed");return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
