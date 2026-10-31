using System;
using System.IO;
using System.Collections.Generic;
using System.Linq;
using System.Threading;

internal sealed class LibrarySource
{
    public string Kind,Path,Name;
    public string Key {get{return Kind+"\u001f"+System.IO.Path.GetFullPath(Path).TrimEnd('\\').ToUpperInvariant();}}
    public string Database {get{return System.IO.Path.Combine(Path,Kind=="calibre"?"metadata.db":"zotero.sqlite");}}
}
internal sealed class LibrarySourceFailure {public LibrarySource Source;public string Error;}
internal sealed class LibraryAttachment
{
    public string StableKey="";public long Id;public string Name="",Format="",Path="",Status="";public long? Size;public bool CatalogSize;
}
internal sealed class LibraryItem
{
    public string StableKey="",SourceKey="",SourceName="",SourcePath="";public long Id;public string Title="",Creator="",Year="",Added="",Series="",SeriesIndex="",Type="",Publication="",Cover="";
    public readonly List<string> Tags=new List<string>();public readonly HashSet<long> Collections=new HashSet<long>();public readonly List<LibraryAttachment> Attachments=new List<LibraryAttachment>();
    public string SearchText {get{return Title+"\n"+Creator+"\n"+Year+"\n"+Publication+"\n"+Series+"\n"+String.Join("\n",Tags);}}
}
internal sealed class LibraryGroup {public long Id,Parent;public string Name,SourceName="",StableKey="";}
internal sealed class LibraryCatalog
{
    public readonly List<LibrarySource> Sources=new List<LibrarySource>();public readonly List<LibrarySourceFailure> Failures=new List<LibrarySourceFailure>();
    public readonly List<LibraryItem> Items=new List<LibraryItem>();public readonly List<LibraryGroup> Groups=new List<LibraryGroup>();public string Identity,Kind,Root;
    public static long Number(string value){long n;return Int64.TryParse(value,out n)?n:0;}
    public static string Within(string root,string relative){try{if(String.IsNullOrEmpty(relative)||System.IO.Path.IsPathRooted(relative)||relative.IndexOf(':')>=0)return "";string basis=System.IO.Path.GetFullPath(root).TrimEnd('\\')+"\\",path=System.IO.Path.GetFullPath(System.IO.Path.Combine(basis,relative.Replace('/','\\')));return path.StartsWith(basis,StringComparison.OrdinalIgnoreCase)?path:"";}catch{return "";}}
    public static bool LocalPath(string path){return path!=null&&path.Length>3&&Char.IsLetter(path[0])&&path[1]==':'&&path[2]=='\\'&&path.IndexOf('\0')<0;}
    public static LibraryCatalog Open(string root,string kind,CancellationToken token){var cat=new LibraryCatalog{Kind=kind,Root=root,Identity=root};using(var db=new CatalogDb(System.IO.Path.Combine(root,kind=="calibre"?"metadata.db":"zotero.sqlite"),token)){if(kind=="calibre")Calibre(cat,db);else if(kind=="zotero")Zotero(cat,db);else throw new IOException("Unknown library type.");}if(cat.Items.Count>100000)throw new IOException("Catalogue exceeds the 100,000-item v0 limit.");Stamp(cat,new LibrarySource{Kind=kind,Path=root,Name=System.IO.Path.GetFileName(root.TrimEnd('\\'))});return cat;}
    static void Stamp(LibraryCatalog cat,LibrarySource source)
    {
        cat.Sources.Clear();cat.Sources.Add(source);string basis=Digest(source.Key+"\u001f"+cat.Identity);
        foreach(var item in cat.Items){item.SourceKey=basis;item.SourceName=source.Name;item.SourcePath=source.Path;item.StableKey=basis+"\u001f"+item.Id;foreach(var a in item.Attachments)a.StableKey=basis+"\u001f"+a.Id;}
        foreach(var group in cat.Groups){group.SourceName=source.Name;group.StableKey="c:"+basis+"\u001f"+group.Id;}
    }
    static string Digest(string value){using(var hash=System.Security.Cryptography.SHA256.Create())return BitConverter.ToString(hash.ComputeHash(System.Text.Encoding.UTF8.GetBytes(value))).Replace("-","");}
    public static LibraryCatalog OpenSelection(IEnumerable<LibrarySource> selection,CancellationToken token)
    {
        var sources=selection.GroupBy(v=>v.Key,StringComparer.OrdinalIgnoreCase).Select(v=>v.First()).OrderBy(v=>v.Key,StringComparer.Ordinal).ToArray();
        if(sources.Length==0||sources.Length>64)throw new IOException("Select between 1 and 64 libraries.");
        if(sources.Length==1){var single=Open(sources[0].Path,sources[0].Kind,token);Stamp(single,sources[0]);return single;}
        var result=new LibraryCatalog{Kind="combined",Root=""};result.Sources.AddRange(sources);
        result.Identity="combined:"+Digest(String.Join("\u001e",sources.Select(v=>v.Key).ToArray()));
        long itemId=0,attachmentId=0,groupId=0;
        foreach(var source in sources){
            token.ThrowIfCancellationRequested();LibraryCatalog part;
            try{part=Open(source.Path,source.Kind,token);Stamp(part,source);}
            catch(OperationCanceledException){throw;}
            catch(Exception e){result.Failures.Add(new LibrarySourceFailure{Source=source,Error=e.Message});continue;}
            if(result.Items.Count+part.Items.Count>100000)throw new IOException("Combined view exceeds the 100,000-item limit. Select fewer libraries.");
            var groups=new Dictionary<long,long>();foreach(var g in part.Groups)groups[g.Id]=++groupId;
            foreach(var g in part.Groups){long parent;g.Id=groups[g.Id];g.Parent=groups.TryGetValue(g.Parent,out parent)?parent:0;result.Groups.Add(g);}
            foreach(var item in part.Items){
                item.Id=++itemId;var membership=item.Collections.Where(groups.ContainsKey).Select(id=>groups[id]).ToArray();item.Collections.Clear();item.Collections.UnionWith(membership);
                foreach(var a in item.Attachments)a.Id=++attachmentId;result.Items.Add(item);
            }
        }
        if(result.Failures.Count==sources.Length)throw new IOException("Could not open any selected library.\n"+String.Join("\n",result.Failures.Select(f=>f.Source.Name+": "+f.Error).ToArray()));
        return result;
    }
    static void Calibre(LibraryCatalog cat,CatalogDb db){
        db.Require("books","id","title","path","uuid","has_cover");db.Require("data","book","name","format","uncompressed_size");var items=new Dictionary<long,LibraryItem>();var paths=new Dictionary<long,string>();var authors=new Dictionary<long,List<string>>();
        db.Read("SELECT uuid FROM library_id LIMIT 1",delegate(string[] r){cat.Identity="calibre:"+r[0];});
        db.Read("SELECT id,title,author_sort,path,pubdate,timestamp,has_cover,series_index FROM books",delegate(string[] r){var b=new LibraryItem{Id=Number(r[0]),Title=r[1],Creator=r[2],Year=r[4].Length>=4?r[4].Substring(0,4):r[4],Added=r[5],Type="Book"};paths[b.Id]=r[3];if(r[6]=="1")b.Cover=Within(cat.Root,r[3]+"/cover.jpg");items[b.Id]=b;cat.Items.Add(b);});
        db.Read("SELECT l.book,a.name FROM books_authors_link l JOIN authors a ON a.id=l.author ORDER BY l.id",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[0]),out b)){if(!authors.ContainsKey(b.Id))authors[b.Id]=new List<string>();authors[b.Id].Add(r[1]);}});
        foreach(var b in cat.Items){List<string> a;if(authors.TryGetValue(b.Id,out a))b.Creator=String.Join("; ",a);}authors.Clear();
        db.Read("SELECT d.id,d.book,d.name,d.format,d.uncompressed_size FROM data d",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[1]),out b)){string f=r[3].ToUpperInvariant(),p=Within(cat.Root,paths[b.Id]+"/"+r[2]+"."+f.ToLowerInvariant());b.Attachments.Add(new LibraryAttachment{Id=Number(r[0]),Name=f,Format=f,Path=p,Status=p.Length==0?"Invalid stored path":"",Size=Math.Max(0,Number(r[4])),CatalogSize=true});}});
        db.Read("SELECT l.book,t.name FROM books_tags_link l JOIN tags t ON t.id=l.tag",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[0]),out b))b.Tags.Add(r[1]);});
        db.Read("SELECT l.book,s.name,b.series_index FROM books_series_link l JOIN series s ON s.id=l.series JOIN books b ON b.id=l.book",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[0]),out b)){b.Series=r[1];b.SeriesIndex=r[2];}});
    }
    static void Zotero(LibraryCatalog cat,CatalogDb db){
        db.Require("items","itemID","itemTypeID","libraryID","key");db.Require("itemAttachments","itemID","parentItemID","linkMode","path");var items=new Dictionary<long,LibraryItem>();var keys=new Dictionary<long,string>();var types=new Dictionary<long,string>();var deleted=new HashSet<long>();
        db.Read("SELECT itemID FROM deletedItems",delegate(string[] r){deleted.Add(Number(r[0]));});db.Read("SELECT itemTypeID,typeName FROM itemTypes",delegate(string[] r){types[Number(r[0])]=r[1];});
        db.Read("SELECT itemID,itemTypeID,dateAdded,key FROM items",delegate(string[] r){long id=Number(r[0]);if(deleted.Contains(id))return;string type;types.TryGetValue(Number(r[1]),out type);if(type=="note"||type=="annotation")return;items[id]=new LibraryItem{Id=id,Added=r[2],Type=type??"Reference"};keys[id]=r[3];});
        db.Read("SELECT d.itemID,f.fieldName,v.value FROM itemData d JOIN fields f ON f.fieldID=d.fieldID JOIN itemDataValues v ON v.valueID=d.valueID",delegate(string[] r){LibraryItem b;if(!items.TryGetValue(Number(r[0]),out b))return;if(r[1]=="title")b.Title=r[2];else if(r[1]=="date")b.Year=r[2];else if(r[1]=="publicationTitle"||r[1]=="bookTitle")b.Publication=r[2];});
        var people=new Dictionary<long,List<string>>();db.Read("SELECT l.itemID,c.firstName,c.lastName FROM itemCreators l JOIN creators c ON c.creatorID=l.creatorID ORDER BY l.itemID,l.orderIndex",delegate(string[] r){long id=Number(r[0]);if(!people.ContainsKey(id))people[id]=new List<string>();people[id].Add((r[1]+" "+r[2]).Trim());});foreach(var b in items.Values){List<string> names;if(people.TryGetValue(b.Id,out names))b.Creator=String.Join("; ",names);}
        var child=new HashSet<long>();db.Read("SELECT itemID,parentItemID,linkMode,contentType,path FROM itemAttachments",delegate(string[] r){long id=Number(r[0]),parent=Number(r[1]);if(deleted.Contains(id)||deleted.Contains(parent))return;LibraryItem b;long target=parent!=0?parent:id;if(!items.TryGetValue(target,out b))return;if(parent!=0)child.Add(id);string format=r[3]=="application/pdf"?"PDF":r[3]=="application/epub+zip"?"EPUB":System.IO.Path.GetExtension(r[4]).TrimStart('.').ToUpperInvariant();var a=new LibraryAttachment{Id=id,Name=items.ContainsKey(id)?items[id].Title:"Attachment",Format=format.Length>0?format:"Attachment"};if(r[2]=="0"||r[2]=="1"){string key;keys.TryGetValue(id,out key);a.Path=key!=null&&r[4].StartsWith("storage:",StringComparison.Ordinal)?Within(cat.Root,"storage/"+key+"/"+r[4].Substring(8)):"";a.Status=a.Path.Length==0?"Unresolved stored attachment":"";}else if(r[2]=="2"){if(LocalPath(r[4]))a.Path=System.IO.Path.GetFullPath(r[4]);else a.Status="Linked-file base directory unavailable";}else a.Status="URL-only attachment";b.Attachments.Add(a);});
        db.Read("SELECT l.itemID,t.name FROM itemTags l JOIN tags t ON t.tagID=l.tagID",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[0]),out b))b.Tags.Add(r[1]);});
        db.Read("SELECT collectionID,collectionName,parentCollectionID FROM collections",delegate(string[] r){cat.Groups.Add(new LibraryGroup{Id=Number(r[0]),Name=r[1],Parent=Number(r[2])});});
        db.Read("SELECT collectionID,itemID FROM collectionItems",delegate(string[] r){LibraryItem b;if(items.TryGetValue(Number(r[1]),out b))b.Collections.Add(Number(r[0]));});
        foreach(var b in items.Values)if(!child.Contains(b.Id)){if(String.IsNullOrWhiteSpace(b.Title))b.Title="Untitled "+b.Type;cat.Items.Add(b);}
    }
}
