using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Diagnostics;

// Catalogue SQL is fixed in the adapters. No document or UI supplies SQL.
internal sealed class CatalogDb : IDisposable
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int Progress(IntPtr p);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_open_v2(byte[] file,out IntPtr db,int flags,IntPtr vfs);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_close_v2(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_prepare_v2(IntPtr db,byte[] sql,int n,out IntPtr stmt,IntPtr tail);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_step(IntPtr stmt);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_finalize(IntPtr stmt);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_count(IntPtr stmt);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_column_text16(IntPtr stmt,int at);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_column_bytes16(IntPtr stmt,int at);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern IntPtr sqlite3_errmsg16(IntPtr db);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_db_readonly(IntPtr db,byte[] name);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_stmt_readonly(IntPtr stmt);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_db_config(IntPtr db,int op,int value,out int result);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_busy_timeout(IntPtr db,int ms);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern int sqlite3_limit(IntPtr db,int id,int value);
    [DllImport("folderwing-sqlite.dll",CallingConvention=CallingConvention.Cdecl)] static extern void sqlite3_progress_handler(IntPtr db,int n,Progress fn,IntPtr p);
    IntPtr db;readonly Progress progress;readonly CancellationToken token;readonly long deadline;long units;
    static byte[] Utf8(string value){return Encoding.UTF8.GetBytes(value+"\0");}
    public CatalogDb(string file,CancellationToken cancellation){token=cancellation;deadline=Stopwatch.GetTimestamp()+30*Stopwatch.Frequency;progress=delegate(IntPtr p){return token.IsCancellationRequested||Stopwatch.GetTimestamp()>deadline?1:0;};
        try{if(sqlite3_open_v2(Utf8(file),out db,1|0x40000,IntPtr.Zero)!=0)throw Error();if(sqlite3_db_readonly(db,Utf8("main"))!=1)throw new IOException("The catalogue could not be opened read-only.");int result;if(sqlite3_db_config(db,1010,1,out result)!=0||result!=1||sqlite3_db_config(db,1017,0,out result)!=0||result!=0)throw new IOException("Read-only catalogue configuration failed.");sqlite3_limit(db,0,2*1024*1024);sqlite3_limit(db,7,0);sqlite3_busy_timeout(db,750);sqlite3_progress_handler(db,10000,progress,IntPtr.Zero);Exec("BEGIN");}catch{Dispose();throw;}}
    Exception Error(){token.ThrowIfCancellationRequested();return new IOException("Could not read catalogue: "+(db==IntPtr.Zero?"open failed":Marshal.PtrToStringUni(sqlite3_errmsg16(db))));}
    public void Exec(string sql){Read(sql,delegate(string[] row){});}
    public void Read(string sql,Action<string[]> receive){token.ThrowIfCancellationRequested();IntPtr stmt;int rc=sqlite3_prepare_v2(db,Utf8(sql),-1,out stmt,IntPtr.Zero);if(rc!=0)throw Error();try{if(stmt==IntPtr.Zero||sqlite3_stmt_readonly(stmt)==0)throw new IOException("Catalogue query was not read-only.");int n=sqlite3_column_count(stmt),count=0;while((rc=sqlite3_step(stmt))==100){token.ThrowIfCancellationRequested();if(++count>1000000)throw new IOException("Catalogue relation exceeds the v0 display limit.");var values=new string[n];for(int i=0;i<n;i++){int bytes=sqlite3_column_bytes16(stmt,i);if(bytes>65536)throw new IOException("Catalogue field exceeds the v0 display limit.");units+=bytes;if(units>128*1024*1024)throw new IOException("Catalogue exceeds the v0 memory limit.");IntPtr text=sqlite3_column_text16(stmt,i);values[i]=text==IntPtr.Zero?"":Marshal.PtrToStringUni(text,bytes/2);}receive(values);}if(rc!=101)throw Error();}finally{if(stmt!=IntPtr.Zero)sqlite3_finalize(stmt);}}
    public void Require(string table,params string[] columns){var names=new HashSet<string>();Read("PRAGMA table_info(\""+table+"\")",delegate(string[] row){names.Add(row[1]);});foreach(string col in columns)if(!names.Contains(col))throw new IOException("Unsupported catalogue schema: "+table+"."+col+" is missing.");}
    public void Dispose(){if(db!=IntPtr.Zero){sqlite3_close_v2(db);db=IntPtr.Zero;}}
}
