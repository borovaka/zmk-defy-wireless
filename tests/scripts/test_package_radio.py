# SPDX-License-Identifier: MIT
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

from support import SCRIPTS, ihex, record

# From scripts/, which support puts on sys.path.
import package_radio as radio


SCRIPT = SCRIPTS / "package_radio.py"
START = 0x1C000


def application(stack=0x20008000, reset=START + 0x21):
    return struct.pack("<2I", stack, reset) + bytes(range(8, 64))


def parse(text):
    return radio.read_ihex(text, minimum=START, maximum=0x39000)


class RadioPackageTest(unittest.TestCase):
    def setUp(self):
        self.memory = parse(ihex(application(), START))

    def test_application_and_seal_have_verified_crcs_and_derived_version(self):
        program, seal = radio.make_package(self.memory)
        self.assertEqual(program, application())
        fields = struct.unpack("<7I", seal)
        self.assertEqual(fields[:2], (1, 28))
        self.assertEqual(fields[3:5], (START, len(program)))
        self.assertEqual(fields[2], zlib.crc32(seal[:8] + bytes(4) + seal[12:]))
        self.assertEqual(fields[5], zlib.crc32(program))
        self.assertEqual(fields[6], zlib.crc32(program))
        self.assertEqual(radio.verify_package(program, seal), {
            "program_start": START, "program_length": 64, "version": zlib.crc32(program),
        })

    def test_version_never_matches_stock_erased_or_zero(self):
        program, _ = radio.make_package(self.memory)
        for reserved in (0, 0x10201, 0xFFFFFFFF):
            with self.subTest(reserved=hex(reserved)), patch.object(radio.zlib, "crc32", return_value=reserved):
                self.assertEqual(radio.image_version(program), reserved ^ 0x80000000)

    def test_rejects_mbr_softdevice_bootloader_metadata_and_uicr_records(self):
        for address in (0, 0x1000, 0x3004, 0x1BFFC, 0x39000, 0x3D000, 0x10001014):
            text = "\n".join((record(0, 4, (address >> 16).to_bytes(2, "big")),
                              record(address & 0xFFFF, 0, bytes(4)), record(0, 1)))
            with self.subTest(address=hex(address)), self.assertRaises(ValueError):
                parse(text)

    def test_final_slot_word_stays_free_for_bootloader_padding(self):
        self.memory[0x38FFB] = 0x42
        program, seal = radio.make_package(self.memory)
        self.assertEqual(len(program), 0x39000 - START - 4)
        self.assertEqual(program[-1], 0x42)
        radio.verify_package(program, seal)
        self.memory[0x38FFC] = 0
        with self.assertRaises(ValueError):
            radio.make_package(self.memory)

    def test_sparse_bytes_and_partial_word_are_erased_value(self):
        self.memory[START + 0x80] = 0x42
        program, seal = radio.make_package(self.memory)
        self.assertEqual(program[0x40:0x80], b"\xff" * 0x40)
        self.assertEqual(program[0x80:], b"\x42\xff\xff\xff")
        radio.verify_package(program, seal)

    def test_rejects_unrelocated_or_incomplete_image(self):
        for memory in ({address - START + 0x1000: value for address, value in self.memory.items()},
                       {address: value for address, value in self.memory.items() if address != START + 5}, {}):
            with self.subTest(size=len(memory)), self.assertRaises(ValueError):
                radio.make_package(memory)

    def test_rejects_invalid_initial_stack_and_reset_vectors(self):
        for stack, reset in ((0x20008008, START + 0x21), (0x20000000, START + 0x21),
                             (0x20007004, START + 0x21), (0x20007000, START + 0x20),
                             (0x20007000, START + 1), (0x20007000, START + 0x101), (0x20007000, 0x1021)):
            with self.subTest(stack=hex(stack), reset=hex(reset)), self.assertRaises(ValueError):
                radio.make_package(parse(ihex(application(stack, reset), START)))

    def test_rejects_reset_into_missing_data(self):
        del self.memory[START + 0x20]
        with self.assertRaisesRegex(ValueError, "missing radio application data"):
            radio.make_package(self.memory)

    def test_rejects_damaged_program_and_seal(self):
        program, seal = radio.make_package(self.memory)
        damaged = bytearray(program)
        damaged[16] ^= 1
        for bad_program in (damaged, program[:-4], program + bytes(4)):
            with self.subTest(length=len(bad_program)), self.assertRaises(ValueError):
                radio.verify_package(bad_program, seal)
        for offset in (0, 4, 8, 12, 16, 20, 24):
            damaged = bytearray(seal)
            damaged[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                radio.verify_package(program, damaged)
        for bad_seal in (seal[:-1], seal + b"\x00"):
            with self.subTest(length=len(bad_seal)), self.assertRaises(ValueError):
                radio.verify_package(program, bad_seal)

    def test_c_source_embeds_the_image(self):
        program, seal = radio.make_package(self.memory)
        source = radio.c_source(program, seal)
        embedded = bytes(int(value, 16) for value in
                         source.split("defy_radio_image[", 1)[1].split("{", 1)[1].split("}", 1)[0].split(","))
        self.assertEqual(embedded, program)

    def test_cli_rejects_protected_region_before_writing_any_output(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "radio.hex"
            outputs = [directory / name for name in ("radio.bin", "radio.seal", "radio.c")]
            source.write_text(ihex(application(), 0x1000))
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
                                     "--output-bin", str(outputs[0]), "--output-seal", str(outputs[1]),
                                     "--output-source", str(outputs[2])], capture_output=True, text=True)
            # A failure, not a usage error.
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("usage:", result.stderr)
            self.assertFalse(any(path.exists() for path in outputs))


if __name__ == "__main__":
    unittest.main()
