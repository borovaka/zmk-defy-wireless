# Neuron flash and bootloader

The wireless Neuron is an nRF52833 with 512 KiB of flash. ZMK and this module's UF2 loader replace Dygma's application and the SoftDevice it ran on. Nordic's MBR and Dygma's `n2_bootloader` 1.0.0 stay, and an installer writes the loader and ZMK through that bootloader over USB. This page is the contract an installer has to keep; [defy-installer](https://github.com/borovaka/defy-installer) follows it. No step described here backs up the keyboard's settings or flash, so back up the layout with Bazecor before the first install.

## Flash layout

| Range | Content |
| --- | --- |
| `0x00000..0x01000` | Nordic's MBR, never written |
| `0x01000..0x0D000` | The UF2 loader, 48 KiB; see [UF2 loader](uf2-loader.md) |
| `0x0D000..0x6C000` | ZMK code, 380 KiB |
| `0x6C000..0x72000` | ZMK settings: pairings, host profiles, the lighting preference, look and brightness, Studio changes |
| `0x72000..0x77000` | Dygma's settings, never written |
| `0x77000..0x7DF54` | Dygma's `n2_bootloader` 1.0.0, never written |
| `0x7E000` | The MBR parameter page |
| `0x7F000` | The boot request page; see [software bootloader entry](software-bootloader.md) |

The bootloader starts whatever the vector table at `0x1000` names. It never calls a SoftDevice or checks for one. Dygma's firmware sits at `0x27000`, behind Nordic's SoftDevice S140 at `0x1000..0x27000`, whose reset handler forwards there. ZMK has its own Bluetooth stack and never used S140, so this port's image replaces S140. Its UF2 loader sits at `0x1000` and starts ZMK through the vector table at `0xD000`, unless asked to show its drive. S140 is needed only to go back to Dygma's firmware, and the installer then writes it back.

ZMK and the loader both build for the Neuron's board, `defy_neuron`, and take its partitions from one shared include. ZMK uses the board's `zmk` variant and places itself through `code_partition`. The loader, a Zephyr app in `apps/defy_loader/`, uses the plain target, `defy_neuron/nrf52833`, and places itself through `loader_partition`. `scripts/build` checks both parts and merges them into `neuron-<variant>.hex`; `neuron-<variant>.uf2` beside it holds ZMK alone, for the loader's drive. [The build](uf2-loader.md#the-build) lists the checks.

## Entering the bootloader

From a running ZMK Neuron, an installer enters the bootloader through the Studio serial port, or through the log port on the logging builds; see [software bootloader entry](software-bootloader.md). The UF2 loader's serial port takes the same request while it shows its drive; see [UF2 loader](uf2-loader.md#into-dygmas-bootloader).

From Dygma's firmware, an installer asks for the bootloader as Bazecor does, over Focus, the serial protocol Bazecor uses: `upgrade.start`, then `upgrade.isReady` until it answers `true`, then `upgrade.neuron`. Dygma's firmware allows that at once when the left half is not cabled or holds no valid program, and otherwise after a press of the left half's top-left Esc key. It then erases its own settings at `0x72000..0x77000`, writes the boot request and resets into the bootloader without replying. `defy-installer install`, `update` and `advanced neuron write` ask. The other `advanced neuron` commands need the button, so they never erase those settings. With the halves' cables out, `neuron write` has entered this way and written ZMK in 25 s; with the halves cabled, `install` has, after the Esc press.

The Neuron has two buttons, and the one farther from the USB socket enters the bootloader. Disconnect the halves' cables from the Neuron, unplug it, hold that button, plug the Neuron back in while holding it, and release after about 5 seconds. Halves switched to wireless keep the Neuron powered through their cables, so it would not restart. The bootloader shows up as USB `35ef:0013` with its own serial port, on macOS `/dev/cu.usbmodem0000000000001`, and with a USB serial number different from the firmware's. A plain replug starts the firmware again. Entered with the button, the bootloader acknowledges the start request after a write but stays in bootloader mode; unplug the Neuron and plug it back in without the button. The button has not been tried since this port's first install removed S140.

## Writing ZMK

Before any erase, an installer identifies the bootloader: USB `35ef:0013`, then the bootloader's own CRC command over its code, `0x77000..0x7DF54`, with `0x080F9450`, the CRC32 of Dygma's `n2_bootloader` 1.0.0. The CRC command takes an address, a length and the CRC32 the installer expects. The bootloader answers `ACK` when that range's CRC32 matches and `ERR` when it does not. The installer then:

1. erases `0x1000..0x72000` with one erase command that gives an address and a length. That covers 113 pages; allow up to 60 s for the reply;
2. uploads and writes 4 KiB pages, the loader's vector page at `0x1000` last;
3. checks every page with the bootloader's CRC command;
4. asks the bootloader to start the firmware, with `S#`, and waits for ZMK to enumerate again, as USB `1d50:615e`. The bootloader starts the loader, which starts ZMK at once. The acknowledgement alone does not show that the firmware started.

The UF2 loader changes none of these steps. The installer writes the loader and ZMK together, from one Intel HEX file, with the same erase, the same page checks and the same order as when it wrote ZMK alone at `0x1000`.

Every erase and write is checked before it is sent: whole pages within `0x1000..0x72000`. An erase always gives a length, because the bootloader's address-only erase runs to `0x77000` and would take Dygma's settings with it. The bootloader itself refuses writes below `0x1000` and from `0x77000` up, so the MBR and the bootloader stay out of reach either way.

A Neuron image is an Intel HEX file with the loader's vector table at `0x1000` and ZMK's at `0xD000`. Pages are written by address, so gaps in the file, such as the end of the loader's 48 KiB, stay erased. An image without a vector table at `0x1000`, such as Dygma's `Wireless_neuron.hex`, which needs S140, is refused; that one goes back through [the stock route](#back-to-stock).

A full write clears ZMK's keymap changes and Bluetooth pairings in `0x6C000..0x72000`. The halves keep their side of the split pairing until the installer's next write of the halves resets both sides; see [fresh split pairing](automatic-transports.md#fresh-split-pairing). Dygma's own settings at `0x72000..0x77000` stay as they were, and Dygma's firmware finds them again after a round trip, unless Dygma's firmware itself entered the bootloader, for Bazecor or for an installer, which erases them.

If a write is interrupted, the page at `0x1000`, the loader's vector page, is still erased. The bootloader then jumps through erased vectors at every start, so no firmware runs. The CPU should lock up and reset over and over, which drains a docked half faster; this has not been seen on the hardware, and a write cut short by a power loss has not been tried. Enter the bootloader with the button and write again. A copy to the loader's drive that stops partway is different: the loader stays, and shows its drive again at the next start.

### Keeping the settings

A ZMK-to-ZMK update with the same settings layout can keep the settings. It erases only `0x1000..0x6C000`, 107 pages, the loader and ZMK, and leaves the whole `0x6C000..0x72000` settings partition alone. An image that reaches into the settings is refused before any bootloader command. The page checks and the vector page going last are unchanged.

Pairings, host profiles, the lighting preference, look and brightness, and saved Studio changes survive. `defy-installer update` then clears the Studio changes; see [Studio's keymap changes](half-updater.md#studios-keymap-changes). A saved Studio change can override a binding in the newly compiled keymap. Use a full write for a first installation and after Dygma's firmware has run.

## Bazecor

Do not let Bazecor write the Neuron while it runs ZMK. Bazecor erases and writes only from `0x27000` and cannot put S140 back, so neither ZMK nor Dygma's firmware would start afterwards. Bazecor does not recognise the keyboard while it runs ZMK, but it could still offer an update to a Neuron in its bootloader. If it writes one, the button and a stock restore recover it.

## Checking S140

An installer can compare S140 on the Neuron with Dygma's `n2_s140_nrf52_7.2.0_softdevice.hex` without writing anything: it sends the bootloader's CRC command for each of S140's 38 pages, `0x1000..0x27000`, with the CRC32 of that page in the file, then starts the firmware again. The bootloader answers `ACK` for a page that matches and `ERR` for one that differs. With this port installed, those pages hold the loader and ZMK and do not match. A match on a stock Neuron shows that a stock restore would put back the bytes that are there.

## Back to stock

Dygma's 2.3.0 files are not part of this repository. `Wireless_neuron.hex` and `keyscanner.bin` are assets of Dygma's [Defy_v2.3.0 release](https://github.com/Dygmalab/Firmware-release/releases/tag/Defy_v2.3.0). [`n2_s140_nrf52_7.2.0_softdevice.hex`](https://github.com/Dygmalab/Firmware-release/blob/main/Defy/n2_s140_nrf52_7.2.0_softdevice.hex) is in the `Defy/` folder of the same repository. An installer checks each against its pinned SHA-256 before it opens any port:

| File | SHA-256 |
| --- | --- |
| `Wireless_neuron.hex` | `975f37521d543942f9a9225a31929369b07b69a4b92053071e22dbaac82d52d1` |
| `n2_s140_nrf52_7.2.0_softdevice.hex` | `4f442805983e4b1245817cdf64369e305cc78959dcb30461b2e9dec049aabb24` |
| `keyscanner.bin` | `02cfd5cd6a6291bd95e489f775f33e100a3deec91dbc54d24499dac1ae918c27` |

While the Neuron is still stock, Bazecor's firmware update returns the halves to stock. Once the Neuron runs ZMK, cable both halves to the Neuron, switch them off and go back in this order. `defy-installer uninstall` runs steps 1 and 2, and downloads the three files:

1. Write the Neuron. From the running ZMK Neuron, from the UF2 loader's drive or from the button bootloader, the installer erases `0x1000..0x72000`: the loader, ZMK and ZMK's settings. It writes Dygma's application from `0x27000`, then S140 from `0x2000` up, and S140's vector page at `0x1000` last, checking each of the 80 pages. It skips the MBR records at the start of the S140 file. Until that last page is written, an interrupted restore leaves erased vectors, and the button brings the bootloader back. Dygma's firmware then starts as USB `35ef:0012`.
2. Write the halves through Dygma's Neuron firmware. ZMK's half updater refuses Dygma's keyscanner, which has no metadata, so the halves go after the Neuron. Dygma's firmware has an updater of its own, over its Focus serial protocol, which Bazecor's firmware update uses. It resets both halves into their factory bootloaders, then writes and validates the keyscanner on each. It waits for the left half's top-left Esc key only when the left half's program answers as Dygma's would, so not while ZMK runs there. ZMK halves do not type through a stock Neuron.
3. Unplug the Neuron and plug it back in, which clears its updater mode.
4. Let the radios return to stock. Dygma's keyscanner, at its first start that reaches the radio bootloader, finds a version other than its own and reinstalls its radio application in the same slot. If wireless typing does not come back, unplug a half's cable and switch the half off and on: Dygma's keyscanner catches the radio bootloader only when both chips power up together.
5. Check the layout. Neither ZMK nor the installer's bootloader writes touch Dygma's settings at `0x72000..0x77000`, so Dygma's firmware finds them, the layout among them, as they were when it last ran, unless it erased them on its way into the bootloader. Then restore them from the Bazecor backup. Changes made under ZMK do not carry over.

Coming back from Dygma's firmware to ZMK is a first install again: back up the layout with Bazecor, then `defy-installer install` enters the bootloader, or the button enters it and keeps the settings.

`defy-installer uninstall` has returned the Neuron and both halves of one keyboard, with this port's radios installed, to Dygma's firmware, and `install` has put this port back. In the first such run, Dygma's settings had never been erased, and the layout came back.
