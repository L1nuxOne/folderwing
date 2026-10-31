window.addEventListener('DOMContentLoaded',function(){
  'use strict';
  var status=document.getElementById('status'),select=document.getElementById('delimiter'),encoding=document.getElementById('encoding'),header=document.getElementById('header'),filter=document.getElementById('filter');
  var more=document.getElementById('more'),restart=document.getElementById('restart'),prev=document.getElementById('prev'),next=document.getElementById('next');
  var head=document.getElementById('head'),body=document.getElementById('body'),columnStatus=document.getElementById('column-status'),sortStatus=document.getElementById('sort-status');
  var rows=[],first=null,units=0,ended=false,loading=false,columnStart=0,columnCount=0,sortColumn=-1,sortDirection=1,cellValue='';
  var pageColumns=24, maxRows=512, maxUnits=4*1024*1024;
  function request(message){if(loading)return;loading=true;more.disabled=restart.disabled=select.disabled=encoding.disabled=true;window.chrome.webview.postMessage(message);}
  function reset(){rows=[];first=null;units=0;ended=false;columnStart=0;sortColumn=-1;head.replaceChildren();body.replaceChildren();}
  function length(row){return row.fields?row.fields.reduce(function(n,v){return n+v.length;},0):0;}
  function displayCell(value){cellValue=value;document.getElementById('value').textContent=value;document.getElementById('detail').hidden=false;}
  function render(){
    var query=filter.value.toLocaleLowerCase(),visible=rows.filter(function(row){
      if(header.checked && row.number===1 && !row.error)return false;
      return !query || (row.error||'').toLocaleLowerCase().includes(query) || (row.fields||[]).some(function(v){return v.toLocaleLowerCase().includes(query);});
    });
    if(sortColumn>=0)visible.sort(function(a,b){
      if(a.error||b.error)return (a.error?1:0)-(b.error?1:0) || a.number-b.number;
      var av=(a.fields[sortColumn]||''),bv=(b.fields[sortColumn]||'');
      return sortDirection*av.localeCompare(bv) || a.number-b.number;
    });
    columnCount=first&&first.fields?first.fields.length:0;
    rows.forEach(function(r){if(r.fields)columnCount=Math.max(columnCount,r.fields.length);});
    if(columnStart>=columnCount)columnStart=Math.max(0,Math.floor((columnCount-1)/pageColumns)*pageColumns);
    var end=Math.min(columnCount,columnStart+pageColumns),tr=document.createElement('tr'),number=document.createElement('th');number.textContent='Row';tr.appendChild(number);
    for(var c=columnStart;c<end;c++)(function(index){
      var th=document.createElement('th'),b=document.createElement('button');
      var name=header.checked && first && first.fields && first.fields[index]!==undefined?first.fields[index]:'Column '+(index+1);
      b.textContent=(name||'(empty)')+(sortColumn===index?(sortDirection===1?' ▲':' ▼'):'');
      b.title='Sort loaded rows by this column';b.addEventListener('click',function(){if(sortColumn===index){if(sortDirection===1)sortDirection=-1;else sortColumn=-1;}else{sortColumn=index;sortDirection=1;}render();});th.appendChild(b);tr.appendChild(th);
    })(c);
    head.replaceChildren(tr);body.replaceChildren();
    visible.forEach(function(row){
      var tr=document.createElement('tr'),n=document.createElement('td');tr.dataset.navRow=String(row.number);n.className='row-number';n.textContent=row.number;tr.appendChild(n);
      if(row.error){var e=document.createElement('td');e.className='error';e.colSpan=Math.max(1,end-columnStart);e.textContent=row.error;tr.appendChild(e);}
      else for(var c=columnStart;c<end;c++)(function(index){var td=document.createElement('td'),v=row.fields[index];if(v===undefined){td.textContent='—';td.title='This row has no field for this column';}else{td.textContent=v.length>2000?v.slice(0,2000)+'…':v;td.title='Click to view or copy the full value';td.addEventListener('click',function(){displayCell(v);});}tr.appendChild(td);})(c);
      body.appendChild(tr);
    });
    var start=rows.length?rows[0].number:0,last=rows.length?rows[rows.length-1].number:0;
    status.textContent=rows.length?'Loaded rows '+start+'–'+last+' · '+visible.length+' shown'+(start>1?' · earlier rows released':'')+(ended?' · end of file':''):'No rows';
    columnStatus.textContent=columnCount?'Columns '+(columnStart+1)+'–'+end+' of '+columnCount:'No columns';
    sortStatus.textContent=sortColumn>=0?'Sorting loaded rows as text':'';prev.disabled=columnStart===0;next.disabled=end>=columnCount;
  }
  function add(batch){
    loading=false;ended=batch.end;
    batch.rows.forEach(function(row){if(!first)first=row;rows.push(row);units+=length(row);});
    while(rows.length>maxRows || (units>maxUnits && rows.length>1)){units-=length(rows.shift());}
    more.disabled=ended;restart.disabled=select.disabled=encoding.disabled=false;more.textContent=ended?'End of file':'Load more rows';render();
    window.chrome.webview.postMessage('table:'+batch.rows.length+':'+batch.rows.filter(function(row){return !!row.error;}).length);
  }
  more.addEventListener('click',function(){request('next');});restart.addEventListener('click',function(){request('reset');});
  select.addEventListener('change',function(){request('delimiter:'+select.value);});header.addEventListener('change',render);filter.addEventListener('input',render);
  encoding.addEventListener('change',function(){request('encoding:'+encoding.value);});
  prev.addEventListener('click',function(){columnStart=Math.max(0,columnStart-pageColumns);render();});next.addEventListener('click',function(){columnStart+=pageColumns;render();});
  document.getElementById('close').addEventListener('click',function(){document.getElementById('detail').hidden=true;});
  document.getElementById('copy').addEventListener('click',function(){var area=document.createElement('textarea');area.value=cellValue;area.style.position='fixed';area.style.left='-10000px';document.body.appendChild(area);area.select();var ok=document.execCommand('copy');area.remove();status.textContent=ok?'Copied cell':'Clipboard unavailable';});
  window.chrome.webview.addEventListener('message',function(event){var data=event.data;
    if(data.kind==='table'){select.value=data.delimiter;request('next');}
    else if(data.kind==='reset'){reset();loading=false;request('next');}
    else if(data.kind==='rows')add(data);
  });
  window.chrome.webview.postMessage('ready');
});
