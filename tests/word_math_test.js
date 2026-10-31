'use strict';
const assert=require('assert');global.window=global;require('../app/word/word-util.js');const parse=FolderwingWord.expressions;
assert.deepStrictEqual(parse('before \\(x^2\\) after').map(x=>x.value),['x^2']);
assert.deepStrictEqual(parse('\\[a+b\\] and $$c+d$$').map(x=>[x.value,x.display]),[['a+b',true],['c+d',true]]);
assert.deepStrictEqual(parse('cost $12 and $23, math $x+1$').map(x=>x.value),['x+1']);
assert.strictEqual(parse('escaped \\\\(x\\) and unmatched \\(x').length,0);
assert.strictEqual(parse('\\('+('x'.repeat(4097))+'\\)').length,0);
console.log('Word LaTeX delimiter, currency and length checks passed.');
