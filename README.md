# zmk-defy-wireless

ZMK firmware for the Dygma Defy wireless. It runs on both halves, each an RP2040 with an nRF52820 radio, and on the wireless Neuron, an nRF52833 that is the split central. This repository is a ZMK module with the board definitions, drivers, build snippets, the radio firmware, build and test scripts, and the specifications an installer works to.

To install it, follow the [quick start](#quick-start). You need no build tools.

## Status

This is an experimental community project, built and tested on one keyboard. A full install replaces Dygma's firmware on the Neuron, both halves and both radios. A failed or interrupted Neuron write leaves the Neuron without working firmware. While the Neuron is still in its bootloader, write it again. If it lost power or was unplugged during the write, no firmware runs when it starts again. Enter its bootloader with the button and write it again. Some update and recovery paths have not been tried on hardware; the [documentation](#documentation) says which, and [hardware assumptions and open checks](#hardware-assumptions-and-open-checks) lists what the boards are assumed to be. You use it at your own risk, without warranty; see the [licence](#licence).

Only the Defy wireless is supported: its two halves and the wireless Neuron, assumed to be Neuron 2 hardware V1.1.

This project is not affiliated with or endorsed by Dygma. Dygma, Defy and Bazecor are Dygma's names, used here only to say which hardware and software this firmware works with.

## Quick start

1. Back up your layout in Bazecor, then quit Bazecor. The first install erases Dygma's settings on the Neuron.
2. Get [defy-installer](https://github.com/borovaka/defy-installer). On macOS or Linux, in a terminal:

   ```sh
   curl -fsSL https://github.com/borovaka/defy-installer/releases/latest/download/install.sh | sh
   ```

   On Windows, in PowerShell:

   ```powershell
   irm https://github.com/borovaka/defy-installer/releases/latest/download/install.ps1 | iex
   ```

   Either puts the installer in a `defy-installer` folder in your home folder, after checking it against the release's checksums. Its README says how to download it by hand instead.
3. Plug the Neuron into the computer, cable both halves to it and switch both halves off.
4. Run the installer and do what it says:

   ```sh
   cd ~/defy-installer
   ./defy-installer install
   ```

   On Windows the program is `.\defy-installer.exe`. On Linux, if it says Linux does not let you open the keyboard's port, follow its advice.
5. Pair your computers over Bluetooth again. ZMK keeps pairings of its own.

Later releases install with `./defy-installer update`. To change the keymap, create your own repository from the [Defy-Wireless-ZMK](https://github.com/borovaka/Defy-Wireless-ZMK) template; its README starts with a quick start of its own. [Installing](#installing) has the details of each step.

## Installing

[defy-installer](https://github.com/borovaka/defy-installer) installs this port and puts Dygma's firmware back: one program for macOS, Linux or Windows. It downloads a [release](https://github.com/borovaka/zmk-defy-wireless/releases) of this module, or your keymap repository's build (see [Defy-Wireless-ZMK](https://github.com/borovaka/Defy-Wireless-ZMK)), checks it against its `manifest.json`, and tells you, step by step, when to plug or unplug a cable and when to switch a half on or off. Its README covers downloading it and its first run on each system. What the installer does, and what the firmware expects of it, is specified in [half updater](docs/half-updater.md), [versions and the manifest](docs/versions.md), [Neuron flash and bootloader](docs/neuron-flash.md), [software bootloader entry](docs/software-bootloader.md), [UF2 loader](docs/uf2-loader.md) and [radio install](docs/radio-install.md).

Quit Bazecor first, which holds the Neuron's serial port while it runs. Never let Bazecor write the Neuron while it runs ZMK. Bazecor writes only from `0x27000` and cannot put back S140, the Bluetooth stack that Dygma's firmware runs on, so neither ZMK nor Dygma's firmware would start afterwards.

### First install, from Dygma's firmware

Back up the layout with Bazecor: the first install erases Dygma's settings on the Neuron. A release's `neuron-hybrid.hex` carries Dygma's default layout, as Bazecor sets it up, with this port's controls added; see [keymap](#keymap). Changes you made in Bazecor do not carry over, so typing can differ from your Bazecor layout until you build your own keymap from the [Defy-Wireless-ZMK](https://github.com/borovaka/Defy-Wireless-ZMK) template.

```sh
defy-installer install                                   # this module's latest release
defy-installer install --from yourname/your-repo         # your keymap repository's build
```

The installer then:

1. Asks for both halves cabled to the Neuron and switched off, with the Neuron plugged into the computer.
2. Asks Dygma's firmware for its bootloader as Bazecor does, which erases Dygma's settings on the Neuron. With the left half cabled, Dygma's firmware waits for one press of its top-left Esc key, as for Bazecor's update; the installer says when. It writes and checks every page of the Neuron and waits for ZMK to start.
3. Writes both halves through the Neuron. A newly written Neuron holds no split pairing, so it first erases both halves' ZMK settings at `0x1C0000..0x1C8000`; see [fresh split pairing](docs/automatic-transports.md#fresh-split-pairing).
4. Waits, with the cables in, until both halves have paired with the Neuron over Bluetooth. Each half first installs the radio firmware and shows it in yellow and orange, then green, then cyan for two seconds once it reaches the Neuron; see [start-up colours](docs/lighting.md#start-up-and-fault-colours). A radio that still runs Dygma's application can install only when both chips power up together, so if a half has not connected after two minutes, the installer asks you to unplug its cable, switch it off, wait five seconds and switch it on. Then type with the cables in, or unplug them and switch the halves on.

ZMK keeps its own Bluetooth pairings, so pair your computers again.

To keep Dygma's settings on the Neuron, enter its bootloader with the button before you run `install`. Of the Neuron's two buttons, use the one farther from the USB socket. Take the halves' cables out, unplug the Neuron, hold the button, plug the Neuron back in and release the button after about 5 seconds. A Neuron that entered its bootloader this way stays in it after the write. Unplug it, plug it back in without the button, and run `install` again for the halves.

### Updating

```sh
defy-installer update
```

The Neuron and each half report what they run, over Bluetooth or the cable, so `update` writes only what changed. After a keymap change that is the Neuron alone, with no cables to move. When the halves need new images, it asks for their cables in and their switches off, and writes them first; typing pauses meanwhile, and the Neuron's LED blinks amber. The `-hybrid-radio` images carry the radio firmware, and a half installs it as it starts only when it differs from the radio's own. Your Bluetooth pairings stay. Each Neuron write clears the keymap changes made in ZMK Studio, so that the installed keymap takes effect.

### Updating by drag and drop

A keymap change touches only the Neuron, and the Neuron can take it without the installer. Its [UF2 loader](docs/uf2-loader.md) shows a USB drive, as on most ZMK keyboards:

1. Plug the Neuron into the computer. Hold FKeys, the left upper thumb key second from the outside, and press Backspace, the right half's top outer key. The Neuron restarts as a drive named `DEFY`, and its LED turns amber.
2. Copy `neuron-hybrid.uf2` from your keymap repository's build onto the drive. The Neuron writes it, blinking its LED green, and starts ZMK again by itself. The computer may complain that the drive was not ejected properly; that is expected.

To leave the drive without copying anything, unplug the Neuron. If a copy stops partway, or the copied ZMK keeps crashing, the Neuron shows the drive again with its LED red; copy a working file. A copy keeps the Neuron's settings, pairings and keymap changes saved in ZMK Studio, which override the new keymap at the keys they changed. [UF2 loader](docs/uf2-loader.md) has the details.

Drag and drop covers keymap changes only. A first install, a return to stock and a new module version still need the installer, and so do the halves. They sit behind the Neuron, and a Neuron image from one module version can fail to work with halves from another. The keymap's `config/defy.conf` holds the halves' settings, so a change to it reaches them only through `defy-installer update`.

### Going back to stock

```sh
defy-installer uninstall
```

With both halves cabled to the Neuron and switched off, `uninstall` downloads Dygma's 2.3.0 firmware, checks it against pinned SHA-256 sums and writes the Neuron, then both halves. Then replug the Neuron, switch each half on with its cable out, and restore your layout from the Bazecor backup. [Back to stock](docs/neuron-flash.md#back-to-stock) has the files, hashes and steps.

## Keymap

The Neuron holds the keymap, [config/defy_neuron.keymap](config/defy_neuron.keymap). ZMK finds it by the board's name. Its Base, Navigation and FKeys layers follow Dygma's default layout, as Bazecor sets it up, with the Defy controls on keys that layout leaves empty. The Mouse layer is new. Comments marked DEMO in the keymap point to ZMK behaviours to try and change. To keep your own keymap, copy the [Defy-Wireless-ZMK](https://github.com/borovaka/Defy-Wireless-ZMK) template, which holds the keymap and settings and pins this module by tag. Its `config/defy_neuron.conf`, for the Neuron, and `config/defy.conf`, for the halves, list the settings a keymap is most likely to change, with the values the firmware uses. All but mouse keys are commented out.

| Layer | How to reach it from Base |
|---|---|
| Base | Default |
| Navigation | Tap the right upper inner thumb key for the next key, or hold it |
| FKeys | Hold the left upper thumb key second from the outside |
| Mouse | Hold the left lower thumb key second from the outside |

The Navigation key is ZMK's sticky layer, `&sl`. After a tap, Navigation stays on for the next key pressed within one second. Layers 4 to 9 are reserved, for ten Studio layers in all.

Base's thumb keys, from the outside in:

| Thumb keys | Outer | Second | Third | Inner |
|---|---|---|---|---|
| Left upper | Copy key | FKeys | Space | Ctrl when held, Backspace when tapped |
| Left lower | Free | Mouse | Win | Alt when held, Enter when tapped |
| Right upper | Lighting on and off | Right Alt | Space | Navigation |
| Right lower | Profile chooser | Battery display | Menu | Ctrl when held, Enter when tapped |

The copy key gives Ctrl+C on a tap, Ctrl+V on a hold, Ctrl+X on two taps and Ctrl+Shift+V on a tap and then a hold. The Ctrl and Alt thumb keys count as held after 250 ms, or once another key is pressed and released while they are down. The left lower outer thumb key is unbound, for a binding of your own in ZMK Studio.

The Defy controls, from [dts/behaviors/defy.dtsi](dts/behaviors/defy.dtsi) and ZMK, are on Base and on the FKeys layer:

| Keys | Action |
|---|---|
| Right upper outer thumb key | Toggle both halves' key lighting and underglow |
| FKeys+;, FKeys+' | Make the lighting dimmer, or brighter |
| The key right of G, or FKeys+/ | Show the next lighting look |
| Right lower outer thumb key | Open the Bluetooth profile chooser |
| Right lower thumb key second from the outside | Show each half's battery state for five seconds |
| FKeys+Q, FKeys+W | Prefer USB, or Bluetooth, output to the computer |
| FKeys+O | Unlock ZMK Studio. Logging builds leave FKeys+O transparent |
| FKeys+Backspace, the right half's top outer key | Restart the Neuron as the [UF2 loader](docs/uf2-loader.md)'s drive |

The preferred output gets the keys when both are ready, and the other one is the fallback. FKeys+Q and FKeys+W do not choose the link between a half and the Neuron. The controls are described in [lighting and controls](docs/lighting.md), [battery](docs/battery.md#battery-display) and [output to the computer](docs/automatic-transports.md#output-to-the-computer). The lighting's looks, brightness, layer colours and key colours are set in the keymap; see [normal lighting](docs/lighting.md#normal-lighting).

### Mouse keys

The keymap uses ZMK's [mouse keys](https://zmk.dev/docs/keymaps/behaviors/mouse-emulation): `&mkp` presses mouse buttons, `&mmv` moves the pointer and `&msc` scrolls. While Mouse is held, E, S, D and F move the pointer, W and R scroll up and down, X and V scroll sideways, and G, T and A click the left, right and middle buttons. `CONFIG_ZMK_POINTING=y` in the keymap's `defy_neuron.conf` turns them on, and the keymap includes `<dt-bindings/zmk/pointing.h>` for them. To turn them off, set `CONFIG_ZMK_POINTING` to `n` and take the `&mkp`, `&mmv` and `&msc` bindings out of the keymap.

The setting adds a mouse to the HID descriptor the Neuron gives each computer. A computer on USB reads the new descriptor when the Neuron restarts. One paired over Bluetooth while the Neuron ran a build without mouse keys keeps the old descriptor, and the mouse keys do nothing there, until you remove the Defy on that computer, clear its profile in the [profile chooser](docs/lighting.md#bluetooth-profile-chooser) and pair again.

[USB report recovery](docs/power-and-wake.md#usb-report-recovery) covers the mouse buttons as well as the keys. ZMK sends no mouse report when the output changes, so a mouse button held while the output moves away from Bluetooth stays pressed on that computer until its next mouse report.

### ZMK Studio

`neuron-hybrid` includes [ZMK Studio](https://zmk.studio). Connect the Neuron by USB, pick its Studio serial port, and press FKeys+O once to unlock editing. The Neuron has a second serial port for the half updater. Studio's port belongs to the first of the Neuron's two USB interfaces. On macOS it is the lower-numbered port. On the tested Mac they were `cu.usbmodem1101` for Studio and `cu.usbmodem1104` for the updater, and the suffixes can change. On Windows it is the port of interface `MI_00`, whose COM number can be the higher one. Studio over USB works whichever output is selected for keys. Close other programs that hold the Studio port first.

Studio changes live in the Neuron's settings. A copy to the UF2 loader's drive keeps them, and so does `advanced neuron write --keep-settings`; see [keeping the settings](docs/neuron-flash.md#keeping-the-settings). `install` and `update` clear them after they write the Neuron. A saved Studio change overrides the compiled binding at the same key, so a new compiled default may not show until you revert that change in Studio. Studio takes key positions, sizes and rotations from the firmware's physical layout. The board's default keymap, `boards/dygma/defy/defy.keymap`, is based on the layout built into Dygma's firmware, which differs from the default Bazecor installs. The Studio test uses it.

## How it fits together

- **Neuron.** This module's UF2 loader runs at `0x1000` and ZMK at `0xD000`, in place of S140, the Nordic SoftDevice that holds the Bluetooth stack Dygma's firmware runs on. ZMK has its own stack and never used S140. Nordic's MBR and Dygma's `n2_bootloader` stay. ZMK keeps its settings at `0x6C000..0x72000` and leaves Dygma's settings at `0x72000..0x77000` alone. The installer writes the Neuron through Dygma's bootloader; see [Neuron flash and bootloader](docs/neuron-flash.md). The loader starts ZMK, or on request shows a USB drive that takes ZMK as a UF2 file; see [UF2 loader](docs/uf2-loader.md).
- **Halves.** ZMK runs behind Dygma's RP2040 bootloader, in the stock application area. The Neuron's half updater writes them over the cable, through the halves' own I2C bootloaders.
- **Radios.** Each nRF52820 runs this port's Bluetooth controller in Dygma's application slot at `0x1C000`. Nordic's S113 stays in place, unused. A half installs the controller through Dygma's radio bootloader.
- **Split link.** Each half reaches the Neuron by cable, over this port's SPI protocol, or by Bluetooth, over ZMK's split. The Neuron picks the link for each half by itself.
- **Computer.** The Neuron sends keys over USB or Bluetooth and keeps ten Bluetooth host profiles.

## Build targets

| Target | Chip | What |
|---|---|---|
| `left-hybrid`, `right-hybrid` | RP2040 | The normal halves: cable or Bluetooth link, sleep after five idle minutes. They keep the radio firmware already installed |
| `neuron-hybrid` | nRF52833 | The normal Neuron: routes each half, sends keys over USB or Bluetooth, runs ZMK Studio and the half updater |
| `radio` | nRF52820 | The Bluetooth controller: `radio.bin`, its seal and `radio.c` |
| `left-hybrid-radio`, `right-hybrid-radio` | RP2040 | The normal halves plus the radio image they install. A batch with these builds `radio` first |
| `left-hybrid-awake`, `right-hybrid-awake` | RP2040 | The normal halves with sleep off |
| `left-hybrid-diag`, `right-hybrid-diag` | RP2040 | The normal halves, except that a crash stays on the key LEDs instead of restarting the half; see [fatal errors](docs/lighting.md#fatal-errors) |
| `neuron-hybrid-log` | nRF52833 | The Neuron with a USB serial log in place of Studio: ZMK debug, Bluetooth connection and security messages, and USB report traces. Battery support is off. Lighting, the overlay and the chooser stay; see [logging builds](docs/split-diagnostics.md#logging-builds) |
| `neuron-hybrid-battery-log` | nRF52833 | The Neuron with a battery log in place of Studio |

`scripts/build` without a target builds `left-hybrid`, `right-hybrid` and `neuron-hybrid`, and `scripts/build all` builds every target. A half comes out as `<target>.bin`, which the installer writes, and `<target>.uf2`, the same package as a UF2 file for the RP2040's ROM bootloader, which the installer does not use. The Neuron comes out as `<target>.hex`, the UF2 loader and ZMK together, which the installer writes, and `<target>.uf2`, ZMK alone, for the loader's drive.

## Building

Builds need Docker and Python 3.9 or newer. They run in ZMK's dev container, pinned by its digest in `scripts/zmk`; `ZMK_IMAGE` runs another image. Clone the repository into a folder of its own, such as `~/defy/zmk-defy-wireless`. `scripts/zmk` mounts the folder above the repository in the container at `/work`, and `scripts/setup` puts the west workspace beside the repository, in `~/defy/zmk-workspace`.

```sh
git clone https://github.com/borovaka/zmk-defy-wireless ~/defy/zmk-defy-wireless
cd ~/defy/zmk-defy-wireless
docker pull zmkfirmware/zmk-dev-arm@sha256:77b62cbc6c62ca0026ed9eb511dd7fc2a13692d1b9ddbaf92bca510612390db8
scripts/setup        # once: ../zmk-workspace with ZMK, Zephyr and modules, about 3 GB
scripts/build        # left-hybrid, right-hybrid and neuron-hybrid
scripts/build left-hybrid-radio right-hybrid-radio
scripts/build --config ../my-config --output ../my-firmware neuron-hybrid
```

- `--config DIR` builds with another ZMK config directory, which holds the keymap and `.conf` files as `config/` does. `--output DIR` writes the images there instead of `firmware/`. `--zmk DIR` names ZMK's checkout, for a west workspace other than `../zmk-workspace`. All three must be inside the folder above the repository, which `scripts/zmk` mounts in the container at `/work`.
- In a keymap repository's workspace, where west lists this module, the build names Zephyr's other modules in `ZEPHYR_MODULES`, so this module links after them as it does here, and the images come out the same.
- Outputs reach the output directory only after every target in the batch has built. Until then they wait in a `.build-*` directory inside it. A killed build leaves that directory behind, and the next build removes it when no other build is running.
- Build trees live in the `zmk-defy-build` Docker volume, at `/build/<target>`, because builds on the macOS file share failed with truncated object files. `scripts/zmk` opens a shell in the container.
- A build tree starts afresh when its target's arguments change, or when a file is added to or removed from `boards/`, `dts/`, `snippets/`, the config directory, or `apps/defy_radio/` and `apps/defy_loader/` outside their `src/`. Deleting `/build/<target>/defy-build-args` forces the same.
- A batch with a Neuron target first builds the UF2 loader, once, in `/build/loader`, then checks it and merges it with each Neuron's ZMK; see [the build](docs/uf2-loader.md#the-build).
- Every Neuron and half image carries the module version, from `git describe` in this repository. The Neuron's also carries the config directory's repository and commit, and the halves' do not, so a keymap commit leaves the half images byte-identical unless it changes `config/defy.conf`. New values recompile only the files that use them; the build trees do not start afresh. After each batch, the output directory gets `manifest.json`, which lists the Neuron's values and each image's size and SHA-256, when it holds `neuron-hybrid.hex` and a pair of half images. See [versions and the manifest](docs/versions.md).
- `--factory-version` sets the 32-bit version the halves report to their bootloader. It defaults to 1. The radio's version is derived from the CRC32 of its image, so every radio change installs.

`config/west.yml` pins the ZMK revision, and `scripts/setup` checks out that revision. Keymap repositories made from the [Defy-Wireless-ZMK](https://github.com/borovaka/Defy-Wireless-ZMK) template import that revision from it. `scripts/setup --container` runs west directly, without Docker, for when it already runs inside ZMK's container, as in CI.

On pushes to `main` and on pull requests, unless they change only files under `docs/` or Markdown files, `.github/workflows/ci.yml` runs `scripts/build all` in the pinned container and uploads the images and `manifest.json` as a build artifact. Keymap repositories call `.github/workflows/build-user-config.yml`, which builds their `config/` with this module's `scripts/build`.

### Releases

A `v*` tag runs `.github/workflows/release.yml`. It builds `left-hybrid-radio`, `right-hybrid-radio` and `neuron-hybrid` from scratch, without the caches CI keeps, and checks that the images carry the tag as their module version. It attaches the two halves' `.bin` files, the Neuron's `.hex` and `.uf2`, `manifest.json` and `SHA256SUMS`, which covers the other five, to a draft release, which the maintainer publishes by hand.

## Tests

```sh
python3 scripts/test-host     # build-script tests, and host C suites under sanitizers
docker pull --platform linux/amd64 zmkfirmware/zmk-dev-arm@sha256:77b62cbc6c62ca0026ed9eb511dd7fc2a13692d1b9ddbaf92bca510612390db8
scripts/test                  # keymap, driver and Studio tests on native_sim
scripts/test drivers --driver defy_hybrid_central
scripts/test --driver defy_hybrid_peripheral-graceful-sleep
```

`scripts/test-host` needs Python 3 and clang, but no keyboard or Docker. `scripts/test` builds and runs the keymap case, the driver suites and the Studio test side by side, one build for every two container CPUs and at most eight; `--jobs N` changes that. It runs the keymap case itself, as ZMK's `run-test.sh` would. Each build keeps its tree in the build volume, under `/build/tests`, and starts afresh only when its arguments change or when a file comes or goes where Zephyr looks for one by name: the suite's own directory outside `src/`, `tests/drivers/common/` or `dts/`. Otherwise only ninja runs again, so a suite that has not changed takes a second or two. Test and firmware builds compile through ccache, whose cache `scripts/zmk` keeps in the build volume. `scripts/test-host` runs its suites side by side too. Both end with a summary that lists any failures and their logs.

CI runs `scripts/test-host` on the Ubuntu runner, which has clang, and `scripts/test` in the pinned container.

| Directory | What it checks |
|---|---|
| `tests/keymap/` | The matrix transform, through a layer of its own: a key of each row of each half and every thumb key |
| `tests/drivers/` | Module drivers on native_sim against fakes: the radio link and radio installer, cable and Bluetooth transports, battery, lighting, the half updater and split pairing reset, the version opcodes and the halves' version reports, sleep, USB recovery and the LED drivers |
| `tests/host/` | Host C suites under AddressSanitizer and UndefinedBehaviorSanitizer: the cable codec, the version record, the battery frame and percentages, the radio's battery filter, the battery key's choice of cable or Bluetooth, the `rf_bootloader` client against a fake bootloader with a power cut in every packet, the radio's start-up, power-off and watchdog, fatal-fault recovery, the WS2812 PIO driver, and the UF2 loader's block checks, file writes, FAT volume and start decision |
| `tests/scripts/` | The build's `.config` checks, build order and fresh builds, the embedded version values, `manifest.json`, packaging, image bounds and CRCs, the Neuron image checks, the merge of the UF2 loader and ZMK, the Neuron's UF2 file, the Neuron's partitions against the build script's, the loader's Board-ID |
| `tests/studio/` | A Studio client against the keymap over a simulated serial port |

A driver suite is a directory of `tests/drivers/` with a `CMakeLists.txt`. Its switches are CMake options, `option(DEFY_TEST_<NAME> "<what it changes>" OFF)`: the suite builds once with all of them off, and once with each one on, as `<suite>-<name>`. `tests/drivers/common/` holds what several suites share: `defy_test.cmake`, which every suite includes first, for the module's and ZMK's paths, HID settings and event manager; the test devicetree bindings; the fake clock and connections; and the fake radio, with its test SPI controller and `radio.overlay`, which `defy_hci_spi` and `defy_radio_install` share. `tests/fakes/rp2040/` stands in for the RP2040's registers in `defy_half_power` and in the host suite `defy_fatal_recovery`.

Snapshots hold the expected output. `ZMK_TESTS_AUTO_ACCEPT=1` rewrites them after a deliberate change; check the diff. The fakes do not show real wiring, electrical timing or power draw.

### On another machine

`scripts/remote` runs any of these commands on a Linux machine with Docker, over SSH:

```sh
DEFY_REMOTE=user@host scripts/remote scripts/test drivers
DEFY_REMOTE=user@host scripts/remote scripts/build neuron-hybrid
```

It copies the working tree, uncommitted changes included, to `~/src/<repository folder>` on that machine (`DEFY_REMOTE_DIR` changes `src`), runs the command there, and copies back any file the command writes to the top of `firmware/`. Flashing stays on this machine, where the keyboard is.

Once, on the remote machine:
1. Install Docker and `clang`.
2. Pull the build image by the digest in `scripts/zmk`, as above.
3. Run `scripts/remote scripts/setup`.

`scripts/zmk` turns SELinux labels off for its container when SELinux is enforcing, as on Fedora. Firmware built there is byte-identical to a local build.

## Changes to ZMK and Zephyr

The module changes or reaches into upstream code in these places only. The build checks each against the pinned ZMK and Zephyr and stops at the first that no longer fits, naming the code to review. `cmake/defy_upstream.cmake` pins by SHA-256 the files the module patches or hooks into by their contents, and checks that upstream still makes each call a wrap or rename relies on. `<dygma/defy_wrap.h>` gives each wrap the type the upstream header declares, so a changed signature stops the compile.

- **Split central patch.** `drivers/split/zmk-split-central.patch` is a unified diff against ZMK's `split/bluetooth/central.c`. The build checks the upstream file's SHA-256, applies the patch to a copy with `git apply` and compiles the copy. A ZMK update that changes the file stops the build until the patch is reviewed. Every change carries a `Defy:` comment, except the added `<zephyr/sys/atomic.h>` include and the call to `retry_scanning()` in place of `start_scanning()` after a failed connection. [Connection recovery](docs/ble-input-recovery.md) explains them.
- **Link-time wraps:**
  - `lighting/defy_feedback_central.c` wraps ZMK's profile and output selection calls to show the host overlay on the halves.
  - `drivers/usb/defy_usb_report_recovery.c` wraps the USB HID senders, `usb_wakeup_request` and `usb_enable` for [USB report recovery](docs/power-and-wake.md#usb-report-recovery).
  - `drivers/split/defy_split_pairing.c` wraps `zmk_ble_put_peripheral_addr` on the Neuron. It keeps ZMK's two split slots in a table of its own, loaded from ZMK's `ble/peripheral_addresses` settings, so that the half updater can report and clear them without a restart.
  - `drivers/bluetooth/defy_bt_identity.c` wraps `bt_le_adv_start` on the halves. It saves the Bluetooth identity before advertising and holds off new advertising while a half shuts Bluetooth down for sleep.
  - The radio firmware wraps `nrfx_twim_init` to record I2C failures for the battery diagnostics.
- **Start hook.** Zephyr's `arch/arm/core/cortex_m/reset.S` calls `soc_reset_hook` before Zephyr's start, and Nordic's SoC support binds that hook to `SystemInit` with `PROVIDE()` in `soc/nordic/common/platform_init.ld`. A definition of the hook elsewhere replaces that binding. The UF2 loader in `apps/defy_loader/` defines it, to choose between ZMK and its drive before `SystemInit()` runs. The radio firmware defines the same hook in `apps/defy_radio/src/startup.c`, to stop the SPI slave that `rf_bootloader` leaves running, before Zephyr clears RAM. Both builds pin the SHA-256 of both Zephyr files.
- **Studio renames.** `studio/CMakeLists.txt` renames two calls at compile time. In ZMK's Studio RPC source, `zmk_endpoint_get_selected` becomes a version that answers USB whenever a USB host is ready. In the Studio BLE transport, `zmk_studio_core_lock` becomes a version that does not lock Studio on a Bluetooth disconnect while USB is ready.
- **Studio's listener.** When the USB connection state changes, `studio/defy_studio_transport.c` calls ZMK's Studio RPC listener, `zmk_listener_studio_rpc`, directly, with an endpoint-changed event of its own. Studio then picks its transport again, while ZMK's other listeners see no endpoint change. The call relies on the listener's name and the event's structure in ZMK; if ZMK renames the listener, the link fails.

To add a wrap, add `zephyr_link_libraries(-Wl,--wrap=<name>)` to the `CMakeLists.txt` beside the source. The source declares the original with `DEFY_WRAP_REAL(<name>)`, defines `__wrap_<name>`, and then checks that the wrap keeps the original's type with `DEFY_WRAP_CHECK(<name>)`. The `CMakeLists.txt` names the upstream file that calls the wrapped function, and the call, with `defy_upstream_uses()`, which stops the build when the call is gone. Code that depends on an upstream file's exact contents pins its SHA-256 with `defy_upstream_pin()`. Add each new place to the list above.

### Upgrading ZMK

`config/west.yml` pins ZMK, and through it Zephyr. The weekly `ZMK main` workflow builds and tests the module against ZMK's main branch, which shows early what the next upgrade needs. To upgrade:

1. Set the new revision in `config/west.yml` and run `scripts/setup`. When ZMK moves to a newer container image, put its digest in `scripts/zmk` and the workflows, where `tests/scripts/test_zmk_image.py` checks that they agree, and in the template's `build.sh`.
2. Run `scripts/build all`. A check that stops it names what to review:
   - the split central patch: rebase it onto the new `central.c` with `git apply --3way`, keep each `Defy:` comment and update its `SHA-256:` line;
   - a start hook: review the loader's `src/start.c` and the radio's `src/startup.c`, then update the pins in their `CMakeLists.txt`;
   - a call a wrap or rename relies on: move the wrap to the function ZMK now calls;
   - a wrap's type: follow the new signature.
3. Run `scripts/test`, `scripts/test-host` and the image comparison.
4. On the keyboard, check what the changed places do: the host overlay on a profile or output change, typing over USB after the computer sleeps and wakes, ZMK Studio over USB while the keyboard types over Bluetooth, the halves reconnecting after a half or the Neuron restarts and after a half sleeps, the half updater's split pairing report, and the battery display.

## Documentation

| Document | Contents |
|---|---|
| [docs/neuron-flash.md](docs/neuron-flash.md) | The Neuron's flash layout, writing it through Dygma's bootloader, keeping settings, Bazecor, going back to stock |
| [docs/software-bootloader.md](docs/software-bootloader.md) | Entering the Neuron bootloader from software, and the boot request |
| [docs/uf2-loader.md](docs/uf2-loader.md) | The Neuron's UF2 loader: its start count, entering and leaving its drive, what the drive takes, the LED's colours in drive mode, the 1200-baud trigger, its registers and USB IDs, its build |
| [docs/half-updater.md](docs/half-updater.md) | The Neuron's half updater, its protocol, the split pairing reset and the half image format |
| [docs/versions.md](docs/versions.md) | What a build embeds, the halves' version report and `manifest.json` |
| [docs/radio-install.md](docs/radio-install.md) | The radio's flash layout and how a half installs the controller |
| [docs/automatic-transports.md](docs/automatic-transports.md) | Cable and Bluetooth routing, the cable protocol and the lighting messages, host output, dock detection, pairing reset |
| [docs/ble-input-recovery.md](docs/ble-input-recovery.md) | How the Neuron recovers stalled split connections and subscriptions |
| [docs/power-and-wake.md](docs/power-and-wake.md) | Half idle polling, dormant sleep and wake, USB report recovery |
| [docs/battery.md](docs/battery.md) | Battery measurement, charger servicing, the battery display, host battery reporting |
| [docs/lighting.md](docs/lighting.md) | The lighting's looks, layer and key colours, the profile chooser, start-up and fault colours, the Neuron's LED |
| [docs/split-diagnostics.md](docs/split-diagnostics.md) | Split status, the key event trace and the logging builds |

## Contents

| Path | What |
|---|---|
| `boards/dygma/defy/` | `defy_left`, `defy_right`: the RP2040 main boards with matrix, physical layout and the factory flash layout |
| `boards/dygma/defy_rf/` | `defy_rf`: the nRF52820 radio board, in Dygma's flash layout |
| `boards/dygma/defy_neuron/` | `defy_neuron`: the Neuron. Its `zmk` variant is the split central, and its plain target, `defy_neuron/nrf52833`, runs the UF2 loader |
| `apps/defy_loader/` | The UF2 loader: the start decision and start counter, the USB drive with its FAT and UF2 parser, flash writes, the 1200-baud trigger |
| `apps/defy_radio/` | The radio firmware: Bluetooth controller over SPI, battery sampling and filtering, charger servicing, power off |
| `drivers/bluetooth/` | The RP2040's HCI driver for the radio, the radio installer and the identity wrap |
| `drivers/split/` | Cable and Bluetooth transports, routing, the Neuron's split slots and the split central patch |
| `drivers/led_strip/` | IS31FL3743B per-key LED driver; `defy_leds` joins per-key LEDs and underglow into one strip; a WS2812 PIO driver copy that packs four-channel pixels |
| `drivers/usb/` | USB report recovery on the Neuron |
| `battery/`, `lighting/`, `status/`, `power/` | Battery telemetry and the halves' control service; lighting, host feedback and the halves' LED displays, start-up colours included; the Neuron's LED and dock detection; half sleep |
| `updater/`, `bootloader/` | The Neuron's half updater, software entry to its bootloader, and `&defy_drive`, which restarts the Neuron as the UF2 loader's drive |
| `diagnostics/` | The halves' fault display and fault recovery, the Neuron's key trace |
| `soc/rp2040/` | RP2040 power off to DORMANT with key wake, and the handoff from Dygma's bootloader, which stops the watchdog it leaves running |
| `studio/` | Keeps Studio on USB |
| `snippets/` | The build snippets the targets combine |
| `dts/` | `behaviors/defy.dtsi`: the behaviours a keymap binds, `&defy_profiles`, `&defy_lighting`, `&defy_battery` and `&defy_drive`, and the nodes it fills with layer and key colours; the devicetree bindings for these and for the drivers |
| `include/dygma/` | Shared protocol constants and interfaces |
| `config/` | Pinned ZMK revision, user settings and the Neuron's keymap |
| `cmake/` | The checks on upstream code, from "Changes to ZMK and Zephyr" |
| `scripts/` | Container wrapper, workspace setup, builds, tests, packaging, and `remote` for running them on another machine |
| `.github/workflows/` | CI, release builds for `v*` tags, the build that keymap repositories call, and the weekly build against ZMK's main branch |

## Hardware assumptions and open checks

- The Neuron board assumes Neuron 2 hardware V1.1. The revision of the tested unit is not confirmed.
- `defy_rf` assumes a 32.768 kHz crystal. It has not been measured.
- The pin map has not had a continuity check against the boards. The half's own USB port is unconfirmed.
- Whether chip select wakes the nRF52820 from System OFF is unconfirmed. Start-up and SPI traffic work with the current delays; the timing margins are unmeasured.
- The 1 ms wait after switching the underglow supply on is a guess. The order of the underglow LEDs around a half is unknown.
- Studio's key geometry is a display model, not a measured drawing.

## Licence

The module is under the MIT licence; see [LICENSE](LICENSE). Three files are based on Zephyr's Apache-2.0 code and keep that licence: `drivers/led_strip/ws2812_rpi_pico_pio.c`, `drivers/bluetooth/defy_hci_spi.c` and `apps/defy_radio/src/main.c`; see [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt). Dygma's firmware is not part of this repository.
