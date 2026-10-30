# SPDX-License-Identifier: MIT
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

from support import SCRIPTS, ihex

# From scripts/, which support puts on sys.path.
import package_factory as factory


SCRIPT = SCRIPTS / "package_factory.py"
VECTOR = 0x10006100


def application(stack=0x20042000, reset=VECTOR + 0x21):
    return struct.pack("<2I", stack, reset) + bytes(range(8, 64))


class FactoryPackageTest(unittest.TestCase):
    def setUp(self):
        self.memory = factory.read_ihex(ihex(application(), VECTOR))

    def test_keyboard_package_bytes_are_pinned(self):
        pinned = {
            "left": ("5945e3bf12ca14f4a0aebf860ae62affa5764e084393829a8f06aa5e14cbe829",
                     "5427966c1bdf42f1a2aa5a9e58369db4e4274402af86032da49dbb9fe89632f3"),
            "right": ("1bbe6534c6a3e7838d351a689d750983dca5511ebf8d2cb95fc936d79bb5714b",
                      "25f291374df0da720f372d725ffdcb70a0f97c8a1f3dc8809fe1783c91114a3d"),
        }
        for side, (package_sha256, uf2_sha256) in pinned.items():
            with self.subTest(side=side):
                package = factory.make_package(self.memory, 0x10203, side=side)
                self.assertEqual(hashlib.sha256(package).hexdigest(), package_sha256)
                self.assertEqual(hashlib.sha256(factory.make_uf2(package)).hexdigest(), uf2_sha256)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "input.hex"
            source.write_text(ihex(application(), VECTOR))
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
                                     "--output-bin", str(root / "output.bin"),
                                     "--output-uf2", str(root / "output.uf2"), "--version", "0x10203",
                                     "--side", "right"],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(hashlib.sha256((root / "output.bin").read_bytes()).hexdigest(), pinned["right"][0])
            self.assertEqual(hashlib.sha256((root / "output.uf2").read_bytes()).hexdigest(), pinned["right"][1])

    def test_seal_and_vector_layout(self):
        package = factory.make_package(self.memory, 0x10203, side="left")
        fields = struct.unpack_from("<7I", package)
        self.assertEqual(fields[:2], (1, 28))
        self.assertEqual(fields[3:5], (0x6000, 0x140))
        self.assertEqual(fields[6], 0x10203)
        self.assertEqual(fields[2], zlib.crc32(package[:8] + bytes(4) + package[12:28]))
        self.assertEqual(fields[5], zlib.crc32(package[0x1000:0x1140]))
        self.assertEqual(package[0x1100:0x1140], application())
        self.assertEqual(factory.verify_package(package)["program_length"], 0x140)

    def test_metadata_binds_role_side_and_version_to_the_program(self):
        packages = []
        for side in ("left", "right"):
            with self.subTest(side=side):
                package = factory.make_package(self.memory, 17, side=side)
                packages.append(package)
                info = factory.verify_package(package)
                self.assertEqual((info["role"], info["side"], info["version"]), ("keyboard", side, 17))
                self.assertEqual(package[0x1000 + factory.METADATA.size:0x1100],
                                 bytes(256 - factory.METADATA.size))
                damaged = bytearray(package)
                damaged[0x1014] ^= 1
                with self.assertRaisesRegex(ValueError, "program CRC"):
                    factory.verify_package(damaged)
        self.assertEqual(len(set(packages)), 2)

    def test_rejects_invalid_identity_and_resealed_metadata(self):
        for side in ("either", "unknown"):
            with self.subTest(side=side), self.assertRaises(ValueError):
                factory.make_package(self.memory, 17, side=side)
        original = factory.make_package(self.memory, 17, side="left")
        for offset, value in ((0, 0), (8, 2), (12, 28), (16, 2), (16, 99), (20, 2), (24, 18),
                              (28, 1), (32, 1)):
            with self.subTest(offset=offset):
                damaged = bytearray(original)
                struct.pack_into("<I", damaged, 0x1000 + offset, value)
                seal = list(factory.SEAL.unpack_from(damaged))
                seal[5] = zlib.crc32(damaged[0x1000:0x1000 + seal[4]])
                seal[2] = 0
                seal[2] = zlib.crc32(factory.SEAL.pack(*seal))
                factory.SEAL.pack_into(damaged, 0, *seal)
                # Without the magic, nothing names the side.
                message = "metadata missing" if offset == 0 else ""
                with self.assertRaisesRegex(ValueError, message):
                    factory.verify_package(damaged)

    def test_uf2_contains_only_package_and_keeps_bootloader_and_storage(self):
        package = factory.make_package(self.memory, 1, side="left")
        uf2 = factory.make_uf2(package)
        count = len(uf2) // 512
        rebuilt = bytearray()
        for index in range(count):
            block = uf2[index * 512:(index + 1) * 512]
            magic0, magic1, flags, address, size, number, total, family = struct.unpack_from("<8I", block)
            self.assertEqual((magic0, magic1, flags), (0x0A324655, 0x9E5D5157, 0x2000))
            self.assertEqual((number, total, family), (index, count, 0xE48BFF56))
            self.assertEqual(address, 0x10005000 + index * 256)
            self.assertEqual(size, 256)
            self.assertLessEqual(address + size, 0x101C0000)
            self.assertEqual(struct.unpack_from("<I", block, 508)[0], 0x0AB16F30)
            rebuilt.extend(block[32:32 + size])
        self.assertEqual(rebuilt, package)

    def test_sparse_image_gaps_and_partial_word_are_padded(self):
        self.memory[VECTOR + 0x84] = 0x42
        package = factory.make_package(self.memory, 1, side="left")
        self.assertEqual(package[0x1140:0x1184], b"\xff" * 0x44)
        self.assertEqual(package[0x1184:0x1188], b"\x42\xff\xff\xff")
        self.assertEqual(factory.verify_package(package)["program_length"], 0x188)

    def test_maximum_image_does_not_emit_a_storage_block(self):
        self.memory[0x101BFFFF] = 0x42
        package = factory.make_package(self.memory, 1, side="left")
        self.assertEqual(len(package), 0x1C0000 - 0x5000)
        uf2 = factory.make_uf2(package)
        self.assertEqual(struct.unpack_from("<I", uf2[-512:], 12)[0] + 256, 0x101C0000)

    def test_rejects_stock_boot2_and_storage_records(self):
        for address in (0x10000000, 0x10005000, 0x10006000, 0x101C0000, 0x20000000):
            with self.subTest(address=hex(address)), self.assertRaises(ValueError):
                factory.read_ihex(ihex(application(), address))

    def test_rejects_wrong_link_address(self):
        shifted = {address + 0x100: value for address, value in self.memory.items()}
        with self.assertRaisesRegex(ValueError, "linked at"):
            factory.make_package(shifted, 1, side="left")

    def test_rejects_invalid_vectors(self):
        for stack, reset in ((0x20042008, VECTOR + 0x21), (0x20000000, VECTOR + 0x21),
                             (0x20041004, VECTOR + 0x21), (0x20041000, VECTOR + 0x20),
                             (0x20041000, 0x10000101), (0x20041000, VECTOR + 0x101)):
            with self.subTest(stack=hex(stack), reset=hex(reset)), self.assertRaises(ValueError):
                factory.make_package(factory.read_ihex(ihex(application(stack, reset), VECTOR)), 1,
                                     side="left")

    def test_rejects_reset_into_gap(self):
        del self.memory[VECTOR + 0x20]
        with self.assertRaisesRegex(ValueError, "missing application data"):
            factory.make_package(self.memory, 1, side="left")

    def test_rejects_missing_vector_bytes(self):
        del self.memory[VECTOR + 5]
        with self.assertRaisesRegex(ValueError, "incomplete vector"):
            factory.make_package(self.memory, 1, side="left")

    def test_rejects_invalid_hex(self):
        valid = ihex(application(), VECTOR)
        # A bad checksum, and no end record.
        for text in (valid[:-1] + "1", valid.rsplit("\n", 1)[0]):
            with self.subTest(text=text[:60]), self.assertRaises(ValueError):
                factory.read_ihex(text)

    def test_rejects_overlapping_records(self):
        rows = ihex(application(), VECTOR).splitlines()
        rows.insert(2, rows[1])
        with self.assertRaisesRegex(ValueError, "overlapping"):
            factory.read_ihex("\n".join(rows))

    def test_rejects_invalid_version(self):
        for version in (-1, 0x100000000):
            with self.subTest(version=version), self.assertRaises(ValueError):
                factory.make_package(self.memory, version, side="left")

    def test_corrupt_seal_program_and_truncation_are_rejected(self):
        package = factory.make_package(self.memory, 1, side="left")
        for offset in (8, 16, 24, 0x1108):
            damaged = bytearray(package)
            damaged[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                factory.verify_package(damaged)
        for damaged in (package[:20], package[:-1], package[:-256], package + bytes(256)):
            with self.subTest(length=len(damaged)), self.assertRaises(ValueError):
                factory.verify_package(damaged)

    def test_cli_rejects_invalid_image_before_creating_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "input.hex"
            output_bin = directory / "output.bin"
            output_uf2 = directory / "output.uf2"
            source.write_text(ihex(application(), 0x10000100))
            result = subprocess.run([sys.executable, str(SCRIPT), "--input", str(source),
                                     "--output-bin", str(output_bin), "--output-uf2", str(output_uf2),
                                     "--version", "1", "--side", "left"], capture_output=True, text=True)
            # A failure, not a usage error.
            self.assertEqual(result.returncode, 1)
            self.assertNotIn("usage:", result.stderr)
            self.assertFalse(output_bin.exists())
            self.assertFalse(output_uf2.exists())


if __name__ == "__main__":
    unittest.main()
