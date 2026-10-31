(function(root){
  'use strict';
  function prepare(document){
    var stack=[document];document.parent=null;
    while(stack.length){
      var node=stack.pop(), counts=new Map();
      (node.children||[]).forEach(function(child){
        child.parent=node;
        if(child.type==='element'){
          var key=(child.ns||'')+'\u0000'+(child.local||'');
          child.position=(counts.get(key)||0)+1;counts.set(key,child.position);
        }
        stack.push(child);
      });
    }
    return document;
  }
  function literal(value){
    value=value||'';
    if(value.indexOf("'")<0)return "'"+value+"'";
    if(value.indexOf('"')<0)return '"'+value+'"';
    return 'concat('+value.split("'").map(function(part){return "'"+part+"'";}).join(', "\'", ')+')';
  }
  function xpath(node){
    if(node.type!=='element' && node.type!=='attribute')return null;
    var parts=[],n=node;
    while(n && n.type!=='document'){
      if(n.type==='element')parts.push('*[local-name()='+literal(n.local)+' and namespace-uri()='+literal(n.ns)+']['+n.position+']');
      else if(n.type==='attribute')parts.push('@*[local-name()='+literal(n.local)+' and namespace-uri()='+literal(n.ns)+']');
      else return null;
      n=n.parent;
    }
    return n && n.type==='document'?'/'+parts.reverse().join('/'):null;
  }
  root.FolderwingXML={prepare:prepare,xpath:xpath,literal:literal};
})(typeof globalThis!=='undefined'?globalThis:this);
