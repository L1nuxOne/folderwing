window.addEventListener('DOMContentLoaded',function(){
  'use strict';
  var view=document.getElementById('cells'),status=document.getElementById('status'),more=document.getElementById('more');
  var outline=document.getElementById('outline'),headings=document.getElementById('headings');
  var md=FolderwingMarkdown.create(window.markdownit),doc=null,shown=0,busy=false,mathErrors=0;
  var chunk=40,maxOutput=1024*1024;
  function appendText(node,value){node.appendChild(document.createTextNode(value));}
  function sectionTitle(token){
    if(!token || !token.children)return '';
    return token.children.map(function(t){return t.type==='text'||t.type==='code_inline'||t.type==='folderwing_math_inline'?t.content:t.type==='softbreak'?' ':'';}).join('').trim();
  }
  function buildOutline(){
    headings.replaceChildren();var count=0;
    doc.cells.forEach(function(cell,index){
      if(!cell || cell.cell_type!=='markdown')return;
      var tokens=md.parse(FolderwingNotebook.text(cell.source),{}),within=0;
      tokens.forEach(function(token,i){
        if(token.type!=='heading_open')return;
        var target='nb-h-'+index+'-'+within++,button=document.createElement('button');
        button.textContent=sectionTitle(tokens[i+1]) || 'Section';
        button.style.marginLeft=(Number(token.tag.slice(1))-1)*12+'px';
        button.addEventListener('click',function(){reveal(index,target);});headings.appendChild(button);count++;
      });
    });
    outline.hidden=!count;
  }
  async function math(node){
    var nodes=node.querySelectorAll('.math');if(!nodes.length)return;
    await MathJax.startup.promise;
    for(var i=0;i<nodes.length;i++){
      var n=nodes[i],formula=n.textContent;
      try{n.replaceChildren(await MathJax.tex2svgPromise(formula,{display:n.dataset.display==='true'}));if(n.querySelector('mjx-merror'))mathErrors++;}
      catch(_){n.textContent=formula;n.classList.add('math-error');mathErrors++;}
    }
  }
  function codeElement(source){
    var pre=document.createElement('pre'),code=document.createElement('code');
    var lang=(doc.language||'').toLowerCase();if(lang==='ipython')lang='python';if(lang==='c++')lang='cpp';
    if(source.length<=100000 && window.hljs && hljs.getLanguage(lang)){
      try{code.innerHTML=hljs.highlight(source,{language:lang,ignoreIllegals:true}).value;}catch(_){code.textContent=source;}
    }else code.textContent=source;
    pre.appendChild(code);return pre;
  }
  function renderOutput(output){
    var box=document.createElement('div');box.className='output'+(output.kind==='error'?' error':'');
    var label=document.createElement('div');label.className='output-label';label.textContent=output.label;box.appendChild(label);
    if(output.kind==='image'){
      var image=document.createElement('img');image.alt='Saved notebook output';
      image.addEventListener('error',function(){label.textContent='Saved image could not be decoded';image.remove();});
      image.src='data:'+output.image.type+';base64,'+output.image.data;box.appendChild(image);
    }else if(output.kind==='text'||output.kind==='error'){
      var pre=document.createElement('pre'),content=output.text || '';
      pre.textContent=content.length>maxOutput?content.slice(0,maxOutput)+'\n[Output truncated; open Source for the full value.]':content;box.appendChild(pre);
    }
    return box;
  }
  async function renderCell(cell,index){
    if(!cell || typeof cell!=='object')cell={};
    var type=cell.cell_type,box=document.createElement('section');box.className='cell';box.id='nb-cell-'+index;
    var label=document.createElement('div');label.className='cell-label';
    label.textContent=type==='code'?'Code'+(Number.isInteger(cell.execution_count)?' · ['+cell.execution_count+']':''):type==='markdown'?'Markdown':type==='raw'?'Raw':'Cell';box.appendChild(label);
    var content=FolderwingNotebook.text(cell.source);
    if(type==='markdown'){
      var markdown=document.createElement('div');markdown.className='markdown';markdown.innerHTML=md.render(content);
      markdown.querySelectorAll('h1,h2,h3,h4,h5,h6').forEach(function(h,n){h.id='nb-h-'+index+'-'+n;});box.appendChild(markdown);view.appendChild(box);await math(markdown);
    }else{
      var code=document.createElement('div');code.className='code';code.appendChild(codeElement(content));box.appendChild(code);
      if(type==='code')FolderwingNotebook.outputs(cell).forEach(function(output){box.appendChild(renderOutput(output));});
      view.appendChild(box);
    }
  }
  async function loadUntil(end){
    if(busy || !doc)return;busy=true;more.disabled=true;
    try{while(shown<Math.min(end,doc.cells.length)){var index=shown++;await renderCell(doc.cells[index],index);}}
    finally{busy=false;more.disabled=false;more.hidden=shown>=doc.cells.length;status.textContent=shown+' of '+doc.cells.length+' cells · saved content only';}
  }
  async function reveal(index,id){
    if(busy)return;
    while(shown<=index){await loadUntil(Math.min(index+1,shown+chunk));await new Promise(function(resolve){setTimeout(resolve,0);});}
    var target=document.getElementById(id);if(target)target.scrollIntoView({block:'start'});
  }
  window.FolderwingNavigationReveal=async function(key){var match=/^nb-cell-(\d+)$/.exec(key);if(!match||!doc||Number(match[1])>=doc.cells.length||busy)return false;await reveal(Number(match[1]),key);return true;};
  async function render(source,limited){
    try{
      doc=FolderwingNotebook.parse(source,limited);view.replaceChildren();shown=0;mathErrors=0;buildOutline();
      await loadUntil(chunk);
      window.chrome.webview.postMessage('notebook:'+shown+':'+mathErrors);
    }catch(e){view.replaceChildren();var p=document.createElement('p');p.className='error-message';appendText(p,e.message+' Switch to Source to inspect the file.');view.appendChild(p);status.textContent='Preview unavailable';more.hidden=true;window.chrome.webview.postMessage('notebook:0:1');}
  }
  more.addEventListener('click',function(){loadUntil(shown+chunk);});
  window.chrome.webview.addEventListener('message',function(event){var data=event.data;if(data && data.kind==='notebook')render(data.source,data.limited);});
  window.chrome.webview.postMessage('ready');
});
