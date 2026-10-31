using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Web.Script.Serialization;
internal static class XmlPreviewReaderTests
{
    static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
    static XmlPreviewReader.Result Parse(byte[] bytes,int limit){using(var input=new MemoryStream(bytes))return XmlPreviewReader.Parse(input,limit);}
    static XmlPreviewReader.Result Parse(string text){return Parse(Encoding.UTF8.GetBytes(text),1024*1024);}
    static void Run()
    {
        string xml="<?xml version='1.0'?><!--before--><?note a?><r xmlns='urn:main' xmlns:x='urn:x' id='a &amp; b'>before<x:item a='1'/><![CDATA[<value>]]><!--inside--><x:item>after</x:item> tail</r><!--end-->";
        var result=Parse(xml);Check(result.error==null,"valid XML: "+result.error);
        var doc=result.root;Check(doc.children[0].type=="declaration" && doc.children[1].type=="comment" && doc.children[2].type=="processing" && doc.children[3].type=="element" && doc.children[4].type=="comment","document order");
        var root=doc.children[3];Check(root.name=="r" && root.local=="r" && root.ns=="urn:main","default namespace");
        Check(root.children[0].type=="namespace" && root.children[1].type=="namespace" && root.children[2].type=="attribute" && root.children[2].value=="a & b","attribute order and entities");
        Check(root.children[3].type=="text" && root.children[3].value=="before" && root.children[4].name=="x:item" && root.children[4].ns=="urn:x","first mixed content");
        Check(root.children[5].type=="cdata" && root.children[5].value=="<value>" && root.children[6].type=="comment" && root.children[7].type=="element" && root.children[8].value==" tail","mixed content order");
        Check(Parse("<r>  <x/>\n</r>").root.children[0].children.Any(x=>x.type=="whitespace"),"whitespace retained");
        Check(Parse("<r><![CDATA[<!DOCTYPE harmless>]]><!-- <!DOCTYPE harmless> --></r>").error==null,"doctype spelling inside literal content");
        Check(Parse("<r><x></r>").error!=null,"malformed XML accepted");
        Check(Parse("<!DOCTYPE r [<!ENTITY x SYSTEM 'file:///C:/secret'>]><r>&x;</r>").error!=null,"external entity accepted");
        Check(Parse("<!DOCTYPE r [<!ENTITY x 'internal'>]><r>&x;</r>").error!=null,"internal DTD accepted");
        var latin=Encoding.GetEncoding(1252).GetBytes("<?xml version='1.0' encoding='windows-1252'?><r>café</r>");
        var encoded=Parse(latin,1024);Check(encoded.error==null && encoded.root.children.Last().children[0].value=="café","XML declared encoding");
        var wide=Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes("<?xml version='1.0' encoding='UTF-16'?><r>漢字</r>")).ToArray();
        encoded=Parse(wide,1024);Check(encoded.error==null && encoded.root.children.Last().children[0].value=="漢字","UTF-16 BOM");
        byte[] small=Encoding.UTF8.GetBytes("<root/>");Check(Parse(small,small.Length).error==null && Parse(small,small.Length-1).error!=null,"byte limit boundary");
        var depth=new StringBuilder();for(int i=0;i<128;i++)depth.Append("<x>");for(int i=0;i<128;i++)depth.Append("</x>");var deep=Parse(depth.ToString());Check(deep.error==null,"128-level depth boundary");
        var serializer=new JavaScriptSerializer{RecursionLimit=512,MaxJsonLength=64*1024*1024};Check(serializer.Serialize(deep).Contains("\"kind\":\"xml\""),"bounded deep model serialization");
        depth.Insert(0,"<x>");depth.Append("</x>");Check(Parse(depth.ToString()).error.Contains("Nesting"),"depth limit");
        var many=new StringBuilder("<r>");for(int i=0;i<50001;i++)many.Append("<x/>");many.Append("</r>");Check(Parse(many.ToString()).error.Contains("50,000"),"node limit");
        var hugeNs=new string('u',10000);var repeated=new StringBuilder("<r xmlns='").Append(hugeNs).Append("'>");for(int i=0;i<1800;i++)repeated.Append("<x/>");repeated.Append("</r>");Check(Parse(repeated.ToString()).error.Contains("data limit"),"expanded namespace budget");
        Console.WriteLine("XML reader passed: ordered content, namespaces, encodings, prohibited DTDs and bounded inputs.");
    }
    static int Main(){try{Run();return 0;}catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
