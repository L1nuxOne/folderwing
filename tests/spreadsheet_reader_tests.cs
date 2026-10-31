using System;
using System.IO;
using System.Linq;
internal static class SpreadsheetReaderTests {
 static void Check(bool ok,string message){if(!ok)throw new Exception(message);}
 static SpreadsheetReader.Cell C(SpreadsheetReader.Row r,int column){return r.cells.FirstOrDefault(x=>x.column==column);}
 static void Reject(string p,bool read){bool failed=false;try{using(var w=new SpreadsheetReader(File.ReadAllBytes(p))){if(read){w.Select(0);w.Next();}}}catch(Exception){failed=true;}Check(failed,"Expected rejection: "+p);}
 static int Main(string[] args){try{
  System.Threading.Thread.CurrentThread.CurrentCulture=System.Globalization.CultureInfo.GetCultureInfo("en-US");
  string dir=args[0];
  foreach(string file in new[]{"base.xlsx","macro.xlsm","moved.xlsx","spoofed.xlsx"})using(var w=new SpreadsheetReader(File.ReadAllBytes(Path.Combine(dir,file)))){
   Check(w.Package.Sheets.Count==3,"sheet count");Check(w.Package.Sheets[1].state=="hidden" && w.Package.Sheets[2].state=="veryHidden","hidden states");
   w.Select(0);var b=w.Next();Check(b.rows.Count==3 && b.end,"three rows");var r=b.rows[1];
   Check(C(r,0).display=="café 漢字","unicode");Check(C(r,1).raw=="42","number");Check(C(r,2).display.Contains("12.50"),"money formatting");Check(C(r,3).display.Contains("2025") && C(r,3).display.Contains("02"),"date formatting");
   Check(C(r,4).formula=="B2+10" && C(r,4).raw=="52" && !C(r,4).missing,"saved formula");Check(C(r,5).display=="#DIV/0!","formula error");Check(C(r,6).raw=="TRUE","boolean");
   Check(C(b.rows[2],4).missing && C(b.rows[2],4).formula=="B3+100","missing saved formula");
   w.Select(1);Check(C(w.Next().rows[0],0).display=="not public","hidden sheet values");
  }
  using(var w=new SpreadsheetReader(File.ReadAllBytes(Path.Combine(dir,"shared.xlsx")))){w.Select(0);var b=w.Next();Check(b.rows.Count==6,"sparse row count");Check(b.rows[3].cells.Count==0,"empty gap");Check(C(b.rows[4],0).formula=="B5+1","shared template");var f=C(b.rows[5],0);Check(f.formula=="" && f.note.Contains("A5") && f.note.Contains("not expanded"),"shared follower");Check(!C(b.rows[5],1).missing,"empty cached string");Check(C(b.rows[5],2).note.Contains("data-table") && C(b.rows[5],2).formulaKind=="dataTable","data-table formula label");}
  using(var w=new SpreadsheetReader(File.ReadAllBytes(Path.Combine(dir,"long.xlsx")))){w.Select(0);var first=w.Next();var second=w.Next();var third=w.Next();Check(first.rows.Count==64 && !first.end && second.rows.Count==64 && third.rows.Count==12 && third.end,"incremental batches");Check(C(second.rows[5],0).formula=="A70+1" && C(third.rows[11],0).formula=="A140+1","formula alignment across batches");}
  Reject(Path.Combine(dir,"inflated.xlsx"),false);Reject(Path.Combine(dir,"external.xlsx"),false);Reject(Path.Combine(dir,"dtd.xlsx"),true);Reject(Path.Combine(dir,"bad-column.xlsx"),true);
  var source=Path.Combine(dir,"base.xlsx");byte[] frozen=WorkbookPackage.Snapshot(source);File.WriteAllText(source,"changed");using(var w=new SpreadsheetReader(frozen)){w.Select(0);Check(C(w.Next().rows[1],4).formula=="B2+10","snapshot independent of source");}
  Console.WriteLine("Spreadsheet reader, formulas, limits and snapshot checks passed.");return 0;
 }catch(Exception e){Console.Error.WriteLine(e);return 1;}}
}
