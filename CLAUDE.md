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
./ssg --drafts  # same, but include pages marked draft = true
make clean    # remove ./ssg and _site/
```

`make` only rebuilds when `ssg.cpp` is newer than `./ssg`. **After editing `ssg.cpp`,
run `make && ./ssg`** — running `./ssg` alone silently renders with the stale binary,
which shows up as unsubstituted `{{...}}` placeholders in the output.

While writing, use the preview server instead — it rebuilds on save and reloads the
browser, so the loop is just Ctrl-S:

```bash
./serve.py                 # http://127.0.0.1:8000, drafts included
./serve.py --host 0.0.0.0  # reachable from a phone on the same network
./serve.py --no-drafts     # the published view, as the deploy sees it
./serve.py --no-reload     # rebuild on save, but leave the browser alone
```

It watches `content/`, `templates/`, `static/`, `ssg.cpp` and `Makefile`; a change to
either of the last two runs `make` before `./ssg`, so editing the generator is the same
one-key loop as editing a page. A failed compile or a bad frontmatter parse prints the
error and keeps serving the last good build rather than blanking the site.

Or serve the built output directly, without the watcher:

```bash
python3 -m http.server 8000 --directory _site
```

The generator resolves all paths relative to the current directory, so run it from the
repository root. `serve.py` chdirs to its own directory first, so it can be started from
anywhere.

## Architecture

```
content/     Markdown sources; directory layout becomes the URL layout
templates/   Full-page HTML templates, selected per page by frontmatter
  partials/  Fragments shared across templates
static/      Copied verbatim into _site/, preserving structure
_site/       Build output — generated, never edit by hand
ssg.cpp      The entire generator
serve.py     Preview server: rebuild on save, serve, reload. Not part of a build
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
| `draft`         | `false`       | Keep the page out of the build until it is published  |
| `list_dir`      | *(siblings)*  | List pages only: content subdirectory to enumerate    |
| `item_template` | `list-item`   | List pages only: partial used per entry               |

`title` and the on-page heading are deliberately separate. `title` always feeds the
`<title>` tag and list entries; `show_title` controls only whether it also appears as an
`<h1>` in the body, via the `{{page_heading}}` placeholder that every template carries.

### Drafts

An unfinished page carries `draft = true` in its frontmatter. It lives in `content/`
alongside everything else and is committed like any other file, but a plain `./ssg`
skips it entirely: no HTML is written, and it appears in no list page. `./ssg --drafts`
builds drafts too, so a draft can be read locally before it goes out. Publishing is
deleting that one line — the file never moves, so its URL, its git history, and any link
already shared stay put.

The skip happens in the collection pass, not at render time. That is what keeps drafts
out of list pages without a second filter: the render pass only ever sees published pages.

Two things follow from the design and are deliberate:

- The deploy workflow runs a bare `./ssg`, so a draft cannot reach the live site however
  the repo is pushed. There is no `--drafts` path to production.
- A build without `--drafts` deletes the output file a previous `--drafts` run left in
  `_site/`, since the generator never otherwise clears stale output. Without that, a local
  preview would keep serving a page the current build deliberately omitted.

A previewed draft gets `[draft] ` prefixed to its `<title>`, so the browser tab
distinguishes it from a live page. In a page template `{{title}}` only reaches the head
partial, so nothing in the page body is affected.

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

### Sidenotes

Write `^[note text]` inline in any markdown body — Pandoc's inline-footnote syntax. The
note is set in the gutter to the right of the prose, level with the line that cites it,
numbered with a superscript at both ends. `\^[` is a literal caret followed by a bracket.

`expand_sidenotes` rewrites the marker into the three elements the layout needs: a
`<label class="sidenote-number">`, a checkbox, and `<span class="sidenote">`. This is a
rewrite in place rather than math's lift-and-splice, because the opposite is wanted here:
the note body stays in the stream for cmark, so emphasis, code spans, links and `$math$`
all work inside a note. It runs *after* `extract_math` so TeX brackets (`\left[ ... \right]`)
are inert placeholders by then and cannot unbalance the bracket matching.

Numbering is a CSS counter, not something the generator writes, so inserting a note
halfway through an article renumbers the rest with nothing to keep in sync. The `id` only
has to be unique within the page, which the running index gives.

Two cases deliberately degrade to literal text rather than failing the build: a marker
that is never closed, and one whose closing bracket comes after a blank line. A note
cannot span a paragraph break anyway — cmark would close the `<p>` mid-span — and without
that second test a stray `^[` in prose would swallow every note after it, up to whichever
`]` happened to balance.

Sidenotes belong in the `--measure` prose column, which in practice means the `article`
template. Nothing stops one being written in a `list` or `default` page, but those put
content straight in `<main>` with no width cap, so the float would be pushed off the
right edge of the page.

### Images

Images live in `static/images/` and are referenced by root-absolute path —
`![alt text](/images/diagram.png)`. The path has to start with `/`: `static/` is copied to
the root of `_site/`, but an article's own URL is a level down (`/articles/renderer.html`),
so a relative `images/…` would resolve to `/articles/images/…` and 404. Nothing about
images is generator-specific; the static copy pass takes any file type, and cmark handles
the markdown.

Remote URLs work too, but everything else the site depends on — fonts, KaTeX, the resume —
is self-hosted, and an image is the one asset most likely to rot or change under you.
Prefer the repo.

`main img` caps images at the prose column. That rule is load-bearing: without it an image
renders at its intrinsic pixel size, crossing the sidenote gutter on a desktop and forcing
the page to scroll sideways on a phone.

Two things markdown cannot express, both of which work today with no new code: a captioned
figure, by writing `<figure>`/`<figcaption>` as raw HTML (`CMARK_OPT_UNSAFE` passes it
through), and a margin figure, by putting the image inside a sidenote —
`^[![alt](/images/x.png) The caption.]` scales it into the gutter. Neither has CSS of its
own yet.

Bear in mind that a diagram with a baked-in white background glares in dark mode. A
transparent PNG or an SVG using `currentColor` sits on either palette.

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

### The preview server

`serve.py` is standard library only, deliberately: it is a convenience for writing, not
part of the build, and it is not worth widening the dependency budget for. That rules out
`watchdog` and the various live-reload packages, so change detection is an mtime poll
every 0.3s over the ~50 watched files, which costs nothing measurable at this size and
needs no platform-specific inotify code. A poll that sees a change waits for the tree to
stop moving before building, since one save often lands as several filesystem events and
some editors write a scratch file alongside the real one.

Nothing in it writes to `_site/`. The reload snippet is spliced in as the HTML leaves the
socket, so the built output stays byte-identical to what `./ssg` wrote and what the deploy
publishes — there is no "works locally, broken live" gap and nothing to strip before
committing. The browser holds a long poll against `/__livereload` carrying the build
counter it was served with; a rebuild bumps the counter, the poll returns, the page
reloads. Responses go out `Cache-Control: no-store`, without which an edited stylesheet
keeps serving from cache and the rebuild looks broken.

A failed `make` deliberately does not fall through to `./ssg`: the binary is stale at that
point, and running it would quietly produce a site full of unsubstituted `{{...}}`. Both
failure modes — compile error, bad frontmatter — print the error and leave the previous
build in place. One thing it only warns about: the generator never clears `_site`, so
deleting or renaming a page leaves the old HTML behind, and the watcher prints the stale
path rather than removing files on your behalf.

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

`.sidenote` floats right with a negative `margin-right` of exactly its own width plus
`--sidenote-gap`, which is what puts it in the gutter without disturbing the prose: the
float's margin box then occupies `-gap` of horizontal space in the column, so no line
wraps around it and nothing in the text moves. Narrowing that margin would eat into the
measure instead of clearing it. `clear: right` keeps consecutive notes stacked rather than
overlapping, at the cost of pushing a note down the page when several cluster in one
paragraph.

Below `72rem` there is no gutter left — 40em of prose at the 1.25rem body size is 800px,
plus `--sidenote-width` + `--sidenote-gap` and the body's padding, is about 1144px — so
the note is hidden and its number becomes a tap target that reveals it inline, via a
`<label>` driving a `display: none` checkbox and `input:checked + .sidenote`. That is why
the generator emits the three elements adjacent with no whitespace between them, and why
this needs no JavaScript. Recompute the breakpoint if either token or `--measure` changes.

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

The article titles in `.page-list` carry the same centre-out underline, but built from a
`background-image` gradient animated on `background-size` rather than a scaled `::after`.
The mechanisms differ because the constraints do: nav links are `white-space: nowrap` and
always one line, while list titles wrap on a narrow viewport, and an absolutely positioned
bar has no dependable containing block across the fragments of a wrapped inline. The
background follows the text, and `box-decoration-break: clone` gives each line its own bar.
Do not "unify" these two onto one implementation without checking a long title on a phone.

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
