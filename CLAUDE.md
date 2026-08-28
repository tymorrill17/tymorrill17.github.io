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
recolor.py   Rewrites an exported SVG's ink to currentColor. Run by hand, not by the build
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

A list page marks its draft entries the same way: `list-item.html` carries a
`{{draft_tag}}` placeholder, filled with a `<span class="draft-tag">[DRAFT]</span>` beside
the title and blanked otherwise, so the tag exists only in a `--drafts` build. It is
substituted explicitly, like `date` and `description`, because `draft` is a boolean and
`substitute_frontmatter` only fills string-valued keys.

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
so a relative `images/…` would resolve to `/articles/images/…` and 404. The static copy
pass takes any file type and cmark handles the markdown, so an ordinary raster image needs
nothing from the generator. `resolve_images` in `ssg.cpp` handles the three cases that do:
inlining an SVG, marking an `_auto` drawing for the dark-mode filter, and resolving a
light/dark pair. All three are described below, and all three leave an image they do not
apply to exactly as cmark wrote it.

Remote URLs work too, but everything else the site depends on — fonts, KaTeX, the resume —
is self-hosted, and an image is the one asset most likely to rot or change under you.
Prefer the repo.

`main img, main svg` caps images at the prose column. That rule is load-bearing: without it
an image renders at its intrinsic pixel size, crossing the sidenote gutter on a desktop and
forcing the page to scroll sideways on a phone.

Two things markdown cannot express, both of which work today with no new code: a captioned
figure, by writing `<figure>`/`<figcaption>` as raw HTML (`CMARK_OPT_UNSAFE` passes it
through), and a margin figure, by putting the image inside a sidenote —
`^[![alt](/images/x.png) The caption.]` scales it into the gutter. Neither has CSS of its
own yet.

Bear in mind that a diagram with a baked-in white background glares in dark mode. There
are four ways out, and the right one depends on how much of the drawing's palette you
want to control:

| route | dark values chosen | good for |
| ------------------------- | ------------- | ------------------------------------- |
| `_auto` + CSS filter      | none          | the default; anything flat-coloured   |
| `currentColor`            | none          | a one-ink line drawing                |
| `var(--fig-*)` + `--map`  | one per colour| when a colour has to be exactly right |
| `_light`/`_dark` pair     | a whole file  | photographs; last resort              |

They are not composable. A drawing painted with `currentColor` must not also be filtered:
its ink is already light on the dark palette, and the filter would flip it straight back to
dark. Pick one per drawing.

#### Automatic dark mode

A drawing whose file is named `something_auto.svg` (or `.png`) keeps every colour its
exporter baked in and is put through a CSS filter on the dark palette instead:

```css
--auto-dark-filter: invert(93%) hue-rotate(180deg);
```

`invert()` flips lightness, and takes hue with it; `hue-rotate(180deg)` puts the hue back.
So light and dark trade places while a red stays a red — the drawing's own palette is
preserved, rather than replaced. 93% rather than 100% stops the ink at a near-white instead
of a pure one; it is the constant Excalidraw ships, and Excalidraw's dark mode is this same
filter. Transparent pixels stay transparent, so the page background shows through and there
is no baked-in white box to strip.

`resolve_images` reads the suffix off the filename and adds `class="auto-dark"`; `main.css`
carries the filter in the same three selector groups the palette itself uses, so the toggle
moves it in both directions. `static/images/spatial_partitioning_auto.svg` is the drawing
on the SPH article, and is a plain unedited Excalidraw export — the point of this route is
that there is nothing to prepare and no dark value to choose.

It is the default, and its cost is that it is all-or-nothing. There is no way to exempt one
colour, so an accent that already read on both grounds gets flipped anyway, and a
photograph or a screenshot embedded in the drawing inverts with everything else. It also
drives the ink to a neutral grey — `#d1d1d1` against a `--fg` of `#e8e8dc` — so a filtered
drawing sits a shade cooler than the prose beside it, and cannot follow if the palette is
retuned. When either of those matters, convert the drawing instead and drop the suffix.

Only the exact-match branch reads the suffix. A `_light`/`_dark` pair is the manual route,
and a name asking for both at once names nothing coherent.

**Export with Excalidraw's dark mode off.** That toggle does not attach a filter to the
export — it bakes the filtered colours into the file, and it is the same filter this site
applies, so the drawing arrives already inverted and gets inverted a second time. The
symptom is a drawing that ignores the theme and merely flips whenever the theme changes:
washed-out pale ink on the light palette, and dark-on-dark once the filter lands. Worse,
`invert(93%)` is not an involution — applied twice, `#1e1e1e` lands on `#383838` rather
than back on `#1e1e1e` — so the dark rendering is wrong rather than accidentally right.
The `_auto` file must be the plain light export; the filter is what produces the dark one.

The tell is a light-dominant ink: `./recolor.py drawing.svg --list` on a correct export
reports a near-black as its most common stroke, and a dark-mode export reports a near-white
(`#d3d3d3` is exactly what `#1e1e1e` becomes). Worth checking whenever a drawing is
re-exported, since nothing in the build can tell the two apart.

#### Inline SVG and currentColor

An SVG whose strokes are `currentColor` inherits the page's `color`, which is `--fg`, so
one file serves both palettes and follows the toggle with nothing to keep in sync. This
only works when the SVG is *inline* in the document: referenced through `<img>` it is a
separate document, no CSS crosses that boundary, and `currentColor` resolves against the
SVG's own initial colour — black — on both themes.

So `resolve_images` in `ssg.cpp` splices the file in. Write the ordinary
`![alt](/images/kernels.svg)` and the drawing is read from `static/` and emitted as an
`<svg>` element in place of the `<img>`, which keeps a screenful of path data out of the
markdown. The alt text becomes `role="img"` plus `aria-label`, since an `<svg>` has no
accessible name unless given one; an empty alt becomes `aria-hidden="true"`, which is what
a decorative image means. An XML declaration or doctype at the head of the file is dropped,
being legal in a standalone `.svg` and not inside an HTML body. A file that is missing or
has no `<svg` root falls back to the original `<img>` and warns, so a broken drawing looks
broken rather than leaving a hole in the page.

`main img, main svg` caps both at the prose column. The second half of that selector is
load-bearing: an inlined drawing carries the `width` attribute its exporter wrote, and
`main img` cannot match an `<svg>`.

Excalidraw has no currentColor option — it bakes literal hex into every shape. `recolor.py`
does the conversion after export; `./recolor.py drawing.svg --list` reports what colours
are in the file, and the default converts the most common stroke. It leaves fills alone
unless asked, because `currentColor` carries exactly one colour and a filled shape turned
text-coloured is rarely wanted, and it only reports the embedded `@font-face` blocks rather
than stripping them, since dropping them changes how text renders. A drawing using several
colours to distinguish things converts only its ink; the accents stay literal, which is
usually right, as a red or a green reads on either palette.

Two limits of inlining, both fine at this size. An inlined SVG is not cached across pages,
so a drawing used on several pages ships with each. And ids inside two SVGs on one page can
collide, since nothing namespaces them — Excalidraw only emits ids under `<defs>`, which is
why this has not bitten yet.

#### A palette of named colours

`currentColor` carries exactly one colour, so it runs out as soon as a drawing uses
colour to single something out — a highlighted region, a second series, a fill behind the
ink. A CSS custom property is the same trick one level up: an inlined SVG resolves
`var(--fig-red)` against `:root` exactly as it resolves `currentColor` against the page's
`color`, so the page owns the value and the drawing owns only the name. One file, both
palettes, no second download.

No `--fig-*` token is defined at the moment — every drawing on the site takes one of the
cheaper routes. Adding one means declaring it in all three places the palette is declared,
so the toggle moves it, exactly like `--fg` or `--accent`.

Choosing the dark value is the work this route asks for, and it is not a matter of dimming
the light one. A bright fill is the case that needs it most: a wash picked against cream
paper is a near-white on a dark ground and reads as a hole cut in the page rather than as a
tint, so its dark value is the same hue composited down to just off `--bg` — a shape sitting
on the page, not a light source on it. A saturated stroke usually survives with a small lift
(`#fa5252` → `#ff6b6b`), and a mid-dark colour goes the other way and wants lifting rather
than dimming (`#2f9e44` → `#51cf66`). The filter route gets all three of those roughly right
for free, which is why this one is reserved for when *roughly* is not good enough.

`recolor.py --map` does the rewrite, and is repeatable:

```bash
./recolor.py drawing.svg --map '#ffc9c9=--fig-red-soft' --map '#fa5252=--fig-red' --in-place
```

It writes an inline `style` declaration rather than putting `var()` back in the presentation
attribute it replaces. `fill="var(--x)"` is SVG 2 behaviour and support for it is uneven; an
engine that does not parse it drops the attribute and paints the shape black, which is the
worst failure available. `var()` in a `style` attribute is as old as custom properties. The
literal stays on as the `var()` fallback, which is also what keeps the drawing correct when
it is opened on its own, outside any page that defines the token. Inline style beats a
stylesheet rule, so a drawing converted this way cannot be repainted from CSS afterwards —
change the token, not the drawing.

`--map` and `--ink` compose: `--ink` sends the ink to `currentColor`, `--map` sends the
accents to tokens, and a run with only `--map` deliberately skips the default ink guess so
it does not convert a stroke nobody asked about. Colours inside an SVG's own `<style>` block
are not rewritten; Excalidraw only puts `@font-face` there.

#### Light and dark variants

For a drawing that survives none of the three cheaper routes — a photograph, or anything
whose fills must genuinely differ per theme rather than merely invert — ship it twice as
`name_light.png` and `name_dark.png` and reference the name that is in neither file:
`![alt](/images/kernels.png)`. `resolve_images` emits both, classed `light-only` and
`dark-only`, which `main.css` shows and hides. This works for any file type, `.svg`
included, in which case both variants are inlined.

The rule is exact match first: if `/images/kernels.png` exists on disk it is emitted
untouched, so this costs nothing for ordinary images and naming a variant outright still
works. A name matching neither the file nor a complete pair is left alone and warned about
on stderr, because rewriting it could only turn one broken URL into a different broken URL.
Remote and relative `src` values are skipped outright — only a root-absolute path names a
file in `static/`.

The class is merged into whatever class the tag already carries, for the same reason
`mark_current_link` merges: a tag with two `class` attributes keeps only the first. The
search is bounded to the root tag, since an Excalidraw file carries a `class` further down
that must not be the one found. And because the pass runs over rendered HTML rather than
markdown, it applies equally to an `<img>` hand-written inside a `<figure>`.

`<picture>` with `prefers-color-scheme` is the obvious alternative and it is wrong here:
that media query sees the OS preference and not the in-page theme toggle, so it would
strand the wrong variant on screen whenever a reader's toggle disagrees with their OS.
Two tags and a class each is what follows the toggle.

This is the last resort of the four, not the first. The cost is that a pair is always paid
for twice — browsers fetch both `<img>` variants regardless of which is displayed, and an
inlined pair puts both drawings in the HTML. Reach for it only when the drawing is a
photograph, where inverting is exactly wrong; anything flat-coloured is better served by
`_auto`, which is one file and no second export.

### Code blocks

A fenced block carries a language and is syntax-highlighted at build time:

````markdown
```odin
density :: proc(ps: ^[dynamic]Particle) -> f32 { ... }
```
````

Four languages are described — `c`, `cpp`, `odin`, `python`, with the usual
aliases (`c++`, `cc`, `cxx`, `h`, `hpp`, `hxx`, `py`). A fence with no language
renders as a plain escaped block. So does one naming a language meant to be left
alone (`text`, `console`, `sh`, `bash`, `make`, `toml`, `md`, `diff` and friends);
any *other* name is a typo or a language nobody has taught the generator yet, and
warns on stderr while still rendering the code.

Highlighting happens in `ssg.cpp` rather than in the browser. A client-side
library was the obvious alternative and buys nothing here: Odin is in neither
highlight.js nor Prism, so either would have wanted a hand-written grammar anyway,
and all four languages are C-like enough to share one tokenizer parameterised by a
handful of flags. Doing it at build time costs the reader no download and no flash
of unhighlighted code, and the output is plain `<span class="tok-…">`, so a
snippet is coloured by the same palette as everything else and follows the theme
toggle for free.

Seven token classes are emitted — `tok-com`, `tok-kw`, `tok-typ`, `tok-str`,
`tok-num`, `tok-fn`, `tok-pre`. Anything the tokenizer cannot classify is left
unmarked and stays at `--fg`, which is what keeps a block from becoming a
rainbow. A named literal (`true`, `nil`, `None`, `NULL`) is deliberately given the
number colour rather than an eighth class of its own.

The lexer knows what each language needs and nothing more: `//` or `#` line
comments, `/* */` blocks (nested, for Odin), `#include <stdio.h>` with the path as
a string, Odin's `#directive`, `$T` and `` `raw string` ``, Python's `"""docstring"""`
and head-of-line `@decorator`, string prefixes that bind to the quote after them
(`f"…"`, `L"…"`), and C++14's `1'000'000`. An identifier followed by `(` is called
a function, which is as much as a lexer can know without a symbol table and reads
correctly often enough to be worth it. A user-defined type is not coloured, since
telling one from a variable needs the same symbol table.

#### How a block survives the other passes

`extract_code` runs **first** in `render_body`, before `extract_math` and
`expand_sidenotes`. This is not a preference, it is required: the languages use
those passes' markers as syntax. `$T` is a polymorphic type parameter in Odin and
two of them in one snippet would be lifted as a math span; `arr^[i]` is an index
through a pointer and would become a sidenote. Without this pass, code would reach
cmark already mangled. Inline `` `code` `` is lifted for the same reason, which is
why `` `$T` `` in prose is now safe too.

It is a lift-and-splice like math, with one difference worth understanding: only
the *body* of a block is replaced by a placeholder, and the fence itself is left in
the markdown. cmark therefore still decides what is a code block, what belongs to a
list item, and what the `class="language-…"` says — this pass never has to reason
about block structure, and there is no risk of a `<pre>` landing inside a `<p>`.
Inline spans keep their backticks the same way. `restore_code` runs last, after
`resolve_images` and `restore_math`, so nothing downstream can read the highlighted
markup as its own.

Three limits follow, all deliberate:

- Fence detection is a flat line scan, so it accepts any leading indent and
  dedents the body by the same amount. That is what lets a block inside a list item
  work, at the cost that a line reading ```` ``` ```` inside a four-space *indented*
  code block would be taken for a fence. The site uses fenced blocks, so this has
  not come up.
- An inline span is bounded to one line. It cannot cross a paragraph break anyway,
  and the bound stops an unmatched backtick reaching for one several paragraphs down
  — the same reasoning as the sidenote scanner's blank-line test.
- `substitute_frontmatter` still runs before all of this, so a literal `{{title}}`
  inside a snippet is substituted. Exact-match only, so ordinary braces are safe.

#### Styling

`main pre` is the box: `--code-bg` a shade of the paper, a `--code-border`
hairline, and `overflow-x: auto`. The box is capped at the prose column exactly as
an image is, so a line longer than the measure scrolls inside the box rather than
pushing the whole page sideways on a phone.

Code is set in Cascadia Code at `0.82em`. The step down is not decoration: a mono
face sets visually larger than ET Book at the same nominal size — the book face has
a small x-height and Cascadia a large one — so code at 1em would shout over the
prose it sits in.

`main pre code { font-family: inherit }` is load-bearing. `normalize.css` sets
`font-family` on `code` directly, and a declaration beats inheritance however
specific the parent's selector is, so without it the `<code>` cmark nests inside
every `<pre>` falls back to the generic monospace and the vendored face never
reaches a code block. Inline code is selected as `main :not(pre) > code`, which is
what keeps the two apart.

### References

Citations go in a sidenote at the point of use. There is no bibliography section and no
back-references — the gutter exists so the source can sit beside the claim it supports,
which is the Tufte convention the layout was built for.

A paper is author, italic title, venue and year, with a stable identifier as the link:

```markdown
The density estimate is the standard kernel sum^[Müller, Charypar and Gross,
*Particle-Based Fluid Simulation for Interactive Applications*, SCA 2003.
[doi:10.1145/846276.846298](https://doi.org/10.1145/846276.846298)]
```

Italic rather than quoted titles: it is the book convention the typography already
follows, and cmark does not smarten quotes, so `"Title"` renders with straight typewriter
quotes against ET Book's curly apostrophes. Link a DOI or an arXiv abstract page, never a
publisher or personal-site PDF, which move.

A website is an inline link on a descriptive phrase — `[VulkanGuide](https://vkguide.dev/)`.
Save a sidenote for when there is something to *say* about the source rather than merely
to point at it.

Two limits worth knowing. Numbering is a per-page CSS counter, so citing one work twice
produces two notes; give the full citation first and a short form (`^[Müller et al. 2003.]`)
after. And a bare URL as link text can overflow the 16rem gutter — hyphens and slashes
give the browser somewhere to break, but a long unbroken token does not, so title the link
instead.

Internal links follow the same shape but point at the *built* path, root-absolute:
`[renderer](/articles/renderer.html)`. Linking the `.md` source 404s — only `static/` is
copied verbatim — and a relative path resolves against `/articles/`, not the site root.

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

Code is the second exception: `--font-mono` is Cascadia Code, self-hosted from
`static/fonts/cascadia-code/` under OFL 1.1, latin and latin-ext only, and variable over
`wght` 200–700 on the same terms as Roboto Mono — two files, ~82KB, and the weight range
must stay a range.

This is Microsoft's upstream release, not the Nerd Fonts patched build of it,
**CaskaydiaCode**. They are the same drawing; the patch only adds ~3500 icon glyphs, which
no C, C++, Odin or Python snippet has a use for, at roughly thirty times the file size.
Reach for the patched build only if a snippet ever genuinely needs a Powerline or devicon
glyph, and subset it before vendoring if so.

No italic is vendored, which is why comments are told apart by colour alone. Adding one
would put a third font file on every page that carries code; if a code block is ever set in
italic without it, the browser will synthesise a slant rather than fall back, and it will
look wrong next to ET Book's real italic.

Cascadia's programming ligatures (`->`, `!=`, `>=`) are contextual alternates and are on by
default. `font-variant-ligatures: none` on `main pre` turns them off.

Headings follow book-typography convention rather than web convention — hierarchy comes
from size and style, so they stay at `font-weight: 400` and `h3` is italic. Do not
"fix" this by bolding them.

`static/css/normalize.css` is vendored (normalize.css v8.0.1) — do not modify.

## Conventions

- Semantic HTML5 (`<header>`, `<nav>`, `<main>`, `<footer>`, `<article>`)
- Comments in `ssg.cpp` explain *why*, and section headers use the existing `// ---` banner style
- Markdown bodies start at `##`; `#` is reserved for the opt-in `show_title` heading
- Never hand-edit `_site/`; change the source and rebuild
