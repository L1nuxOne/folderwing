/* Owned VML textbox fixture, mock clipboard and mock parent. */
(function(){'use strict';var callback,copied='';document.execCommand=function(c){if(c==='copy')copied=document.activeElement.value;return true;};
 window.chrome.webview={addEventListener:function(n,f){callback=f;},postMessage:function(m){
  if(m==='ready')setTimeout(function(){callback({data:{kind:'document',data:window.syntheticTextboxDocx,notices:[]}});},0);
  if(m.indexOf('word:')===0)setTimeout(function(){try{
   if(m!=='word:15:0')throw Error('textbox equation: '+m);
   var root=document.getElementById('viewer').shadowRoot,boxes=root.querySelectorAll('.fw-textbox');if(boxes.length!==1 || root.querySelector('foreignObject'))throw Error('textbox reflow');
   var box=boxes[0],ps=Array.from(box.querySelectorAll('p'));
   function text(s){return ps.find(function(p){return p.textContent.includes(s);});}
   var white=text('White text on an omitted background'),dark=text('White text on a dark background'),green=text('Readable green text'),math=text('Box math');
   if(!white||!dark||!green||!math||!math.querySelector('.fw-tex'))throw Error('textbox contents');
   var whiteNode=white.querySelector('span'),darkNode=dark.querySelector('span'),greenNode=green.querySelector('span');
   if(getComputedStyle(whiteNode).color!=='rgb(32, 33, 36)')throw Error('white text contrast');
   if(getComputedStyle(darkNode).color!=='rgb(255, 255, 255)')throw Error('dark background contrast preserved');
   if(getComputedStyle(greenNode).color!=='rgb(0, 128, 0)')throw Error('readable color preserved');
   var range=document.createRange();range.selectNodeContents(whiteNode);var rect=range.getBoundingClientRect();if(rect.width<=0||rect.height<=0)throw Error('textbox text is invisible');
   window.getSelection().removeAllRanges();window.getSelection().addRange(range);document.getElementById('copy').click();if(!copied.includes('White text'))throw Error('textbox copy');
   var f=document.getElementById('find');f.value='omitted background';f.dispatchEvent(new Event('input'));if(!document.getElementById('find-status').textContent.includes('1 / 1'))throw Error('textbox search');
   var notice=document.getElementById('notice').textContent;if(!notice.includes('Text boxes')||!notice.includes('colors'))throw Error('textbox notice');
   var holder=document.createElement('div'),parent=document.createElement('p'),collapsed=document.createElement('span'),zero=document.createElement('div'),fake=document.createElement('img'),after=document.createElement('div');collapsed.appendChild(zero);zero.style.width='0px';zero.style.height='0px';fake.src=root.querySelector('img').src;fake.style.width='400px';fake.style.height='800px';zero.appendChild(fake);parent.appendChild(collapsed);after.className='fw-textbox';after.textContent='Associated visible text';parent.appendChild(after);holder.appendChild(parent);root.appendChild(holder);
   if(fake.getBoundingClientRect().width>1)throw Error('collapsed image fixture');if(FolderwingWord.reflowCollapsedImages(holder)!==1 || holder.querySelector('.fw-reflow-images img')!==fake || fake.getBoundingClientRect().width<=0 || !parent.nextElementSibling.classList.contains('fw-reflow-images'))throw Error('collapsed image reflow');
   var other=document.createElement('p'),otherBox=document.createElement('div'),normal=document.createElement('img');otherBox.className='fw-textbox';other.appendChild(otherBox);normal.src=fake.src;normal.style.width='30px';normal.style.height='30px';other.appendChild(normal);holder.appendChild(other);if(FolderwingWord.reflowCollapsedImages(other)!==0 || normal.parentElement!==other)throw Error('normal image placement');holder.remove();
   var hostile=document.createElement('div'),svg=document.createElementNS('http://www.w3.org/2000/svg','svg'),fo=document.createElementNS('http://www.w3.org/2000/svg','foreignObject'),inner=document.createElement('div');inner.innerHTML='<script>window.bad=1</script><a href="javascript:window.bad=2" onclick="window.bad=3">text</a>';fo.appendChild(inner);svg.appendChild(fo);hostile.appendChild(svg);FolderwingWord.reflowTextBoxes(hostile);FolderwingWord.cleanTree(hostile);if(hostile.querySelector('script,a[href],[onclick],foreignObject')||!hostile.textContent.includes('text'))throw Error('textbox safety');
   document.documentElement.dataset.test='passed';
  }catch(e){document.documentElement.dataset.test=e.message;}},50);
 }};
})();
