/* Adapt configuration syntax to the shared, read-only structured view. */
(function(root){
  'use strict';
  var lib=root.FolderwingConfigParsers,Y=lib.YAML,maxNodes=200000,maxDepth=128;
  function fail(message){throw new Error(message);}
  function node(parent,key,path,type,start,end){
    return {parent:parent,key:key,path:path,type:type,start:start,end:end,children:null};
  }
  function position(n){return n && n.range ? [n.range[0],n.range[1]] : [0,0];}
  function scalarPath(parent,key){return parent===null?null:parent+'['+JSON.stringify(key)+']';}
  function sequencePath(parent,index){return parent===null?null:parent+'['+index+']';}
  function yamlDocument(doc,source){
    var count=0,anchors=new Map();
    function convert(input,parent,key,keyLabel,path,depth){
      if(depth>maxDepth)fail('Nesting exceeds the preview limit');
      if(++count>maxNodes)fail('Document exceeds the preview node limit');
      var range=position(input),n=node(parent,key,path,'literal',range[0],range[1]);
      n.keyLabel=keyLabel;
      if(!input){n.display='null';n.noCopyValue=true;return n;}
      if(input.anchor){n.badge='&'+input.anchor;n.noCopyValue=true;anchors.set(input.anchor,n);}
      if(input.tag){n.badge=(n.badge?n.badge+' ':'')+input.tag;n.noCopyValue=true;}
      if(Y.isAlias(input)) {n.type='alias';n.badge='alias';n.aliasTarget=anchors.get(input.source)||null;return n;}
      if(Y.isMap(input)) {
        n.type='object';n.children=[];
        input.items.forEach(function(pair){
          var k=pair.key,kr=position(k),keyText=k?source.slice(kr[0],kr[1]):'null';
          var simple=Y.isScalar(k) && typeof k.value==='string',name=simple?k.value:keyText;
          n.children.push(convert(pair.value,n,name,keyText,simple?scalarPath(path,name):null,depth+1));
        });
      } else if(Y.isSeq(input)) {
        n.type='array';n.children=[];
        input.items.forEach(function(item,i){n.children.push(convert(item,n,null,null,sequencePath(path,i),depth+1));});
      } else if(Y.isScalar(input)) {
        if(typeof input.value==='string'){n.type='string';n.value=input.value;}
        else if(typeof input.value==='bigint' || typeof input.value==='number')n.type='number';
        else if(input.value===null && range[0]===range[1]){n.display='null';n.noCopyValue=true;}
      }
      return n;
    }
    if(doc.errors && doc.errors.length){
      var error=doc.errors[0],message=error.message;
      if(error.pos && typeof error.pos[0]==='number'){
        var before=source.slice(0,error.pos[0]),line=(before.match(/\n/g)||[]).length+1,column=before.length-before.lastIndexOf('\n');
        message+=' (line '+line+', column '+column+')';
      }
      return {error:message,warnings:[]};
    }
    return {root:convert(doc.contents,null,null,null,'$',0),source:source,warnings:(doc.warnings||[]).map(function(w){return w.message;})};
  }
  function tomlDocument(source){
    var ast=lib.parseToml(source,{tomlVersion:'1.0.0'}).ast,count=0;
    function create(parent,key,path,type,start,end,synthetic,depth){
      if(depth>maxDepth)fail('Nesting exceeds the preview limit');
      if(++count>maxNodes)fail('Document exceeds the preview node limit');
      var n=node(parent,key,path,type,start,end);n.noCopyValue=!!synthetic;
      if(type==='object'||type==='array'){n.children=[];n.members=new Map();}
      return n;
    }
    var rootNode=create(null,null,'$','object',0,0,true,0);
    function keyId(key){return typeof key+':'+String(key);}
    function pathFor(parent,key){return typeof key==='number'?sequencePath(parent.path,key):scalarPath(parent.path,key);}
    function add(parent,key,child){parent.children.push(child);parent.members.set(keyId(key),child);}
    function ensure(parent,key,next,depth){
      var id=keyId(key),found=parent.members.get(id);if(found)return found;
      var child=create(parent,typeof key==='number'?null:key,pathFor(parent,key),typeof next==='number'?'array':'object',0,0,true,depth);
      add(parent,key,child);return child;
    }
    function keyParts(key){return key.keys.map(function(k){return k.type==='TOMLBare'?k.name:k.value;});}
    function addPair(parent,pair,depth){
      var parts=keyParts(pair.key),current=parent;
      for(var i=0;i<parts.length-1;i++)current=ensure(current,parts[i],parts[i+1],depth+i+1);
      var key=parts[parts.length-1],child=convert(pair.value,current,key,pathFor(current,key),depth+parts.length);
      add(current,key,child);
    }
    function convert(value,parent,key,path,depth){
      var range=position(value),type=value.type==='TOMLArray'?'array':value.type==='TOMLInlineTable'?'object':value.kind==='string'?'string':value.kind==='integer'||value.kind==='float'?'number':'literal';
      var n=create(parent,typeof key==='number'?null:key,path,type,range[0],range[1],false,depth);
      if(type==='string')n.value=value.value;
      if(value.type==='TOMLArray')value.elements.forEach(function(item,i){add(n,i,convert(item,n,i,pathFor(n,i),depth+1));});
      else if(value.type==='TOMLInlineTable')value.body.forEach(function(pair){addPair(n,pair,depth+1);});
      return n;
    }
    ast.body.forEach(function(top){top.body.forEach(function(item){
      if(item.type==='TOMLKeyValue')addPair(rootNode,item,1);
      else if(item.type==='TOMLTable'){
        var current=rootNode,parts=item.resolvedKey;
        parts.forEach(function(part,i){current=ensure(current,part,parts[i+1],i+1);});
        item.body.forEach(function(pair){addPair(current,pair,parts.length+1);});
      }
    });});
    return {root:rootNode,source:source,warnings:[]};
  }
  function iniDocument(source){
    var rootNode=node(null,null,'$','object',0,0),scope=rootNode,count=0,unparsed=0,pos=0,lineNumber=0;
    rootNode.children=[];rootNode.noCopyValue=true;
    function add(parent,child){if(++count>maxNodes)fail('Document exceeds the preview node limit');parent.children.push(child);}
    while(pos<source.length){
      var start=pos;
      while(pos<source.length && source[pos]!=='\r' && source[pos]!=='\n')pos++;
      var end=pos,line=source.slice(start,end);lineNumber++;
      if(pos<source.length && source[pos++]==='\r' && source[pos]==='\n')pos++;
      var trimmed=line.trim();
      if(!trimmed || trimmed[0]===';' || trimmed[0]==='#')continue;
      var section=line.match(/^[ \t]*\[([^\]\r\n]+)\][ \t]*(?:[;#].*)?$/);
      if(section && section[1].trim()){
        var name=section[1],s=node(rootNode,name,scalarPath('$',name),'object',start,end);
        s.keyLabel='['+name+']';s.children=[];s.noCopyValue=true;s.iniName=name;add(rootNode,s);scope=s;continue;
      }
      var equals=line.indexOf('='),key=equals<0?'':line.slice(0,equals).trim();
      if(key && trimmed[0]!=='['){
        var v=node(scope,key,scalarPath(scope.path,key),'string',start+equals+1,end);
        v.keyLabel=key;v.value=source.slice(v.start,v.end);v.iniName=key;add(scope,v);
      }else{
        var unknown=node(scope,'Line '+lineNumber,null,'literal',start,end);
        unknown.keyLabel='Line '+lineNumber;unknown.badge='unparsed';add(scope,unknown);unparsed++;
      }
    }
    function clearPaths(n){n.path=null;if(n.children)n.children.forEach(clearPaths);}
    function mark(scopeNode){
      var groups=new Map();
      scopeNode.children.forEach(function(n){if(n.iniName!==undefined){var key=n.iniName.trim().toLowerCase();groups.set(key,(groups.get(key)||0)+1);}});
      scopeNode.children.forEach(function(n){
        if(n.iniName!==undefined && groups.get(n.iniName.trim().toLowerCase())>1)clearPaths(n);
        else if(n.children)mark(n);
      });
    }
    mark(rootNode);
    return {root:rootNode,source:source,warnings:unparsed?[unparsed+' unparsed line'+(unparsed===1?'':'s')+' shown']:[]};
  }
  function parse(source,format,limited){
    if(limited)fail('File exceeds the configured text-preview size limit');
    if(format==='yaml'){
      var documents=Y.parseAllDocuments(source,{intAsBigInt:true,keepSourceTokens:true,prettyErrors:false,strict:true,uniqueKeys:true,version:'1.2'});
      if(!documents.length){var empty=node(null,null,'$','literal',0,0);empty.display='null';empty.noCopyValue=true;return [{root:empty,source:source,warnings:[]}];}
      return documents.map(function(doc){return yamlDocument(doc,source);});
    }
    if(format==='toml')return [tomlDocument(source)];
    if(format==='ini')return [iniDocument(source)];
    fail('Unsupported configuration format');
  }
  root.FolderwingConfig={parse:parse};
})(typeof globalThis!=='undefined'?globalThis:this);
