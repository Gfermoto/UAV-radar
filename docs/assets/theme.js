/*! NEVOD theme: localStorage key nevod-theme ∈ {dark,light}; default dark. */
(function (global) {
  "use strict";
  var KEY = "nevod-theme";
  var root = document.documentElement;

  function normalize(v) {
    return v === "light" ? "light" : "dark";
  }

  function read() {
    try {
      return normalize(localStorage.getItem(KEY));
    } catch (e) {
      return "dark";
    }
  }

  function apply(theme, persist) {
    theme = normalize(theme);
    root.setAttribute("data-theme", theme);
    root.style.colorScheme = theme;
    var meta = document.querySelector('meta[name="theme-color"]');
    if (meta) {
      meta.setAttribute("content", theme === "light" ? "#eef1ef" : "#070b09");
    }
    if (persist) {
      try {
        localStorage.setItem(KEY, theme);
      } catch (e) {}
    }
    syncControls(theme);
    try {
      global.dispatchEvent(new CustomEvent("nevod-theme", { detail: { theme: theme } }));
    } catch (e) {}
    return theme;
  }

  function syncControls(theme) {
    var nodes = document.querySelectorAll("[data-nevod-theme-toggle]");
    for (var i = 0; i < nodes.length; i++) {
      var btn = nodes[i];
      var pressed = theme === "light";
      btn.setAttribute("aria-pressed", pressed ? "true" : "false");
      btn.setAttribute("data-theme-state", theme);
      var labelDark = btn.getAttribute("data-label-dark") || "Ночь";
      var labelLight = btn.getAttribute("data-label-light") || "День";
      var label = theme === "light" ? labelLight : labelDark;
      btn.setAttribute("aria-label", label);
      btn.setAttribute("title", label);
      var sun = btn.querySelector("[data-ico=sun]");
      var moon = btn.querySelector("[data-ico=moon]");
      if (sun) sun.hidden = theme !== "dark";
      if (moon) moon.hidden = theme !== "light";
    }
  }

  function toggle() {
    return apply(read() === "light" ? "dark" : "light", true);
  }

  function bind() {
    syncControls(read());
    document.addEventListener("click", function (ev) {
      var t = ev.target;
      if (!t || !t.closest) return;
      var btn = t.closest("[data-nevod-theme-toggle]");
      if (!btn) return;
      ev.preventDefault();
      toggle();
    });
  }

  apply(read(), false);
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", bind);
  } else {
    bind();
  }

  global.NevodTheme = { key: KEY, read: read, apply: apply, toggle: toggle };
})(typeof window !== "undefined" ? window : this);
