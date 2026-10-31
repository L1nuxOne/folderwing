window.addEventListener('DOMContentLoaded',function(){
 'use strict';
 var el=function(id){return document.getElementById(id)},status=el('status'),sheet=el('sheet'),filter=el('filter'),formulas=el('formulas'),more=el('more'),restart=el('restart'),copy=el('copy'),head=el('head'),body=el('body');
 var rows=[],units=0,columns=0,start=0,ended=false,loading=false,limited=false,sort=-1,direction=1,selection=null,detail='';
 var pageSize=24,maxRows=512,maxUnits=4*1024*1024;
 function post(m){window.chrome.webview.postMessage(m);}
 function busy(v){loading=v;sheet.disabled=v||!sheet.options.length;more.disabled=v||ended;restart.disabled=v||!sheet.options.length;}
 function request(m){if(loading)return;busy(true);post(m);}
 function clear(){rows=[];units=0;columns=0;start=0;ended=false;limited=false;sort=-1;selection=null;copy.disabled=true;}
 function letter(n){var s='';for(n++;n>0;n=Math.floor((n-1)/26))s=String.fromCharCode(65+(n-1)%26)+s;return s;}
 function cell(r,c){return r.cells.find(function(x){return x.column===c;})||null;}
 function text(c){if(!c)return '';if(formulas.checked && c.formula!==null && c.formula!==undefined)return c.formula?'='+c.formula:(c.note||'[Formula text unavailable]');return c.display;}
 function copyText(value){var a=document.createElement('textarea');a.value=value;a.style.position='fixed';a.style.left='-10000px';document.body.appendChild(a);a.select();document.execCommand('copy');a.remove();}
 function visible(){var q=filter.value.toLocaleLowerCase(),arr=rows.filter(function(r){return !q||r.cells.some(function(c){return (c.display+' '+(c.formula||'')).toLocaleLowerCase().includes(q);});});if(sort>=0)arr.sort(function(a,b){return direction*text(cell(a,sort)).localeCompare(text(cell(b,sort)),undefined,{numeric:true})||a.number-b.number;});return arr;}
 function selectCell(index,column,event,arr){
   if(event.shiftKey && selection){selection.endRow=index;selection.endCol=column;}else selection={startRow:index,endRow:index,startCol:column,endCol:column};
   copy.disabled=false;var c=cell(arr[index],column),lines=['Cell '+letter(column)+arr[index].number,'Displayed: '+(c?c.display:''),'Raw: '+(c?c.raw:'')];
   if(c && c.formula!==null && c.formula!==undefined){lines.push('Formula: '+(c.formula?'='+c.formula:'[formula text unavailable]'));if(c.note)lines.push(c.note);if(c.missing)lines.push('No saved result.');lines.push('Saved formula results may be stale. Formulas are not recalculated.');}
   if(c && c.truncated)lines.push('The displayed value or formula was truncated for preview.');detail=c?c.display:'';el('value').textContent=lines.join('\n');render();
 }
 function render(){var arr=visible(),tr=document.createElement('tr'),corner=document.createElement('th');corner.textContent='#';tr.appendChild(corner);
   var end=Math.min(columns,start+pageSize);for(var i=start;i<end;i++){(function(col){var th=document.createElement('th'),b=document.createElement('button');b.textContent=letter(col);b.title='Sort loaded rows by '+letter(col);b.onclick=function(){direction=sort===col?-direction:1;sort=col;selection=null;copy.disabled=true;render();};th.appendChild(b);tr.appendChild(th);})(i);}head.replaceChildren(tr);body.replaceChildren();
   arr.forEach(function(r,index){var line=document.createElement('tr'),num=document.createElement('td');line.dataset.navRow=String(r.number);num.className='row-number';num.textContent=r.number;line.appendChild(num);
    for(var j=start;j<end;j++){(function(col){var td=document.createElement('td'),c=cell(r,col);td.textContent=text(c);td.title='Click to select; double-click for details';if(c && c.formula!==null && c.formula!==undefined)td.classList.add('formula');if(selection && index>=Math.min(selection.startRow,selection.endRow)&&index<=Math.max(selection.startRow,selection.endRow)&&col>=Math.min(selection.startCol,selection.endCol)&&col<=Math.max(selection.startCol,selection.endCol))td.classList.add('selected');td.onclick=function(e){selectCell(index,col,e,arr);};td.ondblclick=function(){el('detail').hidden=false;};line.appendChild(td);})(j);}body.appendChild(line);});
   el('column-status').textContent=columns?'Columns '+letter(start)+'–'+letter(end-1)+' of '+letter(columns-1)+(limited?' (limited to 4,096)':''):'No columns yet';el('prev').disabled=start===0;el('next').disabled=end>=columns;el('sort-status').textContent=sort<0?'Sort and filter apply to loaded rows':('Sorted by '+letter(sort)+'; loaded rows only');
   status.textContent=rows.length?rows.length+' loaded rows'+(rows[0].number>1?' (earlier rows released)':'')+(ended?' · end of sheet':'')+(rows.some(function(r){return r.omitted;})?' · some wide rows truncated':'')+' · formula results can be stale':(ended?'Empty sheet':'No rows loaded yet');status.className='';
 }
 sheet.onchange=function(){clear();render();request('sheet:'+sheet.value);};more.onclick=function(){request('next');};restart.onclick=function(){clear();render();request('sheet:'+sheet.value);};
 el('prev').onclick=function(){start=Math.max(0,start-pageSize);selection=null;copy.disabled=true;render();};el('next').onclick=function(){start+=pageSize;selection=null;copy.disabled=true;render();};filter.oninput=function(){selection=null;copy.disabled=true;render();};formulas.onchange=render;
 copy.onclick=function(){if(!selection)return;var arr=visible(),lines=[];for(var r=Math.min(selection.startRow,selection.endRow);r<=Math.max(selection.startRow,selection.endRow);r++){var fields=[];for(var c=Math.min(selection.startCol,selection.endCol);c<=Math.max(selection.startCol,selection.endCol);c++){var v=text(cell(arr[r],c));fields.push(/[\t\r\n"]/.test(v)?'"'+v.replace(/"/g,'""')+'"':v);}lines.push(fields.join('\t'));}copyText(lines.join('\r\n'));};
 document.addEventListener('keydown',function(e){if(e.ctrlKey && e.key.toLowerCase()==='c' && selection && el('detail').hidden){copy.click();e.preventDefault();}});
 el('close').onclick=function(){el('detail').hidden=true;};el('copy-detail').onclick=function(){copyText(detail);};
 window.chrome.webview.addEventListener('message',function(e){var data=e.data;
  if(data.kind==='workbook'){sheet.replaceChildren();data.sheets.forEach(function(s,i){var o=document.createElement('option');o.value=i;o.textContent=s.name+(s.state==='visible'?'':' ('+s.state+')');sheet.appendChild(o);});busy(false);if(sheet.options.length)request('sheet:0');}
  else if(data.kind==='rows'){data.rows.forEach(function(r){r._units=r.cells.reduce(function(n,c){return n+(c.display||'').length+(c.raw||'').length+(c.formula||'').length;},0);rows.push(r);units+=r._units;while(rows.length>maxRows || (units>maxUnits && rows.length>1))units-=rows.shift()._units;});columns=data.columns;limited=data.columnsLimited;ended=data.end;busy(false);selection=null;copy.disabled=true;render();post('table:'+rows.length+':0');}
  else if(data.kind==='error'){busy(false);ended=true;more.disabled=true;status.textContent=data.message;status.className='error';post('table:0:1');}
 });post('ready');
});
