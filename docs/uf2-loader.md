# UF2 loader

The Neuron holds a loader of this module's own in the 48 KiB in front of ZMK. On request it shows the computer a USB drive, and a ZMK image copied onto that drive as a UF2 file replaces ZMK on the Neuron, as on most ZMK keyboards. A change to the keymap or the Neuron's settings then needs no installer. The loader is a Zephyr app in `apps/defy_loader/`, beside the radio's `apps/defy_radio/`. This page is the contract it keeps.

## Where it sits

| Range | Content |
| --- | --- |
| `0x01000..0x0D000` | The loader, 48 KiB |
| `0x0D000..0x6C000` | ZMK |

Dygma's `n2_bootloader` starts whatever the vector table at `0x1000` names, now the loader's; the rest of the layout is in [Neuron flash and bootloader](neuron-flash.md#flash-layout).

The loader builds for `defy_neuron/nrf52833`, the plain target of the Neuron's board in `boards/dygma/defy_neuron/`. ZMK builds the same board's `zmk` variant, whose devicetree includes the plain one. The partitions sit in one include, `defy_neuron-partitions.dtsi`, so the two cannot place ZMK differently. The plain target links an app into `code_partition`; the loader's `app.overlay` picks `loader_partition` instead, as MCUboot's overlays pick `boot_partition`. Build-time checks in `src/main.c` tie the loader's link address, `src/layout.h` and the devicetree together.

Everything that touches clocks or UICR is in the plain target, so the loader and ZMK share it, and so is the status LED. The 32 kHz clock runs from the calibrated RC oscillator at 500 ppm, because the Neuron has no 32 kHz crystal and a crystal setting could hang at the clock's start. The NFC pins are GPIOs, P0.18 is not set up as a reset pin, and APPROTECT follows UICR. The loader drives the [status LED](#the-led) through PWM and no other pin, and uses no SPI, UART or Bluetooth.

The loader's own code never writes UICR. When it starts ZMK it jumps before `SystemInit()` runs, so that path reaches no UICR code at all. Before drive mode it runs `SystemInit()` from Nordic's nrfx, as ZMK does at each of its starts. That function clears the NFC protection in UICR's `NFCPINS` and resets, once, while the register still selects NFC. On a Neuron that has run ZMK or Dygma's firmware the register is already clear, so nothing is written.

## Start

Every Neuron start passes through the loader. It decides in Zephyr's SoC reset hook, `src/start.c`, the first code after the reset vector, before `SystemInit()` and before any clock, timer or USB starts, so ZMK starts from the state `n2_bootloader` left. The hook reads GPREGRET and ZMK's vector table at `0xD000`, uses no RAM but its stack, and writes GPREGRET's new value:

1. If GPREGRET holds `0x57`, the drive request, the loader clears it and shows the drive.
2. Otherwise, if ZMK's vector table is not valid, it clears GPREGRET and shows the drive. Valid means a word-aligned initial stack pointer in RAM and a Thumb reset vector inside `0xD000..0x6C000`.
3. Otherwise, if GPREGRET records three unconfirmed starts of ZMK, it leaves that value and shows the drive.
4. Otherwise it adds one to the count of unconfirmed starts, points VTOR at ZMK's vector table, takes the main stack pointer from it and jumps to ZMK's reset vector.

Nordic's SoC support binds the hook to `SystemInit()` with `PROVIDE()` in its linker script, `soc/nordic/common/platform_init.ld`, and `start.c`'s definition replaces it. The loader's `CMakeLists.txt` pins the SHA-256 of that script and of Zephyr's Cortex-M `reset.S`, which calls the hook; another version of either stops the build until `start.c` is reviewed against it. `start.c` refuses to build without `CONFIG_SOC_RESET_HOOK`, or with `CONFIG_CMAKE_LINKER_GENERATOR`, under which Nordic's binding would win.

## The start count

A ZMK image that is valid but cannot run, such as one whose keymap crashes it at start, would otherwise start at every reset and keep the drive out of reach. The loader counts ZMK's starts in GPREGRET, and ZMK confirms the ones that work:

- Each start of ZMK adds one, as `0xC1`, `0xC2` and `0xC3`.
- ZMK on the Neuron clears the count 10 s after its application init, `bootloader/defy_loader_confirm.c` with `CONFIG_DEFY_LOADER_CONFIRM`. It also clears it before its 1200-baud entry into `n2_bootloader`, since a restart on request is not a failure. It leaves any other value alone.
- ZMK on the Neuron restarts after a fatal error, `bootloader/defy_neuron_fatal.c` with `CONFIG_DEFY_NEURON_FATAL_RESET`, where Zephyr's default handler would halt it.
- At `0xC3`, three starts in a row without a confirmation, the loader shows the drive instead of a fourth start.

Ten seconds covers ZMK's own start, the halves' reconnection, whose Bluetooth attempts the split recovery cancels after 5 s, and the first key presses after a restart. A crash anywhere in that time counts. A longer wait costs little, because every restart that ZMK or the loader makes on request clears or replaces the count. A crash after 10 s restarts ZMK and is not counted.

Once the drive shows for the count, GPREGRET keeps `0xC3` until a power-on or brownout reset, a complete copy, a fatal error in the loader or a 1200-baud touch on its port. A Neuron on USB alone loses power when unplugged, so a replug gives ZMK three more starts. Cabled halves keep the Neuron powered: unplugging USB then puts it in System OFF with the count kept, and plugging it in shows the drive again.

## Entering the drive

- **The `&defy_drive` key.** The Neuron board's `zmk` variant includes ZMK's `nrf52833_uf2_boot_mode.dtsi` and implies `CONFIG_RETENTION_BOOT_MODE`. `&defy_drive`, `bootloader/behavior_defy_drive.c`, runs on the Neuron. It sets ZMK's bootloader boot mode, which writes `0x57` into GPREGRET, and restarts the Neuron. `0x57` is the value Adafruit's UF2 bootloader uses for the same request. `include/dygma/defy_loader.h` holds it as `DEFY_LOADER_DRIVE_MAGIC`, and `behavior_defy_drive.c` fails to build unless ZMK's `CONFIG_ZMK_BOOTMODE_BOOTLOADER_MAGIC_VALUE` equals it and ZMK's magic goes into GPREGRET. The key does nothing while a half update holds the Neuron. ZMK's own `&bootloader` would not do: it runs on the side whose key was pressed, and every Defy key is on a half, so it restarts that half instead. In this module's keymap, `config/defy_neuron.keymap`, the key is on the FKeys layer, at the right half's top outer key, Backspace on Base. FKeys is held on the left upper thumb key second from the outside. The halves can be on the cable or on Bluetooth, but the Neuron must be plugged into the computer for the drive to show.
- **No valid ZMK.** A start without a valid vector table at `0xD000`, as after an interrupted copy, shows the drive with no key and no button.
- **A ZMK that keeps failing.** Three unconfirmed starts in a row; see [the start count](#the-start-count).

The installer does not need the drive, but it can start from it. It enters Dygma's bootloader from ZMK as before, or from the loader's serial port while the drive shows; see [entering the bootloader](neuron-flash.md#entering-the-bootloader). From the drive, `install` and `update` first enter the bootloader and start the firmware without writing. The loader then starts ZMK if it can, and they update that ZMK as usual. If the drive comes back instead, they write the Neuron as `update` does, with its settings kept, and then the halves. `uninstall` writes Dygma's firmware from the drive as it does from ZMK.

## The drive

The loader is a composite USB device on Zephyr's USB device stack, `USB_DEVICE_STACK_NEXT`, with its mass-storage and CDC ACM classes: a drive and one serial port. ZMK runs Zephyr's older stack, `USB_DEVICE_STACK`. On macOS that stack's mass-storage class kept its bulk-in endpoint stalled and no disk appeared, so the loader uses the newer one. The newer class answers START STOP UNIT, MODE SENSE(6) and (10), PREVENT ALLOW MEDIUM REMOVAL and READ FORMAT CAPACITIES. It answers commands it does not implement, such as SYNCHRONIZE CACHE, with CHECK CONDITION and ILLEGAL REQUEST.

macOS, Windows and Linux mount the drive and its serial port with their own drivers. The drive is an emulated FAT16 volume of 4 MiB labelled `DEFY`. It holds `INDEX.HTM`, which links to this module's README, and `INFO_UF2.TXT`:

```
UF2 Bootloader 1.1 zmk-defy-wireless
Model: Dygma Neuron (wireless)
Board-ID: nRF52833-DefyNeuron-v1
```

The SCSI INQUIRY data carries the same version, `1.1`. The `zmk` variant's `board.cmake` sets up Zephyr's `uf2` runner with that Board-ID, so `west flash` on a ZMK build copies `zmk.uf2` onto a drive that shows it.

Before it enables USB, the loader checks its disk through Zephyr's disk access by name, as the mass-storage class will find it: the disk must be registered and ready, with 8192 sectors of 512 bytes, and sector 0 must end in `55 AA`. If any check fails, it leaves drive mode as below instead of showing a drive that cannot work.

## Which blocks it takes

The loader takes only UF2 blocks with:

- the UF2 magic numbers, `0x0A324655` and `0x9E5D5157` at the start and `0x0AB16F30` at the end;
- the family-ID flag, `0x2000`, and family `0x621E937A`, Zephyr's for the nRF52833;
- neither the not-main-flash flag, `0x1`, nor the file-container flag, `0x1000`;
- a payload of 256 bytes;
- a block count from 1 to 1520, the blocks in ZMK's 380 KiB, and a block number below it;
- the address `0xD000` plus 256 times the block number.

The last rule is the layout of Zephyr's UF2 output and of `neuron-<variant>.uf2`. It places every block inside ZMK's part, and a block for the MBR, the loader itself, the settings or Dygma's areas is never written. It also means a file built for another place is refused block by block, before anything is erased. The loader ignores every other sector the host writes, such as the FAT, directories and the metadata files macOS adds.

A foreign UF2 file therefore changes nothing. The host reports the copy as done, and the Neuron keeps showing the drive with ZMK as it was. Such files include the halves' `<target>.uf2` files, which are for the RP2040's ROM bootloader and carry another family, apps for Adafruit's nRF52 bootloader, which start after its SoftDevice, and any file with gaps between its blocks' addresses. The file to copy is `neuron-<variant>.uf2`, ZMK alone; see [the build](#the-build).

## Writing a file

The loader knows a file by its block count. It erases each 4 KiB page before its first write in that file. When the file starts, it erases ZMK's first page, `0xD000..0xE000`, and keeps the 16 blocks for that page in RAM. It writes the other blocks as they arrive and reads each back. Once every block has arrived, counted by block number up to the file's block count, it writes the first page:

1. bytes 8 to 4095, then reads them back;
2. the vector table, bytes 0 to 7, then reads it back.

If a write or a read-back fails, it erases the first page again. A complete file then waits 500 ms, for the host to finish its own writes, and the loader clears GPREGRET and resets into ZMK with a fresh start count.

Before each erase of ZMK's first page, the loader writes zeros over the vector table at `0xD000..0xD007`, unless it reads as erased or as zeros already. Power lost during the erase can leave the page's words partly erased. The old vector table would still pass the start check; zeros fail it. The nRF52833 allows two writes to a word between erases, the vector table's own and this one.

The file starts over, with its blocks forgotten and ZMK's first page erased, on:

- a block with a different block count;
- a block already stored whose data differs from what is stored, compared with the copy in RAM or read back from flash; the same data is ignored;
- block 0 of a file a flash error abandoned, which a copy of the same file sends again. The abandoned file's other blocks are ignored.

ZMK's first page goes last, so an interrupted copy leaves ZMK without a valid vector table. The next start shows the drive again, and the same file can be copied again.

Suppose a copy stops partway and the host copies a different file of the same size without a replug. At the first block where the new file's data differs from a stored block, the loader starts the file over, so that copy does not complete. The next copy completes the new file. One block order defeats this: every block the first file lacked, sent before any block that differs. A replug clears the stored blocks and rules that out.

The pages from `0x6C000` up are never touched. ZMK's settings stay: pairings, host profiles, the lighting preference, look and brightness, and the keymap changes saved in ZMK Studio. A saved Studio change still overrides the new keymap at the key it changed. `defy-installer update` clears those changes after it writes the Neuron; a copy to the drive does not.

## Leaving the drive

ZMK can start when its vector table is valid and the loader did not stop it for its start count.

- **A complete file.** The loader resets into ZMK by itself, 500 ms after the last block. The host may report that the drive was not ejected properly, as it does with other UF2 loaders.
- **Unplugging.** VBUS goes away. If ZMK can start, the loader resets into it. Otherwise it enters System OFF, from which plugging USB in wakes the Neuron into the drive again. VBUS that is still there, or already back, would never wake it, so the loader checks VBUS once more just before System OFF and resets into the drive instead if it is present.
- **Five minutes idle.** On a drive shown on request, with a ZMK that can start, the loader resets into ZMK five minutes after its start unless a UF2 block for ZMK has arrived. That covers a stray press of the key, a Neuron on a charger with no host, and a drive the host ejected.
- **A failed setup.** If the loader cannot set up the drive or USB, a reset would only fail the same way. While ZMK cannot start, the LED shows red until the Neuron is unplugged, which powers it down to System OFF; the next plug-in tries again. With ZMK able to start, the loader resets into it.
- **A fatal error.** The loader clears GPREGRET and resets, which starts ZMK if it is valid, where Zephyr's default handler would halt it.
- **Ejecting the drive** only takes it off the host. On a drive shown on request, the idle timeout then returns to ZMK.

## The LED

While the drive shows, the Neuron's status LED says what a reset would start:

- **Amber**, red and green together: ZMK can start, as on a drive shown by the key. Unplugging, or five minutes without a copy, goes back to it.
- **Red**: ZMK cannot start, because its vector table is not valid or it failed three starts in a row. The drive waits for a file.
- **Green, blinking**, 100 ms on and 100 ms off: UF2 blocks for ZMK are being written. The blink stops 1 s after the last block. A complete file resets into ZMK 500 ms after its last block, so the blink lasts until the reset.

A copy erases ZMK's first page when its first block arrives, so the LED turns red when a copy stops partway. A foreign UF2 file writes nothing and leaves the colour as it was. The LED lights once the loader sees VBUS, before it sets up the drive. It is dark when the loader starts ZMK straight away, and in System OFF.

The brightness is 3%, the default of ZMK's `CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS`. One loader build serves every keymap, so it does not see the keymap's settings. `src/led.c` sets up PWM0 through nrfx's register functions, as Zephyr's PWM driver would take 3 KB of the loader's 48 KiB; the LED takes 1.3 KB. It reads the pins, channels and period from the `led_green` and `led_red` nodes of the plain board's devicetree, which ZMK drives through Zephyr's driver.

## Into Dygma's bootloader

The loader's serial port takes the same 1200-baud trigger as ZMK's Studio port; see [software bootloader entry](software-bootloader.md). The host sets the port to 1200 baud with DTR low. 250 ms later the loader checks that both still hold, checks the CRC32 of Dygma's bootloader, writes the boot request at `0x7F000`, clears GPREGRET and resets into `n2_bootloader`. The checks and the request write are shared with `bootloader/defy_neuron_bootloader.c`. An installer that finds the drive's USB IDs can go on from there as for a Neuron found in its bootloader, without the button. With GPREGRET clear, the ZMK that `n2_bootloader` starts next gets a fresh start count.

## Registers

GPREGRET holds 8 bits for the loader and ZMK, as `include/dygma/defy_loader.h` defines them:

| Value | Meaning |
| --- | --- |
| `0x00` | Nothing: after a power-on reset, a confirmation, or a reset by the loader |
| `0x57` | `&defy_drive`'s request for the drive |
| `0xC1`, `0xC2`, `0xC3` | One, two or three starts of ZMK without a confirmation |

Any other value counts as `0x00`. Dygma's `n2_bootloader`, which runs between the reset and the loader, leaves the register alone.

GPREGRET2 belongs to the half updater's "halves held" marker; see [interrupted updates](half-updater.md#interrupted-updates-and-neuron-restarts). The loader never reads or writes it, and the plain board gives Zephyr's GPREGRET driver no device for it.

Both registers keep their values through a soft reset, a watchdog reset, a CPU lockup, a pin reset and a wake from System OFF. A power-on or brownout reset clears them.

## USB IDs

The drive enumerates as `2fe3:0100`, Zephyr's vendor and product IDs for testing, with manufacturer `zmk-defy-wireless` and product `Defy UF2 loader`, all set in the loader's Kconfig. Every Zephyr sample uses the same IDs, so an installer tells the loader apart by its product string. The IDs are placeholders until a product ID of its own is assigned. Windows caches drivers by vendor and product ID, so the drive does not reuse ZMK's `1d50:615e`. Its USB serial number is the one ZMK reports, from Zephyr's hwinfo, so an installer can follow the Neuron between ZMK and the drive.

## The build

The loader's `CMakeLists.txt` adds this module as a board root, so a plain build needs no other setting:

```sh
west build -s apps/defy_loader -b defy_neuron/nrf52833
```

Its image is `zephyr/zephyr.hex`, linked at `0x1000`. `scripts/build` builds the loader once in each batch with a Neuron target, before the first Neuron. The loader has no keymap, so one build serves every Neuron variant. The script refuses a batch unless:

- the loader's configuration has `CONFIG_FLASH_LOAD_OFFSET` `0x1000`, `CONFIG_FLASH_LOAD_SIZE` `0xC000` and the RC oscillator for the 32 kHz clock, and each ZMK build's has `0xD000` and `0x5F000`;
- each part's image has a vector table at the part's start, with an initial stack pointer in RAM and a Thumb reset vector inside the part, and no data outside it: the loader within `0x1000..0xD000`, ZMK within `0xD000..0x6C000`;
- no address is in both parts;
- each Neuron's `zmk.uf2`, which Zephyr's `CONFIG_BUILD_OUTPUT_UF2` makes from ZMK alone, is made of 256-byte blocks for family `0x621E937A`, each at `0xD000` plus 256 times its number, which together hold every byte of `zmk.hex`.

It merges the loader's `zephyr.hex` and ZMK's `zmk.hex` into `neuron-<variant>.hex` for the installer, and copies `zmk.uf2` to `neuron-<variant>.uf2` beside it. It prints the loader's size and the bytes left before `0xD000`, and for each Neuron the bytes ZMK leaves before the settings at `0x6C000`. `manifest.json` lists only the `.hex`; see [versions and the manifest](versions.md#manifestjson).

## Tests

`scripts/test-host` runs three host suites on the loader's code without Zephyr:

- `tests/host/defy_loader_start`: the vector-table check, the start decision and the start count through runs of starts, failures and confirmations;
- `tests/host/defy_loader_uf2`: the block checks for every address, files in order, reversed and shuffled, repeated blocks, another file of the same size, restarts, flash failures at each step of the first page, and the zeros before its erase, on a fake flash that allows two writes per word between erases;
- `tests/host/defy_loader_fat`: the volume as a host mounts it, and `INFO_UF2.TXT`.

`scripts/test drivers --driver defy_loader_led` runs `src/led.c` on native_sim against fake PWM and GPIO registers: the set-up from the devicetree, the colours, the blink and its end 1 s after the last block, and the LED kept off before System OFF.

`tests/scripts/test_loader_layout.py` compares the partitions with the constants of `scripts/neuron_image.py`, and the loader's Board-ID with the board's `board.cmake`. `main.c` checks at build time that `src/layout.h` agrees with the partitions.

## What it does not cover

The drive writes ZMK on the Neuron and nothing else. The halves sit behind the Neuron, so a first install, a return to stock and any change of module version go through the installer. A module change can change the halves and the split protocol between them and the Neuron. The drive covers changes to the keymap and its `config/defy_neuron.conf`, which touch only the Neuron. A change to its `config/defy.conf`, the halves' settings, reaches the halves only through the installer's `update`.

## Limits

Not tried on hardware: a Neuron on a charger with no computer, a copy interrupted while the halves are cabled, and the five-minute idle timeout.
