// Light/dark toggle. The button's glyph is drawn by CSS from the resolved
// theme, so this only has to flip the attribute and remember the choice.
document.addEventListener("DOMContentLoaded", function () {
  var button = document.getElementById("theme-toggle");
  if (!button)
    return;

  button.addEventListener("click", function () {
    var root = document.documentElement;
    var current = root.getAttribute("data-theme");

    // No explicit choice yet: the active theme is whatever the OS asked for.
    if (current !== "light" && current !== "dark")
      current = window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";

    var next = current === "dark" ? "light" : "dark";
    root.setAttribute("data-theme", next);
    try {
      localStorage.setItem("theme", next);
    } catch (error) {
      // Not persisting is fine; the toggle still works for this page load.
    }
  });
});
