using System;
using System.IO;
using System.Xml;
using System.Text;
using System.Collections.Generic;

// Streams the worksheet in lockstep with ExcelDataReader. A pending sparse row
// stays pending while the value reader emits intervening empty rows.
internal sealed class FormulaCursor : IDisposable
{
    internal sealed class Formula { public string text,kind,note; public bool missing,truncated; }
    readonly XmlReader xml;
    readonly Dictionary<string,string> shared = new Dictionary<string,string>();
    Dictionary<int,Formula> pending;
    int pendingRow,lastRow; bool inside,ended; string worksheetNamespace;
    internal FormulaCursor(Stream stream) { xml=WorkbookPackage.Xml(stream); }
    static int Column(string address,out int row) {
        row=0; if(String.IsNullOrEmpty(address))return 0; int i=0,col=0;
        while(i<address.Length && address[i]>='A' && address[i]<='Z') { col=col*26+address[i]-'A'+1; if(col>16384)throw new InvalidDataException("Worksheet column exceeds Excel limits."); i++; }
        if(i==0 || !Int32.TryParse(address.Substring(i),out row) || row<1 || row>1048576)throw new InvalidDataException("Invalid worksheet cell reference.");
        return col;
    }
    static string ReadText(XmlReader r,out bool clipped) {
        var text=new StringBuilder(); clipped=false;
        using(var sub=r.ReadSubtree())while(sub.Read())if(sub.NodeType==XmlNodeType.Text || sub.NodeType==XmlNodeType.CDATA) {
            int count=Math.Min(8192-text.Length,sub.Value.Length); if(count>0)text.Append(sub.Value,0,count); if(count<sub.Value.Length)clipped=true;
        }
        return text.ToString();
    }
    void ReadRow() {
        pending=null;
        while(!ended && xml.Read()) {
            if(xml.NodeType==XmlNodeType.Element && xml.Depth==0) {
                worksheetNamespace=xml.NamespaceURI;
                if(xml.LocalName!="worksheet" || (worksheetNamespace!="http://schemas.openxmlformats.org/spreadsheetml/2006/main" && worksheetNamespace!="http://purl.oclc.org/ooxml/spreadsheetml/main"))throw new InvalidDataException("Unsupported worksheet XML namespace.");
            }
            if(xml.NodeType==XmlNodeType.Element && xml.Depth==1 && xml.LocalName=="sheetData" && xml.NamespaceURI==worksheetNamespace) {inside=!xml.IsEmptyElement;continue;}
            if(xml.NodeType==XmlNodeType.EndElement && xml.Depth==1 && xml.LocalName=="sheetData" && xml.NamespaceURI==worksheetNamespace) {ended=true;break;}
            if(!inside || xml.NodeType!=XmlNodeType.Element || xml.Depth!=2 || xml.LocalName!="row")continue;
            int n; string attr=xml.GetAttribute("r");
            if(attr==null)n=lastRow+1; else if(!Int32.TryParse(attr,out n))throw new InvalidDataException("Invalid row number.");
            if(n<=lastRow || n>1048576)throw new InvalidDataException("Invalid or unordered worksheet rows.");
            lastRow=pendingRow=n; pending=new Dictionary<int,Formula>(); int previous=0;
            using(var row=xml.ReadSubtree())while(row.Read())if(row.NodeType==XmlNodeType.Element && row.Depth==1 && row.LocalName=="c") {
                string address=row.GetAttribute("r"),type=row.GetAttribute("t"); int r=0, col=address==null?previous+1:Column(address,out r);
                if(address!=null && r!=n)throw new InvalidDataException("Cell and row references disagree.");
                if(col<=previous || col>16384)throw new InvalidDataException("Invalid or unordered worksheet columns."); previous=col;
                bool hasFormula=false,hasValue=false,valueNonempty=false,formulaTruncated=false; string formula=null,kind=null,si=null;
                using(var cell=row.ReadSubtree())while(cell.Read()) {
                    if(cell.NodeType!=XmlNodeType.Element || cell.Depth!=1)continue;
                    if(cell.LocalName=="f") {hasFormula=true;kind=cell.GetAttribute("t")??"normal";si=cell.GetAttribute("si");formula=ReadText(cell,out formulaTruncated);}
                    else if(cell.LocalName=="v") {hasValue=true;bool ignored;string value=ReadText(cell,out ignored);valueNonempty=value.Length!=0;}
                }
                if(hasFormula) {
                    string note=null;
                    if(kind=="shared") {
                        if(si==null)note="Shared formula index is missing.";
                        else if(!String.IsNullOrEmpty(formula)) { if(shared.Count>=65536 && !shared.ContainsKey(si))throw new InvalidDataException("Too many shared formulas."); shared[si]=(address??("column "+col+", row "+n)); note="Shared formula template."; }
                        else { string anchor; note=shared.TryGetValue(si,out anchor) ? "Shared formula; template at "+anchor+". Relative references are not expanded." : "Shared formula; template unavailable. Relative references are not expanded."; }
                    } else if(kind=="array")note="Array formula anchor; saved results may cover additional cells.";
                    else if(kind=="dataTable")note="Excel data-table formula; formula text is not available.";
                    else if(kind!="normal")note="Formula type: "+kind+".";
                    if(col<=4096)pending[col-1]=new Formula {text=formula,kind=kind,note=note,missing=!hasValue || (!valueNonempty && type!="str"),truncated=formulaTruncated};
                }
            }
            return;
        }
        ended=true;
    }
    internal Dictionary<int,Formula> Take(int number) {
        if(pending==null && !ended)ReadRow();
        if(pending!=null && pendingRow<number)throw new InvalidDataException("Worksheet readers lost row alignment.");
        if(pending!=null && pendingRow==number) {var result=pending;pending=null;return result;}
        return new Dictionary<int,Formula>();
    }
    public void Dispose(){xml.Dispose();}
}
