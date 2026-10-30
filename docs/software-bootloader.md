# Neuron software bootloader entry

A running ZMK Neuron can enter Dygma's bootloader through its Studio serial port, so updates need no button and no replug. The `neuron-hybrid-log` and `neuron-hybrid-battery-log` builds have no Studio port; there, the USB log port takes the request. The half updater's port never does. `bootloader/defy_neuron_bootloader.c` implements it, and `CONFIG_DEFY_NEURON_BOOTLOADER` turns it on.

The [UF2 loader](uf2-loader.md)'s serial port takes the same trigger while the loader shows its drive, so an installer can go from the drive to Dygma's bootloader without the button. The loader shares the checks and the request write with `bootloader/defy_neuron_bootloader.c`.

## How it works

1. The installer switches the port to 1200 baud, lowers DTR and closes it, without sending or reading data. Opening it at 115200 baud first and then changing the rate does this.
2. 250 ms after the change to 1200 baud, the firmware checks that the port is still at 1200 baud with DTR low. A return to 115200 baud cancels the entry.
3. The firmware checks the partition layout and the CRC32 of Dygma's bootloader, `0x77000..0x7DF54`, against `0x080F9450`. It then writes the boot request at `0x7F000`, reads it back and resets. A flash failure does not reset. If the page already holds the request, it is not written again.
4. Dygma's bootloader finds the request, clears it and waits for commands.

ZMK ignores the trigger while its half updater holds the Neuron: during a half update, and while it releases the halves after the Neuron starts. Let that finish before retrying, unplugging cables or using the button. An interrupted half write needs a complete retry.

## The boot request

The request takes the first 16 bytes of the 4 KiB page at `0x7F000`: four little-endian 32-bit words, version 1, size 16, a CRC32 and the request, 1. The CRC32 is the IEEE one, over all 16 bytes with the CRC field zero. The whole record reads `01000000 10000000 5d5077f6 01000000`.

The request page is the only page this erases. ZMK's settings and pairings, the firmware, the UF2 loader, the MBR and the bootloader stay as they were. Dygma's firmware writes the same request but erases its own settings first; ZMK does not.

## The installer's side

The installer should follow the device's USB location through the change, refuse a missing location, a wrong identity and an ambiguous match, and identify the bootloader, USB `35ef:0013` and then its CRC, before any erase or write; see [writing ZMK](neuron-flash.md#writing-zmk). The bootloader has a different USB serial number from the firmware. Firmware without this entry ignores the trigger. A Neuron that runs Dygma's firmware is entered over Focus, or with the button; see [entering the bootloader](neuron-flash.md#entering-the-bootloader).

After `S#`, the installer closes the port and waits for the firmware to enumerate again. The acknowledgement alone does not show that the firmware started, so a write that finished and verified can still need a manual replug.
