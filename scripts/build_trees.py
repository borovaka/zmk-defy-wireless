# SPDX-License-Identifier: MIT
"""When a build tree starts afresh, for scripts/build and scripts/test.

west, Zephyr and ZMK find some files by name rather than through the build
arguments: boards, snippets, keymaps, overlays and devicetree bindings. CMake
tracks edits to the files it found, but not a file that appears or goes away.
Zephyr also keeps a tree's .config while its configuration fragments stay the
same, so a changed default in a Kconfig file does not reach it. So a build
tree starts afresh, with west's -p always, when its arguments change, when a
file comes or goes where those lookups happen, or when a Kconfig file changes.
A stamp in the tree records all three after each successful build.
"""

import hashlib
import json
from pathlib import Path


STAMP = "defy-build-args"


def layout(root, directories, skip=()):
    """The files under directories, except those under skip, all relative to root."""
    skipped = [root / directory for directory in skip]
    return sorted(str(path.relative_to(root)) for directory in directories
                  for path in (root / directory).rglob("*")
                  if path.is_file() and not any(path.is_relative_to(other) for other in skipped))


def kconfig(root, directories, skip=()):
    """A SHA-256 of the names and contents of the Kconfig files under directories, except
    those under skip, all relative to root."""
    skipped = [root / directory for directory in skip]
    digest = hashlib.sha256()
    for directory in directories:
        for path in sorted((root / directory).rglob("Kconfig*")):
            if path.is_file() and not any(path.is_relative_to(other) for other in skipped):
                digest.update(str(path.relative_to(root)).encode() + b"\0")
                digest.update(path.read_bytes() + b"\0")
    return digest.hexdigest()


def key(arguments, files, kconfig_digest):
    """What the stamp records: the build arguments, the layout() files and the kconfig()
    digest."""
    return json.dumps({"arguments": arguments, "layout": files, "kconfig": kconfig_digest})


def pristine(directory, wanted):
    """west build's -p for a tree: auto if its last successful build had the key wanted."""
    try:
        previous = (Path(directory) / STAMP).read_text().strip()
    except OSError:
        previous = None
    return "auto" if previous == wanted else "always"


def record(directory, built):
    """Stamps a tree with the key of the build that just succeeded."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    (directory / STAMP).write_text(built + "\n")
