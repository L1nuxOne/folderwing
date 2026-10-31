using System;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using System.Threading.Tasks;
using Microsoft.Web.WebView2.Core;

internal sealed class BrowserNavigation
{
    readonly NavigationStore store;
    readonly JavaScriptSerializer json=new JavaScriptSerializer{MaxJsonLength=8192,RecursionLimit=16};
    bool busy;
    public BrowserNavigation(NavigationStore source){store=source;}
    public static bool Dispatch(CoreWebView2 core,int message){
        if(message!=0x8000+90&&message!=0x8000+91)return false;
        if(core!=null)core.ExecuteScriptAsync(message==0x8000+90?"window.FolderwingReadingRail && window.FolderwingReadingRail.toggle()":"window.FolderwingReadingRail && window.FolderwingReadingRail.openMenu(true,35)");
        return true;
    }
    public async Task<bool> Handle(CoreWebView2 core,string message){
        if(!message.StartsWith("fw-nav:",StringComparison.Ordinal))return false;
        if(message.Length>8192)return true;
        Dictionary<string,object> request=null;string scope="",token="";bool owned=false;
        try {
            request=json.Deserialize<Dictionary<string,object>>(message.Substring(7));
            object value;scope=request.TryGetValue("scope",out value)?value as string:"";token=request.TryGetValue("token",out value)?value as string:"";
            if(scope==null||scope.Length>1024||token==null||token.Length>80)return true;
            string op=request.TryGetValue("op",out value)?value as string:null;if(op!="list"&&op!="add"&&op!="remove")return true;
            if(busy){core.PostWebMessageAsJson(new JavaScriptSerializer().Serialize(new {kind="fw-nav",scope=scope,token=token,error="Bookmark operation in progress. Try again."}));return true;}
            busy=true;owned=true;
            uint page=0;float y=0;string title=null,id=null;
            if(op=="add"){
                if(!request.TryGetValue("page",out value)||!UInt32.TryParse(Convert.ToString(value,System.Globalization.CultureInfo.InvariantCulture),out page))return true;
                if(!request.TryGetValue("y",out value)||!Single.TryParse(Convert.ToString(value,System.Globalization.CultureInfo.InvariantCulture),System.Globalization.NumberStyles.Float,System.Globalization.CultureInfo.InvariantCulture,out y)||Single.IsInfinity(y)||Single.IsNaN(y))return true;
                title=request.TryGetValue("title",out value)?value as string:null;if(title==null||title.Length>159)return true;
            }else if(op=="remove"){id=request.TryGetValue("id",out value)?value as string:null;if(id==null||id.Length>16)return true;}
            var marks=await Task.Run(delegate {if(op=="add")store.Add(scope,page,y,title);else if(op=="remove")store.Remove(scope,id);return store.Load(scope);});
            core.PostWebMessageAsJson(new JavaScriptSerializer{MaxJsonLength=1024*1024}.Serialize(new {kind="fw-nav",scope=scope,token=token,marks=marks}));
        }catch(Exception e){try{core.PostWebMessageAsJson(new JavaScriptSerializer().Serialize(new {kind="fw-nav",scope=scope,token=token,error=NavigationStore.Error(e)}));}catch(InvalidOperationException){}}
        finally{if(owned)busy=false;}
        return true;
    }
}
