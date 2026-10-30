# SPDX-License-Identifier: MIT
"""The Neuron's partitions against scripts/neuron_image.py, which checks and stages
the images the UF2 loader writes and starts, and the loader's Board-ID against
west flash's runner; docs/uf2-loader.md."""
import re
import unittest

from support import ROOT

# From scripts/, which support puts on sys.path.
import neuron_image


SOURCE = ROOT / "apps" / "defy_loader" / "src"
NEURON_BOARD = ROOT / "boards" / "dygma" / "defy_neuron"
PARTITIONS = NEURON_BOARD / "defy_neuron-partitions.dtsi"


def partitions(path):
    """The fixed partitions in a devicetree source, by label, as (start, size)."""
    pattern = re.compile(r"(\w+):\s*partition@[0-9a-f]+\s*\{\s*reg\s*=\s*<\s*(0x[0-9a-fA-F]+)"
                         r"\s+(0x[0-9a-fA-F]+)\s*>")
    return {label: (int(start, 16), int(size, 16))
            for label, start, size in pattern.findall(path.read_text())}


class LoaderLayoutTest(unittest.TestCase):
    def test_partitions(self):
        found = partitions(PARTITIONS)
        start, size = found["loader_partition"]
        self.assertEqual((start, start + size), (neuron_image.LOADER_START, neuron_image.ZMK_START))
        start, size = found["code_partition"]
        self.assertEqual((start, start + size), (neuron_image.ZMK_START, neuron_image.SETTINGS_START))
        start, size = found["storage_partition"]
        self.assertEqual((start, start + size),
                         (neuron_image.SETTINGS_START, neuron_image.SETTINGS_END))

    def test_board_id(self):
        # west flash's uf2 runner finds the drive by the Board-ID in INFO_UF2.TXT.
        loader = re.search(r'^#define\s+DEFY_LOADER_BOARD_ID\s+"([^"]+)"',
                           (SOURCE / "fat.h").read_text(), re.MULTILINE)
        runner = re.search(r'--board-id=([^"\s)]+)', (NEURON_BOARD / "board.cmake").read_text())
        self.assertIsNotNone(loader)
        self.assertIsNotNone(runner)
        self.assertEqual(runner.group(1), loader.group(1))


if __name__ == "__main__":
    unittest.main()
