# Half updater

`neuron-hybrid` writes both halves' RP2040 applications over the cables, through the halves' own factory I2C bootloaders, while the Neuron keeps running ZMK with its settings and pairings. An installer on the computer drives it over USB serial; [defy-installer](https://github.com/borovaka/defy-installer) does. The same serial port also answers read-only status requests: [split diagnostics](split-diagnostics.md), battery snapshots, cable and dock detection, and the key event trace.

For an update, the Neuron stays connected to the computer by USB, both halves are cabled to it, and both wireless switches are off. No button, Esc press or Neuron replug is needed. The write itself goes over the cables, and a half switched on writes the same; that was checked on the keyboard. The switches go off so that each half runs from its cable alone: if the Neuron loses power during a write, the halves restart with it, where a half on its battery stays in its factory bootloader until it is switched off and on; see [interrupted updates](#interrupted-updates-and-neuron-restarts). A first install also needs each half's radio to power up together with its RP2040, which only switching the half off and on again, with its cable out, does; see [radio install](radio-install.md#when-a-half-installs-it). Typing pauses while a session is open and resumes when it closes; meanwhile the Neuron's [status LED](lighting.md#neuron-status-led) blinks amber. The Neuron's firmware and settings stay in place, and writes stop before the halves' settings. Only a [pairing reset](#split-pairing) erases those, together with the Neuron's split bonds.

## USB interface

The Neuron has a second USB serial interface for the updater, beside Studio's. Both carry the Neuron's USB serial number. An installer finds the updater by its versioned HELLO reply, `DEFYUPD1`, and should check the Neuron's serial number before it starts a session. The updater interface ignores the 1200-baud bootloader trigger, so [software entry to the Neuron bootloader](software-bootloader.md) works only through the Studio port, or the log port on the logging builds. Close any Studio connection before updating the Neuron itself.

## Half images

A half image, `<target>.bin`, holds the RP2040's application area from flash offset `0x5000`, behind the factory bootloader, which it never contains:

| Offset | Content |
| --- | --- |
| `0x5000` | Seal, seven little-endian words: format 1, the seal's size 28, the CRC32 of the seal with this word zeroed, then the program's start `0x6000`, size, CRC32 and version |
| `0x6000` | Metadata page: `DFYRP204`, then six little-endian words: format 1, size 32, role 1 for a keyboard half, side 0 for right or 1 for left, the program's version and 0. The rest of the 256-byte page is zero |
| `0x6100` | The vector table, then the rest of the program |
| `0x1C0000` | ZMK's settings, which no image reaches. The first 32 KiB, `0x1C0000..0x1C8000`, hold the eight NVS sectors, the half's Bluetooth identity and bonds among them |

The flash is mapped at `0x10000000`, so the settings start at `0x101C0000` in the CPU's address space. The version is the 32-bit value set by `scripts/build --factory-version`, 1 by default. The CRCs and the metadata catch packaging mistakes, such as a swapped or duplicated side; they do not authenticate the firmware. `scripts/package_factory.py` builds the package, and `include/dygma/defy_half_package.h` checks it on the Neuron.

## Protocol

Every frame starts with a 16-byte header, little-endian: `DU`, version 1, opcode, a 32-bit session, a 32-bit sequence, a 16-bit payload length and a signed 16-bit status. Up to 260 payload bytes and a CRC32 of the header and payload follow. A request carries status 0. A reply copies the request's header, sets bit `0x80` in the opcode and fills in its own length and status, a negative errno on failure. `include/dygma/defy_updater.h` and `updater/defy_updater.c` define the fields.

| Opcode | Request | Payload sent | Payload returned |
| ---: | --- | --- | --- |
| 1 | HELLO | None | 24 bytes: `DEFYUPD1`, the page size 256 and the largest payload 260 as 16-bit values, then the writable range `0x5000..0x1C0000` and the idle limit, 10,000 ms, as 32-bit values |
| 2 | OPEN: start a session and put both halves into their bootloaders | None | None |
| 3 | PROBE: read one half's bootloader information and seal | The side | 20 bytes: the side, 1 if the seal is valid, two zero bytes, then the hardware version, the flash start, and the program's version and CRC32 from the seal, as 32-bit values |
| 4 | START: begin a transfer to one half | 20 bytes: the side, three zero bytes, then the transfer length, the CRC32 of every page to be sent, the version and the program's CRC32, as 32-bit values | None |
| 5 | PAGE: write one 256-byte page | The page's 32-bit address, then the page | The next address |
| 6 | FINISH: validate the half's program | None | The version and CRC32 the bootloader validated |
| 7 | CLOSE: release the halves and resume typing | None | None |
| 8 | Split diagnostics; see [split diagnostics](split-diagnostics.md) | None | 104 bytes |
| 9 | Battery snapshot; see [battery](battery.md#reporting-to-the-computer) | None | The Neuron's cached battery records |
| 10 | Cable and dock detection; see [cable and dock detection](automatic-transports.md#cable-and-dock-detection) | None | 8 bytes |
| 11 | Key event trace; see [split diagnostics](split-diagnostics.md#key-event-trace) | 5 bytes | A page of records |
| 12 | PAIRING: the Neuron's split slots; see [split pairing](#split-pairing) | None | 4 bytes |
| 13 | RESET PAIRING: clear the Neuron's split pairing and erase both halves' settings; see [split pairing](#split-pairing) | None | None |
| 14 | VERSION: what the Neuron's firmware was built from; see [versions](#versions) | None | Up to 251 bytes |
| 15 | HALF VERSIONS: what each half last reported; see [versions](#versions) | None | 80 bytes |
| 16 | CLEAR KEYMAP CHANGES: drop the keymap changes saved by ZMK Studio; see [Studio's keymap changes](#studios-keymap-changes) | None | None |

The side is 0 for left and 1 for right, the reverse of the metadata's side ids.

HELLO, opcodes 8 to 12 and opcodes 14 to 16 need no session, take session and sequence 0, and do not refresh the session's idle limit. Firmware that predates an opcode answers it, sent without a session, with `-ENOTCONN`, as for PAIRING below. OPEN takes a new nonzero random session and sequence 1. It fails with `-EBUSY` while another session is open or closing, or while software entry to the Neuron bootloader owns the update. Every later request in the session raises the sequence by one; a wrong session gets `-ENOTCONN` and a wrong sequence `-EPROTO`.

A session first runs PROBE on both halves, then START, the pages in address order from `0x5000` and FINISH for each half in turn. The transfer length is a whole number of 4 KiB sectors, at least two, and must end with the sector that holds the program's last byte. The first page must hold a seal that matches START's length, program CRC32 and version. The page at `0x6000` must hold the metadata for the side being written, with the same version. Any other page there fails with `-EBADMSG` before it is written, so images without the metadata, Dygma's keyscanner among them, cannot go through the updater. FINISH checks the CRC32 of the pages sent, then needs the bootloader's validation to report the same version and program CRC32. A half that has finished cannot be started again in the same session. RESET PAIRING goes after both PROBEs and before the first START, once per session, and fails with `-EINVAL` anywhere else.

A request that repeats the last session request's session and sequence gets the cached reply, without repeating an erase or write, so an installer can resend a request whose reply never arrived. A different request with the same sequence gets `-EPROTO`. When a command within the session fails, the updater closes the session and releases the halves; a wrong session or sequence alone does not close it.

The updater accepts writes only to the half application area, `0x5000..0x1C0000`. The factory bootloaders below it are out of reach, and of the settings above it only RESET PAIRING erases the first 32 KiB. There is no command to read arbitrary flash or to jump. A closed serial port, a USB suspend or 10 s without a valid session request ends the session. The serial port is interrupt-driven. The updater thread sleeps until input arrives, and polls DTR every 5 ms only while a session, a partial frame or a cached reply is pending, so a brief close and reopen during a blocking I2C transfer may go unseen; a new session still has to pass the session and sequence checks.

## Split pairing

The Neuron and each half keep their side of a split bond: the half in its settings at `0x1C0000`, the Neuron in its own settings together with one split slot per half. A slot holds the half's address from its first connection attempt on, before any bond exists. Both sides must be cleared together; with one side cleared, the half connects and its key subscription fails with ATT error 5, insufficient authentication. See [fresh split pairing](automatic-transports.md#fresh-split-pairing).

PAIRING returns version 1, the slot count 2, then one byte per slot:

| Bit | Meaning |
| --- | --- |
| 0 | The slot holds a half's address |
| 1 | The Neuron holds a bond for that address |

The Neuron is paired with both halves when both slots have both bits set. Firmware without the readout answers PAIRING, sent without a session, with `-ENOTCONN`, and RESET PAIRING inside a session with `-ENOTSUP`, which closes the session; an installer reads PAIRING first and never sends RESET PAIRING to firmware that did not answer it.

RESET PAIRING clears the Neuron's side first: it removes the bond for each slot's address, empties both slots and deletes their stored addresses. Then it erases the eight 4 KiB sectors at `0x1C0000..0x1C8000` on each half through its bootloader, left first. Clearing the Neuron first means that a failure partway leaves the Neuron unpaired, so PAIRING reports it and a new session resets again. After CLOSE, each half starts without settings, makes a new Bluetooth identity and pairs with the Neuron over Bluetooth as a new device, with its cable in or out.

An installer resets the pairing whenever the Neuron is not paired with both halves: after a full Neuron write, on a first install, or after a pairing that failed. `defy-installer advanced halves write --reset-pairing` also resets it when both halves are paired.

## Versions

VERSION returns what the Neuron's firmware embeds; see [versions and the manifest](versions.md). Its payload is format 1, three zero bytes, then three strings, each a length byte followed by that many ASCII bytes without a terminator:

1. the module version, at most 32 bytes;
2. the source, `owner/repo` or `local`, at most 140 bytes;
3. the commit, at most 72 bytes, possibly empty.

HALF VERSIONS returns the Neuron's latest record from each half. Its payload is version 1, 80 bytes, little-endian, in the style of the battery snapshot: an 8-byte header with the version, the record count 2, the record size 36 as a 16-bit value and the Neuron's uptime in milliseconds as a 32-bit value. One record per side, left first, follows:

| Bytes | Field |
| --- | --- |
| 0 | Side: 0 left, 1 right |
| 1 | The link the record came by, as a bit of the battery snapshot's route mask: 1 cable, 2 Bluetooth; 0 with no record |
| 2..3 | Reserved, 0 |
| 4..7 | Milliseconds since it arrived; `0xFFFFFFFF` with no record |
| 8..35 | The half's 28-byte report, as [versions](versions.md#the-halves-report) gives it; zero with no record |

A side has no record while its half is off, asleep, held by a session, or runs firmware without the report. Neither request wakes or resets a half.

## Studio's keymap changes

ZMK Studio saves its keymap changes in the Neuron's settings, under `keymap/`, and they replace the bindings of the keymap built into the image. CLEAR KEYMAP CHANGES deletes every setting there: each saved binding, the layer names and the layer order. Then it calls ZMK's `zmk_keymap_reset_settings()`, the function behind Studio's "Restore Stock Settings", to reload the built-in keymap and layer order. Pairings, profiles and every other setting stay.

It does not leave the deletes to `zmk_keymap_reset_settings()`, because that function deletes only a binding whose first 10 bytes differ from the built-in one: the behaviour and the low 16 bits of its first parameter. A change only to a second parameter, such as a mod-tap's tap key, would survive it and return at the next restart. Studio shows the old layer names until the Neuron restarts, since ZMK does not reload them.

It fails with `-EBUSY` while a session is open or closing, or while Studio is unlocked, so it never runs beside a Studio change. A build with Studio but without its locking, `CONFIG_ZMK_STUDIO_LOCKING=n`, counts as always unlocked; the Neuron's builds lock. Each delete scans the stored settings, so the reply can take seconds when many changes are saved; an installer should allow 10 s. It fails with `-ENOTSUP` on a build without Studio's settings storage, and passes on the first failed delete's error. An installer sends it after it writes a new Neuron image with the settings kept, once ZMK has started; Studio is locked after a restart.

## Factory bootloader backend

`updater/defy_half_bootloader.c` talks to the halves' factory bootloaders in separate I2C transactions at 100 kHz, left at `0x5a` and right at `0x5b`. The cable I2C pins use H0D1 drive without pull resistors, as stock firmware drives them. The pinned nrfx driver reports both a NACK and a completion timeout as `-EIO`, and the build sets a 600 ms completion timeout. The updater thread owns the bus.

- **Entry.** OPEN drives both cable reset lines low for 10 ms, releases them as inputs and waits 50 ms. It opens both bootloaders before it replies.
- **Writing.** Each 4 KiB sector is erased before its first page, and 256-byte pages go in order. The CRC32 the factory bootloader returns for each page must match the page.
- **Validation.** FINISH needs the factory bootloader's success reply and a valid seal with matching program metadata.
- **Settings erase.** RESET PAIRING erases `0x1C0000..0x1C8000` with the same erase command, one 4 KiB sector at a time. No page can be written there. The half's INFO must report at least `0x1C3000` bytes of available flash from `0x5000`, to reach `0x1C8000`; otherwise the erase fails with `-ERANGE` before any transfer.
- **Release.** CLOSE and every abort send no `F`. They release each acquired half through its reset line, and the factory bootloader's own start checks then decide whether the application runs. A half whose reset failed is retried on its own, without pulsing the other half again. Before any half is acquired, abort does nothing.

The factory bootloader's INFO reply reports `programSpaceStart=0x5100`, while the package seal places the program at `0x6000`. INFO's guard checks `0x5100`, and seal validation checks `0x6000`. INFO also reports flash and validation start `0x5000`, available flash `0x1fb000`, erase alignment 4096, write alignment 256 and a maximum transfer of 1024.

## Split transport maintenance

`defy_hybrid_central_set_maintenance(true)` closes key input, pauses cable rearming and Bluetooth health recovery, then releases both halves' keys through the system work queue before it returns. It runs on the updater thread; calls from an interrupt or from the system work queue fail. Each wait is limited to 500 ms. A failed wait leaves maintenance on until a later retry succeeds.

Maintenance keeps Bluetooth connections, bonds and each transport's enable state. It holds back battery display and lighting sends, and new control-service work waits. An SPI transfer already armed can finish once, with its prepared outgoing bytes, and its received data is discarded. Active DMA buffers are not rewritten. Resuming clears queued input and waits for fresh snapshots from the halves before it publishes keys again.

## Interrupted updates and Neuron restarts

OPEN writes a marker to the Neuron's GPREGRET2 register after it pauses the split transports and before it resets the halves into their bootloaders. Session cleanup clears the marker once it has released both halves through their reset lines, after CLOSE, an error, a disconnect or the idle limit. Resuming the transports can still need retries after that, but no half is held by then.

GPREGRET2 keeps its value through a soft reset, a watchdog reset, a CPU lockup and a pin reset. A power-on or brownout reset clears it. Dygma's `n2_bootloader` does not use the register, nor do the `&defy_drive` key and the [UF2 loader](uf2-loader.md#registers), which use GPREGRET. ZMK's boot mode reaches GPREGRET through Zephyr's GPREGRET driver. A `BUILD_ASSERT` in `updater/defy_updater_usb.c` fails the build when that driver is on and the devicetree's `gpregret2` node is enabled, so the driver cannot hand GPREGRET2 to anything. It does not catch code that writes the register directly.

At boot the updater reads the marker. Without it, the cabled halves are left alone. With it, the updater waits for the update guard, pauses the split transports, holds both reset lines low for 10 ms, waits 50 ms, clears the marker and resumes. The first attempt runs before the updater reads any request. If it fails, it is retried with back-off while requests are served. Until it succeeds or gives up:

- OPEN returns `-EBUSY`, status -16. Retry after a few seconds.
- Other session commands, RESET PAIRING among them, return `-ENOTCONN`.
- HELLO, split diagnostics, battery status, dock status, the key trace, PAIRING, VERSION and HALF VERSIONS still answer.
- CLEAR KEYMAP CHANGES returns `-EBUSY`.
- Software entry to the Neuron bootloader is refused.

If the reset lines cannot be driven, the updater gives up after five failed releases in a row, about 1.5 to 2 seconds with the back-off. Typing then resumes for any half that is not held, the update guard is released so software bootloader entry works again, and the marker stays set so the next Neuron reset tries once more. Each new OPEN starts a fresh count of five.

A Neuron power cycle clears the marker. A half whose wireless switch is on keeps running from its battery through that power loss. If it was held in its factory bootloader at that moment, it stays there until it is switched off and on, or until the next OPEN resets it. How long the factory bootloader waits on battery has not been measured.

## Limits

An interrupted write can leave a half's application unusable until a complete retry through the same updater; the factory bootloader stays intact. This is recovery, not an atomic rollback. The updater writes only ZMK half images, so returning the halves to Dygma's firmware goes through Dygma's own Neuron firmware; see [back to stock](neuron-flash.md#back-to-stock). Power loss or cable removal during an erase or write has not been tested on hardware.
