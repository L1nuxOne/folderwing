using System;
using System.IO;
using System.IO.Compression;
using System.Xml;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.ComponentModel;
using Microsoft.Win32.SafeHandles;

// Both readers consume this immutable, bounded snapshot, never the live file independently.
internal sealed class WorkbookPackage : IDisposable
{
    internal sealed class Sheet { public string name {get;set;} public string state {get;set;} internal string part; }
    internal readonly List<Sheet> Sheets = new List<Sheet>();
    internal readonly byte[] Bytes;
    readonly MemoryStream input;
    readonly ZipArchive zip;
    readonly Dictionary<string,ZipArchiveEntry> entries = new Dictionary<string,ZipArchiveEntry>(StringComparer.OrdinalIgnoreCase);
    const long PartLimit=64L*1024*1024, TotalLimit=256L*1024*1024;
    const int StringLimit=250000;
    [StructLayout(LayoutKind.Sequential)] struct FileInfo { public uint Attributes, CreationLow, CreationHigh, AccessLow, AccessHigh, WriteLow, WriteHigh, Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFileW(string name,uint access,uint share,IntPtr security,uint creation,uint flags,IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool GetFileInformationByHandle(SafeFileHandle h,out FileInfo info);
    static bool Same(FileInfo a,FileInfo b) { return a.Volume==b.Volume && a.IndexHigh==b.IndexHigh && a.IndexLow==b.IndexLow && a.WriteLow==b.WriteLow && a.WriteHigh==b.WriteHigh && a.SizeLow==b.SizeLow && a.SizeHigh==b.SizeHigh; }
    internal static byte[] Snapshot(string path) {
        using(var handle=CreateFileW(path,0x80000000,7,IntPtr.Zero,3,0x08000000,IntPtr.Zero)) {
            if(handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            FileInfo before,after; bool got=GetFileInformationByHandle(handle,out before);
            using(var stream=new FileStream(handle,FileAccess.Read)) {
                long len=stream.Length;
                if(len<1 || len>128L*1024*1024) throw new InvalidDataException("Workbook exceeds the 128 MiB preview limit or is empty.");
                var bytes=new byte[(int)len]; int n=0,k;
                while(n<bytes.Length && (k=stream.Read(bytes,n,bytes.Length-n))>0)n+=k;
                if(n!=bytes.Length || stream.ReadByte()!=-1 || (got && GetFileInformationByHandle(handle,out after) && !Same(before,after)))
                    throw new IOException("Workbook changed while opening. Select it again to retry.");
                return bytes;
            }
        }
    }
    internal static XmlReader Xml(Stream s) { return XmlReader.Create(s,new XmlReaderSettings { DtdProcessing=DtdProcessing.Prohibit, XmlResolver=null, IgnoreComments=true, IgnoreWhitespace=true, MaxCharactersInDocument=PartLimit }); }
    internal WorkbookPackage(byte[] bytes) {
        Bytes=bytes; input=new MemoryStream(bytes,false); zip=new ZipArchive(input,ZipArchiveMode.Read);
        try {
            if(zip.Entries.Count>4096)throw new InvalidDataException("Too many workbook parts.");
            long total=0;
            foreach(var e in zip.Entries) {
                string name=e.FullName.Replace('\\','/');
                if(name.StartsWith("/",StringComparison.Ordinal) || name.Contains("../") || name.Contains(":") || entries.ContainsKey(name)) throw new InvalidDataException("Invalid or duplicate workbook part.");
                entries.Add(name,e);
                if(name.EndsWith(".xml",StringComparison.OrdinalIgnoreCase)||name.EndsWith(".rels",StringComparison.OrdinalIgnoreCase)) {
                    if(e.Length>PartLimit || (total+=e.Length)>TotalLimit)throw new InvalidDataException("Workbook XML exceeds preview limits.");
                    long actual=0; var buffer=new byte[32768]; using(var s=e.Open()){ int n; while((n=s.Read(buffer,0,buffer.Length))>0) {actual+=n;if(actual>PartLimit || actual>e.Length)throw new InvalidDataException("Workbook part is larger than declared.");} }
                    if(actual!=e.Length)throw new InvalidDataException("Incomplete workbook part.");
                }
            }
            string workbook="xl/workbook.xml";
            ZipArchiveEntry root;
            if(entries.TryGetValue("_rels/.rels",out root)) using(var x=Xml(root.Open())) while(x.Read()) if(x.NodeType==XmlNodeType.Element && x.LocalName=="Relationship" && RelationshipType(x.GetAttribute("Type"),"officeDocument")) { if(String.Equals(x.GetAttribute("TargetMode"),"External",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("External workbook relationship is unsupported."); workbook=x.GetAttribute("Target"); break; }
            // Match ExcelDataReader's root relationship semantics and reject ambiguous paths.
            // The library falls back to xl/workbook.xml when the root target is absent.
            // Keep exactly the same selection so values and formulas never cross workbooks.
            if(workbook==null || !entries.ContainsKey(workbook))workbook="xl/workbook.xml";
            workbook=PathPart("",workbook,false);
            if(!entries.ContainsKey(workbook)) throw new InvalidDataException("Workbook XML was not found; encrypted or unsupported workbook.");
            int slash=workbook.LastIndexOf('/'); string dir=slash<0?"":workbook.Substring(0,slash+1); string baseName=workbook.Substring(slash+1);
            var relations=new Dictionary<string,string>(StringComparer.Ordinal); string shared=null;
            ZipArchiveEntry rel;
            if(entries.TryGetValue(dir+"_rels/"+baseName+".rels",out rel)) using(var x=Xml(rel.Open())) while(x.Read()) if(x.NodeType==XmlNodeType.Element && x.LocalName=="Relationship") {
                string id=x.GetAttribute("Id"),type=x.GetAttribute("Type")??"",target=x.GetAttribute("Target");
                if(id==null || target==null)continue;
                if(RelationshipType(type,"worksheet")||RelationshipType(type,"sharedStrings")||RelationshipType(type,"styles")) {
                    if(String.Equals(x.GetAttribute("TargetMode"),"External",StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("External workbook part is unsupported.");
                    string part=PathPart(dir,target,true);
                    if(RelationshipType(type,"worksheet")) {if(relations.ContainsKey(id))throw new InvalidDataException("Duplicate worksheet relationship.");relations[id]=part;}
                    if(RelationshipType(type,"sharedStrings"))shared=part;
                }
            }
            if(shared!=null && entries.ContainsKey(shared)) CheckStrings(entries[shared]);
            using(var x=Xml(entries[workbook].Open())) {
                string ns=null; bool inSheets=false;
                while(x.Read()) {
                    if(x.NodeType==XmlNodeType.Element && x.Depth==0) {
                        ns=x.NamespaceURI;
                        if(x.LocalName!="workbook" || (ns!="http://schemas.openxmlformats.org/spreadsheetml/2006/main" && ns!="http://purl.oclc.org/ooxml/spreadsheetml/main"))throw new InvalidDataException("Unsupported workbook XML namespace.");
                    }
                    if(x.NodeType==XmlNodeType.Element && x.Depth==1 && x.LocalName=="sheets" && x.NamespaceURI==ns) {inSheets=!x.IsEmptyElement;continue;}
                    if(x.NodeType==XmlNodeType.EndElement && x.Depth==1 && x.LocalName=="sheets") {inSheets=false;continue;}
                    if(!inSheets || x.NodeType!=XmlNodeType.Element || x.Depth!=2 || x.LocalName!="sheet" || x.NamespaceURI!=ns)continue;
                    string relationNs=ns=="http://purl.oclc.org/ooxml/spreadsheetml/main" ? "http://purl.oclc.org/ooxml/officeDocument/relationships" : "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
                    string id=x.GetAttribute("id",relationNs),part;
                    relations.TryGetValue(id??"",out part);
                    var sheet=new Sheet { name=x.GetAttribute("name")??"", state=x.GetAttribute("state")??"visible", part=part };
                    if(sheet.name.Length>256 || Sheets.Count>=512)throw new InvalidDataException("Too many sheets or sheet name too long.");
                    Sheets.Add(sheet);
                }
            }
            if(Sheets.Count==0)throw new InvalidDataException("No worksheets found in workbook.");
        } catch { Dispose(); throw; }
    }
    static bool RelationshipType(string type,string kind) {
        return type=="http://schemas.openxmlformats.org/officeDocument/2006/relationships/"+kind ||
               type=="http://purl.oclc.org/ooxml/officeDocument/relationships/"+kind;
    }
    static string PathPart(string dir,string target,bool relative) {
        if(String.IsNullOrEmpty(target)||target.Contains("\\")||target.Contains(":")||target.Contains("?")||target.Contains("#")||target.Contains("%")||target.Contains("../")||target.StartsWith("./",StringComparison.Ordinal)) throw new InvalidDataException("Unsupported workbook relationship path.");
        if(target.StartsWith("/",StringComparison.Ordinal))return target.Substring(1);
        return relative ? dir+target : target;
    }
    static void CheckStrings(ZipArchiveEntry entry) {
        int count=0; long chars=0;
        using(var x=Xml(entry.Open())) while(x.Read()) {
            if(x.NodeType==XmlNodeType.Element && x.LocalName=="sst") { string declared=x.GetAttribute("uniqueCount"); int n; if(declared!=null && (!Int32.TryParse(declared,out n)||n<0||n>StringLimit))throw new InvalidDataException("Shared string count exceeds preview limit."); }
            if(x.NodeType==XmlNodeType.Element && x.LocalName=="si" && ++count>StringLimit)throw new InvalidDataException("Too many shared strings for preview.");
            if(x.NodeType==XmlNodeType.Text || x.NodeType==XmlNodeType.CDATA) {chars+=x.Value.Length;if(chars>32L*1024*1024 || x.Value.Length>1024*1024)throw new InvalidDataException("Shared strings exceed preview limit.");}
        }
    }
    internal Stream OpenSheet(int index) { var sheet=Sheets[index]; ZipArchiveEntry entry; if(sheet.part==null || !entries.TryGetValue(sheet.part,out entry))throw new InvalidDataException("This sheet type is not supported by the preview."); return entry.Open(); }
    public void Dispose() { if(zip!=null)zip.Dispose(); if(input!=null)input.Dispose(); }
}
