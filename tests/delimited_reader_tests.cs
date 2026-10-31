using System;
using System.IO;
using System.Text;
class DelimitedReaderTests
{
    static int failed;
    static void Check(string name,bool ok){Console.WriteLine((ok?"ok ":"FAIL ")+name);if(!ok)failed++;}
    static int Main(){
        var p=new DelimitedReader(new StringReader("name,value,tail\r\n\"a,b\",\"a\"\"b\",\r\n\"multi\r\nline\",007,x\n"),',');
        var h=p.Read();var a=p.Read();var b=p.Read();
        Check("header",h.fields.Length==3 && h.fields[0]=="name");
        Check("quoted delimiter and escaped quote",a.error==null && a.fields[0]=="a,b" && a.fields[1]=="a\"b" && a.fields[2]=="");
        Check("quoted newline and exact numeric text",b.error==null && b.fields[0]=="multi\r\nline" && b.fields[1]=="007");
        Check("end without phantom record",p.Read()==null && p.Ended);
        p=new DelimitedReader(new StringReader("x\t\"y\tz\"\rnext\titem"),'\t');a=p.Read();b=p.Read();
        Check("tab delimiter and lone CR",a.fields[1]=="y\tz" && b.fields[0]=="next");
        p=new DelimitedReader(new StringReader("\"bad\"x,z\nvalid,1\n"),',');a=p.Read();b=p.Read();
        Check("malformed row is reported and following row remains available",a.error!=null && b.error==null && b.fields[0]=="valid");
        p=new DelimitedReader(new StringReader("\"unclosed"),',');a=p.Read();Check("unclosed quoted field",a.error!=null);
        p=new DelimitedReader(new StringReader(new string('x',1024*1024+1)+"\nnext\n"),',');a=p.Read();b=p.Read();
        Check("oversized row is bounded and next row remains available",a.error!=null && b.error==null && b.fields[0]=="next");
        p=new DelimitedReader(new StringReader(new string(',',4096)+"\nnext\n"),',');a=p.Read();b=p.Read();
        Check("column bound and recovery",a.error!=null && b.error==null);
        var many=new StringBuilder();for(int i=1;i<=70;i++)many.Append(i).Append(";value\n");
        p=new DelimitedReader(new StringReader(many.ToString()),';');bool pages=true;
        for(int i=1;i<=64;i++){var r=p.Read();pages &= r!=null && r.number==i && r.fields[0]==i.ToString();}
        for(int i=65;i<=70;i++){var r=p.Read();pages &= r!=null && r.number==i && r.fields[0]==i.ToString();}
        Check("semicolon records continue across a batch boundary",pages && p.Read()==null);
        var ansi=Encoding.GetEncoding(1252).GetBytes("name\ncafé\n");
        using(var r=new StreamReader(new MemoryStream(ansi),Encoding.GetEncoding(1252),true)){p=new DelimitedReader(r,',');p.Read();Check("Windows-1252 cell text",p.Read().fields[0]=="café");}
        var unicode=Encoding.Unicode.GetPreamble();var payload=Encoding.Unicode.GetBytes("name\n你好\n");var combined=new byte[unicode.Length+payload.Length];Array.Copy(unicode,combined,unicode.Length);Array.Copy(payload,0,combined,unicode.Length,payload.Length);
        using(var r=new StreamReader(new MemoryStream(combined),Encoding.UTF8,true)){p=new DelimitedReader(r,',');p.Read();Check("UTF-16 BOM detection",p.Read().fields[0]=="你好");}
        p=new DelimitedReader(new StringReader(""),',');Check("empty file",p.Read()==null);
        Console.WriteLine(failed==0?"Delimited reader checks passed.":"Delimited reader checks failed.");return failed==0?0:1;
    }
}
