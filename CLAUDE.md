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
- `libcmark` (markdown → HTML), linked with `-lcmark`, called with `CMARK_OPT_UNSAFE`
  so raw HTML in markdown survives (cmark strips it by default since 0.29)
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

Templates receive exactly six placeholders — `{{head}}`, `{{header}}`, `{{footer}}` (the
partials), `{{title}}`, `{{content}}` and `{{page_heading}}` — plus `{{math_assets}}`,
which only the head partial uses. Templates never see the frontmatter table, so an
arbitrary key like `{{description}}` will *not* resolve in a page template; that works
only in markdown bodies and in `list-item.html`.

Substitution order matters in two places: `{{head}}` goes in before `{{title}}` because
the head partial contains `{{title}}`, and `{{math_assets}}` goes in after `{{head}}`
because that is where its placeholder lives.

Within a page's own markdown body, any string-valued frontmatter key is available as
`{{key}}`.

A caveat worth knowing: a `{{key}}` with no matching frontmatter is left in the output
verbatim rather than blanked. `grep -rn "{{" --include="*.html" _site/` after a build
catches this — scope it to HTML, since minified `katex.min.js` contains `{{`.

### Frontmatter keys

| Key             | Default       | Meaning                                              |
| --------------- | ------------- | ---------------------------------------------------- |
| `title`         | filename stem | Page title, used in `<title>` and templates           |
| `template`      | `default`     | Which `templates/<name>.html` to render into          |
| `date`          | *(none)*      | `"Month D, YYYY"` — this exact format is what sorts   |
| `description`   | *(none)*      | One-line summary shown under the title in list pages  |
| `show_title`    | `false`       | Render the title as an `<h1>` at the top of the page  |
| `list_dir`      | *(siblings)*  | List pages only: content subdirectory to enumerate    |
| `item_template` | `list-item`   | List pages only: partial used per entry               |

`title` and the on-page heading are deliberately separate. `title` always feeds the
`<title>` tag and list entries; `show_title` controls only whether it also appears as an
`<h1>` in the body, via the `{{page_heading}}` placeholder that every template carries.

### Math

Write LaTeX as `$inline$` and `$$display$$` in any markdown body. `\$` is a literal
dollar, and an unpaired `$` stays literal (inline math may not cross a blank line).

This needs generator support and cannot be done by writing the TeX straight into the
markdown: CommonMark treats a backslash before ASCII punctuation as an escape, so `\\`,
`\,`, `\{` and `\%` would be silently eaten, and `*` inside a formula would become
emphasis. `extract_math` therefore lifts every math span out before cmark runs, leaves an
inert `KTXMATH<n>END` placeholder, and `restore_math` splices the TeX back afterwards
inside `<span class="math">`. Display math is a `span` too, blocked out with CSS —
cmark wraps the placeholder in a `<p>`, and a block element inside `<p>` is invalid HTML.

KaTeX (self-hosted in `static/katex/`, MIT) renders the spans in the browser via
`static/js/math.js`. It is ~300KB, so the generator injects the assets through
`{{math_assets}}` in the head partial **only on pages that actually contain math**. Only
the `.woff2` fonts are vendored; the CSS lists `woff`/`ttf` fallbacks that no current
browser will reach for.

### The resume page

`templates/resume.html` embeds `static/resume/TylerMorrillResume.pdf` in an `<object>`
sized to the PDF's 612x792pt letter MediaBox via `aspect-ratio`, and always renders a
download link beneath it. The link is not a fallback — several mobile browsers draw a
blank box rather than triggering `<object>`'s fallback content, so it has to be
unconditional. Replacing the resume means dropping in a new PDF at the same path; if its
page size differs, update the `aspect-ratio` in `.pdf-embed`.

### List pages

`template = "list"` auto-generates a `<ul class="page-list">` of other pages, sorted
newest first with title as the tiebreaker. It enumerates the directory named by
`list_dir`, or its own directory when that key is absent — the homepage uses
`list_dir = "articles"` to list posts that live one level down. Any markdown in the list
page's own body renders above the generated list.

Date sorting parses the `"Month D, YYYY"` format into a sortable integer. Dates that
don't parse sort to the bottom rather than failing the build, so a mistyped date shows up
as a misplaced entry.

`date` and `description` are optional, so the list renderer substitutes them explicitly
after `substitute_frontmatter` — otherwise a page missing one would leak a literal
`{{date}}` into the output. Their elements are always emitted and collapsed with `:empty`
in CSS when blank. Any further optional key added to `list-item.html` needs the same
treatment. Descriptions are plain text, not markdown.

## Styling

`static/css/main.css` drives everything from CSS custom properties defined at the top.
Light is the base palette on `:root`; dark is declared twice — once under
`@media (prefers-color-scheme: dark)` guarded by `:not([data-theme="light"])`, and once
under `[data-theme="dark"]` — so the in-page toggle can override the OS preference in
both directions.

For visual changes, edit the tokens rather than individual rules wherever the change can
be expressed as a color. When adding a color, add it as a token and define it in all
three places above; a color defined only inside the media query breaks the toggle.

`--selection` covers both browser highlights: `::selection` for dragged-over text and
`-webkit-tap-highlight-color` for the flash mobile browsers paint on tap. The latter is
inherited, so it is set once on `html`. The token is translucent on purpose — `::selection`
sets only `background`, leaving selected text at `--fg`, so there is no second color to
keep contrast-safe across both themes. Its alpha is higher in dark mode, where the same
tint reads weaker. Never write `::selection, ::-moz-selection` as one selector list: a
single unrecognised pseudo-element invalidates the entire rule.

The nav is a three-track grid — `1fr auto 1fr` — with `.nav-left`, the `.nav-name` link,
and `.nav-right` in it. The outer tracks stay equal whatever they hold, so the name sits on
the page's centre line rather than halfway between the groups, and adding a link to either
side is a `header.html` edit with no CSS to touch. Each group is a wrapping flex row. Below
`34rem` the grid collapses to one column and the name is pulled up with `order: -1`.

Nav links carry an underline bar on `::after` that grows out from the centre on hover — a
pseudo-element rather than `text-decoration` so it can be animated, laid out at full width
and only `scaleX`-ed, so it costs a compositor transform and nothing in the nav shifts.
Hover changes no colors; the bar is the whole affordance. `.nav-name` is excluded, being an
`<a>` with no href. The same bar stays out on the current page: the generator adds
`class="is-current"` to whichever nav link's `href` equals the page being rendered
(`mark_current_link` in `ssg.cpp`), which keeps the active state static rather than
depending on JavaScript. Pages with no nav entry, such as articles, simply match nothing.

The theme toggle's sun/moon glyph is CSS `content` on `.theme-toggle::before`, keyed off
the same selectors as the palette, so it is correct on first paint. `static/js/theme.js`
only flips the attribute and persists the choice; an inline script in the head partial
applies the saved theme before paint to avoid a flash.

The sun is written `"\2600\FE0E"`, not `"\2600"`. U+2600 has both a text and an emoji
presentation, and iOS resolves the bare codepoint to Apple Color Emoji; the trailing VS15
demands the monochrome form. `font-variant-emoji: text` on `.theme-toggle` asks for the
same thing in the newer spelling — keep both, since neither is universally honoured. The
moon (U+263E) has no emoji presentation and so needs neither.

Prose is set in ET Book, self-hosted from `static/fonts/et-book/` and declared as
`@font-face` blocks at the top of `main.css`, with `--font-body` as the token. It is MIT
licensed (`static/fonts/et-book/LICENSE`), and is the same face Tufte CSS uses. Four
`.woff` faces are present: roman 400, italic 400, semi-bold 600, bold 700.

The nav is the one exception: it uses Roboto Mono (`--font-nav`), self-hosted from
`static/fonts/roboto-mono/` under OFL 1.1, so the navigation reads as an interface strip
rather than as more of the book. Only Google's latin and latin-ext subsets are vendored —
~56KB, with the `unicode-range` declarations carried over from Google's CSS. Characters
outside those ranges fall back, which is what the theme toggle's sun/moon glyph already
relies on.

The two files are variable fonts covering `wght` 100–700, so there is one per subset
rather than one per weight, and each `@font-face` declares `font-weight: 100 700`.
Narrowing that to a single value would leave the browser synthesising every other weight.

Headings follow book-typography convention rather than web convention — hierarchy comes
from size and style, so they stay at `font-weight: 400` and `h3` is italic. Do not
"fix" this by bolding them.

`static/css/normalize.css` is vendored (normalize.css v8.0.1) — do not modify.

## Conventions

- Semantic HTML5 (`<header>`, `<nav>`, `<main>`, `<footer>`, `<article>`)
- Comments in `ssg.cpp` explain *why*, and section headers use the existing `// ---` banner style
- Markdown bodies start at `##`; `#` is reserved for the opt-in `show_title` heading
- Never hand-edit `_site/`; change the source and rebuild
