/* Owned synthetic DOCX and a mock parent; no user input or network. */
(function(){'use strict';var callback,copied='';document.execCommand=function(c){if(c==='copy')copied=document.activeElement.value;return true;};
 window.chrome.webview={addEventListener:function(n,f){callback=f;},postMessage:function(m){
  if(m==='ready')setTimeout(function(){callback({data:{kind:'document',data:window.syntheticDocx,notices:['Synthetic external link omitted.']}});},0);
  if(m.indexOf('word:')===0)setTimeout(async function(){try{
   if(m!=='word:14:0')throw Error('expected twelve Word equations and two LaTeX expressions: '+m);
   var root=document.getElementById('viewer').shadowRoot;if(!root)throw Error('missing isolated document');
   if(root.querySelectorAll('math').length!==12)throw Error('native equations');
   if(!root.querySelector('msubsup') || !root.querySelector('mover[accent="true"]') || !root.querySelector('menclose[notation="box"]') || !root.querySelector('mphantom') || !root.querySelector('mfrac[linethickness="0"]') || !root.querySelector('mfrac[bevelled="true"]') || !Array.from(root.querySelectorAll('math')).some(function(m){return m.textContent==='e/f';}))throw Error('extended native equation structures');
   if(root.querySelectorAll('.fw-tex').length!==2 || root.querySelectorAll('.fw-tex svg').length!==2)throw Error('cross-run LaTeX');
   var separate=document.createElement('div'),para=document.createElement('p');['Before \\', '(x+', '1\\', ') after'].forEach(function(piece){var n=document.createElement('span');n.textContent=piece;para.appendChild(n);});separate.appendChild(para);var segmented=await FolderwingWord.renderMath(separate,window.MathJax);if(segmented.rendered!==1 || !para.querySelector('.fw-tex') || !para.textContent.includes('Before') || !para.textContent.includes('after'))throw Error('delimiters split across formatting runs');
   var multiple=document.createElement('div'),line=document.createElement('p');line.textContent='One \\(a\\) then \\(b\\) end';multiple.appendChild(line);var rendered=await FolderwingWord.renderMath(multiple,window.MathJax);if(rendered.rendered!==2 || line.querySelectorAll('.fw-tex').length!==2 || !line.textContent.includes('end'))throw Error('multiple formulas in one run');
   var split=Array.from(root.querySelectorAll('p')).find(function(p){return p.textContent.includes('Split');});if(!split || !split.querySelector('.fw-tex') || split.textContent.includes('\\(x^2\\)'))throw Error('split formula location');
   if(root.querySelector('a[href],script,iframe,object,embed'))throw Error('unsafe markup');
   var hostile=document.createElement('div');hostile.innerHTML='<a href="javascript:window.bad=1" onclick="window.bad=2">link</a><img src="https://invalid.test/x" onerror="window.bad=3"><iframe src="https://invalid.test/"></iframe><span style="position:fixed;display:none;color:red">x</span>';FolderwingWord.cleanTree(hostile);if(hostile.querySelector('a[href],img[src],iframe,[onclick],[onerror]')||/position|display:none/.test(hostile.querySelector('span').getAttribute('style')||''))throw Error('sanitization');
   var sheets=document.createElement('div'),style=document.createElement('style');style.textContent='@import url(https://invalid.test/x); :host {color:red} .docx {position:fixed;color:blue;background-image:url(https://invalid.test/x)}';sheets.appendChild(style);var safe=FolderwingWord.cleanCss(sheets);if(/position|url|:host/i.test(safe)||!safe.includes('color'))throw Error('stylesheet isolation');
   if(root.querySelectorAll('img').length!==1)throw Error('embedded image');
   if(root.querySelectorAll('table').length!==1)throw Error('table');
   if(document.getElementById('outline').hidden || !document.getElementById('headings').textContent.includes('Example heading'))throw Error('outline');
   if(document.getElementById('notice').hidden)throw Error('omission notice');
   var node=Array.from(root.querySelectorAll('p')).find(function(p){return p.textContent.includes('Ordinary');}).querySelector('span').firstChild;var range=document.createRange();range.selectNodeContents(node);window.getSelection().removeAllRanges();window.getSelection().addRange(range);document.getElementById('copy').click();if(!copied.includes('Ordinary'))throw Error('copy selection');
   var input=document.getElementById('find');input.value='ordinary bold';input.dispatchEvent(new Event('input'));if(document.getElementById('find-status').textContent.indexOf('1 / 1')<0)throw Error('cross-run find');
   document.documentElement.dataset.test='passed';
  }catch(e){document.documentElement.dataset.test=e.message;}},50);
 }};
})();
