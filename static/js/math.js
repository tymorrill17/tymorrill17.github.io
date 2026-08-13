// Renders the math spans the generator left behind. Each one holds its original
// TeX as text; KaTeX replaces the element's contents with the typeset output.
// This file is deferred and loaded after katex.min.js, so katex is defined and
// DOMContentLoaded has not fired yet.
document.addEventListener("DOMContentLoaded", function () {
  if (typeof katex === "undefined")
    return; // KaTeX failed to load; leave the raw TeX visible rather than blanking it

  document.querySelectorAll(".math").forEach(function (element) {
    katex.render(element.textContent, element, {
      displayMode: element.classList.contains("math-display"),
      // A malformed formula renders in red instead of throwing and killing the
      // rest of the page's math.
      throwOnError: false,
    });
  });
});
