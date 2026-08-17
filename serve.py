#!/usr/bin/env python3
"""Preview server: rebuild the site on save, serve it, reload the browser.

    ./serve.py                 http://127.0.0.1:8000, drafts included
    ./serve.py --port 9000
    ./serve.py --host 0.0.0.0  reachable from a phone on the same network
    ./serve.py --no-drafts     the published view, as the deploy sees it
    ./serve.py --no-reload     rebuild on save, but leave the browser alone

Standard library only, on purpose: the repo's dependency budget is a compiler,
make, libcmark and toml.hpp, and a preview server is not worth widening it for.
Change detection is a 50-file mtime poll rather than inotify, which at this size
costs nothing measurable and stays portable.

Nothing here writes to _site/. The reload snippet is spliced into HTML on its way
out of the socket, so the built output stays byte-identical to what ./ssg wrote
and what gets deployed.
"""

import argparse
import errno
import functools
import http.server
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

# Directories whose contents feed a build, plus the files that mean the
# generator itself has to be recompiled first.
WATCHED_DIRS = ("content", "templates", "static")
COMPILED_SOURCES = ("ssg.cpp", "Makefile")

OUTPUT_DIR = "_site"

# Editors write scratch files next to the real one; rebuilding on those is noise
# at best and a build of a half-written file at worst.
IGNORED_SUFFIXES = (".swp", ".swo", ".swx", ".tmp", ".bak", "~")
IGNORED_PREFIXES = (".#", "#", ".goutputstream")

POLL_SECONDS = 0.3
# After the first change, wait for the tree to stop moving. A single save often
# lands as several filesystem events (write temp, rename, chmod), and some tools
# save every file in a directory at once.
SETTLE_SECONDS = 0.15

LIVERELOAD_PATH = "/__livereload"
# How long a poll is parked server-side before answering "nothing yet". Long
# enough that idle polling is rare, short enough to stay under any proxy or
# browser idle timeout.
LIVERELOAD_TIMEOUT = 25.0

# Injected before </body>. The generation it starts from is stamped in at serve
# time, so a page built mid-poll still notices the build that replaced it.
RELOAD_SNIPPET = """
<script>
// Injected by serve.py; not present in the built output.
(function () {
  var known = %d;
  function poll() {
    fetch("%s?generation=" + known, { cache: "no-store" })
      .then(function (response) { return response.text(); })
      .then(function (body) {
        if (parseInt(body, 10) !== known) location.reload();
        else poll();
      })
      // The server is gone (restart, or Ctrl-C). Keep trying so the page comes
      // back on its own rather than needing a manual refresh.
      .catch(function () { setTimeout(poll, 1000); });
  }
  poll();
})();
</script>
"""


# ---------------------------------------------------------------------------
# Watching
# ---------------------------------------------------------------------------


def is_ignored(name: str) -> bool:
    return name.endswith(IGNORED_SUFFIXES) or name.startswith(IGNORED_PREFIXES)


def snapshot() -> dict[str, float]:
    """Map every watched file to its mtime. Comparing two of these detects
    content edits, new files and deletions in one shot."""
    state: dict[str, float] = {}

    for name in COMPILED_SOURCES:
        path = Path(name)
        if path.is_file():
            state[name] = path.stat().st_mtime

    for directory in WATCHED_DIRS:
        for parent, subdirectories, filenames in os.walk(directory):
            # Hidden directories are tooling, not content.
            subdirectories[:] = [d for d in subdirectories if not d.startswith(".")]
            for filename in filenames:
                if is_ignored(filename):
                    continue
                path = Path(parent) / filename
                try:
                    state[path.as_posix()] = path.stat().st_mtime
                except FileNotFoundError:
                    # Vanished between listing and stat; the next poll settles it.
                    pass

    return state


def wait_for_changes(previous: dict[str, float]) -> tuple[dict[str, float], set[str]]:
    """Block until the tree changes and then stops changing. Returns the new
    snapshot and the paths that differ from the one passed in."""
    while True:
        time.sleep(POLL_SECONDS)
        current = snapshot()
        if current == previous:
            continue

        while True:
            time.sleep(SETTLE_SECONDS)
            settled = snapshot()
            if settled == current:
                break
            current = settled

        changed = {
            path
            for path in previous.keys() | current.keys()
            if previous.get(path) != current.get(path)
        }
        return current, changed


# ---------------------------------------------------------------------------
# Building
# ---------------------------------------------------------------------------


def run(command: list[str]) -> tuple[int, str]:
    result = subprocess.run(command, capture_output=True, text=True)
    return result.returncode, (result.stdout or "") + (result.stderr or "")


def build(include_drafts: bool, compile_first: bool) -> bool:
    """make (only when the generator's own sources moved) then ./ssg. Returns
    whether the site in _site/ is now current."""
    started = time.monotonic()

    if compile_first:
        code, output = run(["make"])
        if code != 0:
            # Do not fall through to ./ssg: the binary is now the stale one, and
            # running it would quietly emit a site full of {{placeholders}}.
            print(output.rstrip())
            print("compile failed; still serving the last good build\n")
            return False
        # A real compile is worth seeing; "'ssg' is up to date" is not, and it
        # would otherwise print on every save that only touched content.
        noise = "is up to date"
        lines = [line for line in output.splitlines() if noise not in line]
        if any(line.strip() for line in lines):
            print("\n".join(lines).rstrip())

    command = ["./ssg"] + (["--drafts"] if include_drafts else [])
    code, output = run(command)
    if code != 0:
        print(output.rstrip())
        print("build failed; still serving the last good build\n")
        return False

    pages = sum(1 for line in output.splitlines() if line.startswith("built:"))
    elapsed = (time.monotonic() - started) * 1000
    print(f"  rebuilt {pages} page{'' if pages == 1 else 's'} in {elapsed:.0f}ms")
    for line in output.splitlines():
        # The draft-skipping notice is worth surfacing; the per-page lines are not.
        if line.startswith("skipped"):
            print(f"  {line}")
    return True


def warn_about_stale_output(removed: set[str]) -> None:
    """The generator only ever writes; it does not clear _site. A deleted or
    renamed page therefore lingers in the output until someone says otherwise."""
    for path in sorted(removed):
        if not path.startswith("content/") or not path.endswith(".md"):
            continue
        relative = Path(path).relative_to("content")
        stem = relative.parent / (relative.stem + ".html")
        print(f"  note: {path} is gone; {OUTPUT_DIR}/{stem} is now stale (make clean)")


# ---------------------------------------------------------------------------
# Serving
# ---------------------------------------------------------------------------


class Reloader:
    """A build counter the HTTP threads can park on until it moves."""

    def __init__(self) -> None:
        self.generation = 0
        self._changed = threading.Condition()

    def bump(self) -> None:
        with self._changed:
            self.generation += 1
            self._changed.notify_all()

    def wait_past(self, known: int, timeout: float) -> int:
        with self._changed:
            if self.generation == known:
                self._changed.wait(timeout)
            return self.generation


class PreviewHandler(http.server.SimpleHTTPRequestHandler):
    reloader: Reloader
    inject_reload: bool = True
    verbose: bool = False

    def do_GET(self) -> None:
        if urlsplit(self.path).path == LIVERELOAD_PATH:
            self.serve_livereload()
            return

        if self.inject_reload:
            page = self.locate_html()
            if page is not None:
                self.serve_html(page)
                return

        super().do_GET()

    def locate_html(self) -> Path | None:
        """The .html file this request resolves to, if it is one. Directory URLs
        without a trailing slash are left to the base class, which has to answer
        with a redirect first or every relative link on the page resolves against
        the wrong base."""
        path = Path(self.translate_path(self.path))
        if path.is_dir():
            if not urlsplit(self.path).path.endswith("/"):
                return None
            path = path / "index.html"
        return path if path.suffix == ".html" and path.is_file() else None

    def serve_html(self, page: Path) -> None:
        try:
            body = page.read_bytes()
        except OSError:
            self.send_error(404)
            return

        snippet = (RELOAD_SNIPPET % (self.reloader.generation, LIVERELOAD_PATH)).encode()
        if b"</body>" in body:
            body = body.replace(b"</body>", snippet + b"</body>", 1)
        else:
            body += snippet

        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.write_body(body)

    def serve_livereload(self) -> None:
        query = parse_qs(urlsplit(self.path).query)
        try:
            known = int(query.get("generation", ["-1"])[0])
        except ValueError:
            known = -1

        generation = str(self.reloader.wait_past(known, LIVERELOAD_TIMEOUT)).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(generation)))
        self.end_headers()
        self.write_body(generation)

    def write_body(self, body: bytes) -> None:
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            # Navigating away mid-request, or a parked poll outliving its tab.
            pass

    def end_headers(self) -> None:
        # Every response, including the ones the base class builds. A cached
        # stylesheet is the classic "why is my change not showing" of a dev
        # server, and it also stops a conditional request from getting a 304
        # that predates the rebuild.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_request(self, code="-", size="-") -> None:
        status = code.value if hasattr(code, "value") else code
        if self.verbose or (isinstance(status, int) and status >= 400):
            super().log_request(code, size)

    def log_message(self, format: str, *args) -> None:
        sys.stderr.write("  %s\n" % (format % args))


def start_server(host: str, port: int, handler) -> http.server.ThreadingHTTPServer:
    try:
        server = http.server.ThreadingHTTPServer((host, port), handler)
    except OSError as error:
        if error.errno == errno.EADDRINUSE:
            sys.exit(f"port {port} is already in use; try --port {port + 1}")
        raise
    # Parked livereload polls must not keep the process alive on Ctrl-C.
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--host", default="127.0.0.1",
                        help="0.0.0.0 to reach the preview from another device")
    parser.add_argument("--no-drafts", action="store_true",
                        help="build as the deploy does, without draft pages")
    parser.add_argument("--no-reload", action="store_true",
                        help="rebuild on save without reloading the browser")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="log every request, not just failures")
    arguments = parser.parse_args()

    # Python block-buffers stdout when it is not a terminal, which would hide the
    # rebuild log behind a pipe or a tmux capture until the buffer filled.
    sys.stdout.reconfigure(line_buffering=True)

    # The generator resolves content/, templates/ and static/ against the current
    # directory, so anchor to the repo regardless of where this was invoked.
    os.chdir(Path(__file__).resolve().parent)

    include_drafts = not arguments.no_drafts
    reloader = Reloader()

    print(f"building {OUTPUT_DIR}/{' with drafts' if include_drafts else ''}")
    build(include_drafts, compile_first=True)

    class BoundHandler(PreviewHandler):
        pass

    BoundHandler.reloader = reloader
    BoundHandler.inject_reload = not arguments.no_reload
    BoundHandler.verbose = arguments.verbose

    # directory is a constructor argument rather than a class attribute, so the
    # handler goes to the server pre-bound to the output tree.
    server = start_server(arguments.host, arguments.port,
                          functools.partial(BoundHandler, directory=OUTPUT_DIR))
    print(f"serving http://{arguments.host}:{arguments.port}/  (Ctrl-C to stop)")
    print(f"watching {', '.join(WATCHED_DIRS)} and {', '.join(COMPILED_SOURCES)}\n")

    state = snapshot()
    try:
        while True:
            state, changed = wait_for_changes(state)

            for path in sorted(changed):
                print(f"changed: {path}")
            warn_about_stale_output({p for p in changed if not Path(p).exists()})

            compile_first = any(path in COMPILED_SOURCES for path in changed)
            if build(include_drafts, compile_first) and not arguments.no_reload:
                reloader.bump()
    except KeyboardInterrupt:
        print("\nstopping")
        server.shutdown()

    return 0


if __name__ == "__main__":
    sys.exit(main())
