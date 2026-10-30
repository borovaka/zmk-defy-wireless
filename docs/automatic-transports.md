# Cable and Bluetooth connections

Each half has two links to the Neuron: the cable, over this port's SPI protocol, and Bluetooth, over ZMK's split. Both stay up when they can, and the Neuron picks one for each half. One half can be cabled while the other is wireless. The halves need this port's radio controller, which the `*-hybrid-radio` images install.

## Routing

Each half keeps its key state current on both links. The Neuron uses a half's cable once it has a valid snapshot from it, and Bluetooth otherwise. A snapshot names its side by the key positions it may set, so neither the cable socket nor the Bluetooth connection order decides which half is which.

| Timing | Value |
| --- | ---: |
| Bluetooth snapshot from each half | every 100 ms |
| Cable poll, connected | every 2 ms |
| Cable probe, nothing connected | every 20 ms |
| Neuron gives up on a cable | 250 ms without valid data |
| Neuron drops a Bluetooth snapshot | after 1,000 ms |
| Bluetooth grace after a cable loss | 200 ms |

After 30 s without a key change, a half polls less often; see [connected idle](power-and-wake.md#connected-idle). The values are deadlines. Work-queue scheduling can stretch them.

When a cable drops, a known Bluetooth link has 200 ms to deliver a snapshot newer than the loss. Keys held in both stay pressed across the handoff. Without one, the Neuron releases the half's keys. A full event queue can drop Bluetooth transitions, but the last snapshot still puts the keys right. A cable queue overflow resends the current keys instead of the missed history.

If the radio does not start after the driver's bounded attempts, the half still works over its cable.

A half turns cyan for two seconds once its cable is acknowledged or its Bluetooth link is up, then shows the [everyday colours](lighting.md#normal-lighting). Encryption and the key subscription can still be pending at that point, so typing is the check.

## Cable link

| Side | Hardware |
| --- | --- |
| Half | SPI0 master, chip select GPIO21, mode 1, 1 MHz, 32-byte full-duplex transfers, 10 µs chip-select margin |
| Neuron | SPIS1 and SPIS2 on the stock cable pins. Either socket takes either half |

Key frames are 32 bytes:

| Bytes | Field |
| --- | --- |
| 0..1 | `DW` |
| 2 | Version, 2 |
| 3 | Kind: 1 data, 2 acknowledgement |
| 4 | Side: 0 left, 1 right |
| 5 | Flags: 1 sync, 2 resync |
| 6 | Payload length: 9 for data, 0 for an acknowledgement |
| 7 | Reserved, 0 |
| 8..11 | Session, little-endian |
| 12..15 | Sequence, little-endian |
| 16..24 | Bitmap of the 70 key positions |
| 25..29 | Reserved, 0 |
| 30..31 | CRC16/CCITT-FALSE over bytes 0..29, little-endian |

A half may set only its own 35 positions.

- **Half.** It queues up to 32 snapshots and resends the oldest until the Neuron acknowledges it. Sequence 0 with the sync flag starts a session. It drops the connection after 100 ms without a valid acknowledgement. A full queue discards the history and starts a new session with the current keys, so a long interruption can lose taps but never leaves a key stuck.
- **Neuron.** The cable worker hands each changed snapshot to the router's 128-entry queue, which the system work queue turns into ZMK key events. The Neuron acknowledges a frame only after its snapshot is queued; when the queue is full, the frame goes unacknowledged and the half sends it again. An unexpected session or sequence gets a resync request, and duplicates do not repeat a change. The worker checks for a silent cable at least every 10 ms and gives up after 250 ms without valid data. A ZMK error on a key event is logged, and the event is not replayed. A second half claiming a side that is already connected is refused until the first one times out.
- **Reconnection** sends the current keys. Taps made while disconnected are not replayed.

Frames that start with `DL` carry the Neuron's commands to a half, the battery display, the lighting state, its [key groups](lighting.md#key-colours) and host feedback, and a half's lighting acknowledgements back. A transfer carries one command in place of an acknowledgement the half already has, so a state that names n groups the half has not confirmed follows them about 2n transfers later; see [lighting messages](#lighting-messages). Frames that start with `DB` carry battery telemetry from a half in place of an idle keep-alive. Frames that start with `DV` carry the half's [version report](versions.md#the-halves-report) the same way, once after the link comes up and then every 60 s.

The halves use the RP2040 SPI driver's polled mode. Pulling a cable does not stop the half's clock. A local SPI clock or FIFO fault has no bounded recovery.

### Lighting messages

The lighting state, its key groups and host feedback carry the same fields over both links. Over Bluetooth each is a message on the halves' [control service](battery.md#measurement-and-transport) that starts with its version and opcode. Over the cable a `DL` frame holds the version and opcode in bytes 2 and 3, the side in byte 4, a flag in byte 5 and the cable session in bytes 8 to 11, and the message's other fields from byte 12. Values of more than one byte are little-endian.

| Message | Version, opcode | Over Bluetooth | In a `DL` frame |
| --- | --- | --- | --- |
| Lighting state from the Neuron, and a half's acknowledgement | 3; 2, and 3 for the acknowledgement | 20 bytes: the version, the opcode, the on flag, the boot and the revision as 32-bit values, the colour of the per-key LEDs and the colour of the underglow, three bytes each in red, green, blue order, the groups it names as 16 bits, and the brightness | The on flag as the flag; the boot and the revision in bytes 12 to 19; the colours, the groups and the brightness in bytes 20 to 28 |
| Key group | 3; 6 | 15 bytes: the version, the opcode, the boot as a 32-bit value, the group's number, its colour at full strength, and five bytes with a bit for each of the half's 35 per-key LEDs, in LED order | Flag 0; the boot and the other fields in bytes 12 to 24 |
| Host feedback | 3; 5 | 20 bytes: the version, the opcode, the boot and a sequence as 32-bit values, the display's remaining time in milliseconds as 16 bits, the selected profile, the flags, the profile count, the profiles cleared since the Neuron started, and the paired and connected profiles as 16-bit masks | Flag 0; bytes 2 to 19 of the message in bytes 12 to 29 |

The halves' wake report is at lighting version 3 too. A group has no acknowledgement of its own. The Neuron counts a half's groups as delivered once the half has acknowledged a state that names them, and until then sends any others again before each state it sends. Each side drops a lighting or host feedback message whose version is not its own; see [matching builds](lighting.md#matching-builds).

## Bluetooth link

The halves are ZMK split peripherals. Each half saves its Bluetooth identity to its settings before it advertises, so the identity stays the same across restarts. Pairing keys and nonces come from the radio's hardware random number generator, through HCI LE Rand. The Neuron keeps one split slot per half, bound to the half's stored address; the half updater can [reset them](#fresh-split-pairing). [Connection recovery](ble-input-recovery.md) covers how the Neuron recovers stalled connections and subscriptions.

### Radio restarts

The radio's controller sends a ready event once it has started. A ready event while Bluetooth runs means that the controller restarted on its own, and every Bluetooth connection went with it. Data ready held high for 1 s without a readable message counts as a restart too. After 60 s of uptime the half then restarts cold, so that ZMK's Bluetooth and the controller start over together. Within 60 s of its start, the half stops Bluetooth instead. Otherwise a controller that kept restarting would keep the half restarting. The half then works only over its cable until it next starts. `controller_restarted()` in `drivers/bluetooth/defy_hci_spi.c` makes the choice.

Dygma's radio application starts the nRF52820's hardware watchdog, which only a power-on, pin or watchdog reset stops. Whenever it runs, the controller feeds it at reset and then every 125 ms. The same timer is a software watchdog. The controller's link thread must show progress within 5 s of the start and then at least once a second, or the radio restarts, and the half sees a restart as above. `apps/defy_radio/src/watchdog.c` holds both.

## Output to the computer

The Neuron uses ZMK's host selection. USB counts as ready only once the computer has configured HID, so power from the dock alone does not select USB. USB is preferred by default, and a connected Bluetooth host is the fallback. `&out OUT_USB` and `&out OUT_BLE`, FKeys+Q and FKeys+W in the template's keymap, change the preference, and it persists.

`CONFIG_BT_MAX_PAIRED=12` gives bonds for ten host profiles plus the two halves. `CONFIG_BT_MAX_CONN=9` allows both halves and seven hosts at once. ZMK keeps every paired host in range connected, not only the selected one. With seven hosts connected, an eighth cannot connect until one drops, even for the selected profile.

The [profile chooser](lighting.md#bluetooth-profile-chooser) selects and clears host profiles. Clearing a profile never clears the halves' bonds. A selected empty profile advertises as `Defy` for pairing.

Release all keys before moving the Neuron between USB and the dock. ZMK clears the old HID report, and held keys do not carry over to the new host. [USB report recovery](power-and-wake.md#usb-report-recovery) covers held keys and mouse buttons across a USB suspend.

## Key order with hold-taps

The Neuron builds ZMK's own `keymap.c`, so ZMK's hold-tap, combo and keymap listeners run in ZMK's order. ZMK calls event listeners in link order, and module sources link before ZMK's. The module's own listeners on the Neuron therefore run first: lighting activity and the profile chooser on key positions, and the key trace on keycodes. Only the open profile chooser holds keys back: it takes them for itself until it closes. The driver suite `defy_profile_chooser` checks which keys the open chooser takes and which reach the keymap. Nothing in this module tests the order of ZMK's own listeners.

## Cable and dock detection

The Neuron reads the halves' reset lines to see which cables are in, as stock firmware does. On Neuron hardware V1.1, nRST2 on P0.29 runs to the left half and nRST1 on P0.28 to the right; the board's `defy_cables` node lists them as `run-gpios`. Both lines are inputs without pull. A cabled half pulls its line high through the pull-up on its RP2040 RUN pin, so high means the cable is in. An unconnected line reads low. Only the half updater drives the lines, low for 10 ms, to reset the halves.

`status/defy_neuron_dock.c` samples both lines every 250 ms on the system work queue. A new level counts after two equal samples in a row, so a plugged or pulled cable shows within half a second. While the half updater holds the lines, sampling pauses and the last result stays. `CONFIG_DEFY_NEURON_DOCK` is on by default.

The Neuron then reports one of four places:

| Place | When |
| --- | --- |
| computer | A USB host has configured HID, including while it is suspended |
| docked | Exactly one line high and no configured USB host, the stock rule |
| charger | USB power without a configured host, with any other line state |
| alone | No USB power and not docked |

The dock raises the right half's line, nRST1 on P0.28, although the left half holds the Neuron. One cable with the Neuron on a charger also reads as docked; stock firmware has the same limit. A computer does not see the Neuron's USB socket while the Neuron is docked, so the half updater's port cannot report the docked state. The docked marker on the halves is the check.

While docked, every host feedback frame carries `DEFY_HOST_DOCKED`, bit 7 of the flags. When a half shows the host overlay or the profile chooser, it lights the innermost key of the second row cyan. That key is the middle battery cell, which neither display uses. The open chooser resends its frame every 500 ms, so the marker follows the dock within about a second.

The half updater's opcode 10 reads the result; see [split diagnostics](split-diagnostics.md). Its payload is version 1, 8 bytes, written by `defy_neuron_dock_status()`:

| Byte | Field |
| --- | --- |
| 0 | Version, 1 |
| 1 | Line levels of the last sample: bit 0 left, bit 1 right |
| 2 | Cables in, debounced, with the same bits |
| 3 | Sampling: bit 0 set once the pins are set up, bit 1 set while the half updater holds the lines |
| 4 | USB: 0 no power, 1 powered, 2 HID configured |
| 5 | Place: 0 alone, 1 charger, 2 docked, 3 computer |
| 6..7 | Reserved, 0 |

Firmware without the readout answers opcode 10 with `-ENOTSUP`.

## Fresh split pairing

A half keeps its Bluetooth identity and bonds in its ZMK settings, and the Neuron keeps the split bonds in its own, with one split slot per half. Clearing one side alone does not work. The Neuron keeps both split slots for the old identities and refuses a half with a new one. A half refuses an unauthenticated replacement of its bond: it connects, its key subscription fails with ATT error 5, insufficient authentication, and no keys arrive.

`defy-installer` resets the split pairing whenever it writes the halves, in `install`, `update` or `advanced halves write`, and the Neuron is not paired with both. That covers a first install, a Neuron write without `--keep-settings`, and a pairing that failed. `advanced halves write --reset-pairing` resets it even when the Neuron is paired with both. Before the write, the Neuron clears its split bonds and slots and erases both halves' ZMK settings, and each half pairs again as a new device, with its cable in or out; [split pairing](half-updater.md#split-pairing) gives the requests and the erase.

The reset erases nothing else. The Neuron's host pairings, Studio changes and lighting preference stay, and so do the rest of each half's 256 KiB settings partition, its factory bootloader and its radio. Do not install stock half firmware as a pairing reset, because it can replace the radio controller.

On the tested keyboard, RESET PAIRING erased both halves' settings through Dygma's factory bootloaders, and with their cables out both halves paired again as new devices and typed.
