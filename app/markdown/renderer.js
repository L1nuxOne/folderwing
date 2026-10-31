/* Markdown is parsed before math is rendered: never preprocess source with HTML. */
(function (root) {
  'use strict';
  function create(markdownit) {
    var md = markdownit({html: false, linkify: false, typographer: false});
    var escape = md.utils.escapeHtml;
    function mathToken(state, type, value, block) {
      var token = state.push(type, '', block ? 0 : 0);
      token.content = value;
      token.block = block;
    }
    md.block.ruler.before('fence', 'folderwing_math', function (state, start, end, silent) {
      var pos = state.bMarks[start] + state.tShift[start];
      if (state.sCount[start] - state.blkIndent >= 4) return false;
      var first = state.src.slice(pos, state.eMarks[start]).trim();
      if (first.length > 4 && first.slice(0, 2) === '$$' && first.slice(-2) === '$$') {
        if (!silent) mathToken(state, 'folderwing_math_block', first.slice(2, -2).trim(), true);
        state.line = start + 1;
        return true;
      }
      var close = first === '\\[' ? '\\]' : first === '$$' ? '$$' : null;
      if (!close) return false;
      var line = start + 1;
      for (; line < end; line++) {
        if (state.sCount[line] < state.blkIndent && state.src.slice(state.bMarks[line], state.eMarks[line]).trim()) break;
        var body = state.src.slice(state.bMarks[line] + state.tShift[line], state.eMarks[line]).trim();
        if (body === close) break;
      }
      if (line >= end || state.src.slice(state.bMarks[line] + state.tShift[line], state.eMarks[line]).trim() !== close) return false;
      if (silent) return true;
      var value = state.getLines(start + 1, line, state.blkIndent, false);
      mathToken(state, 'folderwing_math_block', value, true);
      state.line = line + 1;
      return true;
    }, {alt: ['paragraph', 'reference', 'blockquote', 'list']});
    md.inline.ruler.before('escape', 'folderwing_math_inline', function (state, silent) {
      var src = state.src, start = state.pos, dollar = src[start] === '$';
      var bracket = src.slice(start, start + 2) === '\\(';
      if (!dollar && !bracket) return false;
      if (dollar && (src[start + 1] === '$' || !src[start + 1] || /\s|\d/.test(src[start + 1]))) return false;
      var opener = bracket ? 2 : 1, delimiter = bracket ? '\\)' : '$';
      var at = start + opener;
      while ((at = src.indexOf(delimiter, at)) !== -1) {
        var slashes = 0, i = at - 1;
        while (i >= start && src[i--] === '\\') slashes++;
        if (!(slashes & 1) && src[at - 1] && !/\s/.test(src[at - 1]) &&
            (bracket || !/\d/.test(src[at + 1] || ''))) break;
        at += delimiter.length;
      }
      if (at < 0 || src.slice(start + opener, at).indexOf('\n\n') !== -1) return false;
      if (!silent) mathToken(state, 'folderwing_math_inline', src.slice(start + opener, at), false);
      state.pos = at + delimiter.length;
      return true;
    });
    md.renderer.rules.folderwing_math_inline = function (tokens, i) {
      return '<span class="math" data-display="false">' + escape(tokens[i].content) + '</span>';
    };
    md.renderer.rules.folderwing_math_block = function (tokens, i) {
      return '<div class="math" data-display="true">' + escape(tokens[i].content) + '</div>\n';
    };
    var fence = md.renderer.rules.fence;
    md.renderer.rules.fence = function (tokens, i, options, env, renderer) {
      if (/^(math|tex|latex)$/i.test(tokens[i].info.trim()))
        return '<div class="math" data-display="true">' + escape(tokens[i].content) + '</div>\n';
      return fence(tokens, i, options, env, renderer);
    };
    /* Do not load remote or local images as a side effect of selecting a file. */
    md.renderer.rules.image = function (tokens, i) { return '<span class="image-alt">' + escape(tokens[i].content) + '</span>'; };
    return md;
  }
  root.FolderwingMarkdown = {create: create};
})(typeof globalThis !== 'undefined' ? globalThis : this);
