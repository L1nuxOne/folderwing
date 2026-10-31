using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

// One connection per operation: no shared cursor or state crosses worker threads.
internal sealed class SqliteReader : IDisposable
{
    const int Ok=0, Row=100, Done=101, ReadOnly=1, PrivateCache=0x40000;
    const int MaxObjects=10000, MaxColumns=128, CellChars=2048;
    const int MaxValueBytes=2*1024*1024;
    const int Blob=4, Null=5;
    static readonly IntPtr Transient=new IntPtr(-1);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Progress(IntPtr context);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_open_v2(byte[] path,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close_v2(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_errmsg16(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_prepare_v2(IntPtr db,byte[] sql,int length,out IntPtr statement,IntPtr tail);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_step(IntPtr statement);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_finalize(IntPtr statement);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_stmt_readonly(IntPtr statement);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_count(IntPtr statement);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_column_name16(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_column_decltype16(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_type(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_column_text16(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_bytes16(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_bytes(IntPtr statement,int column);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl,CharSet=CharSet.Unicode)] static extern int sqlite3_bind_text16(IntPtr statement,int index,string value,int length,IntPtr destructor);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_busy_timeout(IntPtr db,int ms);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_db_readonly(IntPtr db,byte[] name);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_db_config(IntPtr db,int operation,int value,out int result);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_limit(IntPtr db,int category,int value);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern void sqlite3_progress_handler(IntPtr db,int instructions,Progress callback,IntPtr context);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFileW(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    IntPtr db;
    readonly Progress progress;
    readonly CancellationToken cancellation;
    readonly long deadline;
    public sealed class Table { public string Name, Type, Schema; public override string ToString(){return Name+(Type=="view"?"  (view)":"");} }
    public sealed class Catalog { public readonly List<Table> Tables=new List<Table>(); public bool Truncated; }
    public sealed class Column { public string Name, Type; }
    public sealed class Cell { public string Text; public bool IsNull, IsBlob, Truncated; }
    public sealed class Page { public readonly List<Column> Columns=new List<Column>(); public readonly List<Cell[]> Rows=new List<Cell[]>(); public bool HasMore; }

    static byte[] Utf8(string value){ return Encoding.UTF8.GetBytes(value+"\0"); }
    static string Ptr(IntPtr value){ return value==IntPtr.Zero?null:Marshal.PtrToStringUni(value); }
    static string Quote(string name){ return "\""+name.Replace("\"","\"\"")+"\""; }
    static bool IsNetwork(string path){return (path.StartsWith("\\\\",StringComparison.Ordinal) && !path.StartsWith("\\\\?\\",StringComparison.Ordinal)) || path.StartsWith("\\\\?\\UNC\\",StringComparison.OrdinalIgnoreCase);}
    static void VerifyHeader(string path,bool archived)
    {
        using (SafeFileHandle handle=CreateFileW(path,0x80000000,7,IntPtr.Zero,3,0,IntPtr.Zero)) {
            if(handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            using(var stream=new FileStream(handle,FileAccess.Read)) {
                byte[] header=new byte[20]; int count=0, n;
                while(count<header.Length && (n=stream.Read(header,count,header.Length-count))>0)count+=n;
                if(count<20 || Encoding.ASCII.GetString(header,0,16)!="SQLite format 3\0")
                    throw new InvalidDataException("This file is not a SQLite 3 database, or its header is incomplete.");
                bool wal=header[18]==2 || header[19]==2;
                if(archived && wal) throw new InvalidDataException("This archive entry uses SQLite WAL mode. The preview extracts only this database file, so committed data in a companion -wal file could be missing. Extract the database with its companion files and open it from a folder.");
                if(wal && IsNetwork(path)) throw new InvalidDataException("SQLite WAL databases require local shared-memory locking. Open this database from a local Windows drive.");
            }
        }
    }
    public SqliteReader(string path,bool archived,CancellationToken token)
    {
        cancellation=token; deadline=Stopwatch.GetTimestamp()+7*Stopwatch.Frequency; progress=CheckProgress;
        token.ThrowIfCancellationRequested(); VerifyHeader(path,archived);
        try {
            int rc=sqlite3_open_v2(Utf8(path),out db,ReadOnly|PrivateCache,IntPtr.Zero);
            if(rc!=Ok) throw Error("Could not open the database",rc);
            if(sqlite3_db_readonly(db,Utf8("main"))!=1) throw new InvalidOperationException("The database could not be verified as read-only.");
            int result;
            if(sqlite3_db_config(db,1010,1,out result)!=Ok || result!=1 ||
               sqlite3_db_config(db,1017,0,out result)!=Ok || result!=0)
                throw new InvalidOperationException("Could not enable SQLite's safe read-only configuration.");
            sqlite3_limit(db,0,MaxValueBytes); // Value/row size, including view-generated values.
            sqlite3_limit(db,1,200000);        // SQL text length.
            sqlite3_limit(db,7,0);             // No attached databases.
            sqlite3_busy_timeout(db,1000);
            sqlite3_progress_handler(db,10000,progress,IntPtr.Zero);
        } catch { Dispose(); throw; }
    }
    int CheckProgress(IntPtr unused) { return cancellation.IsCancellationRequested || Stopwatch.GetTimestamp()>deadline ? 1:0; }
    Exception Error(string label,int code)
    {
        if(cancellation.IsCancellationRequested)return new OperationCanceledException(cancellation);
        string detail=db==IntPtr.Zero?"SQLite error":Ptr(sqlite3_errmsg16(db));
        if(Stopwatch.GetTimestamp()>deadline) return new TimeoutException("This database query took too long for preview. Try a smaller filter or table.");
        return new InvalidOperationException(label+": "+(detail??"SQLite error")+" ("+code+").");
    }
    IntPtr Prepare(string sql)
    {
        IntPtr statement;int rc=sqlite3_prepare_v2(db,Utf8(sql),-1,out statement,IntPtr.Zero);
        if(rc!=Ok)throw Error("Could not read the database",rc);
        if(statement==IntPtr.Zero || sqlite3_stmt_readonly(statement)==0) {
            if(statement!=IntPtr.Zero)sqlite3_finalize(statement);
            throw new InvalidOperationException("The preview refused a statement that is not read-only.");
        }
        return statement;
    }
    bool Step(IntPtr statement)
    {
        int rc=sqlite3_step(statement);
        if(rc==Row)return true;
        if(rc==Done)return false;
        throw Error("Could not read the database",rc);
    }
    static string Text(IntPtr statement,int column,int max,out bool truncated)
    {
        IntPtr value=sqlite3_column_text16(statement,column); int length=sqlite3_column_bytes16(statement,column)/2;
        truncated=length>max; if(value==IntPtr.Zero)return "";
        return Marshal.PtrToStringUni(value,Math.Min(length,max));
    }
    public Catalog ReadCatalog()
    {
        var result=new Catalog();bool ignored, truncated;
        IntPtr statement=Prepare("SELECT name, type, coalesce(sql,'') FROM main.sqlite_schema WHERE type IN ('table','view') ORDER BY name COLLATE NOCASE LIMIT 10001");
        try {
            while(Step(statement)) {
                if(result.Tables.Count==MaxObjects){result.Truncated=true;break;}
                string name=Text(statement,0,4096,out truncated);
                if(truncated)throw new InvalidOperationException("A database object name exceeds the 4,096-character preview limit.");
                string type=Text(statement,1,16,out ignored), schema=Text(statement,2,32768,out truncated);
                if(truncated)schema+="\n… (statement shortened for preview)";
                result.Tables.Add(new Table {Name=name,Type=type,Schema=schema});
            }
        } finally { sqlite3_finalize(statement); }
        return result;
    }
    public Page ReadPage(Table table,long offset,int size,int sortColumn,bool descending,string filter)
    {
        if(table==null || offset<0 || offset>1000000000 || size<1 || size>100) throw new ArgumentException("Invalid page request.");
        if(filter==null)filter="";if(filter.Length>256)throw new ArgumentException("The search text is too long.");
        var result=new Page();
        string name="main."+Quote(table.Name);
        IntPtr metadata=Prepare("SELECT * FROM "+name+" LIMIT 0");
        try {
            int count=sqlite3_column_count(metadata);
            if(count>MaxColumns)throw new InvalidOperationException("This table has more than 128 columns; it is too wide for this preview.");
            for(int i=0;i<count;i++)result.Columns.Add(new Column{Name=Ptr(sqlite3_column_name16(metadata,i))??"",Type=Ptr(sqlite3_column_decltype16(metadata,i))??""});
        } finally {sqlite3_finalize(metadata);}
        if(sortColumn < -1 || sortColumn>=result.Columns.Count)throw new ArgumentException("Invalid sort column.");
        var sql=new StringBuilder("SELECT * FROM ");sql.Append(name);
        if(filter.Length>0 && result.Columns.Count>0) {
            sql.Append(" WHERE (");
            for(int i=0;i<result.Columns.Count;i++) {
                if(i>0)sql.Append(" OR ");string col=Quote(result.Columns[i].Name);
                sql.Append("(typeof(").Append(col).Append(") <> 'blob' AND instr(lower(CAST(").Append(col).Append(" AS TEXT)),lower(?1)) > 0)");
            }
            sql.Append(')');
        }
        if(sortColumn>=0)sql.Append(" ORDER BY ").Append(Quote(result.Columns[sortColumn].Name)).Append(descending?" DESC":" ASC");
        sql.Append(" LIMIT ").Append(size+1).Append(" OFFSET ").Append(offset);
        IntPtr statement=Prepare(sql.ToString());
        try {
            if(filter.Length>0 && result.Columns.Count>0) {
                int rc=sqlite3_bind_text16(statement,1,filter,-1,Transient);if(rc!=Ok)throw Error("Could not set the filter",rc);
            }
            while(Step(statement)) {
                if(result.Rows.Count==size){result.HasMore=true;break;}
                var row=new Cell[result.Columns.Count];
                for(int i=0;i<row.Length;i++) {
                    int type=sqlite3_column_type(statement,i);var cell=new Cell();
                    if(type==Null){cell.IsNull=true;cell.Text="NULL";}
                    else if(type==Blob){cell.IsBlob=true;cell.Text="[BLOB: "+sqlite3_column_bytes(statement,i)+" bytes]";}
                    else {bool truncated;cell.Text=Text(statement,i,CellChars,out truncated);cell.Truncated=truncated;if(truncated)cell.Text+=" …";}
                    row[i]=cell;
                }
                result.Rows.Add(row);
            }
        } finally {sqlite3_finalize(statement);}
        return result;
    }
    public void Dispose(){if(db!=IntPtr.Zero){sqlite3_progress_handler(db,0,null,IntPtr.Zero);sqlite3_close_v2(db);db=IntPtr.Zero;}}
}
