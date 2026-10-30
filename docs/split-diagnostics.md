# Split diagnostics

The Neuron answers read-only diagnostic requests on its [half updater](half-updater.md)'s serial port, not on the Studio port. None of them opens a maintenance session, resets the halves or writes firmware. Each is an updater frame with session, sequence and status zero.

[defy-installer](https://github.com/borovaka/defy-installer) sends opcode 8 to wait for the halves to connect after an install, or after an update that resets the split pairing, and opcodes 14, 15, 12 and 9 for its `status`. No public client reads opcodes 10 and 11 yet. They are all specified here, with opcode 9 in [battery](battery.md#reporting-to-the-computer) and opcodes 12, 14 and 15 in the [half updater](half-updater.md#protocol), for anyone writing a client.

## Split status

Opcode 8 reads the Bluetooth split snapshot, and opcode 10 the cable and dock state; see [cable and dock detection](automatic-transports.md#cable-and-dock-detection). Together they give the split status.

Each of the two records describes a connection slot, not a fixed half. It holds connection and disconnection counts, the last disconnect reason, setup progress, discovery and subscription errors, the count of valid key reports, subscription removals and missing-report watchdog attempts. It holds no keystrokes, key bitmaps or Bluetooth addresses.

The opcode 8 payload is version 1, 104 bytes, little-endian. Its 8-byte header holds the version, the record count 2, the record size 48 as a 16-bit value, and the Neuron's uptime in milliseconds as a 32-bit value. Each record holds:

| Bytes | Field |
| --- | --- |
| 0 | Slot |
| 1 | Phase: 0 idle, 1 connected, 2 service discovery, 3 characteristic discovery, 4 CCC discovery, 5 subscribing, 6 subscribed, 7 reporting, 8 subscription removed, 9 setup failed, 10 recovery |
| 2 | Flags, below |
| 3 | Last disconnect reason |
| 4..7 | Connection epoch |
| 8..11 | Last discovery error, signed |
| 12..15 | Last subscription error, signed |
| 16 | Last subscription ATT error |
| 17 | Discovery type of the last discovery step |
| 18..19 | Reserved, 0 |
| 20..23 | Connections |
| 24..27 | Disconnections |
| 28..31 | Valid key reports |
| 32..35 | Subscription removals |
| 36..39 | Missing-report watchdog attempts |
| 40..43 | Milliseconds since the last valid key report |
| 44..47 | Milliseconds since the last connection |

The flag bits are:

| Bit | Meaning |
| --- | --- |
| 0 | A connection has been seen and has not disconnected |
| 1 | The key characteristic's handle is known |
| 2 | The key subscription is tracked |
| 3 | A valid key report arrived on this connection |

Counters, last errors and event ages survive a disconnection. The phase and bit 3 describe the current connection. An age of `0xFFFFFFFF` means the event has never happened. The epoch numbers the prepared connection. `defy_hybrid_ble_diagnostics()` in `drivers/split/defy_hybrid_ble.c` writes the payload.

An automatic CCC discovery can fail inside Zephyr and show up only as a subscription removal, without an error from the call. Zero error fields therefore do not prove a working subscription. The reporting phase and a rising count of valid key reports show that valid snapshots are arriving; all-up snapshots count too.

The snapshot does not report scan start or stop errors or the scanner's state. A slot waiting to connect cannot name a scan failure. Use the phase, report ages and counters to tell whether input arrives.

Reading does not refresh a session's idle limit or replace a cached write reply.

## Key event trace

`CONFIG_DEFY_KEY_TRACE` is on by default wherever the half updater is. The Neuron then keeps its last 64 key steps in 768 bytes of RAM, 12 bytes each, with a microsecond time. It records four kinds:

- `poll`, kind 4, when the cable worker takes a frame that changes a half's keys. It names the port, the frame's sequence and when its SPI transfer completed.
- `snapshot`, kind 1, when the hybrid central queues a snapshot from either link. It names the side, the link, the Bluetooth slot, the queue order and when the frame arrived, to the millisecond.
- `position`, kind 2, for each event the hybrid central hands to ZMK, with the queue order of the snapshot behind it.
- `keycode`, kind 3, for each ZMK keycode event, seen by the listener `defy_key_trace`.

ZMK calls listeners in link order, not by name, and module sources link before ZMK's. The trace listener therefore sees keycode events before `hid_listener` does. It never consumes them.

A client traces a typing mistake by clearing the trace before the typing and reading it afterwards.

Opcode 11 reads the trace without a session. Its 5-byte request holds the first record wanted, as a 32-bit sequence number, and a flags byte, where bit 0 clears the trace before reading. A reply holds a 16-byte header and up to 20 records, so reading all 64 records takes four requests. The header gives the version 1, the record size 12, the capacity 64 and the record count in this reply, then the number of records written since the last clear, the sequence number of the first record in the reply and the trace clock at readout. Each record is the trace clock, the kind, a flags byte, a 16-bit value and a 32-bit extra word. The trace clock counts microseconds of uptime and wraps after 71 minutes. `include/dygma/defy_key_trace.h` defines the kinds and flags. Firmware without the trace answers with `-ENOTSUP`.

## Logging builds

Two Neuron targets swap ZMK Studio for a USB serial log. Both keep the keymap, both links to the halves and USB and Bluetooth output to the computer. Studio's unlock, FKeys+O in the template's keymap, stays transparent and types O.

| Target | Log | Buffer | Log thread starts after |
| --- | --- | ---: | ---: |
| `neuron-hybrid-log` | ZMK debug, Bluetooth connection and security messages, USB report traces. Battery support off; lighting, the overlay and the chooser stay | 8 KiB | 10 s |
| `neuron-hybrid-battery-log` | Warnings, plus the battery module's messages | 4 KiB | 1 s |

Both keep the earliest messages when the buffer fills. The delay applies to the log thread only; Bluetooth and the keyboard start at the normal time. Open the serial reader before replugging the Neuron to catch the start.

Install a logging build with a Neuron write that [keeps the settings](neuron-flash.md#keeping-the-settings) to keep pairings. Without Studio, the Neuron does not load saved Studio keymap changes, but it keeps them for the next Studio build. ZMK's debug log can include keycodes, so capture only controlled tests.
