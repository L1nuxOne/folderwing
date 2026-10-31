using System;
using System.Collections.Generic;
using System.IO;
using System.Xml;

// Parses only a bounded copy of the selected file. No DTD, external entity,
// stylesheet or other network/file resolution is permitted.
internal static class XmlPreviewReader
{
    const int MaxNodes=50000, MaxDepth=128, MaxModelCharacters=16*1024*1024;
    public sealed class Item
    {
        public string type, name, local, ns, value;
        public List<Item> children;
    }
    public sealed class Result
    {
        public string kind="xml", error;
        public Item root;
        public int nodes;
    }
    static Item Add(Item parent,string type,string name,string local,string ns,string value,ref int count,ref long characters,bool container)
    {
        if(++count>MaxNodes)throw new InvalidOperationException("Document exceeds the 50,000-node structured preview limit.");
        characters+=(name==null?0:name.Length)+(local==null?0:local.Length)+(ns==null?0:ns.Length)+(value==null?0:value.Length);
        if(characters>MaxModelCharacters)throw new InvalidOperationException("Document exceeds the structured preview data limit.");
        var item=new Item{type=type,name=name,local=local,ns=ns,value=value,children=container?new List<Item>():null};
        parent.children.Add(item);return item;
    }
    public static Result Parse(Stream input,int byteLimit)
    {
        var result=new Result();
        try {
            using(var bytes=new MemoryStream()) {
                byte[] chunk=new byte[16384];int read;
                while((read=input.Read(chunk,0,Math.Min(chunk.Length,byteLimit+1-(int)bytes.Length)))>0) {
                    bytes.Write(chunk,0,read);
                    if(bytes.Length>byteLimit)throw new InvalidOperationException("Document exceeds the configured text preview size limit.");
                }
                if(bytes.Length==byteLimit && input.ReadByte()!=-1)throw new InvalidOperationException("Document exceeds the configured text preview size limit.");
                bytes.Position=0;
                var settings=new XmlReaderSettings{DtdProcessing=DtdProcessing.Prohibit,XmlResolver=null,IgnoreWhitespace=false,IgnoreComments=false,IgnoreProcessingInstructions=false,MaxCharactersInDocument=byteLimit,MaxCharactersFromEntities=4096,CloseInput=false};
                var document=new Item{type="document",name="Document",children=new List<Item>()};int count=1;long characters=0;
                var stack=new Stack<Item>();stack.Push(document);
                using(var reader=XmlReader.Create(bytes,settings)) {
                    while(reader.Read()) {
                        if(reader.Depth>=MaxDepth)throw new InvalidOperationException("Nesting exceeds the 128-level structured preview limit.");
                        Item parent=stack.Peek();
                        switch(reader.NodeType) {
                        case XmlNodeType.Element:
                            var element=Add(parent,"element",reader.Name,reader.LocalName,reader.NamespaceURI,null,ref count,ref characters,true);
                            if(reader.HasAttributes) {
                                while(reader.MoveToNextAttribute()) {
                                    bool declaration=reader.Name=="xmlns" || reader.Prefix=="xmlns";
                                    Add(element,declaration?"namespace":"attribute",reader.Name,reader.LocalName,reader.NamespaceURI,reader.Value,ref count,ref characters,false);
                                }
                                reader.MoveToElement();
                            }
                            if(!reader.IsEmptyElement)stack.Push(element);
                            break;
                        case XmlNodeType.EndElement:
                            if(stack.Count>1)stack.Pop();break;
                        case XmlNodeType.Text: Add(parent,"text",null,null,null,reader.Value,ref count,ref characters,false);break;
                        case XmlNodeType.CDATA: Add(parent,"cdata",null,null,null,reader.Value,ref count,ref characters,false);break;
                        case XmlNodeType.Whitespace:
                        case XmlNodeType.SignificantWhitespace: Add(parent,"whitespace",null,null,null,reader.Value,ref count,ref characters,false);break;
                        case XmlNodeType.Comment: Add(parent,"comment",null,null,null,reader.Value,ref count,ref characters,false);break;
                        case XmlNodeType.ProcessingInstruction: Add(parent,"processing",reader.Name,null,null,reader.Value,ref count,ref characters,false);break;
                        case XmlNodeType.XmlDeclaration: Add(parent,"declaration",reader.Name,null,null,reader.Value,ref count,ref characters,false);break;
                        default: throw new InvalidOperationException("Unsupported XML node type: "+reader.NodeType+".");
                        }
                    }
                }
                result.root=document;result.nodes=count;
            }
        } catch(XmlException ex) {result.error=ex.Message;}
          catch(InvalidOperationException ex) {result.error=ex.Message;}
        return result;
    }
}
