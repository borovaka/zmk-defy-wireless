# Lighting and controls

The halves' per-key and underglow LEDs show each half's state and what the Neuron's controls do. The module's README lists the [controls](../README.md#keymap).

## Normal lighting

When a half connects to the Neuron, every LED on it shows cyan for two seconds. Then the keys and the underglow take the colours the Neuron sends with its lighting state: those of the current look, or of an active layer, at the current brightness. By default the keys light their white LEDs at 24 of 255, and the underglow is dark. Until the half has the Neuron's state it stays cyan, and so does a half whose radio failed, connected by its cable. Before it connects, a half shows its start-up stage in every LED, dimly, at 24 of 255; see [start-up and fault colours](#start-up-and-fault-colours).

The looks and layer colours live in the keymap, on the Neuron. A Neuron update changes them, by the installer or by [drag and drop](uf2-loader.md), without writing the halves.

### Looks and brightness

A look is a colour for the keys and one for the underglow. In the template's keymap, the key right of G shows the next look. With FKeys held, on the left upper thumb key second from the outside, / shows the next look too, and ; and ' step the brightness down and up through 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192 and 255. Each of these keys also turns the lighting on if the toggle, the right upper outer thumb key, had turned it off. The Neuron saves the look and the brightness one second after the last change. In ZMK Studio, `&defy_lighting`'s commands show as Toggle, Brighter, Dimmer and Next look.

A keymap without looks of its own gets four: white keys, warm white keys, white keys and underglow, and white underglow alone. A keymap sets its own on `&defy_lighting`, after `#include <dt-bindings/dygma/defy_lighting.h>`:

```dts
&defy_lighting {
    brightness = <24>;
    plain { keys = <DEFY_WHITE>; };
    warm { keys = <DEFY_WARM_WHITE>; underglow = <DEFY_WARM_WHITE>; };
    night { underglow = <DEFY_PURPLE>; };
};
```

The lighting starts on the first look. `brightness` sets the starting brightness, 1 to 255, 24 if left out. A colour a look leaves out is off. Looks in the keymap replace the four rather than adding to them.

The Neuron saves the chosen look and brightness with a CRC32 of the keymap's looks and starting brightness. A build that changes either starts from its own first look and brightness. A build that changes only the rest of the keymap keeps the saved choice.

### Layer colours

A layer can have colours of its own while it is active, such as purple underglow while FKeys is held or red keys on a gaming layer. Each goes in `&defy_layer_lighting` as a child:

```dts
&defy_layer_lighting {
    fkeys { layer = <2>; underglow = <DEFY_PURPLE>; };
    gaming { layer = <5>; keys = <DEFY_RED>; underglow = <DEFY_OFF>; };
};
```

`layer` numbers the layer as `&mo` and `&tog` do. Of the active layers that have colours, here or in [key colours](#key-colours), the highest decides. Layers rank in keymap order, as ZMK ranks them when it picks a key's binding. A colour the child leaves out comes from the current look, and `DEFY_OFF` turns it off. A layer without colours lets those below it show. The brightness scales layer colours too, and the toggle and the idle darkness turn them off with the rest. In the template's keymap, Base has no colours of its own and shows the look. Navigation, FKeys and Mouse turn their keys off, so that only their [key colours](#key-colours) light, and give the underglow green, purple and blue.

A layer change reaches the halves as a new lighting state, over the cable or on the next Bluetooth connection event. While the lights are off, layer changes send nothing.

### Key colours

Single keys of a layer can have a colour of their own, such as WASD on a gaming layer. Each group of keys goes in `&defy_key_lighting` as a child:

```dts
&defy_key_lighting {
    wasd { layer = <5>; key-positions = <16 29 30 31>; colour = <DEFY_RED>; };
    numbers { layer = <5>; key-positions = <1 2 3 4 5>; colour = <DEFY_BLUE>; };
};
```

`key-positions` numbers the keys as ZMK's combos do:

```
 0  1  2  3  4  5  6       7  8  9 10 11 12 13
14 15 16 17 18 19 20      21 22 23 24 25 26 27
28 29 30 31 32 33 34      35 36 37 38 39 40 41
42 43 44 45 46 47            48 49 50 51 52 53
         54 55 56 57      58 59 60 61
         62 63 64 65      66 67 68 69
```

The rows follow the keymap: the number row, the rows of Q, A and Z, then the upper and lower thumb keys. On the template's layout, W is 16 and A, S and D are 29 to 31.

The key colours show while their layer decides, over the `keys` colour of the layer or the look; a key in two groups takes the later one's colour. Holding a layer with colours of its own, such as FKeys, over the gaming layer shows that layer's colours and none of the gaming layer's keys. The brightness scales key colours too. A keymap can have 16 groups, each with any number of keys. The template's keymap has ten, which light the keys Navigation, FKeys and Mouse use:

- Navigation: the arrows, Home, End, Page Up, Page Down and Print Screen yellow, the numpad's digits green and its other keys orange.
- FKeys: grave and F1 to F12 white, the media keys cyan, display brightness, sleep and power off red, and the Defy controls orange.
- Mouse: the pointer keys cyan, the scroll keys green and the buttons red.

The Neuron sends each half the groups once per connection, before the first lighting state that names them, and a half takes a state only once it holds every group the state names. After that a layer change is one lighting state again.

### Colours

`dt-bindings/dygma/defy_lighting.h` names these colours, at full strength:

| Name | Red, green, blue |
| --- | --- |
| `DEFY_OFF` | 0, 0, 0 |
| `DEFY_WHITE` | 255, 255, 255 |
| `DEFY_WARM_WHITE` | 255, 220, 180 |
| `DEFY_RED` | 255, 0, 0 |
| `DEFY_ORANGE` | 255, 80, 0 |
| `DEFY_YELLOW` | 255, 200, 0 |
| `DEFY_GREEN` | 0, 255, 0 |
| `DEFY_CYAN` | 0, 255, 255 |
| `DEFY_BLUE` | 0, 0, 255 |
| `DEFY_PURPLE` | 128, 0, 255 |
| `DEFY_PINK` | 255, 64, 128 |

`DEFY_RGB(r, g, b)` gives any other colour, 0 to 255 each, and so does a plain `0xRRGGBB`. The brightness scales every channel: at 24, `DEFY_WHITE` reaches the LEDs as 24, 24, 24. A brighter setting draws more from the halves' batteries.

Every per-key and underglow LED has a white LED beside its red, green and blue ones. Red, green and blue mixed to white look pink or green on them, so the halves light the white LED with the part of a colour common to all three channels, and red, green and blue with the rest. White at 24 lights the white LEDs alone, and draws a third of the current of red, green and blue at 24. `DEFY_WARM_WHITE` at 24 is the white LEDs at 17 with red at 7 and green at 4. A colour with a channel at 0, such as cyan or orange, lights red, green and blue only. The same goes for every display on the halves: the white of the profile chooser and of the command bits lights the white LEDs.

### On, off and idle

The Neuron keeps one lighting preference, on at first. The toggle, `&defy_lighting DEFY_LIGHTING_TOGGLE`, turns it on or off. The Neuron saves a changed preference after one second and sends the current state, with the colours, to both halves over every link they have. A reconnecting half gets the state again. Each link confirms the applied state, and the Neuron stops resending after the confirmation or six attempts.

With the preference on, a connected half keeps its lights on, and a half that is not connected turns them off two minutes after its last state change. With the preference off, the lights stay dark through connection changes. Before a half has the Neuron's preference, and while it shows a start-up or failure stage, its LEDs go off two minutes after the last state change. After a radio failure they stay on for ten minutes instead. A new stage or a connection change lights them again, and a failed radio's underglow blinks again.

The Neuron also turns the lights off after 60 seconds without a key press on either half, in every cable and wireless arrangement. The next key press turns them back on if the preference is on. This idle darkness never changes the saved preference. The Neuron counts a press before ZMK processes it, so layer and hold-tap keys count too. Releases, replayed events, battery reports and acknowledgements do not count. In a keymap with combos, a press that a combo held back can count twice, as ZMK raises it again. That does no harm. A reconnect snapshot with a held key counts once; an all-up one does not. A half that wakes from sleep counts as activity.

The battery display shows its cells over either preference and then restores it. Start-up, installation and failure displays take priority over the preference.

## Bluetooth profile chooser

The Neuron keeps ten Bluetooth host profiles. The chooser follows stock Dygma's pairing screen, extended from five slots to ten. The left half shows profiles 1 to 5 on keys 1 to 5, and the right half shows profiles 6 to 10 on keys 6 to 0, as the keycaps read. Each half lights only its own five.

| Key in the chooser | Action |
| --- | --- |
| 1 to 5, 6 to 0 | Select profiles 1 to 10, prefer Bluetooth output and close |
| Hold Q to T or Y to P for 3 s | Clear the paired profile on the number key above |
| Esc or Backspace | Prefer USB output and close |
| Caps Lock, C or V | Close |

The chooser also closes 30 seconds after the last key. Once it closes, the halves keep showing it for two seconds, with the new selection, while keys already reach the computer. Clearing leaves it open, with the cleared profile selected and waiting for a new host. Keys pressed while it is open never reach the computer. A selected empty profile advertises as `Defy` for pairing, docked or not. Clearing a profile does not clear the halves' bonds.

The halves use the battery display's brightness, 128 of 255:

| Key | Colour | Meaning |
| --- | --- | --- |
| Outer end of the number row | Yellow | The USB key, Esc on the left and Backspace on the right |
| Profile key | Dim blue | Empty |
| Profile key | White | Paired, host not connected |
| Profile key | Dim green | Host connected, profile not selected |
| Profile key | Bright green | Selected, host connected |
| Profile key | Blue, breathing | Selected and empty, advertising for a new host |
| Profile key | White, breathing slower and dimmer | Selected and paired, waiting for its host |
| Key below a profile | Red, half brightness | Paired, so holding this key clears it |
| Underglow | Two blue LEDs chasing round | The selected profile has no host connected |
| Innermost key of the second row | Cyan | The Neuron is docked |
| Every key | Blue for one second | A profile was just cleared |

ZMK keeps every paired host in range connected, so more than one profile can show green. Only the selected one gets the keys. The blue breath uses stock's curve and takes about two seconds, from 49 to 128. The white one takes about four seconds and peaks at 64, so a host that is away never looks like an empty slot. The breathing and the chase run only while the chooser shows. The Neuron sends the chooser again every 500 ms, so a host that connects turns green within about half a second.

### Host overlay

After a profile change, or an output change such as FKeys+Q or FKeys+W in the template, both halves show the selection for two seconds, at the same brightness:

| Key | Colour | Meaning |
| --- | --- | --- |
| The selected profile's number key, on the half that has it | Green | Its host is connected |
| The same key | Amber | Its host is not connected |
| Innermost key of the number row, both halves | White | USB is preferred and a computer is ready |
| The same key | Amber | USB is preferred and no computer is ready |
| The same key | Blue | Bluetooth is preferred and the selected host is connected |
| The same key | Purple | Bluetooth is preferred and the selected host is not connected |
| Innermost key of the second row | Cyan | The Neuron is docked |

When the preferred output is not ready, keys go to the other one if it is. The output key stays dark if neither output is preferred.

### Matching builds

The halves and the Neuron must run the same build. When their lighting or host feedback versions differ, each drops the other's messages of that kind. The chooser and the overlay then do not light, although the chooser still opens on the Neuron and takes its keys until it closes. A lighting mismatch also keeps the halves cyan once connected, and the toggle and the idle darkness do not reach them. Typing and the battery display keep working. `defy-installer update` writes matching builds. [Lighting messages](automatic-transports.md#lighting-messages) gives the versions and layouts.

## Keymap bindings

A keymap gets the Defy's behaviours with `#include <behaviors/defy.dtsi>`. They run on the Neuron, whichever half has the key, and act on the press. `&defy_lighting` takes one parameter, a command from `#include <dt-bindings/dygma/defy_lighting.h>`. The others take none.

| Binding | Action | Key in the template |
| --- | --- | --- |
| `&defy_lighting DEFY_LIGHTING_TOGGLE` | Turn the lighting preference on or off | The right upper outer thumb key |
| `&defy_lighting DEFY_LIGHTING_DIMMER` | One brightness step down | FKeys+; |
| `&defy_lighting DEFY_LIGHTING_BRIGHTER` | One brightness step up | FKeys+' |
| `&defy_lighting DEFY_LIGHTING_NEXT` | The next look | The key right of G, and FKeys+/ |
| `&defy_profiles` | Open the [profile chooser](#bluetooth-profile-chooser) | The right lower outer thumb key |
| `&defy_battery` | Show each half's [battery state](battery.md#battery-display) for five seconds | The right lower thumb key second from the outside |
| `&defy_drive` | Restart the Neuron as the [UF2 loader](uf2-loader.md)'s drive | FKeys+Backspace, the right half's top outer key |

FKeys is held on the left upper thumb key second from the outside. The colours are nodes rather than bindings: the [looks](#looks-and-brightness) on `&defy_lighting`, and [layer colours](#layer-colours) and [key colours](#key-colours) on `&defy_layer_lighting` and `&defy_key_lighting`.

The chooser's keys are properties of `&defy_profiles`, numbered as in [key colours](#key-colours). `select-positions` and `clear-positions` each hold ten keys, in profile order. A build with any other count fails. `usb-positions` and `close-positions` take any number. The defaults are the keys in the chooser's table above:

| Property | Default | Keys |
| --- | --- | --- |
| `select-positions` | 1 2 3 4 5 8 9 10 11 12 | 1 to 5, 6 to 0 |
| `clear-positions` | 15 16 17 18 19 22 23 24 25 26 | Q to T, Y to P |
| `usb-positions` | 0 13 | Esc and Backspace |
| `close-positions` | 28 45 46 | Caps Lock, C and V |

To close the chooser with the left Space key as well:

```dts
&defy_profiles {
    close-positions = <28 45 46 56>;
};
```

The halves' display stays on the default keys whatever the positions.

## Start-up and fault colours

`lighting/defy_startup_leds.c` lights every LED of a half, dimly, in the colour of the furthest stage it has reached. `lighting/defy_half_leds.c` stacks that with the half's other displays: the battery display, the low-battery warning, the host overlay, the chooser and the lighting preference.

| Colour | Stage |
| --- | --- |
| Blue | ZMK is running |
| Yellow | Dygma's radio bootloader answered |
| Orange, a growing share of the LEDs | The radio firmware is being written, about half a minute |
| Green | The radio runs this port's controller and Bluetooth is up. This alone does not show advertising or a connection |
| Cyan | Connected to the Neuron. After two seconds the [everyday colours](#normal-lighting) take over |
| Red, blinking with another colour | The radio never came up. The other colour is the last stage reached before |

Yellow and orange come from the radio installation in the `*-hybrid-radio` builds.

### Radio failure report

When the radio fails, the underglow keeps blinking and the per-key LEDs show what the radio driver saw. Rows run top to bottom, with keys counted from the outside edge of each half. Rows 1, 2, 3 and 5 and the install trace come from the radio installation, so only the `*-hybrid-radio` builds show them (`lighting/defy_radio_install_leds.c`):

| Row | Shows |
| --- | --- |
| 1 | Red: why the first bootloader session failed, by the number lit. 1 timeout, 2 protocol error, 3 I/O error, 4 not Dygma's bootloader, 5 bad CRC, 6 invalid argument, 7 anything else. All green: it began |
| 2 | White: when that session began after Zephyr started, 20 ms per LED |
| 3 | White: the same, 200 ms per LED |
| 4 | The radio's first answer other than `00 00` or `ff ff`. Dygma's link in yellow, 1 to 6 for its state `0x80` to `0x85`; this port's controller all green; anything else all blue |
| 5, thumb keys | Purple: the number of bootloader sessions, up to 3 |

After a failed install attempt, a radio-installing build shows the install trace instead:

- The first four keys of each main row hold one hexadecimal digit each, least significant bit first: the high and low digits of received byte 0, then of received byte 1.
- The two groups of four thumb keys hold the protocol phase and the error code, numbered as in row 1 above. `defy_radio_factory_phase` in `include/dygma/defy_radio_factory.h` defines the phases.
- Lit keys are blue for command `B`, cyan for `I` and purple for any other command. Top-row key 5 shows that colour, and key 6 is green for success or red for an error.
- The command's eight bits show in white on per-key LEDs 11, 12, 13, 18, 19, 20, 25 and 26.
- Top-row key 7 shows progress: blue before an install attempt, purple after an attempt, orange after the erases, cyan once every byte was written, and green once the finish command went out. Green there does not show that the controller started.

A half attempts the install at most once per start of its Bluetooth driver. A session that fails to begin is tried again, up to three sessions within the power-on window; a bootloader that is not Dygma's fails at once. Dygma's radio application never begins a session, so a radio that runs it fails the start. A failed install or a controller that does not start cannot cause repeated erases; any restart of the half starts a fresh attempt.

### Fatal errors

A Zephyr fatal error on a half flashes the per-key LEDs red in turn with another colour: blue for a CPU exception, white for a stack overflow, green for anything else. The pattern is written through the RP2040's registers alone, as Dygma's bootloader lights the LEDs. It shows for 0.5 s, and the watchdog restarts the half 2 s after the fault. The count of quick faults clears after 60 s of uptime. On the third quick fault in a row the half starts with its LEDs off and waits in DORMANT until a key is pressed, which starts it afresh, as switching it off and on does.

The `left-hybrid-diag` and `right-hybrid-diag` builds turn that recovery off (`CONFIG_DEFY_FATAL_RECOVERY=n`). A crash then stays on the key LEDs, in a loop: the error pattern for about a second, then the faulting address and the last HCI commands for 6 s each, until the half is switched off. Install one of them when a half keeps restarting and the 0.5 s flash is too short to read.

## Neuron status LED

The Neuron's small LED is green while ZMK's USB HID connection is configured and awake. It is off before enumeration, after disconnection, during USB suspend, and when the Neuron runs on dock or charger power without a USB host. Resume brings green back if USB is still configured.

While the [half updater](half-updater.md) holds the halves, typing is paused and the LED blinks amber, red and green together, 500 ms on and 500 ms off. That covers the installer writing the halves, and their release when the Neuron restarts in the middle of a write. Green, or dark, returns once the halves are released.

The [UF2 loader](uf2-loader.md#the-led) drives the same LED while its drive shows: amber when ZMK can start, red when it cannot, and green blinking while a file is copied.

`CONFIG_DEFY_NEURON_STATUS_LED_BRIGHTNESS` sets the brightness in percent of full, 1 to 100, and is 3 by default; the keymap's `defy_neuron.conf` lists it. The loader always uses 3. The LED does not show whether the halves are connected or which output is selected, and the lighting toggle, the lighting idle timer and the battery display do not affect it.

The module drives green on P0.10 and red on P0.09, active high, from PWM0's channels 0 and 1 with a 100 µs period; the board configures these NFC pins as GPIO. USB events and every change of the halves' hold schedule a work item that reads the current state. Only the amber blink uses a timer.
