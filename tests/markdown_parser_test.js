'use strict';
const assert = require('assert');
const markdownit = require('../app/markdown/vendor/markdown-it.min.js');
require('../app/markdown/renderer.js');
const md = global.FolderwingMarkdown.create(markdownit);
const source = [
  '# Heading', '', '\\[', '\\mathrm{inj}_{\\sigma,t}(r) \\quad \\mathrm{match}_{x}', '\\]', '',
  'A citation \\[User Query\\] and math \\(x^2\\) and $x+1$.',
  'A price $5 and $10 stays text.', '',
  '```', '\\[', 'not math', '\\]', '```', '',
  '```math', 'x^2+y^2', '```', '', '$$ x^2 $$', '',
  '- item', '', '  \\[', '  x_y', '  \\]', '',
  '<script>alert(1)</script>', '![](https://example.com/a.png)'
].join('\n');
const html = md.render(source);
assert.strictEqual((html.match(/data-display="true"/g) || []).length, 4);
assert.strictEqual((html.match(/data-display="false"/g) || []).length, 2);
assert(html.includes('[User Query]'));
assert(html.includes('$5 and $10'));
assert(html.includes('\\mathrm{inj}_{\\sigma,t}(r) \\quad \\mathrm{match}_{x}'));
assert(!html.includes('<script>') && !html.includes('<img'));
for (const path of process.argv.slice(2)) {
  const text = require('fs').readFileSync(path, 'utf8');
  const output = md.render(text);
  if (path.includes('FORMAL_META_REALIZATION_THEORY_V9')) {
    assert.strictEqual((output.match(/data-display="true"/g) || []).length, 26);
    assert(output.includes('\\mathrm{inj}_{\\sigma,t}(r)'));
  }
  if (path.includes('TF-Lang Project Analysis')) assert(output.includes('[User Query]'));
}
console.log('Markdown parser checks passed.');
