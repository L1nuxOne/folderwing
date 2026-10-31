using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

// All paths and databases are created by sqlite_preview_tests.ps1 in its private directory.
internal static class SqlitePreviewTests
{
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_open_v2(byte[] path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close_v2(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_exec(IntPtr db,byte[] sql,IntPtr callback,IntPtr context,out IntPtr error);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern void sqlite3_free(IntPtr value);
    static byte[] Utf8(string value){return Encoding.UTF8.GetBytes(value+"\0");}
    static void Check(bool value,string message){if(!value)throw new Exception(message);}
    static void Exec(IntPtr db,string sql)
    {
        IntPtr error;int rc=sqlite3_exec(db,Utf8(sql),IntPtr.Zero,IntPtr.Zero,out error);
        if(rc!=0){string msg=error==IntPtr.Zero?"":Marshal.PtrToStringAnsi(error);if(error!=IntPtr.Zero)sqlite3_free(error);throw new Exception("Fixture SQL failed: "+msg+" ("+rc+")");}
    }
    static IntPtr Open(string file,int flags){IntPtr db;int rc=sqlite3_open_v2(Utf8(file),out db,flags,IntPtr.Zero);Check(rc==0,"fixture open "+rc);return db;}
    static string Hash(string path){using(var h=SHA256.Create())using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete))return Convert.ToBase64String(h.ComputeHash(f));}
    static SqliteReader.Table Find(SqliteReader.Catalog catalog,string name){return catalog.Tables.Single(t=>t.Name==name);}
    static void Reject(Action action,string contains)
    {
        try{action();throw new Exception("Expected rejection: "+contains);}catch(InvalidDataException e){Check(e.Message.Contains(contains),"Incorrect error: "+e.Message);}
    }
    static void Run(string root)
    {
        string file=Path.Combine(root,"quoted.sqlite3"),bad=Path.Combine(root,"bad.db"),wal=Path.Combine(root,"live.sqlite");
        IntPtr writer=Open(file,2|4);
        try {
            Exec(writer,"CREATE TABLE \"a\"\" quoted\"(id INTEGER, name TEXT, b BLOB, missing TEXT); CREATE VIEW \"sample view\" AS SELECT id,name FROM \"a\"\" quoted\";");
            Exec(writer,"BEGIN");
            for(int i=0;i<101;i++)Exec(writer,"INSERT INTO \"a\"\" quoted\" VALUES("+i+",'row "+i+"',x'0102',NULL)");
            Exec(writer,"INSERT INTO \"a\"\" quoted\" VALUES(9223372036854775807,'literal '' OR 1=1 -- café',x'',NULL); COMMIT;");
        } finally {sqlite3_close_v2(writer);}
        string before=Hash(file);
        using(var reader=new SqliteReader(file,false,CancellationToken.None)) {
            var catalog=reader.ReadCatalog();var table=Find(catalog,"a\" quoted");Check(Find(catalog,"sample view").Type=="view","view not listed");
            var first=reader.ReadPage(table,0,50,0,false,"");Check(first.Rows.Count==50 && first.HasMore,"first page");
            Check(first.Columns.Count==4 && first.Columns[0].Name=="id" && first.Columns[0].Type=="INTEGER","column schema");
            Check(first.Rows[0][0].Text=="0" && first.Rows[0][2].IsBlob && first.Rows[0][2].Text.Contains("2 bytes") && first.Rows[0][3].IsNull,"cell types");
            var last=reader.ReadPage(table,100,50,0,false,"");Check(last.Rows.Count==2 && !last.HasMore,"last page");
            Check(last.Rows[1][0].Text=="9223372036854775807","integer precision");
            var descending=reader.ReadPage(table,0,50,0,true,"");Check(descending.Rows[0][0].Text=="9223372036854775807","descending sort");
            var filter=reader.ReadPage(table,0,50,-1,false,"' OR 1=1 --");Check(filter.Rows.Count==1 && filter.Rows[0][1].Text.Contains("café"),"literal filter or Unicode");
            Check(reader.ReadPage(table,0,50,-1,false,"ROW 1").Rows.Count==12,"case insensitive search");
            var view=reader.ReadPage(Find(catalog,"sample view"),0,50,-1,false,"");Check(view.Columns.Count==2 && view.HasMore,"view rows");
        }
        Check(before==Hash(file),"read-only browsing changed the database");
        using(var locked=File.Open(file,FileMode.Open,FileAccess.Read,FileShare.Read))
        using(var reader=new SqliteReader(@"\\?\"+file,true,CancellationToken.None))Check(reader.ReadCatalog().Tables.Count>=2,"archive path or shared lock");
        File.WriteAllText(bad,"not a database");Reject(delegate{using(var reader=new SqliteReader(bad,false,CancellationToken.None)){}},"not a SQLite");
        IntPtr active=Open(wal,2|4);
        try {
            Exec(active,"PRAGMA journal_mode=WAL; PRAGMA wal_autocheckpoint=0; CREATE TABLE live(value TEXT); INSERT INTO live VALUES('committed in WAL');");
            Check(File.Exists(wal+"-wal") && File.Exists(wal+"-shm"),"WAL fixture not active");
            string walBefore=Hash(wal+"-wal"),dbBefore=Hash(wal);
            using(var reader=new SqliteReader(wal,false,CancellationToken.None)) {
                var table=Find(reader.ReadCatalog(),"live");var result=reader.ReadPage(table,0,50,-1,false,"");
                Check(result.Rows.Count==1 && result.Rows[0][0].Text=="committed in WAL","committed WAL data missing");
            }
            Check(dbBefore==Hash(wal) && walBefore==Hash(wal+"-wal"),"read-only browsing changed the main database or WAL");
            Reject(delegate{using(var reader=new SqliteReader(wal,true,CancellationToken.None)){}},"archive entry uses SQLite WAL mode");
        } finally {sqlite3_close_v2(active);}
        using(var reader=new SqliteReader(wal,false,CancellationToken.None))Check(Find(reader.ReadCatalog(),"live")!=null,"closed WAL mode database");
        Console.WriteLine("SQLite preview checks passed: tables, views, paging, sorting, literal search, types, read-only files, extended archive paths and live WAL.");
    }
    static int Main(string[] args){try{if(args.Length!=1)return 2;Run(args[0]);return 0;}catch(Exception ex){Console.Error.WriteLine(ex);return 1;}}
}
