# Radio install

Each half's nRF52820 runs this port's Bluetooth controller, `apps/defy_radio`, in Dygma's application slot. The half installs it itself, through Dygma's radio bootloader, `rf_bootloader`, the way Dygma's keyscanner installs its own radio application. No installer command talks to the radio.

## Flash layout

| Range | Content |
| --- | --- |
| `0x00000..0x01000` | Nordic's MBR, never written |
| `0x01000..0x1C000` | Nordic's SoftDevice S113, never written. It only passes reset on to `0x1C000`, and the controller leaves it disabled |
| `0x1C000..0x39000` | The application slot: this port's controller in place of Dygma's radio application. The slot's last word stays free, because the bootloader programs one word past every write |
| `0x39000..0x3D000` | Dygma's `rf_bootloader`, never written |
| `0x3D000..0x40000` | The bootloader's state, with the seal at `0x3D000` |

`scripts/build radio` builds the controller as `radio.bin` and its 28-byte seal, `radio.seal`, in the layout Dygma's keyscanner sends for its own application; `radio.c` embeds both in the radio-installing halves. The bootloader validates the image against the seal and stores the seal itself. The seal's version comes from the image's CRC32, so every change to the controller installs, and it never equals the stock application's version.

## When a half installs it

Routine `left-hybrid` and `right-hybrid` builds keep the radio firmware already installed. To change it, build `left-hybrid-radio right-hybrid-radio` and write those images through the [half updater](half-updater.md). The half updater writes only the RP2040s and reports nothing about the radios.

When a radio-installing half starts, its Bluetooth driver catches Dygma's radio bootloader before the rest of ZMK starts. A radio that already runs this port's controller is rebooted into its bootloader first. The driver installs the embedded image if the version or seal differs, and leaves without writing when they match. It accepts the controller only after a successful version check. Allow about half a minute for an install before checking typing; that is an estimate, not a measured limit. The [start-up colours](lighting.md#start-up-and-fault-colours) show the install in yellow and orange.

A radio that still runs Dygma's application cannot be switched to its bootloader over SPI. Its first install can need both chips to power up together: disconnect the half's cable, switch the half off for five seconds and switch it on again. The installer asks for that only when a half has not connected two minutes after its write. Dygma's radio bootloader listens for about 300 ms after power-on.

## Failures

The driver attempts the install at most once per start. After a failure it reports the error on the LEDs and does not erase or write the radio again until the half restarts; see [radio failure report](lighting.md#radio-failure-report). The driver looks at the radio again at every start of the half, not only after a power cycle. The radio's MBR, S113 and bootloader are never written. The vectors at `0x1C000` go last, so an interrupted install leaves them erased; S113 then waits, and the next power-on installs again. A power cut during a radio install has been tested only against a fake bootloader, not on hardware.

Dygma's keyscanner, put back on a half, finds a version other than its own in the seal and reinstalls its radio application in the same slot; see [back to stock](neuron-flash.md#back-to-stock).
