/* Synthetic configuration browser fixture; no external resources or clipboard changes. */
(function(){
  'use strict';
  var callback,phase=0;
  var yaml='defaults: &defaults\n  big: 9223372036854775807\n  name: "<img src=x onerror=alert(1)>"\ncopy: *defaults\n---\ncustom: !widget hello\n';
  var toml='big=9223372036854775807\nwhen=1979-05-27T07:32:00Z\n[owner]\nname="Rose"\n[[fruit]]\nname="apple"\n[[fruit]]\nname="banana"\n';
  var ini='global=on\n[General]\nx=one\nx=two\nunknown command\n[General]\nx=three\n';
  window.chrome.webview={addEventListener:function(name,fn){callback=fn;},postMessage:function(message){
    if(message==='ready')setTimeout(function(){callback({data:{kind:'config',format:'yaml',source:yaml,limited:false}});},0);
    if(message==='ini-encoding:windows1252')setTimeout(function(){callback({data:{kind:'config',format:'ini',source:ini,limited:false,encoding:'windows-1252'}});},0);
    if(message.indexOf('structured:')===0)setTimeout(function(){
      try{
        if(phase===0){
          if(message!=='structured:2:0')throw Error('YAML documents');
          var buttons=document.querySelectorAll('#document-list button');if(buttons.length!==2)throw Error('document selector');
          if(!document.getElementById('tree').textContent.includes('*defaults'))throw Error('alias');
          var anchor=Array.from(document.querySelectorAll('#tree button')).find(function(b){return b.textContent==='Go to anchor';});
          if(!anchor)throw Error('anchor action');anchor.click();
          if(!document.querySelector('.match') || !document.querySelector('.match').textContent.includes('defaults'))throw Error('anchor navigation');
          if(document.querySelector('main img'))throw Error('unsafe HTML');
          buttons[1].click();if(!document.getElementById('status').textContent.includes('!widget'))throw Error('tag warning');
          phase=1;callback({data:{kind:'config',format:'toml',source:toml,limited:false}});
        } else if(phase===1) {
          if(message!=='structured:1:0')throw Error('TOML document');
          var view=document.getElementById('tree');
          if(!view.textContent.includes('9223372036854775807') || !view.textContent.includes('1979-05-27T07:32:00Z'))throw Error('exact values');
          if(!view.textContent.includes('owner') || !view.textContent.includes('fruit'))throw Error('tables');
          var input=document.getElementById('find');input.value='banana';input.dispatchEvent(new Event('input'));
          if(!document.getElementById('find-status').textContent.includes('1 match'))throw Error('search');
          document.getElementById('next').click();
          if(!document.querySelector('.match') || !document.querySelector('.match').textContent.includes('banana'))throw Error('search navigation');
          if(!view.textContent.includes('[1]:'))throw Error('array indices');
          phase=2;callback({data:{kind:'config',format:'toml',source:'when=2023-02-30\n',limited:false}});
        } else if(phase===2) {
          if(message!=='structured:0:1')throw Error('invalid TOML result');
          if(!document.getElementById('tree').textContent.includes('line 1'))throw Error('error position');
          phase=3;callback({data:{kind:'config',format:'ini',source:ini,limited:false,encoding:'utf-8'}});
        } else if(phase===3) {
          if(message!=='structured:1:0')throw Error('INI result');
          var iniTree=document.getElementById('tree');
          if(document.getElementById('ini-controls').hidden || !document.getElementById('status').textContent.includes('1 unparsed line'))throw Error('INI status');
          var sections=Array.from(iniTree.querySelectorAll('summary')).filter(function(s){return s.textContent.includes('[General]');});
          if(sections.length!==2 || sections.some(function(s){return Array.from(s.querySelectorAll('button')).some(function(b){return b.textContent==='Copy path';});}))throw Error('repeated sections');
          var iniFind=document.getElementById('find');iniFind.value='two';iniFind.dispatchEvent(new Event('input'));document.getElementById('next').click();
          if(!document.querySelector('.match') || !document.querySelector('.match').textContent.includes('two'))throw Error('INI search');
          var encoding=document.getElementById('ini-encoding');phase=4;encoding.value='windows1252';encoding.dispatchEvent(new Event('change'));
        } else {
          if(message!=='structured:1:0' || !document.getElementById('status').textContent.includes('windows-1252'))throw Error('INI encoding control');
          document.documentElement.dataset.test='passed';
        }
      }catch(e){document.documentElement.dataset.test=e.message;}
    },100);
  }};
})();
