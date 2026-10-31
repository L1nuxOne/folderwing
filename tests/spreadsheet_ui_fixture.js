/* Synthetic model and fake clipboard: no local documents or desktop input. */
(function(){
 var cb,copied='',phase=0;
 document.execCommand=function(c){if(c==='copy')copied=document.activeElement.value;return true;};
 function send(d){setTimeout(function(){cb({data:d});},0);}
 function row(n,v,formula){return {number:n,cells:[{column:0,display:v,raw:v,formula:formula===undefined?null:formula,note:formula?'Shared formula template.':null,missing:false,truncated:false},{column:1,display:'v'+n,raw:'v'+n,formula:null}]};}
 function clickCell(r,c,shift){var td=document.querySelectorAll('#body tr')[r].querySelectorAll('td')[c+1];td.dispatchEvent(new MouseEvent('click',{bubbles:true,shiftKey:shift}));}
 window.chrome.webview={addEventListener:function(n,f){cb=f;},postMessage:function(m){
  if(m==='ready')send({kind:'workbook',sheets:[{name:'First',state:'visible'},{name:'Hidden',state:'hidden'},{name:'Empty',state:'visible'}]});
  else if(m==='sheet:0')send({kind:'rows',rows:[row(1,'<img onerror=alert(1)>','B1+1'),row(2,'apple')],end:false,columns:30,columnsLimited:false});
  else if(m==='next')send({kind:'rows',rows:[row(3,'banana')],end:true,columns:30,columnsLimited:false});
  else if(m==='sheet:1')send({kind:'rows',rows:[row(1,'hidden data')],end:true,columns:2,columnsLimited:false});
  else if(m==='sheet:2')send({kind:'rows',rows:[],end:true,columns:0,columnsLimited:false});
  else if(m.indexOf('table:')===0)setTimeout(function(){try{
   if(phase===0){
    if(document.querySelector('#body img'))throw Error('unsafe document content');
    if(document.getElementById('sheet').options[1].textContent!=='Hidden (hidden)')throw Error('hidden sheet label');
    if(!document.getElementById('column-status').textContent.includes('A–X'))throw Error('column paging');
    document.getElementById('next').click();if(!document.getElementById('column-status').textContent.includes('Y–AD'))throw Error('next columns');document.getElementById('prev').click();
    clickCell(0,0,false);clickCell(1,1,true);document.getElementById('copy').click();if(copied!=='<img onerror=alert(1)>\tv1\napple\tv2')throw Error('rectangular copy');
    document.getElementById('formulas').click();if(!document.getElementById('body').textContent.includes('=B1+1'))throw Error('formula toggle');
    clickCell(0,0,false);document.querySelector('#body tr td:nth-child(2)').dispatchEvent(new MouseEvent('dblclick',{bubbles:true}));if(!document.getElementById('value').textContent.includes('may be stale'))throw Error('formula detail');document.getElementById('close').click();
    phase=1;document.getElementById('more').click();
   }else if(phase===1){
    if(document.querySelectorAll('#body tr').length!==3)throw Error('incremental rows');var f=document.getElementById('filter');f.value='banana';f.dispatchEvent(new Event('input'));if(document.querySelectorAll('#body tr').length!==1)throw Error('loaded row filter');f.value='';f.dispatchEvent(new Event('input'));
    phase=2;var s=document.getElementById('sheet');s.value='1';s.dispatchEvent(new Event('change'));
   }else if(phase===2){if(!document.getElementById('body').textContent.includes('hidden data'))throw Error('sheet switch');phase=3;var s=document.getElementById('sheet');s.value='2';s.dispatchEvent(new Event('change'));}else{if(!document.getElementById('status').textContent.includes('Empty sheet'))throw Error('empty sheet status');document.documentElement.dataset.test='passed';}
  }catch(e){document.documentElement.dataset.test=e.message;}},50);
 }};
})();
