/* Jupyter notebook data is treated as read-only, untrusted content. */
(function(root){
  'use strict';
  function text(value) {
    if(typeof value==='string')return value;
    if(Array.isArray(value))return value.filter(function(part){return typeof part==='string';}).join('');
    return '';
  }
  function parse(source,limited) {
    if(limited)throw new Error('Notebook exceeds the configured text-preview size limit. Increase the limit or use Source.');
    var data;
    try { data=JSON.parse(source); } catch (_) { throw new Error('The notebook is not valid JSON.'); }
    if(!data || typeof data!=='object' || !Array.isArray(data.cells))throw new Error('The notebook has no readable cell list.');
    var metadata=data.metadata && typeof data.metadata==='object'?data.metadata:{};
    var info=metadata.language_info && typeof metadata.language_info==='object'?metadata.language_info:{};
    var kernel=metadata.kernelspec && typeof metadata.kernelspec==='object'?metadata.kernelspec:{};
    var language=typeof info.name==='string'?info.name:typeof kernel.language==='string'?kernel.language:'';
    return {cells:data.cells, language:language, version:data.nbformat};
  }
  function image(data) {
    if(!data || typeof data!=='object')return null;
    var type=data['image/png']?'image/png':data['image/jpeg']?'image/jpeg':null;
    if(!type)return null;
    var encoded=text(data[type]).replace(/[\r\n\t ]/g,'');
    if(!encoded || encoded.length>8*1024*1024 || encoded.length%4!==0 || !/^[A-Za-z0-9+/]*={0,2}$/.test(encoded))return null;
    return {type:type,data:encoded};
  }
  function outputs(cell) {
    var list=cell && Array.isArray(cell.outputs)?cell.outputs:[],result=[];
    list.forEach(function(output){
      if(!output || typeof output!=='object')return;
      if(output.output_type==='stream')result.push({kind:'text',label:output.name==='stderr'?'stderr':'output',text:text(output.text)});
      else if(output.output_type==='error') {
        var trace=text(output.traceback);
        if(!trace)trace=[text(output.ename),text(output.evalue)].filter(Boolean).join(': ');
        result.push({kind:'error',label:'error',text:trace.replace(/\x1b\[[0-?]*[ -/]*[@-~]/g,'')});
      } else if(output.output_type==='display_data' || output.output_type==='execute_result') {
        var data=output.data && typeof output.data==='object'?output.data:{},picture=image(data);
        if(picture)result.push({kind:'image',label:'saved image',image:picture});
        else if(data['text/plain']!==undefined)result.push({kind:'text',label:'saved output',text:text(data['text/plain'])});
        else result.push({kind:'unsupported',label:'Saved output format is not displayed'});
      }
    });
    return result;
  }
  root.FolderwingNotebook={parse:parse,text:text,image:image,outputs:outputs};
})(typeof globalThis!=='undefined'?globalThis:this);
