/* One widget for the browser-backed readers. All document text stays text. */
(function(global){
 'use strict';
 function clamp(n,a,b){return Math.max(a,Math.min(b,n));}
 function label(node){return (node.textContent||'').replace(/\s+/g,' ').trim().slice(0,159);}
 function visible(node){return !!(node&&node.getClientRects().length);}
 var bridge=global.chrome&&global.chrome.webview;
 function Navigation(){
  var self=this;this.scope='';this.marks=[];this.anchors=[];this.token=0;this.error='';this.pending=false;this.hover=null;this.drag=false;this.active=-1;this.scheduled=0;this.observedShadow=null;
  var ui=document.createElement('div');ui.className='fw-nav-ui';ui.dataset.fwNavUi='true';this.ui=ui;
  var save=document.createElement('button');save.type='button';save.className='fw-nav-save';save.setAttribute('aria-label','Bookmark this reading position');save.title='Bookmark this position (Ctrl+D); right-click for saved marks';this.save=save;
  var svg=document.createElementNS('http://www.w3.org/2000/svg','svg');svg.setAttribute('viewBox','0 0 16 20');svg.setAttribute('aria-hidden','true');var path=document.createElementNS(svg.namespaceURI,'path');path.setAttribute('d','M2 1 H14 V18 L8 14 L2 18 Z');path.setAttribute('stroke','currentColor');path.setAttribute('stroke-width','1.5');this.ribbon=path;svg.appendChild(path);save.appendChild(svg);ui.appendChild(save);
  var canvas=document.createElement('canvas');canvas.tabIndex=0;canvas.setAttribute('role','slider');canvas.setAttribute('aria-label','Document sections and reading position');canvas.setAttribute('aria-valuemin','0');canvas.setAttribute('aria-valuemax','100');this.canvas=canvas;ui.appendChild(canvas);
  var tip=document.createElement('div');tip.className='fw-nav-tip';tip.hidden=true;tip.setAttribute('role','tooltip');this.tip=tip;var title=document.createElement('strong'),detail=document.createElement('small');tip.appendChild(title);tip.appendChild(detail);this.tipTitle=title;this.tipDetail=detail;ui.appendChild(tip);
  var menu=document.createElement('div');menu.className='fw-nav-menu';menu.hidden=true;menu.setAttribute('role','dialog');menu.setAttribute('aria-label','Reading bookmarks');this.menu=menu;ui.appendChild(menu);document.body.appendChild(ui);
  save.onclick=function(){self.toggle();};save.oncontextmenu=function(e){e.preventDefault();self.openMenu(true,e.clientY);};
  canvas.onpointermove=function(e){self.hover=e.clientY;if(self.drag)self.scrub(e.clientY);self.draw();};
  canvas.onpointerleave=function(){if(!self.drag){self.hover=null;self.tip.hidden=true;self.draw();}};
  canvas.onpointerdown=function(e){if(e.button!==0)return;e.preventDefault();canvas.focus();self.hover=e.clientY;var near=self.nearest(e.clientY);if(near&&near.distance<=7)self.jump(near.item);else self.scrub(e.clientY);self.drag=true;canvas.setPointerCapture(e.pointerId);};
  canvas.onpointerup=function(e){self.drag=false;if(canvas.hasPointerCapture(e.pointerId))canvas.releasePointerCapture(e.pointerId);};canvas.onlostpointercapture=function(){self.drag=false;};
  canvas.oncontextmenu=function(e){e.preventDefault();self.openMenu(false,e.clientY);};
  canvas.onkeydown=function(e){if(e.key==='ArrowUp'||e.key==='ArrowDown'){var items=self.targets();self.active=clamp(self.active+(e.key==='ArrowUp'?-1:1),0,Math.max(0,items.length-1));if(items[self.active]){self.hover=self.tickY(items[self.active].top);self.draw();}e.preventDefault();}else if(e.key==='Enter'){var item=self.targets()[self.active];if(item)self.jump(item);e.preventDefault();}else if(e.key==='Home'||e.key==='End'){self.setTop(e.key==='Home'?0:self.range());e.preventDefault();}else if(e.key==='Escape'){self.menu.hidden=true;self.tip.hidden=true;}};
  document.addEventListener('keydown',function(e){if(e.ctrlKey&&!e.altKey&&e.key.toLowerCase()==='d'){e.preventDefault();if(e.shiftKey)self.openMenu(true,35);else self.toggle();}else if(e.key==='Escape'&&!menu.hidden){menu.hidden=true;save.focus();e.preventDefault();}});
  document.addEventListener('pointerdown',function(e){if(!ui.contains(e.target)){menu.hidden=true;tip.hidden=true;}},true);
  document.addEventListener('scroll',function(){self.draw();},true);global.addEventListener('resize',function(){self.schedule();});
  this.observer=new MutationObserver(function(changes){if(changes.some(function(c){return !(c.target.nodeType===1&&c.target.closest('[data-fw-nav-ui]'));}))self.schedule();});this.observer.observe(document.body,{childList:true,subtree:true,attributes:true,attributeFilter:['open','hidden']});
  if(bridge)bridge.addEventListener('message',function(e){var d=e.data;if(!d||d.kind!=='fw-nav'||d.scope!==self.scope||d.token!==String(self.token))return;self.pending=false;self.error=d.error||'';if(!d.error)self.marks=Array.isArray(d.marks)?d.marks:[];self.draw();if(!menu.hidden)self.openMenu(true,35,false);});
  this.schedule();
 }
 Navigation.prototype.detect=function(){
  var root,scroller,kind,word=document.getElementById('viewer'),grid=document.querySelector('.grid');
  if(word&&word.shadowRoot){root=word.shadowRoot;scroller=word;kind='word';if(this.observedShadow!==root){this.observer.observe(root,{childList:true,subtree:true,attributes:true,attributeFilter:['open','hidden']});this.observedShadow=root;}}
  else if(grid){root=grid;scroller=grid;kind=document.getElementById('sheet')?'spreadsheet':'table';}
  else if(document.getElementById('cells')){root=document.getElementById('cells');scroller=document.scrollingElement;kind='notebook';}
  else if(document.getElementById('tree')){root=document.getElementById('tree');scroller=document.scrollingElement;kind=location.pathname.replace(/.*\//,'');}
  else{root=document.getElementById('content');scroller=document.scrollingElement;kind='markdown';}
  if(!root)return false;this.root=root;this.scroller=scroller;this.kind=kind;
  scroller.classList.add('fw-nav-scroll');if(scroller===document.scrollingElement)document.body.classList.add('fw-nav-document');else scroller.classList.add('fw-nav-element');
  var scope=kind,sheet=document.getElementById('sheet'),record=document.querySelector('#record-list .selected,#document-list .selected');if(sheet&&sheet.selectedOptions.length)scope+=':'+sheet.selectedOptions[0].textContent;if(record)scope+=':'+record.textContent;
  if(scope!==this.scope){this.scope=scope;this.marks=[];this.error='';this.request('list');}
  return true;
 };
 Navigation.prototype.rect=function(){return this.scroller===document.scrollingElement?{top:0,bottom:innerHeight,height:innerHeight}:this.scroller.getBoundingClientRect();};
 Navigation.prototype.top=function(){return this.scroller?this.scroller.scrollTop:0;};
 Navigation.prototype.range=function(){return this.scroller?Math.max(0,this.scroller.scrollHeight-this.scroller.clientHeight):0;};
 Navigation.prototype.inset=function(){if(this.kind==='table'||this.kind==='spreadsheet'){var head=this.root.querySelector('thead');return head?head.getBoundingClientRect().height:0;}if(this.scroller===document.scrollingElement){var h=document.querySelector('body>header');return h&&getComputedStyle(h).position==='sticky'?h.getBoundingClientRect().height:0;}return 0;};
 Navigation.prototype.nodeTop=function(node){return Math.max(0,node.getBoundingClientRect().top-this.rect().top+this.top()-this.inset());};
 Navigation.prototype.setTop=function(top){this.scroller.scrollTop=clamp(top,0,this.range());this.draw();};
 Navigation.prototype.schedule=function(){var self=this;if(this.scheduled)return;this.scheduled=setTimeout(function(){self.scheduled=0;self.rebuild();},100);};
 Navigation.prototype.rebuild=function(){
  if(!this.detect())return;var self=this,items=[];
  if(this.kind==='word'&&global.FolderwingNavigationWord){items=global.FolderwingNavigationWord().map(function(x){return {node:x.node,title:x.text,key:x.text,depth:x.level};});}
  else if(this.kind==='markdown'){items=Array.from(this.root.querySelectorAll('h1,h2,h3,h4,h5,h6')).map(function(n){return {node:n,title:label(n),key:label(n),depth:Number(n.tagName.slice(1))-1};});}
  else if(this.kind==='notebook'){items=Array.from(this.root.querySelectorAll('.cell')).map(function(n){var heading=n.querySelector('h1,h2,h3,h4,h5,h6'),name=n.querySelector('.cell-label');return {node:n,title:heading?label(heading):label(name||n).slice(0,90),key:n.id,depth:heading?0:1};});}
  else if(this.kind!=='table'&&this.kind!=='spreadsheet'){items=Array.from(this.root.querySelectorAll('[data-nav-key]')).map(function(n){var line=n.querySelector('summary')||n;return {node:line,title:n.dataset.navTitle||n.dataset.navKey,key:n.dataset.navKey,depth:Math.min(5,Number(n.dataset.navDepth)||0)};});}
  this.anchors=items.filter(function(x){return x.title&&visible(x.node);}).slice(0,2048).map(function(x,i){x.top=self.nodeTop(x.node);x.page=i+1;return x;});this.draw();
 };
 Navigation.prototype.targets=function(){var self=this,items=this.anchors.map(function(a){return {top:a.top,title:a.title,detail:'Section',anchor:a};});if(!items.length){for(var i=0;i<=(this.range()?20:0);i++)items.push({top:this.range()*i/20,title:(i*5)+'%',detail:this.kind==='table'||this.kind==='spreadsheet'?'Loaded rows':'Preview position'});}
  this.marks.forEach(function(m){var top=self.markTop(m);if(top!==null)items.push({top:top,title:m.title,detail:'Personal bookmark',mark:m});});return items.sort(function(a,b){return a.top-b.top;});};
 Navigation.prototype.markTop=function(m){
  if(this.kind==='table'||this.kind==='spreadsheet'){var row=this.root.querySelector('[data-nav-row="'+String(Number(m.page))+'"]');return row&&visible(row)?this.nodeTop(row)+m.y*row.getBoundingClientRect().height:null;}
  if(!m.page)return m.y*this.range();var a=this.anchors[m.page-1];if(!a||String(a.key).slice(0,159)!==m.title){a=this.anchors.find(function(x){return String(x.key).slice(0,159)===m.title;});}if(!a)return null;var at=this.anchors.indexOf(a),end=at+1<this.anchors.length?this.anchors[at+1].top:this.range();return a.top+clamp(m.y,0,1)*Math.max(0,end-a.top);
 };
 Navigation.prototype.capture=function(){
  var top=this.top();if(this.kind==='table'||this.kind==='spreadsheet'){var rows=Array.from(this.root.querySelectorAll('[data-nav-row]')).filter(visible),self=this,row=null;rows.forEach(function(n){if(self.nodeTop(n)<=top+2)row=n;});if(!row)row=rows[0];if(!row)return null;return {page:Number(row.dataset.navRow),y:clamp((top-this.nodeTop(row))/Math.max(1,row.getBoundingClientRect().height),0,1),title:'Row '+row.dataset.navRow};}
  var a=null;this.anchors.forEach(function(x){if(x.top<=top+2)a=x;});if(a){var index=this.anchors.indexOf(a),end=index+1<this.anchors.length?this.anchors[index+1].top:this.range();return {page:index+1,y:clamp((top-a.top)/Math.max(1,end-a.top),0,1),title:String(a.key).slice(0,159)};}
  return {page:0,y:this.range()?top/this.range():0,title:Math.round(this.range()?top/this.range()*100:0)+'%'};
 };
 Navigation.prototype.current=function(){var self=this;return this.marks.find(function(m){var top=self.markTop(m);return top!==null&&Math.abs(top-self.top())<=8;});};
 Navigation.prototype.request=function(op,mark){if(!bridge){this.error='Bookmarks are available in Folderwing.';this.draw();return;}var message=Object.assign({op:op,scope:this.scope,token:String(++this.token)},mark||{});this.pending=true;bridge.postMessage('fw-nav:'+JSON.stringify(message));};
 Navigation.prototype.toggle=function(){if(this.pending)return;var current=this.current(),capture=this.capture();if(current)this.request('remove',{id:current.id});else if(capture)this.request('add',capture);else{this.error='No loaded rows to bookmark.';this.draw();}};
 Navigation.prototype.jump=async function(item){if(item.mark){var top=this.markTop(item.mark);if(top===null&&global.FolderwingNavigationReveal){var scope=this.scope;try{await global.FolderwingNavigationReveal(item.mark.title);}catch(_){}if(scope!==this.scope)return;this.rebuild();top=this.markTop(item.mark);}if(top===null){this.error=this.kind==='table'||this.kind==='spreadsheet'?'Load the bookmarked row and clear filters to reveal it.':'The bookmarked section is not available in this preview.';this.draw();return;}this.setTop(top);}else this.setTop(item.top);this.menu.hidden=true;};
 Navigation.prototype.axis=function(){var r=this.canvas.getBoundingClientRect();return {first:r.top+10,last:Math.max(r.top+10,r.bottom-10)};};
 Navigation.prototype.tickY=function(top){var a=this.axis();return a.first+(a.last-a.first)*(this.range()?clamp(top/this.range(),0,1):0);};
 Navigation.prototype.nearest=function(y){var self=this,best=null;this.targets().forEach(function(item){var d=Math.abs(self.tickY(item.top)-y);if(!best||d<best.distance||(d===best.distance&&item.mark))best={item:item,distance:d};});return best;};
 Navigation.prototype.scrub=function(y){var a=this.axis();this.setTop((a.last>a.first?clamp((y-a.first)/(a.last-a.first),0,1):0)*this.range());};
 Navigation.prototype.draw=function(){
  if(!this.scroller)return;var rect=this.rect(),height=Math.max(40,Math.min(innerHeight,rect.bottom)-Math.max(0,rect.top));this.ui.style.top=Math.max(0,rect.top)+'px';this.ui.style.height=height+'px';var h=Math.max(10,height-30),scale=devicePixelRatio||1,w=Math.round(28*scale),ph=Math.round(h*scale);
  if(this.canvas.width!==w||this.canvas.height!==ph){this.canvas.width=w;this.canvas.height=ph;this.canvas.style.height=h+'px';}
  var dc=this.canvas.getContext('2d');dc.setTransform(scale,0,0,scale,0,0);dc.clearRect(0,0,28,h);var self=this,canvasTop=this.canvas.getBoundingClientRect().top;
  this.targets().forEach(function(item){var y=self.tickY(item.top)-canvasTop,d=self.hover===null?999:Math.abs(self.hover-y-canvasTop),length=item.mark?14:item.anchor&&item.anchor.depth?6:10;length+=Math.max(0,42-d)*10/42;dc.fillStyle=item.mark?'#e8be5a':'#a5adb9';dc.fillRect(24-length,Math.round(y),length,item.mark?3:1);});
  dc.fillStyle='#6eb4ff';dc.fillRect(25,this.tickY(this.top())-canvasTop-3,3,7);var current=this.current();this.save.setAttribute('aria-pressed',current?'true':'false');this.ribbon.setAttribute('fill',current?'currentColor':'none');this.save.classList.toggle('fw-nav-error',!!this.error);this.save.title=this.error||((current?'Remove bookmark':'Bookmark this position')+' (Ctrl+D); right-click for saved marks');this.canvas.setAttribute('aria-valuenow',String(Math.round(this.range()?this.top()/this.range()*100:0)));
  if(this.hover!==null&&!this.drag&&this.menu.hidden){var near=this.nearest(this.hover);if(near){this.tipTitle.textContent=near.item.title;this.tipDetail.textContent=near.item.detail+' · '+Math.round(this.range()?near.item.top/this.range()*100:0)+'%';this.tip.hidden=false;this.tip.style.top=clamp(this.hover-25,4,Math.max(4,innerHeight-70))+'px';}}else this.tip.hidden=true;
 };
 Navigation.prototype.openMenu=function(personal,y,reload){
  var self=this;this.menu.replaceChildren();var heading=document.createElement('h3');heading.textContent=personal?'Personal bookmarks':'Nearby sections';this.menu.appendChild(heading);
  if(this.error){var warning=document.createElement('p');warning.textContent=this.error;this.menu.appendChild(warning);}var items=personal?this.marks.map(function(m){return {mark:m,title:m.title};}):this.targets().filter(function(item){return Math.abs(self.tickY(item.top)-y)<=8;});
  if(!items.length){var empty=document.createElement('p');empty.textContent=personal?'No personal bookmarks here yet.':'No section at this position.';this.menu.appendChild(empty);}
  items.forEach(function(item){var row=document.createElement('div');row.className='fw-nav-item';var jump=document.createElement('button');jump.type='button';jump.textContent=(item.mark?'★ ':'')+item.title;jump.title=item.title;jump.onclick=function(){self.jump(item);};row.appendChild(jump);if(item.mark){var remove=document.createElement('button');remove.type='button';remove.textContent='×';remove.setAttribute('aria-label','Remove bookmark '+item.title);remove.onclick=function(){self.request('remove',{id:item.mark.id});};row.appendChild(remove);}self.menu.appendChild(row);});
  var toggle=document.createElement('button');toggle.type='button';toggle.className='fw-nav-toggle';toggle.textContent=this.current()?'Remove bookmark here':'Bookmark this position';toggle.onclick=function(){self.toggle();};this.menu.appendChild(toggle);this.menu.hidden=false;this.tip.hidden=true;this.menu.style.top=clamp(y,4,Math.max(4,innerHeight-Math.min(350,this.menu.scrollHeight)-8))+'px';if(personal&&reload!==false&&!this.pending)this.request('list');
 };
 global.FolderwingNavigation={Navigation:Navigation,create:function(){if(!global.FolderwingReadingRail)global.FolderwingReadingRail=new Navigation();return global.FolderwingReadingRail;}};
 if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',global.FolderwingNavigation.create);else global.FolderwingNavigation.create();
})(window);
