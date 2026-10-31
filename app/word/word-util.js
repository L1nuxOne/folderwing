(function(root){
 'use strict';
 var html=new Set('section article main div p span a br hr table thead tbody tfoot tr td th img strong b i em u s small mark sup sub ul ol li header footer blockquote h1 h2 h3 h4 h5 h6 svg g path rect circle ellipse line polyline polygon image defs use symbol title desc mjx-container'.split(' '));
 var math=new Set('math mrow mi mn mo mtext ms mspace mfrac msqrt mroot msub msup msubsup munder mover munderover mmultiscripts mprescripts none mtable mtr mtd menclose mpadded mphantom mstyle'.split(' '));
 var dangerous=new Set('script style iframe object embed form input textarea button link meta base audio video canvas mjx-assistive-mml'.split(' '));
 var props=new Set(('color background-color font font-family font-size font-style font-weight font-variant line-height text-align text-decoration text-decoration-line text-decoration-color text-transform text-indent vertical-align white-space word-break overflow-wrap letter-spacing margin margin-top margin-right margin-bottom margin-left padding padding-top padding-right padding-bottom padding-left border border-top border-right border-bottom border-left border-color border-style border-width border-collapse border-spacing width min-width max-width height min-height max-height display list-style list-style-type list-style-position table-layout direction content counter-reset counter-increment counter-set').split(' '));
 function valueSafe(v){return !!v && v.length<=2048 && !/[{};\u0000-\u001f]/.test(v) && !/(url\s*\(|expression\s*\(|javascript:|@import|behavior\s*:|-moz-binding)/i.test(v);}
 function cleanStyle(style){var result=[];for(var i=0;i<style.length;i++){var p=style[i],v=style.getPropertyValue(p);if(!(props.has(p)||/^--docx-[a-z0-9-]+$/i.test(p))||!valueSafe(v)||(p==='display' && /none/i.test(v)))continue;result.push(p+':'+v+';');}return result.join('');}
 function cleanCss(root){var result='';root.querySelectorAll('style').forEach(function(s){try{var sheet=new CSSStyleSheet();sheet.replaceSync(s.textContent);Array.from(sheet.cssRules).forEach(function(rule){if(rule.type!==CSSRule.STYLE_RULE)return;var sel=rule.selectorText;if(!sel || sel.length>2048 || /:host|::part|::slotted|[{}@]/i.test(sel))return;var body=cleanStyle(rule.style);if(body)result+=sel+'{'+body+'}\n';});}catch(e){}});return result;}
 function cleanTree(root){
  var els=Array.from(root.querySelectorAll('*'));if(els.length>150000)throw Error('Document contains too many preview elements.');
  els.forEach(function(el){if(!root.contains(el))return;var tag=el.localName.toLowerCase();
   if(dangerous.has(tag)){el.remove();return;}
   if(!html.has(tag)&&!math.has(tag)){el.replaceWith.apply(el,Array.from(el.childNodes));return;}
   var inline=el.hasAttribute('style')?cleanStyle(el.style):'';
   Array.from(el.attributes).forEach(function(a){var name=a.name.toLowerCase(),v=a.value||'',keep=false;
    if(name==='class')keep=v.length<=1024;
    else if(name==='id')keep=/^[\w:.-]{1,256}$/.test(v);
    else if(name==='style')keep=false;
    else if(name==='src' && tag==='img')keep=v.length<=24*1024*1024 && /^data:image\/(png|jpeg|gif|webp|bmp);base64,[a-z0-9+/=]+$/i.test(v);
    else if((name==='href'||name==='xlink:href') && (tag==='use'||tag==='a'||tag==='image'))keep=(tag==='use' && /^#MJX-[\w:.-]+$/.test(v)) || (tag==='image' && v.length<=24*1024*1024 && /^data:image\/(png|jpeg|gif|webp|bmp);base64,[a-z0-9+/=]+$/i.test(v));
    else if(['colspan','rowspan','start'].includes(name))keep=/^\d{1,4}$/.test(v) && Number(v)<=8192;
    else if(['width','height'].includes(name))keep=/^\d{1,4}(?:\.\d+)?(?:px|pt|em|ex|%)?$/.test(v) && parseFloat(v)<=8192;
    else if(['lang','dir','title','alt','role','aria-label','viewbox','xmlns','d','x','y','x1','y1','x2','y2','rx','ry','cx','cy','r','points','transform','fill','stroke','stroke-width','stroke-linecap','stroke-linejoin','mathvariant','display','stretchy','accent','linethickness','bevelled','rowspacing','columnspacing','notation','columnalign','rowalign','fence','separator','encoding'].includes(name))keep=v.length<=(name==='d'?100000:1024) && valueSafe(v);
    if(!keep)el.removeAttribute(a.name);
   });
   if(inline)el.setAttribute('style',inline);
  });
  return root;
 }
 // docx-preview places Word/VML text boxes in SVG foreignObject nodes. Move their
 // HTML content into normal flow before sanitizing: unwrapping it inside SVG
 // makes it invisible, while keeping the original absolute layout can clip it.
 function reflowTextBoxes(root){
  var count=0;
  Array.from(root.querySelectorAll('svg')).forEach(function(svg){
   if(!root.contains(svg) || (svg.parentElement && svg.parentElement.closest('svg')))return;
   var boxes=Array.from(svg.querySelectorAll('foreignObject')).filter(function(fo){return !fo.parentElement.closest('foreignObject');});
   if(!boxes.length)return;
   var hasGraphic=Array.from(svg.querySelectorAll('path,rect,circle,ellipse,line,polygon,polyline,image')).some(function(n){return !n.closest('foreignObject');});
   var fragment=document.createDocumentFragment();
   boxes.forEach(function(fo){var box=document.createElement('div');box.className='fw-textbox';while(fo.firstChild)box.appendChild(fo.firstChild);fo.remove();fragment.appendChild(box);count++;});
   if(hasGraphic)svg.after(fragment);else svg.replaceWith(fragment);
  });return count;
 }
 // Word can place a full-page image in a zero-sized absolute drawing next to
 // a text box. Once positions are discarded, its invisible image can still
 // reserve a page of height. Keep the image after the associated text instead.
 function reflowCollapsedImages(root){
  var groups=new Map();
  root.querySelectorAll('img').forEach(function(img){
   var paragraph=img.closest('p'),parent=img.parentElement;if(!paragraph || !parent || !paragraph.querySelector('.fw-textbox'))return;
   var imageBox=img.getBoundingClientRect(),parentBox=parent.getBoundingClientRect();
   if(imageBox.width>1 || parentBox.width>1 || parentBox.height>1)return;
   var list=groups.get(paragraph);if(!list){list=[];groups.set(paragraph,list);}list.push(img);
  });
  var count=0;groups.forEach(function(images,paragraph){var frame=document.createElement('div');frame.className='fw-reflow-images';
   images.forEach(function(img){img.removeAttribute('width');img.removeAttribute('height');img.style.removeProperty('width');img.style.removeProperty('height');frame.appendChild(img);count++;});
   paragraph.after(frame);
  });return count;
 }
 function rgba(value){var m=/^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)(?:\s*,\s*([\d.]+))?\s*\)$/i.exec(value);return m?[+m[1],+m[2],+m[3],m[4]===undefined?1:+m[4]]:null;}
 function luminance(rgb){function channel(c){c/=255;return c<=0.04045?c/12.92:Math.pow((c+0.055)/1.055,2.4);}return 0.2126*channel(rgb[0])+0.7152*channel(rgb[1])+0.0722*channel(rgb[2]);}
 function readableTextBoxes(root){
  var backgrounds=new WeakMap(),seen=new Set(),changed=0;
  function background(el){if(!el)return [255,255,255];if(backgrounds.has(el))return backgrounds.get(el);var below=background(el.parentElement),paint=rgba(getComputedStyle(el).backgroundColor),v=below;if(paint){var alpha=Math.max(0,Math.min(1,paint[3]));v=[0,1,2].map(function(i){return paint[i]*alpha+below[i]*(1-alpha);});}backgrounds.set(el,v);return v;}
  root.querySelectorAll('.fw-textbox').forEach(function(box){var walker=document.createTreeWalker(box,NodeFilter.SHOW_TEXT),node;while((node=walker.nextNode())){
   var el=node.parentElement;if(!el || !node.data.trim() || seen.has(el) || el.closest('math,svg,mjx-container'))continue;seen.add(el);
   var fg=rgba(getComputedStyle(el).color);if(!fg)continue;var bg=background(el),color=[0,1,2].map(function(i){return fg[i]*fg[3]+bg[i]*(1-fg[3]);});var a=luminance(color),b=luminance(bg),contrast=(Math.max(a,b)+0.05)/(Math.min(a,b)+0.05);
   if(contrast<2){el.style.setProperty('color',b>0.5?'#202124':'#ffffff','important');changed++;}
  }});return changed;
 }
 function unescaped(s,i){var n=0;for(var k=i-1;k>=0&&s[k]==='\\';k--)n++;return n%2===0;}
 function expressions(s){var result=[];for(var i=0;i<s.length;){var open='',close='',display=false,dollar=false;
   if(s.slice(i,i+2)==='\\(' && unescaped(s,i)){open='\\(';close='\\)';}
   else if(s.slice(i,i+2)==='\\[' && unescaped(s,i)){open='\\[';close='\\]';display=true;}
   else if(s.slice(i,i+2)==='$$' && unescaped(s,i)){open='$$';close='$$';display=true;}
   else if(s[i]==='$' && unescaped(s,i) && s[i+1] && !/[\s\d$]/.test(s[i+1])){open='$';close='$';dollar=true;}
   if(!open){i++;continue;}var end=i+open.length,found=-1;
   while((end=s.indexOf(close,end))>=0){if(unescaped(s,end)&&end>i+open.length && (!dollar||(!/\s/.test(s[end-1])&&!/\d/.test(s[end+1]||'')))){found=end;break;}end+=close.length;}
   if(found<0){i+=open.length;continue;}var value=s.slice(i+open.length,found);if(value.length<=4096 && value.trim())result.push({start:i,end:found+close.length,value:value,display:display});i=found+close.length;
  }return result;}
 function textNodes(p){var nodes=[],walker=document.createTreeWalker(p,NodeFilter.SHOW_TEXT,{acceptNode:function(n){var e=n.parentElement;return e && !e.closest('math,svg,mjx-container,code,pre')?NodeFilter.FILTER_ACCEPT:NodeFilter.FILTER_REJECT;}}),n;while((n=walker.nextNode()))nodes.push(n);return nodes;}
 function point(nodes,offset,start){for(var i=0;i<nodes.length;i++){var length=nodes[i].data.length;if(offset<length || (offset===length && (!start||i===nodes.length-1)))return [nodes[i],offset];offset-=length;}throw Error('Invalid formula position');}
 async function renderMath(root,mathjax){var done=0,errors=0,limited=false;await mathjax.startup.promise;
  for(var p of Array.from(root.querySelectorAll('p'))){var nodes=textNodes(p);if(!nodes.length)continue;var text=nodes.map(function(n){return n.data;}).join('');if(text.length>1024*1024){limited=true;continue;}var matches=expressions(text);
   for(var j=matches.length-1;j>=0;j--){if(done+errors>=256){limited=true;continue;}var m=matches[j];try{
    var output=await mathjax.tex2svgPromise(m.value,{display:m.display});if(output.querySelector('[data-mml-node="merror"],mjx-merror'))throw Error('Invalid formula');cleanTree(output);
    var a=point(nodes,m.start,true),b=point(nodes,m.end,false),range=document.createRange();range.setStart(a[0],a[1]);range.setEnd(b[0],b[1]);var span=document.createElement('span');span.className='fw-tex';span.setAttribute('role','math');span.setAttribute('aria-label',m.value);if(m.display)span.style.display='block';span.appendChild(output);range.deleteContents();range.insertNode(span);done++;
   }catch(e){errors++;}}
  }return {rendered:done,errors:errors,limited:limited};
 }
 function outline(doc,container){var map={};((doc.stylesPart&&doc.stylesPart.styles)||[]).forEach(function(s){if(!s || !s.id)return;var n=s.paragraphProps&&s.paragraphProps.outlineLevel,match=/^heading\s*([1-9])$/i.exec(s.name||'');if(typeof n!=='number'&&match)n=Number(match[1])-1;if(typeof n==='number'&&n>=0&&n<9)map['docx_'+s.id.replace(/[ .]+/g,'-').replace(/[&]+/g,'and').toLowerCase()]=n;});
  var out=[];container.querySelectorAll('p').forEach(function(p){var classes=Array.from(p.classList),found=classes.find(function(c){return Object.prototype.hasOwnProperty.call(map,c);});var label=p.textContent.trim();if(found&&label&&out.length<512)out.push({node:p,level:map[found],text:label.slice(0,200)});});return out;
 }
 root.FolderwingWord={cleanTree:cleanTree,cleanCss:cleanCss,reflowTextBoxes:reflowTextBoxes,reflowCollapsedImages:reflowCollapsedImages,readableTextBoxes:readableTextBoxes,expressions:expressions,renderMath:renderMath,outline:outline};
})(window);
