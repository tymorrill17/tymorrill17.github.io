#!/usr/bin/env python3
"""Rewrite an SVG's ink colour to currentColor, so one file serves both palettes.

Written for Excalidraw exports, which bake literal hex values into every shape
because the exporter has no idea what document the drawing will land in. An SVG
inlined into a page and painted with currentColor inherits the page's `color`
instead, which on this site is --fg — so it follows the theme toggle with no
second file, no second download, and nothing for the generator to resolve.

Standard library only, for the same reason serve.py is: this is a convenience
for preparing an asset, not part of the build, and not worth widening the
dependency budget for.

Two things it deliberately does not do. It leaves shape fills alone unless asked,
because currentColor carries exactly one colour and turning a filled shape into
a solid block of text-colour is rarely what anyone wants — text is the exception
and always converts, since an SVG paints a label with fill and a label that ignores
the theme while the strokes around it follow is nobody's idea of correct. And it
never strips the
embedded fonts by default, since that changes how the drawing renders — it only
tells you how many bytes they cost, which for a drawing with text is often more
than the PNG you were trying to replace.

Usage:
    ./recolor.py diagram.svg --list          # what colours are in here?
    ./recolor.py diagram.svg -o out.svg      # convert the dominant stroke
    ./recolor.py diagram.svg --ink '#1e1e1e' --strip-background --in-place
"""

import argparse
import re
import sys
from collections import Counter

# Colour-bearing properties, in both spellings an exporter may use: presentation
# attributes (stroke="#1e1e1e") and inline style declarations (stroke: #1e1e1e).
ATTRIBUTE = re.compile(r'\b(stroke|fill)="([^"]*)"')
DECLARATION = re.compile(r'\b(stroke|fill)\s*:\s*([^;"\'}]+)')

# Text is painted with fill, not stroke, so it is the one place a fill is
# unambiguously ink rather than decoration — it converts even when shape fills are
# being left alone. Matching the opening tag only keeps the substitution inside it.
TEXT_TAG = re.compile(r"<(?:text|tspan)\b[^>]*>")

# A block of these is what makes an Excalidraw export with text enormous: the
# whole woff2 arrives base64-encoded inside the file.
FONT_FACE = re.compile(r'@font-face\s*\{[^}]*\}', re.DOTALL)

# Excalidraw writes the canvas background as a full-bleed rect before the
# drawing itself. Matched loosely enough to survive attribute reordering.
BACKGROUND_RECT = re.compile(r'<rect\b(?=[^>]*\bx="0")(?=[^>]*\by="0")(?=[^>]*\bfill=)[^>]*?(?:/>|></rect>)')

NOT_A_COLOUR = {"", "none", "transparent", "currentcolor", "inherit"}


def normalise(value):
    """A comparable form of a colour, or None if it does not name one."""
    value = value.strip().lower()
    if value in NOT_A_COLOUR or value.startswith("url("):
        return None
    # #abc and #aabbcc are the same colour and must compare equal.
    if re.fullmatch(r"#[0-9a-f]{3}", value):
        return "#" + "".join(character * 2 for character in value[1:])
    return value


def survey(svg):
    """Every colour in the file, counted, keyed by the property that carries it."""
    found = {"stroke": Counter(), "fill": Counter()}
    for pattern in (ATTRIBUTE, DECLARATION):
        for prop, value in pattern.findall(svg):
            colour = normalise(value)
            if colour:
                found[prop][colour] += 1
    return found


def recolour(svg, ink, properties):
    """Replace `ink` with currentColor wherever one of `properties` carries it."""
    changed = 0

    def convert(text, wanted):
        def substitute(match, template):
            nonlocal changed
            prop, value = match.group(1), match.group(2)
            if prop in wanted and normalise(value) == ink:
                changed += 1
                return template.format(prop=prop)
            return match.group(0)

        text = ATTRIBUTE.sub(lambda m: substitute(m, '{prop}="currentColor"'), text)
        return DECLARATION.sub(lambda m: substitute(m, "{prop}: currentColor"), text)

    # Text first and always, whatever was asked for: a label left literal while the
    # strokes around it follow the theme is the one result nobody wants.
    if "fill" not in properties:
        svg = TEXT_TAG.sub(lambda m: convert(m.group(0), {"fill"}), svg)
    text_changed = changed

    svg = convert(svg, properties)
    return svg, changed, text_changed


def strip_background(svg):
    match = BACKGROUND_RECT.search(svg)
    if not match:
        return svg, None
    return svg[: match.start()] + svg[match.end() :], match.group(0)


def strip_fonts(svg):
    blocks = FONT_FACE.findall(svg)
    return FONT_FACE.sub("", svg), sum(len(block) for block in blocks), len(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("svg", help="the exported SVG to convert")
    parser.add_argument("-o", "--output", help="write here instead of stdout")
    parser.add_argument("-i", "--in-place", action="store_true", help="overwrite the input file")
    parser.add_argument("--list", action="store_true", help="report the colours present and exit")
    parser.add_argument("--ink", help="the colour to convert (default: the most common stroke)")
    parser.add_argument("--fills", action="store_true", help="convert fills too, not just strokes")
    parser.add_argument("--strip-background", action="store_true", help="drop the full-bleed background rect")
    parser.add_argument("--strip-fonts", action="store_true", help="drop embedded @font-face blocks")
    arguments = parser.parse_args()

    try:
        with open(arguments.svg, encoding="utf-8") as handle:
            svg = handle.read()
    except OSError as error:
        sys.exit(f"error: {error}")

    original_size = len(svg)
    found = survey(svg)

    if arguments.list:
        for prop in ("stroke", "fill"):
            print(f"{prop}:")
            for colour, count in found[prop].most_common():
                print(f"  {colour:<24} {count}")
            if not found[prop]:
                print("  (none)")
        _, font_bytes, font_count = strip_fonts(svg)
        if font_count:
            print(f"\n{font_count} embedded @font-face block(s), {font_bytes / 1024:.1f} KB of {original_size / 1024:.1f} KB")
        return

    ink = normalise(arguments.ink) if arguments.ink else None
    if arguments.ink and ink is None:
        sys.exit(f"error: {arguments.ink!r} does not name a colour")
    if ink is None:
        if not found["stroke"]:
            sys.exit("error: no stroke colours found; pass --ink to say what to convert")
        ink = found["stroke"].most_common(1)[0][0]
        print(f"ink: {ink} (most common stroke; --ink overrides)", file=sys.stderr)

    properties = {"stroke", "fill"} if arguments.fills else {"stroke"}
    svg, changed, text_changed = recolour(svg, ink, properties)
    detail = f" ({text_changed} in text)" if text_changed else ""
    print(f"recoloured {changed} occurrence(s) of {ink}{detail}", file=sys.stderr)
    if not changed:
        print("warning: nothing matched — run with --list to see what is in the file", file=sys.stderr)

    if arguments.strip_background:
        svg, removed = strip_background(svg)
        print(f"background: {'removed ' + removed[:60] if removed else 'no full-bleed rect found'}", file=sys.stderr)

    if arguments.strip_fonts:
        svg, font_bytes, font_count = strip_fonts(svg)
        if font_count:
            print(f"fonts: removed {font_count} block(s), {font_bytes / 1024:.1f} KB — text now falls back to a system face", file=sys.stderr)
        else:
            print("fonts: none embedded", file=sys.stderr)

    print(f"size: {original_size / 1024:.1f} KB -> {len(svg) / 1024:.1f} KB", file=sys.stderr)

    if arguments.in_place:
        with open(arguments.svg, "w", encoding="utf-8") as handle:
            handle.write(svg)
    elif arguments.output:
        with open(arguments.output, "w", encoding="utf-8") as handle:
            handle.write(svg)
    else:
        sys.stdout.write(svg)


if __name__ == "__main__":
    main()
