window.addEventListener('DOMContentLoaded',function(){
  'use strict';
  var tree=document.getElementById('tree'),tools=document.getElementById('tools'),status=document.getElementById('status');
  var find=document.getElementById('find'),findStatus=document.getElementById('find-status');
  var current=null,matches=[],matchAt=0;
  function copy(text){
    var area=document.createElement('textarea');area.value=text;area.style.position='fixed';area.style.left='-10000px';document.body.appendChild(area);area.select();
    var ok=document.execCommand('copy');area.remove();status.textContent=ok?'Copied':'Clipboard unavailable';
  }
  function button(label,handler){var b=document.createElement('button');b.textContent=label;b.addEventListener('click',handler);return b;}
  function span(cls,text){var e=document.createElement('span');e.className=cls;e.textContent=text;return e;}
  function shortened(value){return value.length>500?value.slice(0,500)+'…':value;}
  function label(node){
    var wrap=document.createElement('span'),name=node.name||'',value=node.value||'';
    if(node.type==='document')wrap.appendChild(span('summary','Document · '+node.children.length+' items'));
    else if(node.type==='element'){
      wrap.appendChild(span('key','<'+name+'>'));
      if(node.ns)wrap.appendChild(span('summary','  namespace: '+shortened(node.ns)));
      wrap.appendChild(span('summary','  · '+node.children.length+' items'));
    } else if(node.type==='attribute' || node.type==='namespace'){
      wrap.appendChild(span('key',(node.type==='attribute'?'@':'namespace ')+name+': '));
      wrap.appendChild(span('string xml-value',shortened(value)));
      if(node.type==='attribute' && node.ns)wrap.appendChild(span('summary','  namespace: '+shortened(node.ns)));
    } else if(node.type==='whitespace')wrap.appendChild(span('summary','Whitespace · '+value.length+' characters'));
    else {
      var labels={text:'Text',cdata:'CDATA',comment:'Comment',processing:'Processing instruction',declaration:'XML declaration'};
      wrap.appendChild(span('summary',(labels[node.type]||node.type)+(name?' '+name:'')+': '));
      wrap.appendChild(span('string xml-value',shortened(value)));
    }
    return wrap;
  }
  function row(node){
    var container=!!node.children,element=document.createElement(container?'details':'div'),line=container?document.createElement('summary'):element;
    if(!container)element.className='row';line.appendChild(label(node));
    var actions=document.createElement('span');actions.className='actions';
    if(node.value!==null && node.value!==undefined)actions.appendChild(button('Copy value',function(e){e.preventDefault();e.stopPropagation();copy(node.value);}));
    if(node.type==='element'||node.type==='attribute')actions.appendChild(button('Copy XPath',function(e){e.preventDefault();e.stopPropagation();var path=FolderwingXML.xpath(node);if(path)copy(path);}));
    line.appendChild(actions);    if(node.type==='element'||node.type==='attribute'){element.dataset.navKey=String(FolderwingXML.xpath(node)||node.name||'XML').slice(0,512);element.dataset.navTitle=element.dataset.navKey;var depth=0,parent=node.parent;while(parent&&depth<6){depth++;parent=parent.parent;}element.dataset.navDepth=String(depth);}
    node.element=element;
    if(container){element.appendChild(line);element.addEventListener('toggle',function(){if(element.open && !node.renderCount)children(node);});}
    return element;
  }
  function children(node,required){
    var from=node.renderCount||0;if(typeof required==='number' && required<from)return;
    var to=Math.min(node.children.length,Math.max(from+100,(required||0)+1));
    if(node.moreButton)node.moreButton.remove();
    for(var i=from;i<to;i++)node.element.appendChild(row(node.children[i]));
    node.renderCount=to;
    if(to<node.children.length){node.moreButton=button('Show more ('+(node.children.length-to)+' remaining)',function(){children(node);});node.element.appendChild(node.moreButton);}
  }
  function findMatches(){
    if(!current)return;var query=find.value.toLocaleLowerCase();matches=[];matchAt=0;
    if(query){var stack=[current];while(stack.length && matches.length<1000){
      var n=stack.pop(),content=[n.name||'',n.ns||'',n.value||''].join(' ').toLocaleLowerCase();
      if(content.includes(query))matches.push(n);
      if(n.children)for(var i=n.children.length-1;i>=0;i--)stack.push(n.children[i]);
    }}
    findStatus.textContent=!query?'':matches.length?(matches.length===1000?'1000+ matches':matches.length+' matches'):'No matches in this tree';
  }
  function reveal(node){
    var parents=[],p=node.parent;while(p){parents.push(p);p=p.parent;}
    parents.reverse().forEach(function(parent,i){parent.element.open=true;var child=parents[i+1]||node;children(parent,parent.children.indexOf(child));});
    tree.querySelectorAll('.match').forEach(function(e){e.classList.remove('match');});
    node.element.classList.add('match');node.element.scrollIntoView({block:'center'});
  }
  window.FolderwingNavigationReveal=function(key){if(!current)return false;var stack=[current],visited=0;while(stack.length&&visited++<200000){var node=stack.pop();if((node.type==='element'||node.type==='attribute')&&String(FolderwingXML.xpath(node)).slice(0,159)===key){reveal(node);return true;}if(node.children)for(var i=node.children.length-1;i>=0;i--)stack.push(node.children[i]);}return false;};
  find.addEventListener('input',findMatches);document.getElementById('next').addEventListener('click',function(){if(matches.length)reveal(matches[matchAt++%matches.length]);});
  window.chrome.webview.addEventListener('message',function(event){
    var data=event.data;if(!data || data.kind!=='xml')return;
    tree.replaceChildren();current=null;tools.hidden=true;
    if(data.error || !data.root){var p=document.createElement('p');p.className='error';p.textContent='XML: '+(data.error||'Could not parse this document')+' Switch to Source to inspect the original.';tree.appendChild(p);status.textContent='Preview unavailable';window.chrome.webview.postMessage('structured:0:1');return;}
    current=FolderwingXML.prepare(data.root);tools.hidden=false;tree.appendChild(row(current));current.element.open=true;children(current);
    var top=current.children.find(function(n){return n.type==='element';});if(top && top.element){top.element.open=true;children(top);}
    status.textContent=data.nodes+' nodes · Read only';findMatches();window.chrome.webview.postMessage('structured:1:0');
  });
  window.chrome.webview.postMessage('ready');
});
