using System;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Linq;
internal static class WordPackageTests {
 static void Check(bool ok,string why){if(!ok)throw new Exception(why);}
 static byte[] Variant(byte[] input,Func<string,byte[],byte[]> edit,string extra,byte[] extraData){using(var output=new MemoryStream()){using(var src=new ZipArchive(new MemoryStream(input),ZipArchiveMode.Read))using(var dst=new ZipArchive(output,ZipArchiveMode.Create,true)){foreach(var e in src.Entries){var n=dst.CreateEntry(e.FullName);using(var a=e.Open())using(var m=new MemoryStream()){a.CopyTo(m);var b=edit(e.FullName,m.ToArray());using(var w=n.Open())w.Write(b,0,b.Length);}}if(extra!=null){var n=dst.CreateEntry(extra);using(var w=n.Open())w.Write(extraData,0,extraData.Length);}}return output.ToArray();}}
 static byte[] MoveImage(byte[] input){using(var output=new MemoryStream()){using(var src=new ZipArchive(new MemoryStream(input),ZipArchiveMode.Read))using(var dst=new ZipArchive(output,ZipArchiveMode.Create,true)){foreach(var e in src.Entries){string name=e.FullName=="word/media/pixel.png"?"pictures/pixel.png":e.FullName;var n=dst.CreateEntry(name);using(var a=e.Open())using(var m=new MemoryStream()){a.CopyTo(m);var b=m.ToArray();if(name=="word/_rels/document.xml.rels")b=Replace(b,"Target=\"media/pixel.png\"","Target=\"../pictures/pixel.png\"");using(var w=n.Open())w.Write(b,0,b.Length);}}}return output.ToArray();}}
 static byte[] Replace(byte[] b,string a,string z){return Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(b).Replace(a,z));}
 static void Reject(byte[] input,string name){bool failed=false;try{WordPackage.Prepare(input);}catch(Exception){failed=true;}Check(failed,"Expected rejection: "+name);}
 static int Main(string[] args){try{
  byte[] original=File.ReadAllBytes(args[0]);var good=WordPackage.Prepare(original);Check(good.Notices.Any(x=>x.Contains("External")),"external relationship notice");
  using(var zip=new ZipArchive(new MemoryStream(good.Bytes),ZipArchiveMode.Read)){Check(zip.GetEntry("word/media/pixel.png")!=null,"embedded image kept");using(var sr=new StreamReader(zip.GetEntry("word/_rels/document.xml.rels").Open()))Check(!sr.ReadToEnd().Contains("javascript:"),"external relationship removed");using(var sr=new StreamReader(zip.GetEntry("word/document.xml").Open()))Check(sr.ReadToEnd().Contains("oMath"),"native math retained");}
  var moved=WordPackage.Prepare(MoveImage(original));using(var zip=new ZipArchive(new MemoryStream(moved.Bytes),ZipArchiveMode.Read))Check(zip.GetEntry("pictures/pixel.png")!=null,"relocated embedded image retained");
  byte[] macro=Variant(original,(n,b)=>b,"word/vbaProject.bin",Encoding.UTF8.GetBytes("INERT"));var safe=WordPackage.Prepare(macro);Check(safe.Notices.Any(x=>x.Contains("Macros")),"macro notice");using(var zip=new ZipArchive(new MemoryStream(safe.Bytes),ZipArchiveMode.Read))Check(zip.GetEntry("word/vbaProject.bin")==null,"macro removed");
  byte[] unusual=Variant(original,(n,b)=>n=="word/document.xml"?Replace(b,"<w:sectPr/>","<w:p><m:oMath><m:unsupported><m:r><m:t>x</m:t></m:r></m:unsupported></m:oMath></w:p><w:sectPr/>"):b,null,null);Check(WordPackage.Prepare(unusual).Notices.Any(x=>x.Contains("equations")),"unsupported equation notice");
  byte[] dtd=Variant(original,(n,b)=>n=="word/document.xml"?Replace(b,"<w:document ","<!DOCTYPE w:document [<!ENTITY x 'bad'>]><w:document "):b,null,null);Reject(dtd,"DTD");
  byte[] duplicate=Variant(original,(n,b)=>b,"WORD/DOCUMENT.XML",Encoding.UTF8.GetBytes("x"));Reject(duplicate,"case duplicate");
  byte[] traversal=Variant(original,(n,b)=>b,"../escape.xml",Encoding.UTF8.GetBytes("<x/>"));Reject(traversal,"traversal");
  byte[] oversized=Variant(original,(n,b)=>b,"word/oversized.xml",new byte[33*1024*1024]);Reject(oversized,"inflated size");
  string path=args[1];File.WriteAllBytes(path,original);using(var hold=File.Open(path,FileMode.Open,FileAccess.Read,FileShare.Read)){byte[] snap=WordPackage.Snapshot(@"\\?\"+path);Check(snap.Length==original.Length,"locked extended path snapshot");}Check(File.ReadAllBytes(path).SequenceEqual(original),"source unchanged");
  Console.WriteLine("Word package snapshot, relationships, omission, and bounded XML checks passed.");return 0;
 }catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
