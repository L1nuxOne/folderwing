using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

// Streaming CSV/TSV parser. Values remain strings; no spreadsheet type inference.
internal sealed class DelimitedReader
{
    const int MaxUnits = 1024 * 1024;
    const int MaxColumns = 4096;
    readonly TextReader input;
    readonly char delimiter;
    int number;
    bool ended;
    internal sealed class Row {
        public int number { get; set; }
        public string[] fields { get; set; }
        public string error { get; set; }
    }
    internal DelimitedReader(TextReader source, char separator) { input=source;delimiter=separator; }
    internal bool Ended { get { return ended; } }

    internal Row Read()
    {
        if (ended) return null;
        var fields=new List<string>();var cell=new StringBuilder();
        bool any=false, quoted=false, afterQuote=false, fieldStart=true, tooLong=false, tooMany=false;
        string error=null;int units=0;
        Action<char> append=delegate(char c) { if (++units<=MaxUnits) cell.Append(c); else tooLong=true; };
        Action field=delegate { if(fields.Count<MaxColumns)fields.Add(cell.ToString());else tooMany=true;cell.Length=0;fieldStart=true; };
        for (;;) {
            int raw=input.Read();
            if(raw<0) {
                ended=true;
                if(!any && fields.Count==0 && cell.Length==0) return null;
                if(quoted && error==null)error="Unclosed quoted field";
                field();break;
            }
            any=true;char c=(char)raw;
            if(quoted) {
                if(c=='"') {
                    if(input.Peek()=='"'){input.Read();append('"');}
                    else {quoted=false;afterQuote=true;}
                } else append(c);
                continue;
            }
            if(c=='\r'||c=='\n') {
                if(c=='\r' && input.Peek()=='\n')input.Read();
                field();break;
            }
            if(c==delimiter) {field();afterQuote=false;continue;}
            if(fieldStart && c=='"'){quoted=true;fieldStart=false;continue;}
            if(afterQuote && error==null)error="Unexpected character after a closing quote";
            if(c=='"' && error==null)error="Unexpected quote in an unquoted field";
            afterQuote=false;fieldStart=false;append(c);
        }
        if(tooLong)error="Row exceeds the 1,048,576 UTF-16-unit preview limit";
        if(tooMany)error="Row exceeds the 4,096-column preview limit";
        return new Row { number=++number, fields=error==null?fields.ToArray():null, error=error };
    }
}
