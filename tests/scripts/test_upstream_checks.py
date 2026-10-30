# SPDX-License-Identifier: MIT
"""What a ZMK upgrade relies on: cmake/defy_upstream.cmake's checks pass on
unchanged upstream files and stop the configuration on changed ones, naming
the use to review."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from support import ROOT

HELPER = ROOT / "cmake/defy_upstream.cmake"
UPSTREAM = "int main(void) { return foo(1); }\n"


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class UpstreamCheckTests(unittest.TestCase):
    def configure(self, body):
        """Configures a project that calls the checks on upstream.c, and
        returns its status and output, white space folded."""
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "upstream.c").write_text(UPSTREAM)
            (project / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(check NONE)\n"
                f"include({HELPER.as_posix()})\n{body}\n")
            result = subprocess.run(["cmake", "-S", directory, "-B", str(project / "build")],
                                    capture_output=True, text=True)
            return result.returncode, " ".join((result.stdout + result.stderr).split())

    def test_a_call_still_made_passes_and_a_missing_one_stops(self):
        code, _ = self.configure('defy_upstream_uses(${CMAKE_SOURCE_DIR}/upstream.c "the wrap" "foo(")')
        self.assertEqual(code, 0)
        code, output = self.configure(
            'defy_upstream_uses(${CMAKE_SOURCE_DIR}/upstream.c "the wrap" "foo(" "bar(")')
        self.assertNotEqual(code, 0)
        self.assertIn('no longer has "bar("; review the wrap against it', output)
        code, output = self.configure('defy_upstream_uses(${CMAKE_SOURCE_DIR}/gone.c "the wrap" "foo(")')
        self.assertNotEqual(code, 0)
        self.assertIn("gone.c is gone; review the wrap", output)

    def test_a_pin_stops_on_other_contents(self):
        digest = hashlib.sha256(UPSTREAM.encode()).hexdigest()
        code, _ = self.configure(f'defy_upstream_pin(${{CMAKE_SOURCE_DIR}}/upstream.c {digest} "the hook")')
        self.assertEqual(code, 0)
        code, output = self.configure(
            f'defy_upstream_pin(${{CMAKE_SOURCE_DIR}}/upstream.c {"0" * 64} "the hook")')
        self.assertNotEqual(code, 0)
        self.assertIn(f"changed (SHA-256 {digest}); review the hook against it", output)


if __name__ == "__main__":
    unittest.main()
