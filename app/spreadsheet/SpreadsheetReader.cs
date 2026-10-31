using System;
using System.IO;
using System.Collections.Generic;
using System.Globalization;
using ExcelDataReader;
using ExcelNumberFormat;

internal sealed class SpreadsheetReader : IDisposable
{
    internal sealed class Cell { public int column {get;set;} public string display {get;set;} public string raw {get;set;} public string formula {get;set;} public string formulaKind {get;set;} public string note {get;set;} public bool missing {get;set;} public bool truncated {get;set;} }
    internal sealed class Row { public int number {get;set;} public List<Cell> cells {get;set;} public bool omitted {get;set;} }
    internal sealed class Batch { public string kind {get;set;} public List<Row> rows {get;set;} public bool end {get;set;} public int columns {get;set;} public bool columnsLimited {get;set;} }
    internal readonly WorkbookPackage Package;
    IExcelDataReader reader; FormulaCursor formulas; int row; bool ended; int columns; bool columnsLimited;
    readonly Dictionary<string,NumberFormat> formats=new Dictionary<string,NumberFormat>();
    internal SpreadsheetReader(byte[] bytes) { Package=new WorkbookPackage(bytes); }
    internal void Select(int index) {
        if(index<0 || index>=Package.Sheets.Count)throw new InvalidDataException("Unknown worksheet.");
        if(reader!=null)reader.Dispose();if(formulas!=null)formulas.Dispose();reader=null;formulas=null;
        // Refuse unsupported sheet types explicitly; never silently pair the wrong part.
        var stream=Package.OpenSheet(index);
        try {
            formulas=new FormulaCursor(stream);
            reader=ExcelReaderFactory.CreateOpenXmlReader(new MemoryStream(Package.Bytes,false),new ExcelReaderConfiguration {SinglePassMode=true});
            for(int i=0;i<index;i++)if(!reader.NextResult())throw new InvalidDataException("Worksheet is unavailable.");
            if(reader.Name!=Package.Sheets[index].name)throw new InvalidDataException("Worksheet metadata does not match the reader.");
            row=0;ended=false;columns=0;columnsLimited=false;formats.Clear();
        }catch{if(reader!=null)reader.Dispose();if(formulas!=null)formulas.Dispose();reader=null;formulas=null;throw;}
    }
    static string Limit(string value,out bool clipped) { value=value??"";clipped=value.Length>8192;return clipped?value.Substring(0,8192):value; }
    static string Raw(object v) {if(v==null || v==DBNull.Value)return "";if(v is bool)return (bool)v?"TRUE":"FALSE";if(v is DateTime)return ((DateTime)v).ToString("o",CultureInfo.InvariantCulture);if(v is double)return ((double)v).ToString("R",CultureInfo.InvariantCulture);return Convert.ToString(v,CultureInfo.InvariantCulture);}
    static string Error(CellError err) {switch(err){case CellError.NULL:return "#NULL!";case CellError.DIV0:return "#DIV/0!";case CellError.VALUE:return "#VALUE!";case CellError.REF:return "#REF!";case CellError.NAME:return "#NAME?";case CellError.NUM:return "#NUM!";case CellError.NA:return "#N/A";case CellError.GETTING_DATA:return "#GETTING_DATA";default:return "#ERROR!";}}
    internal Batch Next() {
        if(reader==null)throw new InvalidOperationException("Select a worksheet first.");
        var batch=new Batch {kind="rows",rows=new List<Row>()};int units=0;
        while(!ended && batch.rows.Count<64 && units<1024*1024) {
            var overlay=formulas.Take(row+1);
            if(!reader.Read()) {ended=true;break;}
            row++;if(reader.Depth+1!=row)throw new InvalidDataException("Worksheet readers lost row alignment.");
            int count=reader.FieldCount;columns=Math.Max(columns,Math.Min(count,4096));if(count>4096)columnsLimited=true;
            var item=new Row {number=row,cells=new List<Cell>()};
            for(int i=0;i<Math.Min(count,4096);i++) {
                object value=reader.GetValue(i);var error=reader.GetCellError(i);FormulaCursor.Formula f; overlay.TryGetValue(i,out f);
                if(value==null && !error.HasValue && f==null)continue;
                string raw=error.HasValue?Error(error.Value):Raw(value); string display=raw;
                if(value is double || value is DateTime || value is TimeSpan) {
                    string spec=reader.GetNumberFormatString(i);
                    if(!String.IsNullOrEmpty(spec) && spec.Length<=512)try {NumberFormat format;if(!formats.TryGetValue(spec,out format)){format=new NumberFormat(spec);if(formats.Count<2048)formats.Add(spec,format);}display=format.Format(value,CultureInfo.CurrentCulture)??raw;}catch { display=raw; }
                }
                bool rawClip,displayClip;raw=Limit(raw,out rawClip);display=Limit(display,out displayClip);
                if(f!=null && f.missing)display="[No saved result]";
                var cell=new Cell {column=i,display=display,raw=raw,formula=f==null?null:f.text,formulaKind=f==null?null:f.kind,note=f==null?null:f.note,missing=f!=null && f.missing,truncated=rawClip||displayClip||(f!=null&&f.truncated)};
                item.cells.Add(cell);units+=display.Length+raw.Length+(cell.formula??"").Length;
                if(units>=1024*1024) { item.omitted=i+1<count; break; }
            }
            // A formula without a cached value may be absent from the value reader's width.
            foreach(var pair in overlay)if(pair.Key>=count && units<1024*1024) {var f=pair.Value;item.cells.Add(new Cell {column=pair.Key,display=f.missing?"[No saved result]":"",raw="",formula=f.text,formulaKind=f.kind,note=f.note,missing=f.missing,truncated=f.truncated});columns=Math.Max(columns,pair.Key+1);units+=(f.text??"").Length;}
            batch.rows.Add(item);
        }
        batch.end=ended;batch.columns=columns;batch.columnsLimited=columnsLimited;return batch;
    }
    public void Dispose(){if(reader!=null)reader.Dispose();if(formulas!=null)formulas.Dispose();Package.Dispose();}
}
