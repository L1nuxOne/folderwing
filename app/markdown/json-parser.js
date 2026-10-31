/* Lossless JSON tree: number and string tokens retain their original spelling. */
(function (root) {
  'use strict';
  function parse(source) {
    var i=0, count=0, maxNodes=200000;
    function fail(message) { var e=new Error(message);e.offset=i;throw e; }
    function space() { while (i<source.length && /[\x20\x09\x0a\x0d]/.test(source[i])) i++; }
    function string() {
      var start=i++;
      while (i<source.length) {
        var c=source[i++];
        if (c==='"') {
          var raw=source.slice(start,i), value;
          try { value=JSON.parse(raw); } catch (_) { fail('Invalid string escape'); }
          return {raw:raw,value:value};
        }
        if (c==='\\') { if(i>=source.length) fail('Unterminated escape'); i++; }
        else if (c.charCodeAt(0)<32) fail('Control character in string');
      }
      fail('Unterminated string');
    }
    function value(depth, parent, key, path) {
      if (depth>128) fail('Nesting exceeds the preview limit');
      if (++count>maxNodes) fail('Document exceeds the preview node limit');
      space();var start=i,c=source[i],node={start:start,parent:parent,key:key,path:path,children:null};
      if(c==='{' || c==='[') {
        i++;node.type=c==='{'?'object':'array';node.children=[];space();
        var closing=c==='{'?'}':']';
        if(source[i]!==closing) for(var index=0;;index++) {
          var name=null, childPath;
          if(c==='{') { if(source[i]!=='"') fail('Expected an object key');name=string().value;space();if(source[i++]!==':') fail('Expected a colon');childPath=path+'['+JSON.stringify(name)+']'; }
          else childPath=path+'['+index+']';
          node.children.push(value(depth+1,node,name,childPath));space();
          if(source[i]===closing) break;
          if(source[i++]!==',') fail('Expected a comma or closing bracket');space();
        }
        if(source[i++]!==closing) fail('Unclosed object or array');
      } else if(c==='"') { var s=string();node.type='string';node.value=s.value; }
      else {
        var match=source.slice(i).match(/^(?:-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?|true|false|null)/);
        if(!match) fail('Expected a JSON value');
        i+=match[0].length;node.type=/^-|^[0-9]/.test(match[0])?'number':match[0];node.value=match[0];
      }
      node.end=i;return node;
    }
    var tree=value(0,null,null,'$');space();if(i!==source.length) fail('Unexpected text after JSON value');
    return {root:tree,source:source,count:count};
  }
  root.FolderwingJSON={parse:parse};
})(typeof globalThis!=='undefined'?globalThis:this);
