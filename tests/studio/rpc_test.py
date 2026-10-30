#!/usr/bin/env python3
"""
ZMK Studio's view of the Defy, over the RPC protocol the Studio app speaks.

Starts the native_sim build of tests/studio, connects to its Studio UART and
checks what Studio would show: the device, the physical layout, the keymap and
the behaviors in it.

Usage: rpc_test.py <zmk.exe> <Studio .proto directory> <defy-layouts.dtsi>

SPDX-License-Identifier: MIT
"""

import os
import math
import re
import select
import subprocess
import sys
import tempfile
import threading
import tty

SOF, ESC, EOF = 0xAB, 0xAC, 0xAD
TIMEOUT = 10

KEYS = 70
# defy.keymap's reserved layers, layer_2 to layer_9.
RESERVED_LAYERS = 8


def load_messages(proto_dir, out):
    protos = sorted(f for f in os.listdir(proto_dir) if f.endswith(".proto"))
    subprocess.run(
        [sys.executable, "-m", "grpc_tools.protoc", f"-I{proto_dir}", f"--python_out={out}"]
        + protos,
        check=True,
    )
    sys.path.insert(0, out)
    import behaviors_pb2, core_pb2, keymap_pb2, studio_pb2

    return behaviors_pb2, core_pb2, keymap_pb2, studio_pb2


def frame(payload):
    out = bytearray([SOF])
    for b in payload:
        if b in (SOF, ESC, EOF):
            out.append(ESC)
        out.append(b)
    out.append(EOF)
    return bytes(out)


class Keyboard:
    """The simulated Defy, with its Studio UART."""

    def __init__(self, exe, workdir, studio):
        self.studio = studio
        self.request_id = 0
        self.rx = bytearray()
        self.log = []
        # The simulated flash file lands in the working directory, so each run
        # starts with an empty one.
        self.proc = subprocess.Popen(
            [exe],
            cwd=workdir,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        pty = None
        for line in self.proc.stdout:
            self.log.append(line)
            match = re.search(r"uart_1 connected to pseudotty: (\S+)", line)
            if match:
                pty = match.group(1)
                break
        if pty is None:
            raise RuntimeError("the simulation exited before opening the Studio UART")
        # Keep reading the log, or the simulation blocks once the pipe fills.
        threading.Thread(target=lambda: self.log.extend(self.proc.stdout), daemon=True).start()
        self.fd = os.open(pty, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)

    def stop(self):
        self.proc.kill()
        self.proc.wait()
        os.close(self.fd)

    def read_frame(self):
        payload = None
        escaped = False
        while True:
            while self.rx:
                b = self.rx.pop(0)
                if payload is None:
                    if b == SOF:
                        payload = bytearray()
                elif escaped:
                    payload.append(b)
                    escaped = False
                elif b == ESC:
                    escaped = True
                elif b == EOF:
                    return bytes(payload)
                else:
                    payload.append(b)
            ready, _, _ = select.select([self.fd], [], [], TIMEOUT)
            if not ready:
                raise TimeoutError("no response from the simulated keyboard")
            self.rx += os.read(self.fd, 4096)

    def call(self, **subsystem):
        """Send one request and return its response, skipping notifications."""
        self.request_id += 1
        request = self.studio.Request(request_id=self.request_id, **subsystem)
        os.write(self.fd, frame(request.SerializeToString()))
        while True:
            response = self.studio.Response.FromString(self.read_frame())
            if response.WhichOneof("type") != "request_response":
                continue
            result = response.request_response
            if result.request_id != self.request_id:
                raise RuntimeError(f"response to request {result.request_id}, expected {self.request_id}")
            if result.WhichOneof("subsystem") == "meta":
                raise RuntimeError(f"request {self.request_id} failed: {result.meta}")
            return result


class Checks:
    def __init__(self):
        self.failed = 0

    def __call__(self, ok, what):
        print(f"{'ok  ' if ok else 'FAIL'} {what}")
        self.failed += not ok


def parse_layout(path):
    with open(path) as f:
        text = f.read()
    number = r"\s+\(?(-?\d+)\)?"
    return [tuple(map(int, m)) for m in re.findall(r"&key_physical_attrs" + number * 7, text)]


def key_corners(key):
    w, h, x, y, rotation, rx, ry = key
    angle = math.radians(rotation / 100)
    c, s = math.cos(angle), math.sin(angle)
    return [(rx + (px - rx) * c - (py - ry) * s,
             ry + (px - rx) * s + (py - ry) * c)
            for px, py in [(x, y), (x+w, y), (x+w, y+h), (x, y+h)]]


def polygons_overlap(a, b):
    for polygon in (a, b):
        for i, (x, y) in enumerate(polygon):
            nx, ny = polygon[(i + 1) % len(polygon)]
            axis = (y - ny, nx - x)
            pa = [px * axis[0] + py * axis[1] for px, py in a]
            pb = [px * axis[0] + py * axis[1] for px, py in b]
            if max(pa) <= min(pb) + 0.001 or max(pb) <= min(pa) + 0.001:
                return False
    return True


def overlapping_keys(keys):
    pairs = []
    for i, a in enumerate(keys):
        for j, b in enumerate(keys[i + 1 :], i + 1):
            if polygons_overlap(key_corners(a), key_corners(b)):
                pairs.append((i, j))
    return pairs


def main():
    exe, proto_dir, layout_dtsi = sys.argv[1:]
    check = Checks()

    with tempfile.TemporaryDirectory() as tmp:
        behaviors, core, keymap, studio = load_messages(proto_dir, tmp)
        board = Keyboard(exe, tmp, studio)
        try:
            run(board, check, behaviors, core, keymap, parse_layout(layout_dtsi))
        except Exception as e:
            check(False, f"{type(e).__name__}: {e}")
        finally:
            board.stop()
        if check.failed:
            print("".join(board.log[-40:]))

    print(f"{check.failed} failed" if check.failed else "all passed")
    sys.exit(1 if check.failed else 0)


def run(board, check, behaviors, core, keymap, expected_layout):
    info = board.call(core=core.Request(get_device_info=True)).core.get_device_info
    check(info.name == "Defy", f"device name is {info.name!r}")

    # Physical layout, as Studio draws it.
    layouts = board.call(keymap=keymap.Request(get_physical_layouts=True)).keymap.get_physical_layouts
    check(len(layouts.layouts) == 1, f"{len(layouts.layouts)} physical layout(s)")
    layout = layouts.layouts[0]
    keys = [(k.width, k.height, k.x, k.y, k.r, k.rx, k.ry) for k in layout.keys]
    check(layout.name == "Defy", f"layout name is {layout.name!r}")
    check(len(keys) == KEYS, f"layout has {len(keys)} keys")
    check(keys == expected_layout, "layout matches defy-layouts.dtsi")
    check(all(k[4] == 0 for k in keys[:54]), "main keys are unrotated")
    check(keys[65][1] > keys[0][1] and keys[66][1] > keys[0][1],
          "Backspace and Enter are the large inner thumbkeys")
    check(keys[65][4] > 0 and keys[66][4] < 0, "large thumbkeys have mirrored angles")
    check(all(keys[i][5] < keys[i + 1][5] for i in range(66, 69)),
          "right lower thumbkeys follow physical left-to-right order")
    overlaps = overlapping_keys(keys)
    check(not overlaps, f"no overlapping keys {overlaps if overlaps else ''}")

    # Behavior IDs are assigned at build time; Studio looks up their names.
    ids = board.call(behaviors=behaviors.Request(list_all_behaviors=True)).behaviors.list_all_behaviors.behaviors
    names = {}
    for behavior_id in ids:
        details = board.call(
            behaviors=behaviors.Request(
                get_behavior_details=behaviors.GetBehaviorDetailsRequest(behavior_id=behavior_id)
            )
        ).behaviors.get_behavior_details
        names[details.id] = details.display_name

    # The keymap.
    km = board.call(keymap=keymap.Request(get_keymap=True)).keymap.get_keymap
    check([layer.name for layer in km.layers] == ["Base", "Num"], f"layers {[layer.name for layer in km.layers]}")
    check(km.available_layers == RESERVED_LAYERS, f"{km.available_layers} layers free for Studio")
    check(all(len(layer.bindings) == KEYS for layer in km.layers), "every layer has a binding per key")
    unknown = [(layer.name, pos) for layer in km.layers for pos, b in enumerate(layer.bindings) if b.behavior_id not in names]
    check(not unknown, f"every binding has a known behavior {unknown if unknown else ''}")


if __name__ == "__main__":
    main()
