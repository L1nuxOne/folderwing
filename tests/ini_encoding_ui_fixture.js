/* Added only to a private test copy of the structured preview. */
window.addEventListener('DOMContentLoaded',function(){
  'use strict';
  var step=0,tree=document.getElementById('tree'),status=document.getElementById('status'),encoding=document.getElementById('ini-encoding'),pending=false;
  function check(){
    pending=false;if(step===3)return;
    var section=Array.from(tree.querySelectorAll('details')).find(function(d){return d.firstElementChild && d.firstElementChild.textContent.includes('[General]');});
    if(!section)return;
    if(!section.open){section.open=true;return;}
    var encodingStatus=status.textContent.toLowerCase();
    if(step===0 && encodingStatus.includes('windows-1252 (guessed)') && tree.textContent.includes('café')){
      step=1;encoding.value='utf8';encoding.dispatchEvent(new Event('change'));
    }else if(step===1 && encodingStatus.includes('utf-8') && tree.textContent.includes('caf�')){
      step=2;encoding.value='windows1252';encoding.dispatchEvent(new Event('change'));
    }else if(step===2 && encodingStatus.includes('windows-1252') && !encodingStatus.includes('(guessed)') && tree.textContent.includes('café')){
      step=3;window.chrome.webview.postMessage('structured:99:0');
    }
  }
  function schedule(){if(!pending){pending=true;setTimeout(check,20);}}
  var observer=new MutationObserver(schedule);observer.observe(tree,{childList:true,subtree:true});observer.observe(status,{childList:true,subtree:true,characterData:true});schedule();
});
