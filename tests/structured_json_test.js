'use strict';
const assert=require('assert');
require('../app/markdown/json-parser.js');
const parse=global.FolderwingJSON.parse;
const source='{"big":900719925474099312345,"tiny":1.2300e-99,"escaped":"a\\n\\u263a","big":-0,"list":[true,null,{"x/y":false}]}';
const doc=parse(source), children=doc.root.children;
assert.strictEqual(source.slice(children[0].start,children[0].end),'900719925474099312345');
assert.strictEqual(source.slice(children[1].start,children[1].end),'1.2300e-99');
assert.strictEqual(children[2].value,'a\n☺');
assert.strictEqual(children[3].key,'big');
assert.strictEqual(source.slice(children[3].start,children[3].end),'-0');
assert.strictEqual(children[4].children[2].children[0].path,'$["list"][2]["x/y"]');
for (const invalid of ['', '{"a":}', '[1,]', '{"a":1,}', '01', '1e', 'true false', '"bad\\x"', '{"a" 1}'])
  assert.throws(()=>parse(invalid),Error,'should reject '+invalid);
assert.throws(()=>parse('['.repeat(130)+'0'+']'.repeat(130)),/Nesting/);
const lines=['{"a":1}','not-json','{"n":999999999999999999999999}'];
const outcomes=lines.map(line=>{try{return parse(line).root.type;}catch(_){return 'error';}});
assert.deepStrictEqual(outcomes,['object','error','object']);
console.log('Structured JSON parser checks passed.');
