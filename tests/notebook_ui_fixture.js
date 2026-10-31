/* Synthetic browser fixture; does not execute or load notebook content. */
(function(){
  'use strict';
  var callback, png='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==';
  var cells=[
    {cell_type:'markdown',source:['# Heading\n','\\[\nx^2\n\\]\n','<script>window.pwned=1</script>\n','![remote](https://example.com/x.png)']},
    {cell_type:'code',execution_count:1,source:['import sys\n','print("<script>bad</script>")\n'],outputs:[{output_type:'stream',name:'stdout',text:['saved text\n']},{output_type:'display_data',data:{'image/png':png}}]}
  ];
  for(var i=2;i<41;i++)cells.push({cell_type:'code',source:['print(1)'],outputs:[]});
  cells.push({cell_type:'markdown',source:['## Last Section']});
  var doc={nbformat:4,metadata:{language_info:{name:'python'}},cells:cells};
  window.chrome.webview={addEventListener:function(name,fn){callback=fn;},postMessage:function(message){
    if(message==='ready')setTimeout(function(){callback({data:{kind:'notebook',source:JSON.stringify(doc),limited:false}});},0);
    if(message.indexOf('notebook:')===0)setTimeout(function(){
      try{
        if(message!=='notebook:40:0')throw Error('render count '+message);
        if(document.querySelectorAll('.cell').length!==40)throw Error('first batch');
        if(!document.querySelector('mjx-container'))throw Error('math');
        if(!document.querySelector('.hljs-keyword'))throw Error('code highlighting');
        var image=document.querySelector('.output img');
        if(!image || !image.src.startsWith('data:image/png;base64,') || image.naturalWidth!==1)throw Error('PNG output');
        if(!document.querySelector('.output').textContent.includes('saved text'))throw Error('text output');
        if(window.pwned || document.querySelector('main script'))throw Error('notebook HTML executed');
        if(document.querySelector('img[src^="https"]'))throw Error('remote image');
        var buttons=document.querySelectorAll('#headings button');
        if(buttons.length!==2 || !buttons[1].textContent.includes('Last Section'))throw Error('outline');
        buttons[1].click();
        setTimeout(function(){document.documentElement.dataset.test=document.querySelectorAll('.cell').length===42?'passed':'outline navigation';},500);
      }catch(e){document.documentElement.dataset.test=e.message;}
    },200);
  }};
})();
