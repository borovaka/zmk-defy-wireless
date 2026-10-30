# SPDX-License-Identifier: MIT
import fcntl
import hashlib
import io
import json
from contextlib import redirect_stderr, redirect_stdout
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import zlib

from support import SCRIPTS, load_script, record


SCRIPT = SCRIPTS / "build"
build = load_script("defy_build", "build")


def lines(*records):
    """Intel HEX text, one record to a line."""
    return "".join(f"{row}\n" for row in records)


def vector_hex(start, end, stack, reset):
    """An image with a vector table at start, a Thumb instruction at start +
    0x10, and a last byte at end - 1."""
    records = [record(0, 4, (start >> 16).to_bytes(2, "big")),
               record(start & 0xffff, 0, struct.pack("<2I", stack, reset)),
               record((start + 0x10) & 0xffff, 0, b"\x00\xbf")]
    if end > start + 0x12:
        records += [record(0, 4, ((end - 1) >> 16).to_bytes(2, "big")),
                    record((end - 1) & 0xffff, 0, b"\x00")]
    return lines(*records, record(0, 1)).encode("ascii")


def loader_hex(end=0x1012, stack=0x20020000, reset=0x1011):
    return vector_hex(0x1000, end, stack, reset)


def zmk_hex(end=0xd012, stack=0x20020000, reset=0xd011):
    return vector_hex(0xd000, end, stack, reset)


def memory(image):
    """The data an Intel HEX image places, by address."""
    return build.package_factory.read_ihex(image.decode("ascii"), minimum=0, maximum=0x80000)


def uf2_block(address, index, count, payload, family=0x621e937a, flags=0x2000, size=256):
    header = struct.pack("<8I", 0x0a324655, 0x9e5d5157, flags, address, size, index, count, family)
    return header + payload.ljust(476, b"\x00") + struct.pack("<I", 0x0ab16f30)


def zmk_uf2(image):
    """ZMK's UF2 file as Zephyr makes it from zmk.bin: the image from 0xd000,
    its gaps filled with 0xff, in 256-byte blocks, the last padded with zeros."""
    data = memory(image)
    content = bytes(data.get(address, 0xff) for address in range(0xd000, max(data) + 1))
    count = -(-len(content) // 256)
    return b"".join(uf2_block(0xd000 + index * 256, index, count,
                              content[index * 256:(index + 1) * 256].ljust(256, b"\x00"))
                    for index in range(count))


def half_package(side, version):
    """A factory package with a valid seal around a small application."""
    vector = build.package_factory.FLASH_BASE + build.package_factory.VECTOR_OFFSET
    application = struct.pack("<2I", 0x20042000, vector + 0x21) + bytes(range(8, 64))
    memory = {vector + offset: value for offset, value in enumerate(application)}
    return build.package_factory.make_package(memory, version, side=side)


# Values as a keymap repository's build works them out.
INFO = {"module": "v0.1.0-3-g2826cb2", "source": "borovaka/Defy-Wireless-ZMK-borovaka",
        "commit": "357c43abe9c0c456d29a1fec90e3a2a8c5705e9c",
        "commit_time": "2026-10-24T12:05:31+03:00"}


class BuildTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "project with spaces"
        self.repo.mkdir()
        self.output = self.root / "build"
        self.commands = []
        self.loader_hex = loader_hex()
        self.zmk_hex = zmk_hex()
        # ZMK's UF2 file, when not the one Zephyr would make from zmk_hex.
        self.zmk_uf2 = None
        self.config_overrides = {}
        self.loader_overrides = {}
        # main() works out the values with git; these tests give them.
        patch = mock.patch.object(build, "build_info", return_value=INFO)
        self.build_info = patch.start()
        self.addCleanup(patch.stop)

    def plan(self, target, factory_version=build.DEFAULT_FACTORY_VERSION):
        return build.build_plan(target, self.repo, self.output, factory_version)

    def uf2(self):
        """The UF2 file the fake ZMK build writes."""
        return zmk_uf2(self.zmk_hex) if self.zmk_uf2 is None else self.zmk_uf2

    def west(self, name):
        """The west builds this test ran in the build directory name."""
        return [command for command in self.commands if command[0] == "west" and
                Path(command[command.index("-d") + 1]).name == name]

    def assert_neuron(self, directory, target="neuron-hybrid"):
        """directory holds the target's images: the loader and ZMK merged, and ZMK's UF2 file."""
        self.assertEqual(memory((directory / f"{target}.hex").read_bytes()),
                         {**memory(self.loader_hex), **memory(self.zmk_hex)})
        self.assertEqual((directory / f"{target}.uf2").read_bytes(), self.uf2())

    def loader_build(self, directory):
        """The fake loader build: its image, and its configuration."""
        (directory / "zephyr/zephyr.hex").write_bytes(self.loader_hex)
        return {
            "CONFIG_BOARD": '"defy_neuron"',
            "CONFIG_BOARD_TARGET": '"defy_neuron/nrf52833"',
            "CONFIG_USE_DT_CODE_PARTITION": "y",
            "CONFIG_ROM_START_OFFSET": "0",
            "CONFIG_FLASH_BASE_ADDRESS": "0x0",
            "CONFIG_FLASH_LOAD_OFFSET": "0x1000",
            "CONFIG_FLASH_LOAD_SIZE": "0xc000",
            "CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC": "y",
            **self.loader_overrides,
        }

    def zmk_build(self, directory):
        """A fake ZMK build: its images, and its configuration."""
        neuron = directory.name.startswith("neuron")
        for name in ("zmk.hex", "zephyr.hex"):
            (directory / "zephyr" / name).write_bytes(self.zmk_hex if neuron else name.encode())
        if neuron:
            (directory / "zephyr/zmk.uf2").write_bytes(self.uf2())
        side, _, variant = directory.name.partition("-")
        return {
            "CONFIG_BOARD": f'"defy_{side}"',
            "CONFIG_USE_DT_CODE_PARTITION": "y",
            "CONFIG_ROM_START_OFFSET": "0",
            "CONFIG_FLASH_BASE_ADDRESS": "0x0" if neuron else "0x10000000",
            "CONFIG_FLASH_LOAD_OFFSET": "0xd000" if neuron else "0x6100",
            "CONFIG_FLASH_LOAD_SIZE": "0x5f000" if neuron else "0x1b9f00",
            "CONFIG_BUILD_OUTPUT_UF2": "y" if neuron else "n",
            "CONFIG_RETENTION_BOOT_MODE": "y" if neuron else None,
            "CONFIG_ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE": "0x57" if neuron else None,
            "CONFIG_DEFY_LOADER_CONFIRM": "y" if neuron else None,
            "CONFIG_DEFY_NEURON_FATAL_RESET": "y" if neuron else None,
            "CONFIG_ZMK_SPLIT": "y",
            "CONFIG_ZMK_SPLIT_ROLE_CENTRAL": "y" if neuron else "n",
            "CONFIG_ZMK_BLE": "y",
            "CONFIG_ZMK_SPLIT_BLE": "y",
            "CONFIG_DEFY_HYBRID": "y",
            "CONFIG_DEFY_WIRED": "y",
            "CONFIG_DEFY_RP2040_FACTORY_HANDOFF": "y",
            "CONFIG_INIT_ARCH_HW_AT_BOOT": "y",
            "CONFIG_DEFY_RADIO_INSTALL": "y" if variant == "hybrid-radio" else "n",
            "CONFIG_DEFY_BT_HCI_SPI": "y",
            "CONFIG_DEFY_HALF_SLEEP": "n" if variant == "hybrid-awake" else "y",
            "CONFIG_DEFY_FATAL_RECOVERY": "n" if variant == "hybrid-diag" else "y",
            "CONFIG_ZMK_SETTINGS_RESET_ON_START": "n",
            "CONFIG_DEFY_HALF_UPDATER": "y" if neuron else "n",
            "CONFIG_DEFY_NEURON_BOOTLOADER": "y" if neuron else "n",
            **self.config_overrides,
        }

    def successful_build(self, command, **kwargs):
        self.commands.append(command)
        if command[0] == "west":
            directory = Path(command[command.index("-d") + 1])
            (directory / "zephyr").mkdir(parents=True, exist_ok=True)
            fake = self.loader_build if directory.name == "loader" else self.zmk_build
            (directory / "zephyr/.config").write_text("\n".join(
                f"# {name} is not set" if value == "n" else f"{name}={value}"
                for name, value in fake(directory).items() if value is not None))
        else:
            for option in ("--output-bin", "--output-uf2", "--output-seal", "--output-source"):
                if option in command:
                    destination = Path(command[command.index(option) + 1])
                    data = f"new {destination.name}".encode()
                    # A half's .bin gets a real seal, which manifest.json reads.
                    if option == "--output-bin" and "--side" in command:
                        data = half_package(command[command.index("--side") + 1],
                                            int(command[command.index("--version") + 1]))
                    destination.write_bytes(data)

    def test_unsafe_generated_config_cannot_replace_existing_output(self):
        failures = (
            ("left-hybrid", "CONFIG_BOARD", '"defy_right"'),
            # A -diag half holds fatal errors on its LEDs instead of recovering.
            ("right-hybrid-diag", "CONFIG_DEFY_FATAL_RECOVERY", "y"),
            # Where ZMK starts behind a 40 KiB loader.
            ("neuron-hybrid", "CONFIG_FLASH_LOAD_OFFSET", "0xb000"),
            ("neuron-hybrid", "CONFIG_FLASH_LOAD_SIZE", "bad-number"),
            ("neuron-hybrid", "CONFIG_FLASH_LOAD_SIZE", None),
            # TinyUF2's value, which another bootloader type in ZMK would give.
            ("neuron-hybrid", "CONFIG_ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE", "0xf01669ef"),
        )
        output = self.repo / "firmware"
        output.mkdir()
        previous = output / "neuron-hybrid.hex"
        previous.write_bytes(b"previous image")
        for target, name, value in failures:
            with self.subTest(target=target, name=name):
                self.config_overrides = {name: value}
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, f"^{target}: .*{name}"):
                    build.run_builds([self.plan(target)], self.repo, self.successful_build)
                # A Neuron's batch builds the loader first.
                builds = ["loader", target] if target.startswith("neuron") else [target]
                self.assertEqual([Path(command[command.index("-d") + 1]).name
                                  for command in self.commands], builds)
                self.assertEqual(previous.read_bytes(), b"previous image")
                self.assertEqual(list(output.iterdir()), [previous])

    def test_unsafe_loader_config_stops_the_batch_before_zmk_builds(self):
        failures = (
            # ZMK's variant of the same board.
            ("CONFIG_BOARD_TARGET", '"defy_neuron/nrf52833/zmk"'),
            # A 40 KiB loader slot.
            ("CONFIG_FLASH_LOAD_SIZE", "0xa000"),
            ("CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC", None),
        )
        output = self.repo / "firmware"
        output.mkdir()
        previous = output / "neuron-hybrid.hex"
        previous.write_bytes(b"previous image")
        for name, value in failures:
            with self.subTest(name=name, value=value):
                self.loader_overrides = {name: value}
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, f"^loader: unsafe build configuration: {name}"):
                    build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
                self.assertEqual(len(self.commands), 1)
                self.assertFalse((self.output / "loader/defy-build-args").exists())
                self.assertEqual(list(output.iterdir()), [previous])

    def test_missing_or_malformed_config_is_actionable_and_keeps_cache(self):
        plan = self.plan("left-hybrid")
        for data in (None, b"\xff", b"CONFIG_BOARD", b"CONFIG_BOARD=", b"CONFIG_BOARD=x\nCONFIG_BOARD=y"):
            with self.subTest(data=data):
                plan["directory"].mkdir(parents=True, exist_ok=True)
                cache = plan["directory"] / "defy-build-args"
                cache.write_text("previous configuration")

                def run(command, **kwargs):
                    self.successful_build(command, **kwargs)
                    path = plan["directory"] / "zephyr/.config"
                    if data is None:
                        path.unlink()
                    else:
                        path.write_bytes(data)

                with self.assertRaisesRegex(ValueError, "left-hybrid: .*build configuration"):
                    build.run_builds([plan], self.repo, run)
                self.assertEqual(cache.read_text(), "previous configuration")
                self.assertEqual(list((self.repo / "firmware").iterdir()), [])

    def test_radio_builds_first_only_for_halves_that_embed_it(self):
        self.assertEqual(build.ordered_targets(["neuron-hybrid", "right-hybrid-radio", "left-hybrid"]),
                         ["radio", "neuron-hybrid", "right-hybrid-radio", "left-hybrid"])
        self.assertEqual(build.ordered_targets(["left-hybrid-radio", "radio", "left-hybrid-radio"]),
                         ["radio", "left-hybrid-radio"])
        self.assertEqual(build.ordered_targets(["neuron-hybrid"]), ["neuron-hybrid"])
        targets = [target for target in build.TARGETS[1:] if not target.endswith("-hybrid-radio")]
        self.assertEqual(build.ordered_targets(targets), targets)

    def test_factory_version_must_fit(self):
        for version in (-1, 0x100000000):
            with self.subTest(version=version), \
                    self.assertRaisesRegex(ValueError, "factory version must fit"):
                self.plan("left-hybrid", version)

    def test_all_does_not_hide_an_unknown_target(self):
        for argv in (["all", "lfet-hybrid"], ["--container", "lfet-hybrid", "all", "typo"]):
            with self.subTest(argv=argv), mock.patch.object(build.subprocess, "run") as run, \
                    mock.patch.object(build, "run_builds") as run_builds, \
                    redirect_stderr(io.StringIO()) as error, \
                    self.assertRaises(SystemExit) as raised:
                build.main(argv)
            self.assertEqual(raised.exception.code, 2)
            run.assert_not_called()
            run_builds.assert_not_called()
            self.build_info.assert_not_called()
            unknown = sorted(set(argv) - {"all", "--container"})
            self.assertIn(f"unknown target: {', '.join(unknown)}", error.getvalue())

    def test_launch_returns_the_containers_status(self):
        for status in (0, 1, 130):
            with self.subTest(status=status), mock.patch.object(build.subprocess, "run") as run:
                run.return_value.returncode = status
                self.assertEqual(build.main(["left-hybrid"]), status)
            self.assertNotIn("check", run.call_args.kwargs)

    def test_config_directory_reaches_zmk_and_its_files_force_pristine(self):
        config = self.root / "user/config"
        config.mkdir(parents=True)
        (config / "defy_neuron.keymap").write_text("keymap")
        plan = build.build_plan("neuron-hybrid", self.repo, self.output, config=config)
        self.assertIn(f"-DZMK_CONFIG={config}", plan["arguments"])
        build.run_builds([plan], self.repo, self.successful_build, config=config)
        build.run_builds([plan], self.repo, self.successful_build, config=config)
        (config / "defy.conf").write_text("CONFIG_EXAMPLE=y")
        build.run_builds([plan], self.repo, self.successful_build, config=config)
        for name in ("loader", "neuron-hybrid"):
            pristine = [command[3] for command in self.west(name)]
            self.assertEqual(pristine, ["always", "auto", "always"])

    def test_west_modules_leave_out_this_module_only_when_west_lists_it(self):
        def listing(*paths):
            def run(command, **kwargs):
                self.assertEqual(command, ["west", "list", "-f", "{abspath}"])
                self.assertEqual(kwargs["cwd"], self.root / "zmk")
                return subprocess.CompletedProcess(command, 0, "".join(f"{p}\n" for p in paths))
            return run

        others = (self.root / "keymap repo/config", self.root / "zmk", self.root / "modules/hal")
        self.assertEqual(
            build.west_modules(self.repo, self.root / "zmk",
                               listing(others[0], self.repo, *others[1:])),
            ";".join(str(path) for path in others))
        self.assertIsNone(build.west_modules(self.repo, self.root / "zmk", listing(*others)))

    def test_named_modules_reach_every_build(self):
        for target in ("radio", "left-hybrid-radio", "neuron-hybrid"):
            with self.subTest(target=target):
                plan = build.build_plan(target, self.repo, self.output, modules="/a;/b c")
                self.assertIn("-DZEPHYR_MODULES=/a;/b c", plan["arguments"])
                self.assertFalse(any(argument.startswith("-DZEPHYR_MODULES=")
                                     for argument in self.plan(target)["arguments"]))
        plan = build.build_plan("neuron-hybrid", self.repo, self.output, modules="/a;/b c")
        self.assertEqual(plan["loader"]["arguments"][-2:], ["--", "-DZEPHYR_MODULES=/a;/b c"])
        self.assertFalse(any(argument.startswith("-DZEPHYR_MODULES=")
                             for argument in self.plan("neuron-hybrid")["loader"]["arguments"]))

    def test_changed_kconfig_files_force_pristine(self):
        plan = self.plan("neuron-hybrid")
        kconfig = self.repo / "lighting/Kconfig"
        kconfig.parent.mkdir(parents=True)
        kconfig.write_text("config A\n    default 1\n")
        build.run_builds([plan], self.repo, self.successful_build)
        build.run_builds([plan], self.repo, self.successful_build)
        self.assertEqual([command[2:4] for command in self.commands[-2:]], [["-p", "auto"]] * 2)
        # Zephyr keeps a tree's .config, so a changed default needs a fresh start.
        kconfig.write_text("config A\n    default 2\n")
        build.run_builds([plan], self.repo, self.successful_build)
        self.assertEqual([command[2:4] for command in self.commands[-2:]], [["-p", "always"]] * 2)
        build.run_builds([plan], self.repo, self.successful_build)
        self.assertEqual([command[2:4] for command in self.commands[-2:]], [["-p", "auto"]] * 2)
        # The test suites' Kconfig files are not the firmware's.
        suite = self.repo / "tests/drivers/a/Kconfig"
        suite.parent.mkdir(parents=True)
        suite.write_text("config B\n")
        build.run_builds([plan], self.repo, self.successful_build)
        self.assertEqual([command[2:4] for command in self.commands[-2:]], [["-p", "auto"]] * 2)

    def test_the_loader_and_zmk_may_fill_their_parts_but_no_more(self):
        self.loader_hex = loader_hex(end=0xd000)
        self.zmk_hex = zmk_hex(end=0x6c000)
        build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
        self.assert_neuron(self.repo / "firmware")
        self.zmk_hex = zmk_hex(end=0x6c001)
        with self.assertRaisesRegex(ValueError, "^neuron-hybrid: ZMK overlaps the ZMK settings"):
            build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)

    def test_neuron_rejects_malformed_hex_and_vectors_without_publishing(self):
        missing_vectors = lines(record(0, 4, b"\x00\x00"), record(0xd010, 0, b"\x00\xbf"),
                                record(0, 1)).encode("ascii")
        below = zmk_hex().replace(lines(record(0, 1)).encode(),
                                  lines(record(0x9000, 0, b"\x00"), record(0, 1)).encode())
        invalid = {
            "non-ascii": (b"\xff", "Intel HEX file"),
            "invalid hex": (b"not hex", "invalid ZMK image"),
            "invalid checksum": (zmk_hex().replace(b":00000001FF", b":0000000100"),
                                 "invalid ZMK image"),
            "missing vectors": (missing_vectors, "no vector table"),
            "unaligned stack": (zmk_hex(stack=0x2001ffff), "stack pointer"),
            "stack outside RAM": (zmk_hex(stack=0x20020004), "stack pointer"),
            "non-Thumb reset": (zmk_hex(reset=0xd010), "reset vector"),
            "reset outside image": (zmk_hex(reset=0xd031), "reset vector"),
            "reset into the loader": (zmk_hex(reset=0x1011), "reset vector"),
            "data below its part": (below, "below its part"),
            "outside application": (zmk_hex(end=0x72001), "invalid ZMK image"),
            "linked at 0x1000": (vector_hex(0x1000, 0x1012, 0x20020000, 0x1011), "no vector table"),
            "linked behind S140": (vector_hex(0x27000, 0x27012, 0x20020000, 0x27011),
                                   "no vector table"),
        }
        for reason, (image, message) in invalid.items():
            with self.subTest(reason=reason):
                self.zmk_hex = image
                self.zmk_uf2 = zmk_uf2(zmk_hex())
                with self.assertRaisesRegex(ValueError, "^neuron-hybrid: .*" + message):
                    build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
                self.assertEqual(list((self.repo / "firmware").iterdir()), [])

    def test_loader_is_checked_before_any_zmk_build(self):
        invalid = {
            "reset into ZMK": (loader_hex(end=0xd012, reset=0xd011), "reset vector"),
            "too big": (loader_hex(end=0xd001), "overlaps ZMK's part"),
            "linked at ZMK's place": (zmk_hex(), "no vector table"),
        }
        output = self.repo / "firmware"
        output.mkdir()
        (output / "neuron-hybrid.hex").write_bytes(b"previous")
        for reason, (image, message) in invalid.items():
            with self.subTest(reason=reason):
                self.loader_hex = image
                self.commands.clear()
                with self.assertRaisesRegex(ValueError, "^loader: .*" + message):
                    build.run_builds([self.plan("neuron-hybrid"), self.plan("neuron-hybrid-log")],
                                     self.repo, self.successful_build)
                self.assertEqual(len(self.commands), 1)
                self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()},
                                 {"neuron-hybrid.hex": b"previous"})

    def test_merged_image_holds_the_loader_and_zmk_and_refuses_overlap(self):
        loader = memory(loader_hex(end=0xc000))
        zmk = memory(zmk_hex(end=0x6c000))
        self.assertEqual(build.neuron_image.merge(loader, zmk), {**loader, **zmk})
        for start in (0xd000, 0xd010, 0x6bfff):
            with self.subTest(start=start), \
                    self.assertRaisesRegex(ValueError, f"^the UF2 loader and ZMK overlap at "
                                                       f"0x{start:x}$"):
                build.neuron_image.merge({**loader, start: 0, start + 1: 0}, zmk)
        text = build.package_factory.to_ihex(build.neuron_image.merge(loader, zmk))
        self.assertEqual(build.neuron_image.read(text, "Neuron"), {**loader, **zmk})

    def test_merged_intel_hex_records(self):
        data = {**{0xfff8 + offset: offset for offset in range(24)}, 0x20000: 0xaa,
                0x20010: 0xbb}
        text = build.package_factory.to_ihex(data)
        self.assertEqual(text, lines(
            record(0, 4, b"\x00\x00"), record(0xfff8, 0, bytes(range(8))),
            record(0, 4, b"\x00\x01"), record(0, 0, bytes(range(8, 24))),
            record(0, 4, b"\x00\x02"), record(0, 0, b"\xaa"),
            record(0x10, 0, b"\xbb"), record(0, 1)))
        data = memory(zmk_hex(end=0x6c000))
        text = build.package_factory.to_ihex(data)
        self.assertEqual(build.package_factory.read_ihex(text, minimum=0, maximum=0x80000), data)
        self.assertTrue(all(len(line) <= 1 + 2 * (5 + 16) for line in text.splitlines()))

    def test_neuron_uf2_must_be_zmk_as_the_loader_takes_it(self):
        good = zmk_uf2(zmk_hex())
        payload = good[32:32 + 256]

        def block(**fields):
            return uf2_block(**{"address": 0xd000, "index": 0, "count": 1, "payload": payload,
                                **fields})

        changed = bytearray(good)
        changed[32 + 0x10] ^= 1
        # Block n holds the 256 bytes at 0xd000 + n * 256, as the loader requires.
        swapped = block(address=0xd100, count=2) + block(address=0xd000, index=1, count=2)
        invalid = {
            "truncated": (good[:-1], "not a whole number of 512-byte blocks"),
            "other family": (block(family=0xada52840), "not a flash block for family"),
            "unaligned": (block(address=0xd080), "at a 256-byte boundary"),
            "block count": (block(count=2), "numbered 0 of 2, not 0 of 1"),
            "swapped": (swapped, "is for 0xd100, not 0xd000"),
            "changed": (bytes(changed), "differs from its Intel HEX file at 0xd010"),
            "missing": (good, "has no block for 0xd1ff"),
        }
        output = self.repo / "firmware"
        output.mkdir()
        (output / "neuron-hybrid.uf2").write_bytes(b"previous")
        for reason, (data, message) in invalid.items():
            with self.subTest(reason=reason):
                self.zmk_hex = zmk_hex(end=0xd200) if reason == "missing" else zmk_hex()
                self.zmk_uf2 = data
                with self.assertRaisesRegex(ValueError, "^neuron-hybrid: ZMK's UF2 .*" + message):
                    build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
                self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()},
                                 {"neuron-hybrid.uf2": b"previous"})
        # What Zephyr fills in between ZMK's data does not matter.
        self.zmk_hex = zmk_hex()
        filled = bytearray(good)
        filled[32 + 0x8:32 + 0x10] = bytes(range(8))
        filled[32 + 0x12:32 + 0x100] = b"\x5a" * 0xee
        self.zmk_uf2 = bytes(filled)
        build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
        self.assert_neuron(output)

    def batch(self):
        targets = build.ordered_targets(["left-hybrid-radio", "right-hybrid-radio", "neuron-hybrid"])
        plans = [self.plan(target) for target in targets]
        final = self.repo / "firmware"
        final.mkdir()
        previous = {path.name: f"previous {path.name}".encode()
                    for plan in plans for path in build.staged_outputs(plan, final)}
        previous["unrelated.bin"] = b"preserved"
        for name, data in previous.items():
            (final / name).write_bytes(data)
        return plans, previous

    def assert_previous_batch(self, previous):
        final = self.repo / "firmware"
        self.assertEqual({path.name: path.read_bytes() for path in final.iterdir() if path.is_file()},
                         previous)

    def test_later_build_failure_preserves_prior_images_and_metadata(self):
        plans, previous = self.batch()

        def run(command, **kwargs):
            if command[0] == "west" and "neuron-hybrid" in command[command.index("-d") + 1]:
                raise subprocess.CalledProcessError(1, command)
            self.successful_build(command, **kwargs)

        with self.assertRaises(subprocess.CalledProcessError):
            build.run_builds(plans, self.repo, run)
        self.assert_previous_batch(previous)
        self.assertFalse(any(path.is_dir() for path in (self.repo / "firmware").iterdir()))

    def test_an_unchanged_radio_source_keeps_its_time(self):
        # Else the -hybrid-radio halves compile again on every batch.
        plans = [self.plan(target) for target in
                 build.ordered_targets(["left-hybrid-radio", "right-hybrid-radio"])]
        build.run_builds(plans, self.repo, self.successful_build)
        dependency = self.output / "radio/defy-radio.c"
        self.assertEqual(dependency.read_bytes(), b"new radio.c")
        mtime = dependency.stat().st_mtime_ns
        build.run_builds(plans, self.repo, self.successful_build)
        self.assertEqual(dependency.stat().st_mtime_ns, mtime)

    def test_stale_staging_is_removed_before_the_first_build(self):
        output = self.repo / "firmware"
        output.mkdir()
        stale = [output / ".build-abcd_123", output / ".build-0z9_xyzw"]
        for directory in stale:
            (directory / "nested").mkdir(parents=True)
            (directory / "left-hybrid.bin").write_bytes(b"partial")
        outside = self.root / "outside"
        outside.mkdir()
        (outside / "keep.bin").write_bytes(b"outside")
        # Not a name tempfile gives, and a link out of the output.
        (output / ".build-ABCDEFGH").mkdir()
        (output / ".build-ABCDEFGH/image.bin").write_bytes(b"kept")
        (output / ".build-linklink").symlink_to(outside, target_is_directory=True)

        def run(command, **kwargs):
            for directory in stale:
                self.assertFalse(directory.exists())
            self.successful_build(command, **kwargs)

        with redirect_stdout(io.StringIO()):
            build.run_builds([self.plan("neuron-hybrid")], self.repo, run)
        self.assertEqual((output / ".build-ABCDEFGH/image.bin").read_bytes(), b"kept")
        self.assertEqual((outside / "keep.bin").read_bytes(), b"outside")
        self.assertTrue((output / ".build-linklink").is_symlink())
        self.assert_neuron(output)

    def test_staging_of_a_running_batch_is_kept(self):
        output = self.repo / "firmware"
        output.mkdir()
        live = output / ".build-live_123"
        live.mkdir()
        (live / "right-hybrid.bin").write_bytes(b"in progress")
        other_batch = os.open(output, os.O_RDONLY)
        self.addCleanup(os.close, other_batch)
        fcntl.flock(other_batch, fcntl.LOCK_SH)
        build.run_builds([self.plan("neuron-hybrid")], self.repo, self.successful_build)
        self.assertEqual((live / "right-hybrid.bin").read_bytes(), b"in progress")
        self.assert_neuron(output)

    def test_termination_during_publication_waits_for_the_whole_batch(self):
        plans, previous = self.batch()
        handler = signal.signal(signal.SIGTERM, build.terminate)
        self.addCleanup(signal.signal, signal.SIGTERM, handler)
        replace = Path.replace
        published = []

        def interrupted_replace(path, target):
            if not published:
                os.kill(os.getpid(), signal.SIGTERM)
            published.append(path.name)
            return replace(path, target)

        with mock.patch.object(Path, "replace", interrupted_replace), \
                self.assertRaises(SystemExit) as raised:
            build.run_builds(plans, self.repo, self.successful_build)
        self.assertEqual(raised.exception.code, 128 + signal.SIGTERM)
        final = self.repo / "firmware"
        self.assertEqual(sorted(published), sorted([
            *(name for name in previous if name != "unrelated.bin"), "manifest.json"]))
        self.assertEqual(published[-1], "manifest.json")
        for name in published[:-1]:
            self.assertNotEqual((final / name).read_bytes(), previous[name])
        self.assertTrue((final / "manifest.json").is_file())
        self.assertFalse(any(path.is_dir() for path in final.iterdir()))

    def test_container_mode_turns_sigterm_into_a_clean_exit(self):
        handler = signal.getsignal(signal.SIGTERM)
        self.addCleanup(signal.signal, signal.SIGTERM, handler)
        with mock.patch.object(build, "run_builds") as run_builds, \
                mock.patch.object(build, "west_modules", return_value=None):
            build.main(["--container", "neuron-hybrid"])
        run_builds.assert_called_once()
        self.assertIs(signal.getsignal(signal.SIGTERM), build.terminate)
        with self.assertRaises(SystemExit) as raised:
            build.terminate(signal.SIGTERM, None)
        self.assertEqual(raised.exception.code, 143)

    def test_the_neuron_embeds_every_value_and_the_halves_only_the_module_version(self):
        def values(target, info=INFO):
            arguments = build.build_plan(target, self.repo, self.output, info=info)["arguments"]
            cmake = arguments[arguments.index("--") + 1:]
            self.assertEqual([argument for argument in arguments if "DEFY_BUILD_" in argument],
                             [argument for argument in cmake if "DEFY_BUILD_" in argument])
            return [argument for argument in cmake if argument.startswith("-DDEFY_BUILD_")]

        for target in build.TARGETS:
            with self.subTest(target=target):
                kind = self.plan(target)["kind"]
                expected = {
                    "neuron": [f"-DDEFY_BUILD_MODULE_VERSION={INFO['module']}",
                               f"-DDEFY_BUILD_SOURCE={INFO['source']}",
                               f"-DDEFY_BUILD_COMMIT={INFO['commit']}"],
                    # Empty, so the CMakeLists.txt defaults do not apply.
                    "half": [f"-DDEFY_BUILD_MODULE_VERSION={INFO['module']}",
                             "-DDEFY_BUILD_SOURCE=", "-DDEFY_BUILD_COMMIT="],
                    "radio": [],
                }[kind]
                self.assertEqual(values(target), expected)
                self.assertEqual(values(target, None), [])
                self.assertEqual(self.plan(target)["info"], build.UNKNOWN_BUILD)
                plan = build.build_plan(target, self.repo, self.output, info=INFO)
                self.assertEqual(plan["info"], INFO)
        # A keymap commit leaves the halves' arguments alone.
        other = {**INFO, "source": "someone/keymap", "commit": "0" * 40}
        self.assertEqual(build.build_plan("left-hybrid-radio", self.repo, self.output, info=INFO),
                         {**build.build_plan("left-hybrid-radio", self.repo, self.output,
                                             info=other), "info": INFO})

    def test_new_values_build_on_the_existing_tree(self):
        plan = build.build_plan("neuron-hybrid", self.repo, self.output, info=INFO)
        build.run_builds([plan], self.repo, self.successful_build)
        changed = {**INFO, "commit": "0" * 40 + "-dirty"}
        plan = build.build_plan("neuron-hybrid", self.repo, self.output, info=changed)
        build.run_builds([plan], self.repo, self.successful_build)
        builds = self.west("neuron-hybrid")
        self.assertEqual([command[3] for command in builds], ["always", "auto"])
        self.assertIn(f"-DDEFY_BUILD_COMMIT={changed['commit']}", builds[1])
        # The loader embeds none of them.
        self.assertEqual([command[3] for command in self.west("loader")], ["always", "auto"])
        stamp = json.loads((plan["directory"] / "defy-build-args").read_text())
        self.assertFalse(any("DEFY_BUILD_" in argument for argument in stamp["arguments"]))
        self.assertEqual(stamp["arguments"], self.plan("neuron-hybrid")["arguments"])

    def test_values_the_firmware_cannot_take_are_refused(self):
        longest = {"module": "v" * 32, "source": "o/" + "r" * 138, "commit": "f" * 64 + "-dirty00"}
        for target in ("radio", "left-hybrid", "neuron-hybrid"):
            build.build_plan(target, self.repo, self.output, info={**INFO, **longest})
        printable = "".join(map(chr, range(0x20, 0x7f))).replace('"', "").replace("\\", "")
        build.build_plan("neuron-hybrid", self.repo, self.output,
                         info={**INFO, "source": printable})
        build.build_plan("neuron-hybrid", self.repo, self.output, info={**INFO, "commit_time": ""})
        refused = (
            ("module", "v" * 33, "the module version is 33 bytes, more than the 32"),
            ("source", "s" * 141, "the source is 141 bytes, more than the 140"),
            ("commit", "c" * 73, "the commit is 73 bytes, more than the 72"),
            ("module", 'v0.1.0"', "the module version must be printable ASCII"),
            ("source", "owner/repo\n", "the source must be printable ASCII"),
            ("commit_time", "2026-10-24T12:05:31Z", "the commit time must be ISO 8601"),
        )
        for key, value, message in refused:
            with self.subTest(key=key, value=value), self.assertRaisesRegex(ValueError, message):
                build.build_plan("radio", self.repo, self.output, info={**INFO, key: value})
        # main() checks the values before it starts the container.
        self.build_info.return_value = {**INFO, "module": "v" * 33}
        with mock.patch.object(build.subprocess, "run") as run, \
                redirect_stderr(io.StringIO()) as error, self.assertRaises(SystemExit) as raised:
            build.main(["neuron-hybrid"])
        self.assertEqual(raised.exception.code, 2)
        run.assert_not_called()
        self.assertIn("more than the 32", error.getvalue())

    def test_the_container_works_out_the_values_when_not_given_them(self):
        self.addCleanup(signal.signal, signal.SIGTERM, signal.getsignal(signal.SIGTERM))
        with mock.patch.object(build, "run_builds") as run_builds, \
                mock.patch.object(build, "west_modules", return_value=None), \
                redirect_stdout(io.StringIO()) as printed:
            build.main(["--container", "neuron-hybrid", "--config", str(self.root / "config")])
        self.build_info.assert_called_once_with(SCRIPT.parents[1], (self.root / "config").resolve(),
                                                container=True)
        self.assertEqual(run_builds.call_args.args[0][0]["info"], INFO)
        self.assertIn(f"Module {INFO['module']}, source {INFO['source']}, commit {INFO['commit']}",
                      printed.getvalue())

    def test_a_launch_given_the_values_passes_them_on_without_git(self):
        # As a snapshot comparison runs it, with fixed values.
        with mock.patch.object(build.subprocess, "run") as run:
            build.main(["neuron-hybrid", "--module-version=snapshot", "--source=local",
                        "--commit=", "--commit-time="])
        self.build_info.assert_not_called()
        command = run.call_args.args[0]
        self.assertEqual(command[-4:], ["--module-version=snapshot", "--source=local",
                                        "--commit=", "--commit-time="])

    def manifest_batch(self, targets, info=INFO):
        plans = [build.build_plan(target, self.repo, self.output, info=info)
                 for target in build.ordered_targets(targets)]
        with redirect_stdout(io.StringIO()) as printed:
            build.run_builds(plans, self.repo, self.successful_build)
        self.printed = printed.getvalue()
        return self.repo / "firmware"

    def image(self, final, name):
        data = (final / name).read_bytes()
        return {"name": name, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}

    def half(self, final, name):
        data = (final / name).read_bytes()
        kind, size, seal_crc, start, length, program_crc, version = struct.unpack_from("<7I", data)
        self.assertEqual(program_crc, zlib.crc32(data[start - 0x5000:start - 0x5000 + length]))
        return {**self.image(final, name), "program_crc32": program_crc, "seal_version": version}

    def test_manifest_lists_what_the_neuron_embeds_and_each_image(self):
        final = self.manifest_batch(["left-hybrid-radio", "right-hybrid-radio", "neuron-hybrid"])
        text = (final / "manifest.json").read_text()
        expected = {
            "schema": 1,
            "module": INFO["module"],
            "source": INFO["source"],
            "commit": INFO["commit"],
            "commit_time": INFO["commit_time"],
            "min_installer": "v0.1.0",
            "files": {
                "neuron": self.image(final, "neuron-hybrid.hex"),
                "left": self.half(final, "left-hybrid-radio.bin"),
                "right": self.half(final, "right-hybrid-radio.bin"),
            },
        }
        self.assertEqual(json.loads(text), expected)
        # The keys in this order, two spaces, a final newline.
        self.assertEqual(text, json.dumps(expected, indent=2) + "\n")
        self.assertNotEqual(expected["files"]["left"]["program_crc32"],
                            expected["files"]["right"]["program_crc32"])
        self.assertIn("manifest.json: neuron-hybrid.hex, left-hybrid-radio.bin, "
                      "right-hybrid-radio.bin", self.printed)
        # The same build writes the same bytes.
        self.manifest_batch(["left-hybrid-radio", "right-hybrid-radio", "neuron-hybrid"])
        self.assertEqual((final / "manifest.json").read_text(), text)

    def test_manifest_takes_the_radio_pair_when_both_halves_have_it(self):
        final = self.manifest_batch(["left-hybrid", "right-hybrid", "neuron-hybrid"])
        files = json.loads((final / "manifest.json").read_text())["files"]
        self.assertEqual((files["left"]["name"], files["right"]["name"]),
                         ("left-hybrid.bin", "right-hybrid.bin"))
        self.manifest_batch(["left-hybrid-radio"])
        files = json.loads((final / "manifest.json").read_text())["files"]
        self.assertEqual(files["left"]["name"], "left-hybrid.bin")
        self.manifest_batch(["right-hybrid-radio"])
        files = json.loads((final / "manifest.json").read_text())["files"]
        self.assertEqual((files["left"]["name"], files["right"]["name"]),
                         ("left-hybrid-radio.bin", "right-hybrid-radio.bin"))

    def test_manifest_takes_the_pair_the_batch_built(self):
        # Older -radio images stay in the output beside the new pair.
        self.manifest_batch(["left-hybrid-radio", "right-hybrid-radio", "neuron-hybrid"])
        final = self.manifest_batch(["left-hybrid", "right-hybrid", "neuron-hybrid"])
        files = json.loads((final / "manifest.json").read_text())["files"]
        self.assertEqual((files["left"], files["right"]),
                         (self.half(final, "left-hybrid.bin"), self.half(final, "right-hybrid.bin")))
        # A batch without a pair keeps the order.
        self.manifest_batch(["neuron-hybrid"])
        files = json.loads((final / "manifest.json").read_text())["files"]
        self.assertEqual(files["left"]["name"], "left-hybrid-radio.bin")

    def test_manifest_keeps_the_values_of_the_batch_that_built_the_neuron(self):
        final = self.manifest_batch(["left-hybrid", "right-hybrid", "neuron-hybrid"])
        neuron = json.loads((final / "manifest.json").read_text())["files"]["neuron"]
        later = {"module": "v0.2.0", "source": "someone/keymap", "commit": "0" * 40,
                 "commit_time": "2026-11-01T09:00:00+00:00"}
        self.manifest_batch(["left-hybrid-radio", "right-hybrid-radio"], later)
        document = json.loads((final / "manifest.json").read_text())
        self.assertEqual({key: document[key] for key in INFO}, INFO)
        self.assertEqual(document["files"]["neuron"], neuron)
        self.assertEqual(document["files"]["left"]["name"], "left-hybrid-radio.bin")
        self.manifest_batch(["neuron-hybrid"], later)
        document = json.loads((final / "manifest.json").read_text())
        self.assertEqual({key: document[key] for key in later}, later)

    def test_stale_manifest_is_removed(self):
        final = self.manifest_batch(["left-hybrid", "right-hybrid", "neuron-hybrid"])
        (final / "right-hybrid.bin").unlink()
        self.manifest_batch(["neuron-hybrid"])
        self.assertFalse((final / "manifest.json").exists())
        self.manifest_batch(["right-hybrid", "neuron-hybrid"])
        self.assertTrue((final / "manifest.json").exists())
        (final / "neuron-hybrid.hex").unlink()
        self.manifest_batch(["left-hybrid"])
        self.assertFalse((final / "manifest.json").exists())
        self.assertEqual(sorted(path.name for path in final.iterdir()),
                         ["left-hybrid.bin", "left-hybrid.uf2", "neuron-hybrid.uf2",
                          "right-hybrid.bin", "right-hybrid.uf2"])

    def test_a_neuron_image_no_manifest_lists_gets_none(self):
        final = self.manifest_batch(["left-hybrid", "right-hybrid", "neuron-hybrid"])
        (final / "neuron-hybrid.hex").write_bytes(zmk_hex(end=0xe000))
        self.manifest_batch(["left-hybrid"])
        self.assertFalse((final / "manifest.json").exists())
        self.assertIn("No manifest.json: neuron-hybrid.hex comes from an earlier build",
                      self.printed)
        # A later batch still has no manifest to take the values from.
        self.manifest_batch(["right-hybrid"])
        self.assertFalse((final / "manifest.json").exists())

    def test_half_images_without_a_valid_seal_stop_the_batch(self):
        final = self.repo / "firmware"
        final.mkdir()
        package = half_package("left", 1)
        cases = {
            "factory seal CRC mismatch": package[:8] + bytes(4) + package[12:],
            "not a left half's package": half_package("right", 1),
            "factory package is truncated": b"previous left-hybrid.bin",
        }
        for message, data in cases.items():
            with self.subTest(message=message):
                (final / "left-hybrid.bin").write_bytes(data)
                (final / "neuron-hybrid.hex").write_bytes(b"previous")
                with self.assertRaisesRegex(ValueError, f"left-hybrid.bin: {message}"):
                    self.manifest_batch(["right-hybrid", "neuron-hybrid"])
                self.assertEqual((final / "neuron-hybrid.hex").read_bytes(), b"previous")
                self.assertEqual(sorted(path.name for path in final.iterdir()),
                                 ["left-hybrid.bin", "neuron-hybrid.hex"])


if __name__ == "__main__":
    unittest.main()
