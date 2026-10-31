/* Upgrade the older single-callback mock to WebView's multiple-listener contract.
 * Existing fixtures don't own persisted marks; the dedicated navigation tests do. */
(function(){
 var bridge=window.chrome.webview,add=bridge.addEventListener,post=bridge.postMessage,listeners=[];
 bridge.addEventListener=function(name,fn){if(name!=='message'){add.call(bridge,name,fn);return;}listeners.push(fn);add.call(bridge,name,function(event){listeners.slice().forEach(function(f){f(event);});});};
 bridge.postMessage=function(message){if(message.indexOf('fw-nav:')!==0){post.call(bridge,message);return;}var r=JSON.parse(message.slice(7));setTimeout(function(){listeners.slice().forEach(function(f){f({data:{kind:'fw-nav',scope:r.scope,token:r.token,marks:[]}});});},0);};
})();
