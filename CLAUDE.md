# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Personal site for Tyler Morrill, built by a static site generator written from scratch
in a single C++ file (`ssg.cpp`). Markdown in `content/` plus HTML templates in
`templates/` render to a static `_site/` directory.

The generator exists to serve this site specifically. It is not a general-purpose tool,
and there is no reason to add a feature until a page actually needs it.

## Guiding Principle

**Keep the stack as simple as possible; do not restrict what the site can express.**

Those pull in different directions, and the resolution is: features are welcome, new
moving parts are not. Prefer adding a few lines to `ssg.cpp` over adopting a library,
a framework, or a build step. Reach for a dependency only when the hand-written
alternative would be genuinely large or error-prone.

Current dependency budget, which should stay roughly this small:

- A C++17 compiler and `make`
- `libcmark` (markdown → HTML), linked with `-lcmark`
- `toml.hpp` (frontmatter parsing), vendored in the repo root

No package manager, no CSS preprocessor, no JS bundler, no CI. Client-side JavaScript is
fine where it earns its place; keep it vanilla and load it from `static/js/`.

## Build and Local Development

```bash
make          # compile ssg.cpp -> ./ssg
./ssg         # render content/ + templates/ + static/ -> _site/
make clean    # remove ./ssg and _site/
```

`make` only rebuilds when `ssg.cpp` is newer than `./ssg`. **After editing `ssg.cpp`,
run `make && ./ssg`** — running `./ssg` alone silently renders with the stale binary,
which shows up as unsubstituted `{{...}}` placeholders in the output.

Serve the built site:

```bash
python3 -m http.server 8000 --directory _site
```

The generator resolves all paths relative to the current directory, so run it from the
repository root.

## Architecture

```
content/     Markdown sources; directory layout becomes the URL layout
templates/   Full-page HTML templates, selected per page by frontmatter
  partials/  Fragments shared across templates
static/      Copied verbatim into _site/, preserving structure
_site/       Build output — generated, never edit by hand
ssg.cpp      The entire generator
toml.hpp     Vendored dependency, do not modify
```

### How a page is built

1. **Collect.** Every `.md` file under `content/` is read, its `+++`-delimited TOML
   frontmatter parsed off the front, and its output path and URL derived from its path
   relative to `content/`. A file named `index.md` gets a clean directory URL
   (`content/articles/index.md` → `/articles/`).
2. **Render.** The body markdown is converted to HTML, then dropped into the template
   named by the page's `template` key.
3. **Copy.** Everything in `static/` is copied into `_site/`.

Collection is a separate first pass because list pages need to see every other page
before they can render.

### Templating

Substitution is literal `{{key}}` string replacement — there are no conditionals, loops,
or expressions, by design.

Templates receive `{{head}}`, `{{header}}`, `{{footer}}` (the partials), `{{title}}`, and
`{{content}}`. Note that `{{head}}` is substituted *before* `{{title}}`, because the head
partial itself contains `{{title}}`.

Within a page's own markdown body, any string-valued frontmatter key is available as
`{{key}}`.

A caveat worth knowing: a `{{key}}` with no matching frontmatter is left in the output
verbatim rather than blanked. `grep -r "{{" _site/` after a build catches this.

### Frontmatter keys

| Key             | Default       | Meaning                                              |
| --------------- | ------------- | ---------------------------------------------------- |
| `title`         | filename stem | Page title, used in `<title>` and templates           |
| `template`      | `default`     | Which `templates/<name>.html` to render into          |
| `date`          | *(none)*      | `"Month D, YYYY"` — this exact format is what sorts   |
| `list_dir`      | *(siblings)*  | List pages only: content subdirectory to enumerate    |
| `item_template` | `list-item`   | List pages only: partial used per entry               |

### List pages

`template = "list"` auto-generates a `<ul class="page-list">` of other pages, sorted
newest first with title as the tiebreaker. It enumerates the directory named by
`list_dir`, or its own directory when that key is absent — the homepage uses
`list_dir = "articles"` to list posts that live one level down. Any markdown in the list
page's own body renders above the generated list.

Date sorting parses the `"Month D, YYYY"` format into a sortable integer. Dates that
don't parse sort to the bottom rather than failing the build, so a mistyped date shows up
as a misplaced entry.

## Styling

`static/css/main.css` drives everything from CSS custom properties defined at the top.
Light is the base palette on `:root`; dark is declared twice — once under
`@media (prefers-color-scheme: dark)` guarded by `:not([data-theme="light"])`, and once
under `[data-theme="dark"]` — so the in-page toggle can override the OS preference in
both directions.

For visual changes, edit the tokens rather than individual rules wherever the change can
be expressed as a color. When adding a color, add it as a token and define it in all
three places above; a color defined only inside the media query breaks the toggle.

The theme toggle's sun/moon glyph is CSS `content` on `.theme-toggle::before`, keyed off
the same selectors as the palette, so it is correct on first paint. `static/js/theme.js`
only flips the attribute and persists the choice; an inline script in the head partial
applies the saved theme before paint to avoid a flash.

`static/css/normalize.css` is vendored (normalize.css v8.0.1) — do not modify.

## Conventions

- Semantic HTML5 (`<header>`, `<nav>`, `<main>`, `<footer>`, `<article>`)
- Comments in `ssg.cpp` explain *why*, and section headers use the existing `// ---` banner style
- Article templates already emit `<h1>{{title}}</h1>`, so start markdown bodies at `##`
- Never hand-edit `_site/`; change the source and rebuild
