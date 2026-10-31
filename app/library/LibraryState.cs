using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using System.Runtime.InteropServices;
internal sealed class LibraryAttachmentChoice {public long Item,Attachment;public string ItemKey="",AttachmentKey="";}
internal sealed class LibraryViewState
{
 public string Search="",Format="All formats",Group="All items",GroupKey="",SelectedKey="";public int Sort,Top,DetailWidth=300;public bool Descending,Details=true,ReaderPinned,TitleFixed;public long Selected;public int[] Columns;public List<LibraryAttachmentChoice> Attachments=new List<LibraryAttachmentChoice>();
}
internal sealed class LibraryStates
{
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern bool MoveFileExW(string from,string to,uint flags);
 readonly string file;readonly Dictionary<string,LibraryViewState> states;readonly JavaScriptSerializer json=new JavaScriptSerializer{MaxJsonLength=256*1024};
 public LibraryStates(string path){file=path;states=new Dictionary<string,LibraryViewState>(StringComparer.OrdinalIgnoreCase);if(File.Exists(path)){string text=File.ReadAllText(path);if(text.Length>256*1024)throw new IOException("Library view state exceeds its limit.");var saved=json.Deserialize<Dictionary<string,LibraryViewState>>(text);if(saved!=null)foreach(var pair in saved)if(pair.Value!=null&&states.Count<64)states[pair.Key]=pair.Value;}}
 public LibraryViewState Get(string key){LibraryViewState state;if(!states.TryGetValue(key,out state)){state=new LibraryViewState();if(states.Count<64)states[key]=state;}return state;}
 public void Save(){string directory=Path.GetDirectoryName(file);Directory.CreateDirectory(directory);string temp=file+"."+Guid.NewGuid().ToString("N")+".tmp";try{byte[] bytes=Encoding.UTF8.GetBytes(json.Serialize(states));if(bytes.Length>256*1024)throw new IOException("Library view state exceeds its limit.");using(var f=new FileStream(temp,FileMode.CreateNew,FileAccess.Write,FileShare.None)){f.Write(bytes,0,bytes.Length);f.Flush(true);}if(!MoveFileExW(temp,file,9))throw new IOException("Library view state could not be saved.");}finally{if(File.Exists(temp))File.Delete(temp);}}
}
