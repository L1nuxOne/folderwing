'use strict';
const assert=require('assert');
require('../app/markdown/notebook-util.js');
const api=global.FolderwingNotebook;
const img='iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==';
const fixture={nbformat:4,metadata:{language_info:{name:'python'}},cells:[
  {cell_type:'markdown',source:['# Heading\n','\\[\nx^2\n\\]']},
  {cell_type:'code',source:['print(1)\n'],outputs:[{output_type:'stream',name:'stdout',text:['hello','\n']},{output_type:'display_data',data:{'image/png':[img]}},{output_type:'error',traceback:['\u001b[31merror\u001b[0m']}]}
]};
const result=api.parse(JSON.stringify(fixture),false);
assert.strictEqual(result.language,'python');assert.strictEqual(result.cells.length,2);
assert.strictEqual(api.text(result.cells[0].source),'# Heading\n\\[\nx^2\n\\]');
const outputs=api.outputs(result.cells[1]);
assert.deepStrictEqual(outputs.map(x=>x.kind),['text','image','error']);
assert.strictEqual(outputs[0].text,'hello\n');assert.strictEqual(outputs[1].image.data,img);assert.strictEqual(outputs[2].text,'error');
assert.strictEqual(api.image({'image/svg+xml':'<svg onload=alert(1)>'}),null);
assert.strictEqual(api.image({'image/png':'invalid<>'}),null);
assert.strictEqual(api.outputs({outputs:[{output_type:'display_data',data:{'text/html':'<script>evil()</script>'}}]})[0].kind,'unsupported');
assert.throws(()=>api.parse('{',false),/valid JSON/);
assert.throws(()=>api.parse('{}',false),/cell list/);
assert.throws(()=>api.parse('{}',true),/size limit/);
console.log('Notebook data checks passed.');
