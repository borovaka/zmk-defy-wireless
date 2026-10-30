# SPDX-License-Identifier: MIT
"""The values scripts/build works out for a build to embed; docs/versions.md."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

from support import load_script


build = load_script("defy_build_info", "build")

HEAD = "2826cb2f0c9a3f1d5e7b8a6c4d2e0f1a3b5c7d9e"
KEYMAP_HEAD = "357c43abe9c0c456d29a1fec90e3a2a8c5705e9c"
DESCRIBE = ("describe", "--tags", "--always", "--dirty", "--abbrev=7")
MANIFEST_REV = ("rev-parse", "--verify", "--quiet", "refs/heads/manifest-rev^{commit}")
STATUS = ("status", "--porcelain", "--untracked-files=no")
ORIGIN = ("config", "--get", "remote.origin.url")
TIME = ("log", "-1", "--no-show-signature", "--format=%ci", "HEAD")
WEST = ("west", "list", "-f", "{revision}", "zmk-defy-wireless")


class FakeRun:
    """Answers git and west commands from a table, and records them.

    A key is the directory a command runs in, then its arguments after
    git -C <directory>, or the whole west command. An answer is stdout, a
    (return code, stdout) pair, or an exception to raise.
    """

    def __init__(self, answers):
        self.answers = answers
        self.commands = []

    def __call__(self, command, **kwargs):
        self.commands.append(command)
        if command[0] == "west":
            key = (Path(kwargs["cwd"]), *command)
        else:
            index = command.index("-C")
            key = (Path(command[index + 1]), *command[index + 2:])
        if key not in self.answers:
            raise AssertionError(f"unexpected command: {key}")
        answer = self.answers[key]
        if isinstance(answer, Exception):
            raise answer
        code, stdout = answer if isinstance(answer, tuple) else (0, answer)
        return subprocess.CompletedProcess(command, code, stdout,
                                           f"fatal: {key[1]} failed" if code else "")

    def ran_in(self, directory):
        return [command for command in self.commands
                if command[0] == "git" and Path(command[command.index("-C") + 1]) == directory]


class BuildInfoTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.module = self.root / "zmk-defy-wireless"
        self.keymap = self.root / "Defy-Wireless-ZMK-borovaka"
        self.config = self.keymap / "config"
        for checkout in (self.module, self.keymap):
            (checkout / ".git").mkdir(parents=True)
        self.config.mkdir()
        self.answers = {
            (self.module, *DESCRIBE): "v0.1.0-3-g2826cb2\n",
            (self.module, "rev-parse", "HEAD"): HEAD + "\n",
            (self.config, "rev-parse", "HEAD"): KEYMAP_HEAD + "\n",
            (self.config, *STATUS): "",
            (self.config, *ORIGIN): "git@github.com:borovaka/Defy-Wireless-ZMK-borovaka.git\n",
            (self.config, *TIME): "2026-10-24 12:05:31 +0300\n",
        }

    def info(self, container=False):
        self.run = FakeRun(self.answers)
        return build.build_info(self.module, self.config, self.run, container)

    def test_ssh_and_https_origins_give_the_same_source(self):
        for url in ("git@github.com:borovaka/Defy-Wireless-ZMK-borovaka.git",
                    "ssh://git@github.com:22/borovaka/Defy-Wireless-ZMK-borovaka.git/",
                    "https://x-access-token:secret@github.com/borovaka/Defy-Wireless-ZMK-borovaka"):
            with self.subTest(url=url):
                self.answers[(self.config, *ORIGIN)] = url + "\n"
                self.assertEqual(self.info()["source"], "borovaka/Defy-Wireless-ZMK-borovaka")

    def test_other_remotes_give_their_last_two_path_parts(self):
        for url, name in (("file:///srv/git/owner/repo.git", "owner/repo"),
                          ("/srv/git/owner/repo.git", "owner/repo"),
                          ("../owner/repo", "owner/repo"),
                          ("example.com:repo.git", "repo"),
                          ("", "")):
            with self.subTest(url=url):
                self.assertEqual(build.repository_name(url), name)

    def test_a_repository_without_origin_is_local(self):
        self.answers[(self.config, *ORIGIN)] = (1, "")
        info = self.info()
        self.assertEqual(info["source"], "local")
        self.assertEqual(info["commit"], KEYMAP_HEAD)
        self.assertEqual(info["commit_time"], "2026-10-24T12:05:31+03:00")

    def test_a_config_directory_outside_git_is_local_without_a_commit(self):
        shutil.rmtree(self.keymap / ".git")
        info = self.info()
        self.assertEqual((info["source"], info["commit"], info["commit_time"]), ("local", "", ""))
        self.assertEqual(info["module"], "v0.1.0-3-g2826cb2")
        self.assertEqual(self.run.ran_in(self.config), [])

    def test_without_tags_the_west_revision_names_the_module(self):
        self.answers.update({
            (self.module, *DESCRIBE): "2826cb2\n",
            (self.module, *MANIFEST_REV): HEAD + "\n",
            (self.config, *WEST): "v0.1.0\n",
        })
        self.assertEqual(self.info()["module"], "v0.1.0")
        self.answers[(self.module, *DESCRIBE)] = "2826cb2-dirty\n"
        self.assertEqual(self.info()["module"], "v0.1.0-dirty")

    def test_without_tags_or_a_west_revision_the_hash_names_the_module(self):
        self.answers[(self.module, *DESCRIBE)] = "2826cb2\n"
        cases = {
            "not checked out by west": {(self.module, *MANIFEST_REV): (1, "")},
            "moved off west's revision": {(self.module, *MANIFEST_REV): "0" * 40 + "\n"},
            "not in the config's west.yml": {(self.module, *MANIFEST_REV): HEAD,
                                             (self.config, *WEST): (1, "")},
            "pinned by its hash": {(self.module, *MANIFEST_REV): HEAD,
                                   (self.config, *WEST): HEAD + "\n"},
            "west missing": {(self.module, *MANIFEST_REV): HEAD,
                             (self.config, *WEST): FileNotFoundError("west")},
        }
        for case, answers in cases.items():
            with self.subTest(case=case):
                self.answers.update(answers)
                self.assertEqual(self.info()["module"], "2826cb2")
                for key in answers:
                    del self.answers[key]

    def test_git_in_the_container_trusts_the_two_checkouts(self):
        self.answers[(self.module, *DESCRIBE)] = "2826cb2\n"
        self.answers[(self.module, *MANIFEST_REV)] = (1, "")
        self.info(container=True)
        for directory, top in ((self.module, self.module), (self.config, self.keymap)):
            commands = self.run.ran_in(directory)
            self.assertTrue(commands)
            for command in commands:
                self.assertEqual(command[:5], ["git", "-c", f"safe.directory={top}",
                                               "--no-optional-locks", "-C"])
        self.info(container=False)
        for command in self.run.commands:
            self.assertEqual(command[:3], ["git", "--no-optional-locks", "-C"])

    def test_git_failures_stop_the_build(self):
        self.answers[(self.config, *STATUS)] = (128, "")
        with self.assertRaisesRegex(ValueError, "git status .*failed"):
            self.info()

    def test_the_commit_time_keeps_its_offset(self):
        self.answers[(self.config, *TIME)] = "2026-01-01 00:00:00 -0130\n"
        self.assertEqual(self.info()["commit_time"], "2026-01-01T00:00:00-01:30")


@unittest.skipUnless(shutil.which("git"), "needs git")
class GitTests(unittest.TestCase):
    """The same, with git itself, in throwaway repositories."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        empty = self.root / "gitconfig"
        empty.write_text("")
        environment = mock.patch.dict(os.environ, {
            "GIT_CONFIG_GLOBAL": str(empty), "GIT_CONFIG_NOSYSTEM": "1",
            "GIT_AUTHOR_NAME": "Test", "GIT_AUTHOR_EMAIL": "",
            "GIT_COMMITTER_NAME": "Test", "GIT_COMMITTER_EMAIL": "",
            "GIT_AUTHOR_DATE": "2026-10-24T12:05:31+0300",
            "GIT_COMMITTER_DATE": "2026-10-24T12:05:31+0300"})
        environment.start()
        self.addCleanup(environment.stop)
        self.module = self.root / "zmk-defy-wireless"
        (self.module / "config").mkdir(parents=True)
        (self.module / "config/west.yml").write_text("manifest: {}\n")
        self.git("init", "-q")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "first")
        self.git("tag", "v0.1.0")
        self.git("remote", "add", "origin", "https://github.com/borovaka/zmk-defy-wireless")

    def git(self, *arguments):
        return subprocess.run(["git", "-C", str(self.module), *arguments], check=True,
                              capture_output=True, text=True).stdout.strip()

    def test_a_tagged_clean_checkout(self):
        info = build.build_info(self.module, self.module / "config")
        self.assertEqual(info, {"module": "v0.1.0", "source": "borovaka/zmk-defy-wireless",
                                "commit": self.git("rev-parse", "HEAD"),
                                "commit_time": "2026-10-24T12:05:31+03:00"})
        self.assertEqual(build.build_info(self.module, self.module / "config", container=True),
                         info)

    def test_commits_after_the_tag_and_changes(self):
        (self.module / "config/defy.conf").write_text("CONFIG_EXAMPLE=y\n")
        self.git("add", ".")
        self.git("commit", "-q", "-m", "second")
        head = self.git("rev-parse", "HEAD")
        info = build.build_info(self.module, self.module / "config")
        self.assertEqual((info["module"], info["commit"]), (f"v0.1.0-1-g{head[:7]}", head))
        # Untracked files leave both clean, as git describe --dirty does.
        (self.module / "config/untracked.conf").write_text("")
        info = build.build_info(self.module, self.module / "config")
        self.assertEqual((info["module"], info["commit"]), (f"v0.1.0-1-g{head[:7]}", head))
        (self.module / "config/defy.conf").write_text("CONFIG_EXAMPLE=n\n")
        info = build.build_info(self.module, self.module / "config")
        self.assertEqual((info["module"], info["commit"]),
                         (f"v0.1.0-1-g{head[:7]}-dirty", f"{head}-dirty"))

    def test_untagged_without_west(self):
        self.git("tag", "-d", "v0.1.0")
        self.git("remote", "remove", "origin")
        head = self.git("rev-parse", "HEAD")
        info = build.build_info(self.module, self.module / "config")
        self.assertEqual((info["module"], info["source"]), (head[:7], "local"))


if __name__ == "__main__":
    unittest.main()
