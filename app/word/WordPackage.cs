using System;
using System.IO;
using System.IO.Compression;
using System.Xml;
using System.Xml.Linq;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

// Only this bounded, sanitized snapshot is sent to the isolated browser.
internal static class WordPackage
{
    internal sealed class Result { public byte[] Bytes; public List<string> Notices=new List<string>(); }
    const long FileLimit=32L*1024*1024, PartLimit=32L*1024*1024, TotalLimit=128L*1024*1024;
    [StructLayout(LayoutKind.Sequential)] struct FileInfo { public uint Attributes,CreationLow,CreationHigh,AccessLow,AccessHigh,WriteLow,WriteHigh,Volume,SizeHigh,SizeLow,Links,IndexHigh,IndexLow; }
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern SafeFileHandle CreateFileW(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll",SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle h,out FileInfo info);
    static bool Same(FileInfo a,FileInfo b){return a.Volume==b.Volume && a.IndexHigh==b.IndexHigh && a.IndexLow==b.IndexLow && a.WriteLow==b.WriteLow && a.WriteHigh==b.WriteHigh && a.SizeLow==b.SizeLow && a.SizeHigh==b.SizeHigh;}
    internal static byte[] Snapshot(string path) {
        using(var h=CreateFileW(path,0x80000000,7,IntPtr.Zero,3,0x08000000,IntPtr.Zero)) {
            if(h.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error());
            FileInfo before,after;bool got=GetFileInformationByHandle(h,out before);
            using(var input=new FileStream(h,FileAccess.Read)) {
                long length=input.Length;if(length<1 || length>FileLimit)throw new InvalidDataException("Document is empty or exceeds the 32 MiB preview limit.");
                var data=new byte[(int)length];int used=0,n;while(used<data.Length&&(n=input.Read(data,used,data.Length-used))>0)used+=n;
                if(used!=data.Length || input.ReadByte()!=-1 || (got && GetFileInformationByHandle(h,out after) && !Same(before,after)))throw new IOException("Document changed while opening. Select it again to retry.");
                return data;
            }
        }
    }
    static XmlReaderSettings Settings(){return new XmlReaderSettings {DtdProcessing=DtdProcessing.Prohibit,XmlResolver=null,MaxCharactersInDocument=PartLimit,IgnoreComments=true};}
    static readonly HashSet<string> SupportedMath=new HashSet<string>(new[]{"oMath","f","func","fName","num","den","rad","deg","e","sSup","sSub","sSubSup","sPre","sup","sub","d","nary","eqArr","lim","limLow","limUpp","m","mr","box","borderBox","phant","bar","groupChr","acc"},StringComparer.Ordinal);
    const string MathNamespace="http://schemas.openxmlformats.org/officeDocument/2006/math";
    static bool IsXml(string name){return name.EndsWith(".xml",StringComparison.OrdinalIgnoreCase)||name.EndsWith(".rels",StringComparison.OrdinalIgnoreCase);}
    static bool IsImage(string n){return n.EndsWith(".png",StringComparison.OrdinalIgnoreCase)||n.EndsWith(".jpg",StringComparison.OrdinalIgnoreCase)||n.EndsWith(".jpeg",StringComparison.OrdinalIgnoreCase)||n.EndsWith(".gif",StringComparison.OrdinalIgnoreCase)||n.EndsWith(".bmp",StringComparison.OrdinalIgnoreCase)||n.EndsWith(".webp",StringComparison.OrdinalIgnoreCase);}
    static bool ValidName(string n){if(String.IsNullOrEmpty(n)||n.StartsWith("/",StringComparison.Ordinal)||n.Contains("\\")||n.Contains(":")||n.Contains("//"))return false;foreach(var p in n.Split('/'))if(p=="."||p=="..")return false;return true;}
    static byte[] Read(ZipArchiveEntry entry) {
        if(entry.Length<0||entry.Length>PartLimit)throw new InvalidDataException("A document part exceeds the preview limit.");
        using(var s=entry.Open())using(var output=new MemoryStream()) {var buf=new byte[32768];long count=0;int n;while((n=s.Read(buf,0,buf.Length))>0){count+=n;if(count>PartLimit||count>entry.Length)throw new InvalidDataException("A document part is larger than declared.");output.Write(buf,0,n);}if(count!=entry.Length)throw new InvalidDataException("A document part is incomplete.");return output.ToArray();}
    }
    static byte[] SafeRelationships(byte[] bytes,ref bool omitted) {
        XDocument x;using(var input=new MemoryStream(bytes,false))using(var xml=XmlReader.Create(input,Settings()))x=XDocument.Load(xml);
        var remove=new List<XElement>();
        foreach(var e in x.Descendants())if(e.Name.LocalName=="Relationship") {
            string mode=(string)e.Attribute("TargetMode"),target=(string)e.Attribute("Target"),type=(string)e.Attribute("Type")??"";Uri absolute;
            if(String.IsNullOrEmpty(target)||String.Equals(mode,"External",StringComparison.OrdinalIgnoreCase)||target.StartsWith("//",StringComparison.Ordinal)||target.Contains("\\")||(!target.StartsWith("/",StringComparison.Ordinal)&&Uri.TryCreate(target,UriKind.Absolute,out absolute))||type.EndsWith("/vbaProject",StringComparison.OrdinalIgnoreCase)||type.EndsWith("/oleObject",StringComparison.OrdinalIgnoreCase)||type.EndsWith("/aFChunk",StringComparison.OrdinalIgnoreCase)||type.EndsWith("/font",StringComparison.OrdinalIgnoreCase)) {remove.Add(e);omitted=true;}
        }
        foreach(var e in remove)e.Remove();
        using(var output=new MemoryStream()){using(var xml=XmlWriter.Create(output,new XmlWriterSettings {Encoding=new System.Text.UTF8Encoding(false),CloseOutput=false}))x.Save(xml);return output.ToArray();}
    }
    internal static Result Prepare(byte[] data) {
        var result=new Result();bool external=false,unsupported=false,tracked=false,unsupportedMath=false;int xmlNodes=0;
        using(var input=new MemoryStream(data,false))using(var zip=new ZipArchive(input,ZipArchiveMode.Read))using(var output=new MemoryStream()) {
            if(zip.Entries.Count>4096)throw new InvalidDataException("Document has too many parts.");
            var names=new HashSet<string>(StringComparer.OrdinalIgnoreCase);long total=0;int images=0;bool contentTypes=false,rootRels=false;
            using(var clean=new ZipArchive(output,ZipArchiveMode.Create,true))foreach(var e in zip.Entries) {
                string name=e.FullName;if(!ValidName(name)||!names.Add(name))throw new InvalidDataException("Invalid or duplicate document part.");
                if(name.EndsWith("/",StringComparison.Ordinal))continue;
                if(e.Length>PartLimit || (total+=e.Length)>TotalLimit)throw new InvalidDataException("Document parts exceed preview limits.");
                byte[] part=Read(e);bool xmlPart=IsXml(name);bool media=IsImage(name);
                if((name.StartsWith("word/media/",StringComparison.OrdinalIgnoreCase)&&!media) || (media && ++images>256)){unsupported=true;continue;}
                bool excluded=name.StartsWith("word/embeddings/",StringComparison.OrdinalIgnoreCase)||name.StartsWith("word/activeX/",StringComparison.OrdinalIgnoreCase)||name.StartsWith("word/fonts/",StringComparison.OrdinalIgnoreCase)||name.EndsWith("vbaProject.bin",StringComparison.OrdinalIgnoreCase)||(!xmlPart&&!media);
                if(excluded){unsupported=true;continue;}
                if(xmlPart){
                    using(var ms=new MemoryStream(part,false))using(var reader=XmlReader.Create(ms,Settings())) {
                        int mathDepth=-1,ignoredDepth=-1;
                        while(reader.Read()) {
                            if(reader.Depth>256)throw new InvalidDataException("Document XML nesting exceeds the preview limit.");
                            if(reader.NodeType==XmlNodeType.EndElement) {if(reader.Depth==ignoredDepth)ignoredDepth=-1;if(reader.Depth==mathDepth)mathDepth=-1;continue;}
                            if(reader.NodeType!=XmlNodeType.Element)continue;
                            if(++xmlNodes>500000)throw new InvalidDataException("Document has too many XML elements for preview.");
                            if(reader.LocalName=="altChunk"||reader.LocalName=="object"||reader.LocalName=="control")unsupported=true;
                            if(reader.LocalName=="ins"||reader.LocalName=="del")tracked=true;
                            if(reader.NamespaceURI==MathNamespace) {
                                if(reader.LocalName=="oMath") {if(!reader.IsEmptyElement)mathDepth=reader.Depth;}
                                else if(mathDepth>=0 && ignoredDepth<0) {
                                    if(reader.LocalName=="r" || reader.LocalName.EndsWith("Pr",StringComparison.Ordinal)) {if(!reader.IsEmptyElement)ignoredDepth=reader.Depth;}
                                    else if(!SupportedMath.Contains(reader.LocalName))unsupportedMath=true;
                                }
                            }
                        }
                    }
                    if(name.EndsWith(".rels",StringComparison.OrdinalIgnoreCase))part=SafeRelationships(part,ref external);
                }
                if(name=="[Content_Types].xml")contentTypes=true;if(name=="_rels/.rels")rootRels=true;
                var target=clean.CreateEntry(name,CompressionLevel.Fastest);using(var s=target.Open())s.Write(part,0,part.Length);
            }
            if(!contentTypes||!rootRels)throw new InvalidDataException("This is not a supported Word document, or it is encrypted.");
            if(output.Length>FileLimit)throw new InvalidDataException("Prepared document exceeds the preview limit.");
            result.Bytes=output.ToArray();
        }
        if(external)result.Notices.Add("External links and linked content are not opened in the preview.");
        if(unsupported)result.Notices.Add("Macros, embedded objects or unsupported media were omitted.");
        if(tracked)result.Notices.Add("Tracked changes may not be fully represented.");
        if(unsupportedMath)result.Notices.Add("Some Word equations contain unsupported structures and may be incomplete.");
        return result;
    }
}
