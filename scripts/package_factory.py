# SPDX-License-Identifier: MIT
"""Packages a half's ZMK image for the RP2040's factory bootloader: the seal at
0x10005000, then the metadata page and the program from 0x10006000.
docs/half-updater.md gives the layout.

It also holds the Intel HEX, seal and UF2 helpers that neuron_image.py and
package_radio.py use.
"""

import argparse
from pathlib import Path
import struct
import sys
import zlib


FLASH_BASE = 0x10000000
PACKAGE_OFFSET = 0x5000
PROGRAM_OFFSET = 0x6000
VECTOR_OFFSET = 0x6100
STORAGE_OFFSET = 0x1C0000
# The RP2040's 264 KiB of SRAM.
RAM_START = 0x20000000
RAM_END = 0x20042000
SEAL = struct.Struct("<7I")
METADATA = struct.Struct("<8s6I")
METADATA_MAGIC = b"DFYRP204"
ROLES = {"keyboard": 1}
SIDES = {"right": 0, "left": 1}
# The RP2040's UF2 family, and its flash page: the package's alignment and the
# payload of each UF2 block.
UF2_FAMILY = 0xE48BFF56
BLOCK_SIZE = 256

# UF2's block format, the same for every chip.
UF2_BLOCK = 512
UF2_HEADER = struct.Struct("<8I")
UF2_MAGICS = (0x0A324655, 0x9E5D5157)
UF2_END_MAGIC = 0x0AB16F30
UF2_NOT_MAIN_FLASH = 0x1
UF2_FILE_CONTAINER = 0x1000
UF2_FAMILY_PRESENT = 0x2000
# The data field between the header and the end magic; the payload fills its start.
UF2_DATA = 476

# Data bytes per record that to_ihex() writes, as objcopy does.
RECORD_SIZE = 16


def read_ihex(text, minimum=FLASH_BASE + VECTOR_OFFSET, maximum=FLASH_BASE + STORAGE_OFFSET):
    if not 0 <= minimum < maximum <= 0x100000000:
        raise ValueError("invalid application address bounds")
    memory = {}
    base = 0
    ended = False
    for line_number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        if ended:
            raise ValueError(f"line {line_number}: data after EOF")
        try:
            if not line.startswith(":"):
                raise ValueError()
            record = bytes.fromhex(line[1:])
            if len(record) < 5 or len(record) != record[0] + 5 or sum(record) & 0xFF:
                raise ValueError()
        except ValueError:
            raise ValueError(f"line {line_number}: invalid Intel HEX record") from None
        length, address, kind = record[0], int.from_bytes(record[1:3], "big"), record[3]
        data = record[4:-1]
        if kind == 0:
            if not length or address + length > 0x10000:
                raise ValueError(f"line {line_number}: invalid data span")
            start = base + address
            if start < minimum or start + length > maximum:
                raise ValueError(f"line {line_number}: data at 0x{start:x}..0x{start + length:x} "
                                 f"is not inside 0x{minimum:x}..0x{maximum:x}")
            for offset, value in enumerate(data, start):
                if offset in memory:
                    raise ValueError(f"line {line_number}: overlapping data at {offset:#x}")
                memory[offset] = value
        elif kind == 1:
            if length or address:
                raise ValueError(f"line {line_number}: invalid EOF")
            ended = True
        elif kind in (2, 4):
            if length != 2 or address:
                raise ValueError(f"line {line_number}: invalid extended address")
            base = int.from_bytes(data, "big") << (4 if kind == 2 else 16)
        elif kind in (3, 5):
            if length != 4 or address:
                raise ValueError(f"line {line_number}: invalid entry address")
        else:
            raise ValueError(f"line {line_number}: unsupported record type {kind}")
    if not ended or not memory:
        raise ValueError("Intel HEX requires application data and EOF")
    return memory


def ihex_record(kind, address=0, data=b""):
    record = bytes([len(data)]) + address.to_bytes(2, "big") + bytes([kind]) + data
    return ":" + (record + bytes([-sum(record) & 0xFF])).hex().upper() + "\n"


def to_ihex(memory):
    """Intel HEX text for memory: data records of up to 16 consecutive bytes
    within one 64 KiB segment, an extended linear address record before each
    segment, and the end-of-file record."""
    lines = []
    segment = None
    addresses = sorted(memory)
    first = 0
    while first < len(addresses):
        address = addresses[first]
        last = first + 1
        while (last < len(addresses) and last - first < RECORD_SIZE and
               addresses[last] == address + last - first and addresses[last] & 0xFFFF):
            last += 1
        if address >> 16 != segment:
            segment = address >> 16
            lines.append(ihex_record(4, data=segment.to_bytes(2, "big")))
        lines.append(ihex_record(0, address & 0xFFFF,
                                 bytes(memory[a] for a in addresses[first:last])))
        first = last
    lines.append(ihex_record(1))
    return "".join(lines)


def pack_seal(start, length, program_crc, version):
    """A program's 28-byte seal, its CRC32 taken over the seal with that field zero."""
    fields = (start, length, program_crc, version)
    unsealed = SEAL.pack(1, SEAL.size, 0, *fields)
    return SEAL.pack(1, SEAL.size, zlib.crc32(unsealed), *fields)


def seal_crc_ok(seal):
    """Whether a seal's CRC32 matches the seal with its CRC field zero."""
    return zlib.crc32(seal[:8] + bytes(4) + seal[12:SEAL.size]) == SEAL.unpack_from(seal)[2]


def validate_vectors(data, program_length):
    if len(data) < 8:
        raise ValueError("missing initial stack pointer and reset vector")
    stack, reset = struct.unpack_from("<2I", data)
    if stack % 8 or not RAM_START < stack <= RAM_END:
        raise ValueError("initial stack pointer is outside aligned RP2040 SRAM")
    if not reset & 1:
        raise ValueError("reset vector must be a Thumb address")
    if not FLASH_BASE + VECTOR_OFFSET + 8 <= (reset & ~1) < FLASH_BASE + PROGRAM_OFFSET + program_length:
        raise ValueError("reset vector is outside the factory application image")


def validate_identity(role, side):
    if role not in ROLES or side not in SIDES:
        raise ValueError("unsupported factory image role or side")


def make_package(memory, version, *, side):
    if not 0 <= version <= 0xFFFFFFFF:
        raise ValueError("version must fit in 32 bits")
    validate_identity("keyboard", side)
    vector = FLASH_BASE + VECTOR_OFFSET
    if not memory or min(memory) != vector or max(memory) >= FLASH_BASE + STORAGE_OFFSET:
        raise ValueError("application must be linked at 0x10006100 and end before settings storage")
    if any(address not in memory for address in range(vector, vector + 8)):
        raise ValueError("incomplete vector table")
    program_length = (max(memory) + 1 - FLASH_BASE - PROGRAM_OFFSET + 3) & ~3
    program = bytearray(b"\x00" * (VECTOR_OFFSET - PROGRAM_OFFSET))
    METADATA.pack_into(program, 0, METADATA_MAGIC, 1, METADATA.size,
                       ROLES["keyboard"], SIDES[side], version, 0)
    program.extend(b"\xff" * (program_length - len(program)))
    for address, value in memory.items():
        program[address - FLASH_BASE - PROGRAM_OFFSET] = value
    validate_vectors(program[VECTOR_OFFSET - PROGRAM_OFFSET:], program_length)
    reset = struct.unpack_from("<I", program, VECTOR_OFFSET - PROGRAM_OFFSET + 4)[0] & ~1
    if reset not in memory or reset + 1 not in memory:
        raise ValueError("reset vector points into missing application data")
    header = pack_seal(PROGRAM_OFFSET, len(program), zlib.crc32(program), version)
    package = header + bytes(PROGRAM_OFFSET - PACKAGE_OFFSET - len(header)) + program
    package += bytes(-len(package) % BLOCK_SIZE)
    verify_package(package)
    return package


def verify_package(package):
    if len(package) < PROGRAM_OFFSET - PACKAGE_OFFSET + VECTOR_OFFSET - PROGRAM_OFFSET + 8:
        raise ValueError("factory package is truncated")
    if len(package) % BLOCK_SIZE or len(package) > STORAGE_OFFSET - PACKAGE_OFFSET:
        raise ValueError("factory package size overlaps settings storage or is not page aligned")
    kind, seal_size, _, start, length, program_crc, version = SEAL.unpack_from(package)
    if kind != 1 or seal_size != SEAL.size or start != PROGRAM_OFFSET or length % 4:
        raise ValueError("unsupported factory seal layout")
    if not seal_crc_ok(package[:SEAL.size]):
        raise ValueError("factory seal CRC mismatch")
    offset = start - PACKAGE_OFFSET
    if length < VECTOR_OFFSET - PROGRAM_OFFSET + 8 or offset + length > len(package):
        raise ValueError("factory program is truncated")
    if (offset + length + BLOCK_SIZE - 1) // BLOCK_SIZE * BLOCK_SIZE != len(package):
        raise ValueError("unexpected data after factory program")
    program = package[offset:offset + length]
    if zlib.crc32(program) != program_crc:
        raise ValueError("factory program CRC mismatch")
    validate_vectors(program[VECTOR_OFFSET - PROGRAM_OFFSET:], length)
    if not program.startswith(METADATA_MAGIC):
        raise ValueError("factory image metadata missing")
    _, schema, size, role_id, side_id, metadata_version, reserved = METADATA.unpack_from(program)
    if schema != 1 or size != METADATA.size or reserved != 0 or metadata_version != version:
        raise ValueError("unsupported factory image metadata layout or version")
    role = next((name for name, value in ROLES.items() if value == role_id), None)
    side = next((name for name, value in SIDES.items() if value == side_id), None)
    validate_identity(role, side)
    if any(program[METADATA.size:VECTOR_OFFSET - PROGRAM_OFFSET]):
        raise ValueError("factory image metadata padding must be zero")
    return {"program_start": start, "program_length": length, "program_crc32": program_crc,
            "version": version, "role": role, "side": side}


def make_uf2(package):
    verify_package(package)
    count = len(package) // BLOCK_SIZE
    blocks = []
    for index in range(count):
        address = FLASH_BASE + PACKAGE_OFFSET + index * BLOCK_SIZE
        header = UF2_HEADER.pack(*UF2_MAGICS, UF2_FAMILY_PRESENT, address, BLOCK_SIZE,
                                 index, count, UF2_FAMILY)
        payload = package[index * BLOCK_SIZE:(index + 1) * BLOCK_SIZE]
        blocks.append(header + payload + bytes(UF2_DATA - BLOCK_SIZE) +
                      struct.pack("<I", UF2_END_MAGIC))
    return b"".join(blocks)


def main():
    parser = argparse.ArgumentParser(description="Package a relocated Defy application for the factory RP2040 bootloader.")
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output-bin", type=Path, required=True)
    parser.add_argument("--output-uf2", type=Path, required=True)
    parser.add_argument("--version", type=lambda value: int(value, 0), required=True)
    parser.add_argument("--side", choices=SIDES, required=True)
    args = parser.parse_args()
    if len({path.resolve() for path in (args.input, args.output_bin, args.output_uf2)}) != 3:
        parser.error("input and output paths must be distinct")
    try:
        package = make_package(read_ihex(args.input.read_text()), args.version, side=args.side)
        uf2 = make_uf2(package)
        args.output_bin.write_bytes(package)
        args.output_uf2.write_bytes(uf2)
    except (OSError, UnicodeError, ValueError) as error:
        sys.exit(f"{args.input}: {error}")
    print(f"Packaged {len(package)} bytes at 0x{FLASH_BASE + PACKAGE_OFFSET:08x}.")


if __name__ == "__main__":
    main()
