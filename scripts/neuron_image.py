# SPDX-License-Identifier: MIT
"""Checks and merges the Neuron's images against the flash layout the installer writes.

Dygma's MBR keeps 0x0000..0x1000 and its n2_bootloader starts whatever the
vector table at 0x1000 names: the UF2 loader, which starts ZMK through the
vector table at 0xD000. ZMK's code runs from there to its settings at
0x6C000..0x72000, and Dygma's own settings and bootloader follow. An image
that fails these checks would not start, or would overwrite the settings.
docs/neuron-flash.md and docs/uf2-loader.md describe the layout.
"""

from collections import namedtuple
import struct

import package_factory
from package_factory import (UF2_BLOCK, UF2_END_MAGIC, UF2_FAMILY_PRESENT, UF2_FILE_CONTAINER,
                             UF2_HEADER, UF2_MAGICS, UF2_NOT_MAIN_FLASH)

LOADER_START = 0x1000
ZMK_START = 0xD000
SETTINGS_START = 0x6C000
SETTINGS_END = 0x72000
RAM_START = 0x20000000
RAM_END = 0x20020000

# A part of the Neuron's image: its name in messages, its range, and what
# follows it.
Part = namedtuple("Part", "name start end following")
LOADER = Part("the UF2 loader", LOADER_START, ZMK_START,
              f"ZMK's part at 0x{ZMK_START:x}..0x{SETTINGS_START:x}")
ZMK = Part("ZMK", ZMK_START, SETTINGS_START,
           f"the ZMK settings at 0x{SETTINGS_START:x}..0x{SETTINGS_END:x}")

# What &defy_drive writes into GPREGRET to ask the loader for its drive: ZMK's
# boot mode value for Adafruit's nRF52 bootloader, which the loader takes as
# DEFY_LOADER_DRIVE_MAGIC from include/dygma/defy_loader.h.
DRIVE_MAGIC = 0x57

# The UF2 blocks the loader takes: docs/uf2-loader.md.
UF2_FAMILY = 0x621E937A
UF2_PAYLOAD = 256


def read(text, name):
    """The data of a Neuron Intel HEX file by address, or raises ValueError."""
    try:
        return package_factory.read_ihex(text, minimum=LOADER_START, maximum=SETTINGS_END)
    except ValueError as error:
        raise ValueError(f"invalid {name} image ({error})") from None


def check_part(memory, part):
    """Returns the end of one part's data, or raises ValueError.

    The data must lie inside the part, with a vector table at its start: an
    initial stack pointer in RAM and a Thumb reset vector inside the part.
    """
    name, start, end, following = part
    if any(address not in memory for address in range(start, start + 8)):
        raise ValueError(f"{name} has no vector table at 0x{start:x}")
    stack, reset = struct.unpack("<2I", bytes(memory[address]
                                              for address in range(start, start + 8)))
    if stack % 4 or not RAM_START < stack <= RAM_END:
        raise ValueError(f"{name}'s initial stack pointer is outside the nRF52833's RAM")
    if not reset & 1 or not start <= reset & ~1 < end or (reset & ~1) not in memory:
        raise ValueError(f"{name}'s reset vector is not Thumb code inside its part, "
                         f"0x{start:x}..0x{end:x}")
    first, last = min(memory), max(memory) + 1
    if first < start:
        raise ValueError(f"{name} has data at 0x{first:x}, below its part at "
                         f"0x{start:x}..0x{end:x}")
    if last > end:
        raise ValueError(f"{name} overlaps {following}; its data ends at 0x{last:x}")
    return last


def merge(loader, zmk):
    """The loader's and ZMK's data as one image; refuses an address both hold."""
    shared = loader.keys() & zmk.keys()
    if shared:
        raise ValueError(f"the UF2 loader and ZMK overlap at 0x{min(shared):x}")
    return {**loader, **zmk}


def check_uf2(data, zmk):
    """Checks a UF2 file of ZMK's part against ZMK's data, as the loader takes it.

    Every block must be one the loader writes: a flash block, not a file
    container, of 256 bytes for family 0x621E937A inside ZMK's part. Block n
    of the file must hold the 256 bytes at ZMK's start plus n * 256, as in
    Zephyr's UF2 output and as the loader requires. Together the blocks must
    hold every byte of zmk, the data ZMK's Intel HEX file places; the gaps
    between may hold anything.
    """
    if not data or len(data) % UF2_BLOCK:
        raise ValueError(f"ZMK's UF2 file is {len(data)} bytes, not a whole number of "
                         f"{UF2_BLOCK}-byte blocks")
    count = len(data) // UF2_BLOCK
    for index in range(count):
        block = data[index * UF2_BLOCK:(index + 1) * UF2_BLOCK]
        magic0, magic1, flags, address, size, number, total, family = UF2_HEADER.unpack_from(block)
        if ((magic0, magic1) != UF2_MAGICS or
                struct.unpack_from("<I", block, UF2_BLOCK - 4)[0] != UF2_END_MAGIC):
            raise ValueError(f"ZMK's UF2 block {index} is not a UF2 block")
        if ((flags & (UF2_FAMILY_PRESENT | UF2_NOT_MAIN_FLASH | UF2_FILE_CONTAINER)) !=
                UF2_FAMILY_PRESENT or family != UF2_FAMILY):
            raise ValueError(f"ZMK's UF2 block {index} is not a flash block for family "
                             f"0x{UF2_FAMILY:08x}")
        if (size != UF2_PAYLOAD or address % UF2_PAYLOAD or
                not ZMK_START <= address <= SETTINGS_START - UF2_PAYLOAD):
            raise ValueError(f"ZMK's UF2 block {index} does not hold {UF2_PAYLOAD} bytes at a "
                             f"{UF2_PAYLOAD}-byte boundary inside "
                             f"0x{ZMK_START:x}..0x{SETTINGS_START:x}")
        if (number, total) != (index, count):
            raise ValueError(f"ZMK's UF2 block {index} is numbered {number} of {total}, "
                             f"not {index} of {count}")
        if address != ZMK_START + index * UF2_PAYLOAD:
            raise ValueError(f"ZMK's UF2 block {index} is for 0x{address:x}, not "
                             f"0x{ZMK_START + index * UF2_PAYLOAD:x}")
        payload = block[UF2_HEADER.size:UF2_HEADER.size + UF2_PAYLOAD]
        for offset, value in enumerate(payload, address):
            if zmk.get(offset, value) != value:
                raise ValueError(f"ZMK's UF2 file differs from its Intel HEX file at "
                                 f"0x{offset:x}")
    end = ZMK_START + count * UF2_PAYLOAD
    missing = [address for address in zmk if not ZMK_START <= address < end]
    if missing:
        raise ValueError(f"ZMK's UF2 file has no block for 0x{min(missing):x}")
