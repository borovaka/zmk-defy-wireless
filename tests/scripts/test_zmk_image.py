# SPDX-License-Identifier: MIT
"""The ZMK image that scripts/zmk pins, against its copies in the workflows and
the docs, which .github/dependabot.yml leaves to be updated by hand."""
import re
import unittest

from support import ROOT


PINNED = re.compile(r"zmkfirmware/zmk-dev-arm@sha256:([0-9a-f]{64})")
# Every reference to the image by digest or tag; prose may name it bare.
REFERENCE = re.compile(r"zmkfirmware/zmk-dev-arm([@:]\S*)")
# The files that must name it: the jobs that run in the container, and the
# README's pull commands.
REQUIRED = (".github/workflows/ci.yml", ".github/workflows/release.yml",
            ".github/workflows/build-user-config.yml", "README.md")


class ZmkImageTest(unittest.TestCase):
    def test_every_copy_names_the_digest_scripts_zmk_pins(self):
        pinned = PINNED.findall((ROOT / "scripts/zmk").read_text())
        self.assertEqual(len(pinned), 1)
        expected = f"@sha256:{pinned[0]}"
        files = [*sorted((ROOT / ".github/workflows").glob("*.yml")), ROOT / "README.md",
                 *sorted((ROOT / "docs").rglob("*.md"))]
        found = set()
        for path in files:
            name = str(path.relative_to(ROOT))
            for suffix in REFERENCE.findall(path.read_text()):
                with self.subTest(file=name):
                    self.assertEqual(suffix, expected)
                found.add(name)
        self.assertLessEqual(set(REQUIRED), found)


if __name__ == "__main__":
    unittest.main()
