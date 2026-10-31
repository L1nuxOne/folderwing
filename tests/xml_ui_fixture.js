/* Synthetic UI model; never opens external resources or the real clipboard. */
(function(){
 'use strict';
 var callback,phase=0,copied='';
 function n(type,name,local,ns,value,children){return {type:type,name:name||null,local:local||null,ns:ns||'',value:value===undefined?null:value,children:children||null};}
 var first=n('element','p:item','item','urn:one',null,[n('attribute','p:id','id','urn:one','i1'),n('text',null,null,null,'first')]);
 var second=n('element','r:item','item','urn:one',null,[n('attribute','p:id','id','urn:one','i2'),n('cdata',null,null,null,'magic')]);
 var odd=n('element','z:item','item','urn:Q\'"Z',null,[n('text',null,null,null,'odd')]);
 var children=[n('namespace','xmlns','xmlns','http://www.w3.org/2000/xmlns/','urn:root'),n('namespace','xmlns:p','p','http://www.w3.org/2000/xmlns/','urn:one'),first,n('element','q:item','item','urn:other',null,[n('text',null,null,null,'different')]),second,odd,n('comment',null,null,null,'<img src=x onerror=alert(1)>')];
 var xml='<root xmlns="urn:root" xmlns:p="urn:one" xmlns:q="urn:other" xmlns:r="urn:one" xmlns:z="urn:Q\'&quot;Z"><p:item p:id="i1">first</p:item><q:item>different</q:item><r:item p:id="i2"><![CDATA[magic]]></r:item><z:item>odd</z:item>';
 for(var i=0;i<105;i++){children.push(n('element','extra','extra','urn:root',null,[n('text',null,null,null,'needle@'+i)]));xml+='<extra>needle@'+i+'</extra>';}
 xml+='</root>';
 var parsed=new DOMParser().parseFromString(xml,'application/xml');
 var model=n('document','Document',null,null,null,[n('element','root','root','urn:root',null,children)]);
 document.execCommand=function(command){if(command!=='copy')return false;copied=document.activeElement.value;return true;};
 function search(value){var input=document.getElementById('find');input.value=value;input.dispatchEvent(new Event('input'));document.getElementById('next').click();}
 function copyButton(container){var b=Array.from(container.querySelectorAll('button')).find(function(x){return x.textContent==='Copy XPath';});if(!b)throw Error('missing XPath button');b.click();}
 function evaluate(path){return parsed.evaluate(path,parsed,null,XPathResult.FIRST_ORDERED_NODE_TYPE,null).singleNodeValue;}
 window.chrome.webview={addEventListener:function(name,fn){callback=fn;},postMessage:function(message){
  if(message==='ready')setTimeout(function(){callback({data:{kind:'xml',root:model,nodes:200}});},0);
  if(message.indexOf('structured:')===0)setTimeout(function(){try{
    if(phase===0){
      if(message!=='structured:1:0')throw Error('XML success result');
      var text=document.getElementById('tree').textContent;if(!text.includes('namespace xmlns') || !text.includes('<p:item>'))throw Error('tree and namespace display');
      if(document.querySelector('#tree img'))throw Error('unsafe HTML');
      search('magic');var match=document.querySelector('.match');if(!match || !match.textContent.includes('CDATA'))throw Error('CDATA search');
      var element=match.closest('details'),summary=element.querySelector(':scope > summary');copyButton(summary);
      var result=evaluate(copied);if(!result || result.prefix!=='r' || !copied.includes('[2]'))throw Error('repeated namespaced XPath');
      var attr=Array.from(element.querySelectorAll(':scope > div')).find(function(x){return x.textContent.includes('@p:id');});if(!attr)throw Error('attribute display');copyButton(attr);
      result=evaluate(copied);if(!result || result.value!=='i2')throw Error('attribute XPath');
      search('odd');match=document.querySelector('.match');copyButton(match.closest('details').querySelector(':scope > summary'));
      result=evaluate(copied);if(!result || result.prefix!=='z' || !copied.includes('concat('))throw Error('quoted namespace XPath');
      search('needle@104');match=document.querySelector('.match');if(!match || !match.textContent.includes('needle@104'))throw Error('late child search');
      phase=1;callback({data:{kind:'xml',error:'Malformed document'}});
    }else{
      if(message!=='structured:0:1' || !document.getElementById('tree').textContent.includes('Switch to Source'))throw Error('error and source fallback');
      document.documentElement.dataset.test='passed';
    }
  }catch(e){document.documentElement.dataset.test=e.message;}},50);
 }};
})();
