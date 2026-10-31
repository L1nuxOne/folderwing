/* Actual renderers and rail, synthetic input, bounded mock storage bridge. */
(function(){
 'use strict';
 var listeners=[],saved={},requests=0,mutations=0,kind=window.navigationCase;
 function send(data){setTimeout(function(){listeners.slice().forEach(function(f){f({data:data});});},0);}
 function data(){
  var text=Array.from({length:70},function(_,i){return '## Section '+i+'\n\n'+('Reading text '+i+' ').repeat(70)+'\n\n';}).join('');
  if(kind==='markdown')return text;
  if(kind==='notebook')return {kind:'notebook',source:JSON.stringify({nbformat:4,metadata:{},cells:Array.from({length:50},function(_,i){return {cell_type:'markdown',source:['## Cell '+i+'\n\n'+('Text ').repeat(100)]};})})};
  if(kind==='json')return {kind:'json',source:JSON.stringify(Object.fromEntries(Array.from({length:90},function(_,i){return ['field'+i,{text:'<img onerror=alert(1)>',value:i}];})))};
  if(kind==='jsonl')return {kind:'jsonl'};
  if(kind==='yaml'||kind==='toml'||kind==='ini')return {kind:'config',format:kind,source:kind==='yaml'?Array.from({length:80},function(_,i){return 'section'+i+':\n  name: value'+i;}).join('\n')+'\n---\nother: 2':kind==='toml'?Array.from({length:80},function(_,i){return '[section'+i+']\nname = "value'+i+'"';}).join('\n'):Array.from({length:80},function(_,i){return '[section'+i+']\nname=value'+i;}).join('\n')};
  if(kind==='table')return {kind:'table',delimiter:'comma'};
  if(kind==='spreadsheet')return {kind:'workbook',sheets:[{name:'First',state:'visible'},{name:'Second',state:'visible'}]};
  if(kind==='word')return {kind:'document',data:window.navigationDocx,notices:[]};
  if(kind==='xml')return {kind:'xml',nodes:200,root:{type:'document',name:'Document',children:[{type:'element',name:'root',local:'root',ns:'',children:Array.from({length:90},function(_,i){return {type:'element',name:'item',local:'item',ns:'',children:[{type:'text',value:'Content '+i}]};})}]}};
 }
 window.chrome.webview={addEventListener:function(n,f){listeners.push(f);},postMessage:function(message){
  if(message.indexOf('fw-nav:')===0){requests++;var r=JSON.parse(message.slice(7)),rows=saved[r.scope]||(saved[r.scope]=[]);if(r.op==='add'){mutations++;rows.push({id:String(mutations),page:r.page,y:r.y,title:r.title});}if(r.op==='remove'){mutations++;rows=rows.filter(function(x){return x.id!==r.id;});saved[r.scope]=rows;}send({kind:'fw-nav',scope:r.scope,token:r.token,marks:rows});return;}
  if(message==='ready')send(data());
  if(message==='next'){
   if(kind==='jsonl')send({kind:'records',records:[{number:1,text:JSON.stringify({first:1})},{number:2,text:JSON.stringify({second:2})}],end:true});
   if(kind==='table')send({kind:'rows',rows:Array.from({length:180},function(_,i){return {number:i+1,fields:['Row '+(i+1),'value'+i]};}),end:true});
  }
  if(message.indexOf('sheet:')===0)send({kind:'rows',rows:Array.from({length:180},function(_,i){return {number:i+1,cells:[{column:0,display:'Row '+(i+1),raw:String(i+1)}]};}),columns:1,end:true});
 }};
 function wait(condition){return new Promise(function(resolve,reject){var tries=0;function check(){if(condition())resolve();else if(++tries>120)reject(Error('wait for '+kind));else setTimeout(check,50);}check();});}
 function check(ok,message){if(!ok)throw Error(message);}
 function delay(ms){return new Promise(function(resolve){setTimeout(resolve,ms);});}
 window.addEventListener('DOMContentLoaded',async function(){try{
  await wait(function(){var rail=window.FolderwingReadingRail;return rail&&rail.root&&!rail.pending&&(kind==='word'?rail.anchors.length>0:kind==='table'||kind==='spreadsheet'?rail.root.querySelector('[data-nav-row]'):rail.anchors.length>0);});
  var rail=window.FolderwingReadingRail;
  check(document.querySelectorAll('.fw-nav-ui').length===1,'single rail');
  if(kind==='word'){var p=document.createElement('p');p.textContent='Long page '.repeat(1500);rail.root.querySelector('p').appendChild(p);rail.rebuild();}
  if(kind==='jsonl')check(rail.scope.includes('1'),'record scope');
  if(kind==='table'||kind==='spreadsheet'){
   check(rail.anchors.length===0,'row fallback');rail.setTop(rail.range());var row=rail.capture();check(row.page<180&&row.page>1,'bottom captures first visible source row');
   check(rail.targets()[0].detail==='Loaded rows','honest loaded range');
  }else{check(rail.anchors.length>0,'semantic ticks');rail.setTop(Math.min(rail.range(),rail.anchors[Math.min(4,rail.anchors.length-1)].top));}
  var top=rail.top(),capture=rail.capture();check(capture!==null,'reading position');rail.toggle();await wait(function(){return !rail.pending&&rail.marks.length===1;});
  check(mutations===1,'save once');check(rail.save.getAttribute('aria-pressed')==='true','filled ribbon');
  rail.setTop(0);rail.jump({mark:rail.marks[0]});check(Math.abs(rail.top()-top)<3,'restore position');
  rail.openMenu(true,35);await wait(function(){return !rail.pending;});check(rail.menu.textContent.includes(capture.title),'bookmark menu');check(!rail.menu.querySelector('img,script'),'titles remain text');
  rail.toggle();await wait(function(){return !rail.pending&&rail.marks.length===0;});check(mutations===2,'remove once');
  if(kind==='spreadsheet'||kind==='jsonl'||kind==='yaml'){
   var old=rail.scope;rail.toggle();await wait(function(){return !rail.pending&&rail.marks.length===1;});
   if(kind==='spreadsheet'){var sheet=document.getElementById('sheet');sheet.value='1';sheet.dispatchEvent(new Event('change'));}
   else document.querySelector(kind==='jsonl'?'#record-list button:nth-child(2)':'#document-list button:nth-child(2)').click();
   await wait(function(){return rail.scope!==old&&!rail.pending;});check(!rail.marks.length,'separate contexts');
   if(kind==='spreadsheet'){sheet.value='0';sheet.dispatchEvent(new Event('change'));}else document.querySelector(kind==='jsonl'?'#record-list button':'#document-list button').click();
   await wait(function(){return rail.scope===old&&!rail.pending;});check(rail.marks.length===1,'return restores context marks');
  }
  if(kind==='json'){var hidden={page:1,y:0,title:'$["field89"]["text"]'};check(rail.markTop(hidden)===null,'collapsed branch initially absent');await rail.jump({mark:hidden});check(rail.markTop(hidden)!==null,'bookmark reveals collapsed branch');}
  if(kind==='notebook'){var late={page:50,y:0,title:'nb-cell-49'};check(!document.getElementById('nb-cell-49'),'late notebook cell initially unloaded');await rail.jump({mark:late});check(document.getElementById('nb-cell-49')&&rail.markTop(late)!==null,'bookmark loads later notebook cell');}
  rail.menu.hidden=true;rail.hover=null;rail.draw();await delay(400);var count=requests;await delay(600);check(!rail.scheduled&&requests===count,'no idle rebuild or storage polling');
  rail.request('list');var stale=rail.token-1;send({kind:'fw-nav',scope:rail.scope,token:String(stale),marks:[{id:'wrong',page:0,y:0,title:'stale'}]});await wait(function(){return !rail.pending;});check(!rail.marks.some(function(m){return m.id==='wrong';}),'stale response rejected');
  document.documentElement.dataset.navTest='passed';
 }catch(e){document.documentElement.dataset.navTest=e.message;}});
})();
