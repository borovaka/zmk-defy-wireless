# SPDX-License-Identifier: MIT
"""Cuts the scanning code out of the patched split central.c for the test."""
from pathlib import Path
import sys


def find(source, marker, start=0):
    """Where marker starts in source, from start on."""
    index = source.find(marker, start)
    if index < 0:
        raise ValueError(f"split_central.c has no {marker!r}")
    return index


def function(source, signature):
    start = find(source, signature + " {")
    opening = find(source, "{", start)
    depth = 1
    for end in range(opening + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(f"Missing function boundary: {signature}")


source = Path(sys.argv[1]).read_text()
scan_start = find(source, "/* Defy: scanning that recovers on its own.")
scan_end = find(source, "static bool split_central_eir_found(const bt_addr_le_t *addr) {")
parts = ["#include <zephyr/sys/atomic.h>\n#include <dygma/defy_split_central.h>",
         source[scan_start:scan_end].rstrip()]
for signature in (
    "static int start_scanning(void)",
    "static bool split_central_eir_found(const bt_addr_le_t *addr)",
    "static void split_central_connected(struct bt_conn *conn, uint8_t conn_err)",
    "static int split_central_bt_set_enabled(bool enabled)",
):
    parts.append(function(source, signature))
handler = find(source, "static int central_ble_handle_set(")
start = source.rfind("#if IS_ENABLED(CONFIG_SETTINGS)", 0, handler)
if start < 0:
    raise ValueError("split_central.c has no '#if IS_ENABLED(CONFIG_SETTINGS)' before "
                     "central_ble_handle_set()")
end = find(source, "static int zmk_split_bt_central_listener_cb", handler)
parts.append(source[start:end])
Path(sys.argv[2]).write_text("\n\n".join(parts) + "\n")
