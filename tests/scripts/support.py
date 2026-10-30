# SPDX-License-Identifier: MIT
"""What the tests of scripts/ share: the repository's root, the scripts on
sys.path, and Intel HEX written independently of the scripts' own writer."""
import importlib.machinery
import importlib.util
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))


def load_script(name, filename):
    """A script without a .py suffix, such as build, as a module called name."""
    loader = importlib.machinery.SourceFileLoader(name, str(SCRIPTS / filename))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def record(address, kind, data=b""):
    """One Intel HEX record, without its line end."""
    data = bytes((len(data), address >> 8, address & 0xFF, kind)) + data
    return ":" + (data + bytes((-sum(data) & 0xFF,))).hex().upper()


def ihex(data, address):
    """Intel HEX text placing data at address, 16 bytes to a record, inside one
    64 KiB segment."""
    rows = [record(0, 4, (address >> 16).to_bytes(2, "big"))]
    rows.extend(record((address + offset) & 0xFFFF, 0, data[offset:offset + 16])
                for offset in range(0, len(data), 16))
    rows.append(record(0, 1))
    return "\n".join(rows)
