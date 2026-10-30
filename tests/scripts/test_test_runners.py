# SPDX-License-Identifier: MIT
from contextlib import redirect_stderr, redirect_stdout
import io
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from support import ROOT, load_script

# From scripts/, which support puts on sys.path.
import build_trees
import test_runner as runner


simulated = load_script("defy_simulated_tests", "test")
host = load_script("defy_host_tests", "test-host")


def gnu_diff():
    try:
        return "GNU" in subprocess.run(["diff", "--version"], capture_output=True, text=True).stdout
    except OSError:
        return False


class TestRunnerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo with spaces"
        self.root.mkdir()
        self.output = Path(self.temporary.name) / "new build tree"

    def driver(self, name, cmake=""):
        directory = self.root / "tests/drivers" / name
        directory.mkdir(parents=True)
        (directory / "CMakeLists.txt").write_text(cmake)

    def keymap_case(self, name):
        directory = self.root / "tests/keymap" / name
        directory.mkdir(parents=True)
        (directory / "native_sim.keymap").touch()
        return directory

    def host_suite(self, name):
        source = self.root / "tests/host" / name / "main.c"
        source.parent.mkdir(parents=True)
        source.touch()
        return source

    def run_suites(self, kinds, **kwargs):
        with redirect_stdout(io.StringIO()) as output:
            result = simulated.run_suites(self.root, kinds, build_root=self.output, **kwargs)
        return result, output.getvalue()

    def test_driver_switches_build_once_off_and_once_each_on(self):
        self.driver("plain")
        self.driver("one", 'option(DEFY_TEST_RIGHT_HALF "Render as the right half" OFF)\n')
        self.driver("two", 'option(DEFY_TEST_A "A" OFF)\nif(DEFY_TEST_A)\nendif()\n'
                           '  option( DEFY_TEST_B_C "B, and C" OFF )\n')
        plans = simulated.driver_plans(self.root, self.output)
        self.assertEqual({plan["variant"]: plan["arguments"][1:] for plan in plans}, {
            "one": ["-DDEFY_TEST_RIGHT_HALF=OFF"],
            "one-right-half": ["-DDEFY_TEST_RIGHT_HALF=ON"],
            "plain": [],
            "two": ["-DDEFY_TEST_A=OFF", "-DDEFY_TEST_B_C=OFF"],
            "two-a": ["-DDEFY_TEST_A=ON", "-DDEFY_TEST_B_C=OFF"],
            "two-b-c": ["-DDEFY_TEST_A=OFF", "-DDEFY_TEST_B_C=ON"],
        })
        self.assertEqual(len({plan["directory"] for plan in plans}), 6)
        self.assertTrue(all(plan["arguments"][0] == f"-DEXTRA_ZEPHYR_MODULES={self.root}"
                            for plan in plans))

    def test_every_suite_declares_its_switches(self):
        for source in (ROOT / "tests/drivers").glob("*/CMakeLists.txt"):
            text = source.read_text()
            used = set(simulated.re.findall(r"\b(DEFY_TEST_\w+)\b", text))
            declared = {name for name, _ in simulated.TEST_OPTION.findall(text)}
            with self.subTest(suite=source.parent.name):
                self.assertEqual(used, declared)

    @unittest.skipUnless(gnu_diff(), "the keymap check needs GNU diff, as in ZMK's container")
    def test_keymap_check_filters_events_and_compares_the_snapshot(self):
        case = self.keymap_case("case")
        (case / "events.patterns").write_text("s/.*hid_listener_keycode_//p\n")
        directory = self.output / "keymap/case"
        (directory / "zephyr").mkdir(parents=True)
        executable = directory / "zephyr/zmk.exe"
        executable.write_text("#!/bin/sh\necho '[00:00:00.010,000] <dbg> zmk: "
                              "hid_listener_keycode_pressed: usage 0x04'\necho 'other line'\n")
        executable.chmod(0o755)
        snapshot = case / "keycode_events.snapshot"
        command = ["sh", "-c", simulated.KEYMAP_CHECK, "sh", str(directory), str(case)]

        def check(**environment):
            env = {key: value for key, value in os.environ.items() if key != "ZMK_TESTS_AUTO_ACCEPT"}
            return subprocess.run(command, env={**env, **environment}, capture_output=True,
                                  text=True).returncode

        snapshot.write_text("pressed: usage 0x04 \n")
        self.assertEqual(check(), 0)
        self.assertIn("other line", (directory / "keycode_events_full.log").read_text())
        snapshot.write_text("pressed: usage 0x05\n")
        self.assertEqual(check(), 1)
        self.assertEqual(check(ZMK_TESTS_AUTO_ACCEPT="1"), 0)
        self.assertEqual(snapshot.read_text(), "pressed: usage 0x04\n")
        # A simulator that fails after the expected events fails the case.
        executable.write_text(executable.read_text() + "exit 42\n")
        self.assertEqual(check(), 1)

    def test_failure_runs_the_remaining_plans(self):
        self.keymap_case("case")
        self.driver("first")
        self.driver("second")
        calls = []

        def run(label, command, **kwargs):
            calls.append((label, command, kwargs))
            return label not in ("keymap/case", "drivers/first build", "drivers/second")

        result, _ = self.run_suites(list(simulated.KINDS), run=run)
        self.assertEqual(result, 1)
        self.assertEqual([call[0] for call in calls],
                         ["studio build", "studio", "keymap/case build", "keymap/case",
                          "drivers/first build", "drivers/second build", "drivers/second"])

    def test_a_file_that_appears_or_goes_away_forces_a_pristine_build(self):
        self.driver("a", 'option(DEFY_TEST_X "X" OFF)\n')
        calls = []

        def run(label, command, **kwargs):
            calls.append(command)
            return True

        def pristine():
            calls.clear()
            self.run_suites(["drivers"], drivers=["a"], jobs=1, run=run)
            return calls[0][3]

        self.assertEqual(pristine(), "always")
        self.assertEqual(pristine(), "auto")
        overlay = self.root / "tests/drivers/a/app.overlay"
        overlay.write_text("/ {};\n")
        self.assertEqual(pristine(), "always")
        overlay.write_text("/ { edited; };\n")
        self.assertEqual(pristine(), "auto")
        (self.root / "tests/drivers/a/src").mkdir(exist_ok=True)
        (self.root / "tests/drivers/a/src/new.c").write_text("")
        self.assertEqual(pristine(), "auto")
        for path in ("dts/bindings/new.yaml", "tests/drivers/common/dts/bindings/new.yaml"):
            binding = self.root / path
            binding.parent.mkdir(parents=True)
            binding.write_text("")
            self.assertEqual(pristine(), "always")
            self.assertEqual(pristine(), "auto")
        (self.root / "tests/drivers/b").mkdir()
        (self.root / "tests/drivers/b/app.overlay").write_text("")
        self.assertEqual(pristine(), "auto")
        # A changed Kconfig file, the module's or the suite's own, but not another suite's.
        for path in ("lighting/Kconfig", "tests/drivers/a/Kconfig"):
            kconfig = self.root / path
            kconfig.parent.mkdir(parents=True, exist_ok=True)
            kconfig.write_text("config A\n    default 1\n")
            self.assertEqual(pristine(), "always")
            self.assertEqual(pristine(), "auto")
            kconfig.write_text("config A\n    default 2\n")
            self.assertEqual(pristine(), "always")
        (self.root / "tests/drivers/b/Kconfig").write_text("config B\n")
        self.assertEqual(pristine(), "auto")
        overlay.unlink()
        self.assertEqual(pristine(), "always")
        stamp = self.output / "drivers/a" / build_trees.STAMP
        self.assertEqual(build_trees.pristine(stamp.parent, stamp.read_text().strip()), "auto")

    def test_only_a_build_from_the_stamped_arguments_leaves_them_out(self):
        self.driver("a")
        builds = []

        def run(label, command, **kwargs):
            if label.endswith(" build"):
                builds.append(command)
            return failing not in label

        failing = "nothing"
        self.run_suites(["drivers"], jobs=1, run=run)
        self.run_suites(["drivers"], jobs=1, run=run)
        self.assertEqual(builds[0][2:4], ["-p", "always"])
        self.assertEqual(builds[0][builds[0].index("--") + 1:],
                         [f"-DEXTRA_ZEPHYR_MODULES={self.root}"])
        self.assertEqual(builds[1][2:4], ["-p", "auto"])
        self.assertNotIn("--", builds[1])
        # A failed build from new arguments leaves no stamp, so the next starts afresh.
        (self.root / "tests/drivers/a/app.overlay").write_text("")
        failing = "build"
        result, _ = self.run_suites(["drivers"], jobs=1, run=run)
        self.assertEqual(result, 1)
        (self.root / "tests/drivers/a/app.overlay").unlink()
        failing = "nothing"
        self.run_suites(["drivers"], jobs=1, run=run)
        self.assertEqual(builds[-1][2:4], ["-p", "always"])
        self.assertIn("--", builds[-1])

    def test_host_suites_stop_at_the_first_sanitizer_report(self):
        source = self.host_suite("defy_battery")
        command = host.compile_command(self.root, source, self.output / "battery", "clang")
        # UBSan reports and carries on otherwise, and the suite would pass.
        self.assertIn("-fno-sanitize-recover=all", command)

    def test_host_failure_does_not_skip_other_python_or_c_suites_and_keeps_logs(self):
        self.host_suite("first")
        self.host_suite("second")
        self.host_suite("third")
        calls = []

        def run(label, command, **kwargs):
            calls.append((label, kwargs))
            Path(kwargs["log_path"]).write_text(label)
            return label not in ("tests/scripts", "host/first build", "host/second")

        with redirect_stdout(io.StringIO()) as output:
            self.assertEqual(host.run_suites(self.root, clang="clang", run=run, jobs=1), 1)
        self.assertEqual(
            [call[0] for call in calls],
            ["tests/scripts", "host/first build", "host/second build",
             "host/second", "host/third build", "host/third"],
        )
        self.assertTrue(all(call[1]["timeout"] > 0 for call in calls))
        summary = output.getvalue().splitlines()
        self.assertEqual(summary[0], "Summary: 1 of 4 passed")
        logs = [Path(line.split("(log: ")[1][:-1]) for line in summary[1:]]
        self.assertEqual([log.read_text() for log in logs],
                         ["tests/scripts", "host/first build", "host/second"])
        shutil.rmtree(logs[0].parent)

    def test_host_without_clang_runs_the_python_suites_and_fails(self):
        self.host_suite("a")
        calls = []
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()) as errors:
            result = host.run_suites(self.root, clang=None,
                                     run=lambda label, command, **kwargs: calls.append(label) is None)
        self.assertEqual(result, 1)
        self.assertEqual(calls, ["tests/scripts"])
        self.assertIn("clang is required", errors.getvalue())

    def test_command_creates_log_parent_and_reports_failed_output(self):
        log = self.output / "drivers/example.run.log"
        stderr = io.StringIO()
        with redirect_stderr(stderr), redirect_stdout(io.StringIO()):
            result = runner.run_command(
                "bad suite", [sys.executable, "-c", "print('failure detail'); raise SystemExit(7)"],
                cwd=self.root, timeout=5, log_path=log,
            )
        self.assertFalse(result)
        self.assertEqual(log.read_text().strip(), "failure detail")
        self.assertIn("status 7", stderr.getvalue())
        self.assertIn("failure detail", stderr.getvalue())
        self.assertIn(str(log), stderr.getvalue())

    def test_missing_executable_is_a_failure(self):
        with redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            self.assertFalse(runner.run_command(
                "missing", [str(self.root / "missing")], cwd=self.root, timeout=5))


if __name__ == "__main__":
    unittest.main()
