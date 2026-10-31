window.addEventListener('DOMContentLoaded', function () {
  'use strict';
  var view = document.getElementById('content');
  var parser = FolderwingMarkdown.create(window.markdownit);
  async function render(source) {
    view.innerHTML = parser.render(source);
    var nodes = view.querySelectorAll('.math');
    await MathJax.startup.promise;
    for (var i = 0; i < nodes.length; i++) {
      var node = nodes[i], formula = node.textContent;
      try { node.replaceChildren(await MathJax.tex2svgPromise(formula, {display: node.dataset.display === 'true'})); }
      catch (e) { node.textContent = formula; node.classList.add('math-error'); }
    }
    document.documentElement.dataset.ready = 'true';
    if (window.chrome && window.chrome.webview)
      window.chrome.webview.postMessage('rendered:' + nodes.length + ':' + view.querySelectorAll('mjx-merror,.math-error').length);
  }
  if (window.chrome && window.chrome.webview) {
    window.chrome.webview.addEventListener('message', function (event) {
      if (typeof event.data === 'string') render(event.data).catch(function () {
        view.textContent = 'The rendered preview could not be generated. Switch to Source to read this file.';
      });
    });
    window.chrome.webview.postMessage('ready');
  }
  window.FolderwingRender = render;
});
