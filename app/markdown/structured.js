window.addEventListener('DOMContentLoaded', function () {
  'use strict';
  var tree=document.getElementById('tree'), status=document.getElementById('status'), tools=document.getElementById('tools');
  var list=document.getElementById('record-list'), recordSection=document.getElementById('records'), more=document.getElementById('more');
  var documents=document.getElementById('documents'),documentList=document.getElementById('document-list');
  var iniControls=document.getElementById('ini-controls'),iniEncoding=document.getElementById('ini-encoding');
  var heading=document.getElementById('heading'), find=document.getElementById('find'), findStatus=document.getElementById('find-status');
  var entries=[], entryBytes=0, current=null, selectedEntry=null, matches=[], matchAt=0, loading=false;
  function copy(text) {
    var area=document.createElement('textarea');area.value=text;area.style.position='fixed';area.style.left='-10000px';document.body.appendChild(area);area.select();
    var ok=document.execCommand('copy');area.remove();status.textContent=ok?'Copied':'Clipboard unavailable';
  }
  function button(label, handler) { var b=document.createElement('button');b.textContent=label;b.addEventListener('click',handler);return b; }
  function label(node, source) {
    var wrap=document.createElement('span');
    if(node.key!==null) { var k=document.createElement('span');k.className='key';k.textContent=(node.keyLabel!==undefined && node.keyLabel!==null?node.keyLabel:JSON.stringify(node.key))+': ';wrap.appendChild(k); }
    else if(node.parent && node.parent.type==='array' && typeof node.index==='number'){var item=document.createElement('span');item.className='key';item.textContent='['+node.index+']: ';wrap.appendChild(item);}
    var v=document.createElement('span');
    if(node.children) {v.className='summary';v.textContent=node.type==='object'?'{ '+node.children.length+' keys }':'[ '+node.children.length+' items ]';}
    else {v.className=node.type==='string'?'string':node.type==='number'?'number':'literal';var raw=node.display!==undefined?node.display:source.slice(node.start,node.end);v.textContent=raw.length>500?raw.slice(0,500)+'…':raw;}
    wrap.appendChild(v);
    if(node.badge){var badge=document.createElement('span');badge.className='summary';badge.textContent='  '+node.badge;wrap.appendChild(badge);}
    return wrap;
  }
  function row(node, source) {
    var element=node.children?document.createElement('details'):document.createElement('div');
    var line=node.children?document.createElement('summary'):element;
    if(!node.children)element.className='row';line.appendChild(label(node,source));
    var actions=document.createElement('span');actions.className='actions';
    if(!node.noCopyValue)actions.appendChild(button('Copy value',function(e){e.preventDefault();e.stopPropagation();copy(source.slice(node.start,node.end));}));
    if(node.path!==null)actions.appendChild(button('Copy path',function(e){e.preventDefault();e.stopPropagation();copy(node.path);}));
    if(node.aliasTarget)actions.appendChild(button('Go to anchor',function(e){e.preventDefault();e.stopPropagation();revealNode(node.aliasTarget);}));
    line.appendChild(actions);
    element.dataset.navKey=String(node.path!==null && node.path!==undefined?node.path:(node.keyLabel||node.key||'$')).slice(0,512);
    element.dataset.navTitle=element.dataset.navKey;var depth=0,parent=node.parent;while(parent&&depth<6){depth++;parent=parent.parent;}element.dataset.navDepth=String(depth);
    node.element=element;
    if(node.children){element.appendChild(line);element.addEventListener('toggle',function(){if(element.open && !node.renderCount) children(node,source);});}
    return element;
  }
  function children(node, source, required) {
    var from=node.renderCount||0;
    if(typeof required==='number' && required<from)return;
    var to=Math.min(node.children.length,Math.max(from+100,(required||0)+1));
    if(node.moreButton)node.moreButton.remove();
    for(var i=from;i<to;i++){if(node.type==='array')node.children[i].index=i;node.element.appendChild(row(node.children[i],source));}
    node.renderCount=to;
    if(to<node.children.length){node.moreButton=button('Show more ('+(node.children.length-to)+' remaining)',function(){children(node,source);});node.element.appendChild(node.moreButton);}
  }
  function show(parsed) {
    current=parsed;matches=[];matchAt=0;tree.replaceChildren();tools.hidden=false;
    tree.appendChild(row(parsed.root,parsed.source));
    if(parsed.root.children){parsed.root.element.open=true;children(parsed.root,parsed.source);}
    findStatus.textContent='';if(find.value)findMatches();
  }
  function showError(error, prefix) {
    current=null;tree.replaceChildren();tools.hidden=true;var p=document.createElement('p');p.className='error';
    var location=typeof error.offset==='number'?' (character '+(error.offset+1)+')':typeof error.lineNumber==='number'?' (line '+error.lineNumber+(typeof error.column==='number'?', column '+(error.column+1):'')+')':'';
    p.textContent=prefix+error.message+location+'. Switch to Source to inspect the original.';tree.appendChild(p);
  }
  function selectRecord(entry) {
    selectedEntry=entry;
    Array.from(list.children).forEach(function(b){b.classList.toggle('selected',b===entry.button);});
    if(entry.error){showError(new Error(entry.error),'Record '+entry.number+': ');return;}
    try{show(FolderwingJSON.parse(entry.text));}catch(e){showError(e,'Record '+entry.number+': ');}
  }
  function displayRecords(batch) {
    loading=false;
    batch.records.forEach(function(entry){
      entry.button=button('Line '+entry.number,function(){selectRecord(entry);});list.appendChild(entry.button);entries.push(entry);
      entryBytes+=(entry.text||'').length;
    });
    while(entries.length>512 || (entryBytes>4*1024*1024 && entries.length>1)){var old=entries.shift();entryBytes-=(old.text||'').length;old.button.remove();if(old===selectedEntry){selectedEntry=null;current=null;}}
    status.textContent=entries.length?'Loaded lines '+entries[0].number+'–'+entries[entries.length-1].number+(entries[0].number>1?' · earlier lines released':'')+(batch.end?' · end of file':''):'No records';
    more.disabled=batch.end;more.textContent=batch.end?'End of file':'Load more records';
    if(!selectedEntry && entries.length)selectRecord(entries[0]);
    window.chrome.webview.postMessage('structured:'+batch.records.length+':0');
  }
  function showConfig(data) {
    heading.textContent=data.format==='yaml'?'YAML':data.format==='toml'?'TOML':'INI';
    if(iniControls)iniControls.hidden=data.format!=='ini';
    try{
      var parsed=FolderwingConfig.parse(data.source,data.format,data.limited);
      documentList.replaceChildren();documents.hidden=parsed.length<2;
      function select(index){
        var item=parsed[index];Array.from(documentList.children).forEach(function(b,i){b.classList.toggle('selected',i===index);});
        var details=[];if(parsed.length>1)details.push('Document '+(index+1)+' of '+parsed.length);if(data.encoding)details.push('Encoding: '+data.encoding);if(item.warnings.length)details.push(item.warnings.join('; '));status.textContent=details.join(' · ');
        if(item.error)showError(new Error(item.error),heading.textContent+': ');else show(item);
      }
      parsed.forEach(function(item,i){documentList.appendChild(button('Document '+(i+1)+(item.error?' · invalid':''),function(){select(i);}));});
      select(0);window.chrome.webview.postMessage('structured:'+parsed.length+':'+parsed.filter(function(item){return !!item.error;}).length);
    }catch(e){status.textContent='Preview unavailable';showError(e,heading.textContent+': ');window.chrome.webview.postMessage('structured:0:1');}
  }
  function findMatches() {
    if(!current)return;var query=find.value.toLocaleLowerCase();matches=[];matchAt=0;
    if(query) {var stack=[current.root];while(stack.length&&matches.length<1000){var n=stack.pop();var value=n.children?'':n.display!==undefined?n.display:current.source.slice(n.start,n.end);if((n.key||'').toLocaleLowerCase().includes(query)||value.toLocaleLowerCase().includes(query)||(n.badge||'').toLocaleLowerCase().includes(query)||(n.type==='string'&&n.value.toLocaleLowerCase().includes(query)))matches.push(n);if(n.children)for(var i=n.children.length-1;i>=0;i--)stack.push(n.children[i]);}}
    findStatus.textContent=!query?'':matches.length?(matches.length===1000?'1000+ matches':matches.length+' matches'):'No matches in this tree';
  }
  function revealNode(node) {
    var parents=[],p=node.parent;
    while(p){parents.push(p);p=p.parent;}parents.reverse().forEach(function(parent,i){parent.element.open=true;var child=parents[i+1]||node;children(parent,current.source,parent.children.indexOf(child));});
    tree.querySelectorAll('.match').forEach(function(e){e.classList.remove('match');});node.element.classList.add('match');node.element.scrollIntoView({block:'center'});
  }
  window.FolderwingNavigationReveal=function(key){if(!current)return false;var stack=[current.root],visited=0;while(stack.length&&visited++<200000){var node=stack.pop();if(String(node.path).slice(0,159)===key){revealNode(node);return true;}if(node.children)for(var i=node.children.length-1;i>=0;i--)stack.push(node.children[i]);}return false;};
  function nextMatch() {if(matches.length)revealNode(matches[matchAt++%matches.length]);}
  find.addEventListener('input',findMatches);document.getElementById('next').addEventListener('click',nextMatch);
  if(iniEncoding)iniEncoding.addEventListener('change',function(){window.chrome.webview.postMessage('ini-encoding:'+iniEncoding.value);});
  more.addEventListener('click',function(){if(!loading){loading=true;more.disabled=true;window.chrome.webview.postMessage('next');}});
  document.getElementById('restart').addEventListener('click',function(){if(!loading){loading=true;more.disabled=true;window.chrome.webview.postMessage('reset');}});
  window.chrome.webview.addEventListener('message',function(event){
    var data=event.data;
    if(data.kind==='json') {heading.textContent='JSON';status.textContent=data.limited?'Showing a limited prefix; large documents may be incomplete':'';try{show(FolderwingJSON.parse(data.source));window.chrome.webview.postMessage('structured:1:0');}catch(e){showError(e,'JSON: ');window.chrome.webview.postMessage('structured:1:1');}}
    else if(data.kind==='config')showConfig(data);
    else if(data.kind==='jsonl'){heading.textContent='JSON Lines';recordSection.hidden=false;loading=true;more.disabled=true;window.chrome.webview.postMessage('next');}
    else if(data.kind==='reset'){entries=[];entryBytes=0;selectedEntry=current=null;list.replaceChildren();tree.replaceChildren();tools.hidden=true;window.chrome.webview.postMessage('next');}
    else if(data.kind==='records'){more.disabled=false;displayRecords(data);}
  });
  window.chrome.webview.postMessage('ready');
});
