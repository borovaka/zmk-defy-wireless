# Battery telemetry and charging

Each half's radio measures the battery voltage, services the charger and reports both. The RP2040 passes the reports to the Neuron over the cable or Bluetooth. The battery key, `&defy_battery`, shows each half's own state for five seconds. The Neuron reports the lower of the two halves' percentages to the computer through the standard Bluetooth Battery Service.

On the tested keyboard the fuel gauge at I2C address `0x36` does not answer: both halves get an address NACK. The radios therefore use the analogue voltage fallback. The charger at `0x6a` answers. A percentage from voltage is a coarse estimate, not a calibrated state of charge. A displayed 100% does not prove that charging has finished or that the battery still has its original capacity.

Sampling and charger service do not count as key activity. The radio stops its battery work before it powers off for half sleep. Current draw and battery life have not been measured.

## Charger servicing

The charger at `0x6a` is a FAN5405 or a part with the same register map. [charger.c](../apps/defy_radio/src/charger.c) checks its identity before any configuration write. The supported identity satisfies `(id & 0xfc) == 0x94`. An unknown identity or an unavailable bus leaves the charger unconfigured.

Every five seconds the radio checks the stock register configuration and services the charger's watchdog by writing status register `0x00 = 0xc0`. This runs on its own schedule, apart from battery sampling. To configure the charger, the radio writes, in order:

1. safety `0x06 = 0x70`, twice;
2. control `0x01 = 0xcc`, charging disabled;
3. special `0x05 = 0x44`;
4. current `0x04 = (readback_safety & 0x70) | 0x09`;
5. voltage `0x02 = 0x8c`;
6. the watchdog service;
7. control `0xc8`, charging enabled.

It reads back and checks the control, special, current and voltage writes.

If a step fails after the identity check, the radio writes control `0xcc` to disable charging. That write holds only until the charger's own watchdog expires, 20.5 to 32 seconds after the last service. The chip then returns to its defaults. On the tested keyboard's halves the defaults regulate at 3.54 V, and on the left half they also leave charging disabled. A cell above 3.54 V does not charge under those defaults. A cell below it can still charge up to 3.54 V.

The charger regulates charging itself. The radio configures it, services its watchdog and reads its status. Cable presence alone does not mean charging.

### Safety limits

These limits are the same as in stock Dygma firmware.

- The service ignores the charger's fault bits and keeps servicing the watchdog. While serviced, the charger has no limit on charge duration. Its 15-minute timer runs only in default mode, when nothing services it.
- Charging ends on current termination. A system load above the termination current could hold the cell at 4.20 V.
- Control `0xc8` sets no input current limit. Two charging halves could draw about 2 A through the Neuron's USB, limited only by the 4.533 V input voltage limit that special `0x05 = 0x44` sets.
- Nothing checks battery temperature. The FAN5405 has no thermistor input, and the fuel gauge does not answer.
- The service follows a lower latched current limit. On the tested keyboard the right half's safety register is latched at `0x40`, and the service gives it a lower charge current to match. A safety register latched with a current ceiling above `0x70`, or a voltage ceiling above 4.20 V, makes the service refuse the configuration and disable charging.

### Charging stops when servicing stops

If the radio stops servicing the charger while input power is present, charging stops once the charger's watchdog expires, 20.5 to 32 seconds later. Nothing tells the user. This happens after a radio crash and while the radio waits in its bootloader. Half sleep would also stop it, because the radio powers off. A half therefore stays awake while its last battery sample, if under 90 seconds old, reports charging or charge complete. A half whose charger reports neither, or whose last sample is stale, can still sleep with input power present.

## Voltage to percentage

The radio's comparator compares the battery, through a 0.68 divider, with 64 steps of a reference: VDD, taken as 3.18 V, or the internal 2.4 V. A step's voltage is its lower edge, `reference × (step + 1) / 63 / 0.68`, about 74 mV apart on VDD; neither reference is calibrated. `battery/defy_battery_protocol.c` maps each step to a percentage:

| VDD step | Voltage | Percentage |
| ---: | ---: | ---: |
| 26 to 42 | 2.00 V to 3.19 V | 1 |
| 43, 44 | 3.26 V, 3.34 V | 2 |
| 45 | 3.41 V | 4 |
| 46 | 3.49 V | 6 |
| 47 | 3.56 V | 10 |
| 48 | 3.64 V | 15 |
| 49 | 3.71 V | 20 |
| 50 | 3.79 V | 40 |
| 51 | 3.86 V | 50 |
| 52 | 3.93 V | 70 |
| 53 to 60 | 4.01 V to 4.53 V | 100 |

`defy_battery_protocol.c` rejects the other VDD steps, 0 to 25 and 61 to 63, as they fall outside the 2.0 V to 4.6 V it accepts. A cell reads 100% from 4.01 V up, so a charged cell shows 100% until it falls below that. On the 2.4 V reference the top step is 3.59 V, and the table reaches at most 10%.

## Persistent analogue estimate

[battery_filter.c](../apps/defy_radio/src/battery_filter.c) keeps up to 60 real voltage samples in RAM on each radio, at most one per second. Once the estimate settles, samples come every 10 seconds, so the history covers about the last 10 minutes. A press of the battery key asks for a new observation without clearing this history, so repeated presses do not restart the estimate or give quick samples extra weight.

The filter picks the most frequent voltage in its history, and a tie goes to the lower voltage. The first estimate needs at least five samples over at least four seconds. After that, a lower voltage replaces the estimate as soon as it is at least as frequent. A higher voltage must be more than twice as frequent as the current estimate. A reading that flips between two adjacent thresholds therefore settles on the lower one and stays there, which is the safe side for a battery. With a full history at the settled interval, a real voltage drop takes 30 samples, about five minutes, to replace the estimate. A rise takes 41 samples, about seven minutes.

A charger report of charging, charge complete, boost or a fault clears the history at once. A single unusable sample does not; examples are a comparator timeout, a failed charger read and an out-of-range voltage. Three unusable samples in a row clear the history. So does a gap of more than 20 seconds between discharge samples, or a sample clock that runs backwards. A radio restart clears it too, because it is not stored in flash.

Filtered analogue samples use tag `0xa103`. Word 1 keeps the raw comparator threshold in its low byte and the filtered percentage in its high byte; `255` means unavailable or still settling. The raw reference and millivolts stay available. `0xa102` is the radio's raw layout before filtering and never leaves it, so the halves and the Neuron receive only `0xa103`. The decoder they share with the radio, `battery/defy_battery_protocol.c`, accepts both tags. For `0xa103` it gives the filtered percentage, and only while the charger reports ready. For `0xa102` it gives the table's percentage for the raw threshold, whatever the charger reports. The radio's filter reads each sample that way.

The voltage table is coarse. The observed threshold 52, 3933 mV, maps to 70%, and threshold 53, 4007 mV, maps to 100%. Filtering reduces jumps between these bins but does not calibrate them. The two halves can settle on different values.

## Sampling schedule

Each analogue sample runs a comparator sweep and seven charger register reads, which keep the radio awake for a few milliseconds. Publishing a sample sends an HCI event, and the RP2040 pays for each event with an SPI exchange. Publishing only on change cuts those events and their SPI exchanges. Neither cost has been measured.

| State | Analogue sampling interval |
|---|---|
| History filling after boot, after unplugging, or after it was cleared | 1 s, about five samples |
| Retry after an unusable sample, at most three in a row | 1 s |
| Burst after a refresh request | Five samples 500 ms apart, then the interval for the state |
| Charger reports charging, charge complete or a fault | 5 s |
| Settled estimate on battery | 10 s |
| On battery after three unusable samples in a row | 10 s |

The radio publishes a sample only when the sample status, the filtered percentage, an analogue error code or a charger register has changed. The raw comparator threshold alone does not count, because it can flip between adjacent steps. The first sample after a refresh request is always published, so the battery key and cable changes get a fresh reading. An unchanged sample is still published before the last published one would pass 65 seconds. On the settled schedule that is every sixth sample, about once a minute. The copies on the halves and the Neuron then stay under the 90-second freshness limit, even after the Neuron's 15-second Bluetooth poll.

A refresh request starts the burst. The battery key and cable connection changes send one from the half, as described below. The five-second charger service also requests one inside the radio when the charger status changes. The gauge path, unused on the tested keyboard, samples every 30 seconds, or every 5 seconds while the charger reports input power.

## Battery display

`&defy_battery` shows stock Dygma's three-cell display. In the template's keymap it is the right lower thumb key second from the outside. The cells are the innermost keys of the top three rows on each half, LED indices 20, 13 and 6 from bottom to top. Cells fill from the bottom. Every other LED, the underglow included, stays dark for the five seconds. The cells run at brightness 128 of 255, against 24 for the start-up display, because at most three LEDs are lit.

| Reading | Five-second display on that half |
|---|---|
| Valid level above 70% | Three green cells |
| Valid level from 41% to 70% | Bottom and middle cells green |
| Valid level from 21% to 40% | Bottom cell green |
| Valid level of 20% or less, including zero | Bottom cell breathes red |
| Pending, missing, stale or invalid reading | Bottom cell steady amber |
| Charging | Cells fill in green from empty, one step every 160 ms, and repeat |
| Charging finished | Three green cells |
| Charger fault | Three red cells |

The red breathing uses stock's curve. One breath takes about four seconds, from 49 up to 128 of 255. Stock lights nothing on a charger fault. This firmware lights all three cells red instead, so a fault does not look like an empty display. The breathing and charging animations update the LEDs only while the display is visible. They stop when it ends or when the profile chooser covers it.

Cable connection changes and the battery key request a refresh with vendor HCI command `0xfff2`. The half's SPI driver writes it to the radio itself, not through the Bluetooth host, whose commands can wait without limit when sent from the system work queue. The driver waits at most 100 ms for the SPI bus, then up to 20 ms each for the write's header and data. A link that is down, or a bus that stays busy, skips the request. The driver drops the radio's command reply. The fresh sample and a short burst follow as battery events.

The display can reuse a filtered estimate no older than one second at once. Otherwise it starts as unavailable and accepts a newer sample taken since the request, also no older than one second. A first estimate after boot or after a charge-state change needs the filter's settling period and may not appear during the first five-second display. An established estimate usually appears sooner. Unplugging a cable does not give a valid percentage at once.

The display restores the previous lighting after five seconds, darkness included if the lighting toggle turned the lights off. It does not reset the lighting or sleep timers. An open display postpones half sleep until it ends. Start-up failure and radio installation displays take priority.

The Neuron sends the display command to each half, preferring the cable and falling back to Bluetooth. A sleeping or disconnected half cannot receive it; wake that half and press the battery key again. Queued display commands expire after one second instead of being replayed after a later connection.

### Low-battery warning

A half warns once when two valid samples in a row read 15% or less. It plays stock Dygma's drain on the same three cells: all three red, then the bottom two, then the bottom one, then none, for 250 ms each. The sequence runs three times, about three seconds in all. The warning arms again only after two samples in a row read 25% or more.

The warning waits for an open battery display or host display to end. It does not show while the lights are off or dark, or during start-up and radio installation displays. A waiting warning shows when the lights come back, if the latest sample still reads 15% or less. Turning the lights off during the warning hides it and stops its updates. It does not reset the lighting or sleep timers.

## Measurement and transport

The radio sampler runs on its own work queue, starting one second after boot. It first tries the read-only gauge path. An address NACK on the first identity read, or an unsupported identity, selects the analogue reader and charger registers for the rest of that radio boot. Other I2C failures stay errors instead of switching sources quietly. The gauge path requires identity `0x4031`, DesignCap `0x1284`, completed initialisation, an in-range state of charge and voltage, and a complete read. It writes no gauge configuration. These checks have simulation coverage, but no working gauge has been seen on the tested keyboard.

Vendor HCI event `0x0002` carries a 28-byte payload with version, status, sequence, age and eight raw words. Buffering keeps the sample age. The RP2040 offers a 32-byte frame with side, session, sequence, age, raw sample and CRC16 through an encrypted read-only Bluetooth characteristic and through idle cable keep-alives. A repeated sample does not renew freshness, and a disconnected route cannot accept a late sample.

The Neuron reads Bluetooth telemetry every 15 seconds on its own work queue. Cable telemetry can replace an idle keep-alive at most once every five seconds, after pending key snapshots are acknowledged. These intervals mean the computer can lag behind the local battery display.

The frames travel on the halves' control service (`battery/defy_control_peripheral.c`, read by the Neuron's `battery/defy_control_ble.c`), which also carries the battery display, lighting and the host overlay. Its UUID is `69a897d6-87cf-4e08-9eb2-973bbf71b000`. The half frame characteristic ends in `b001`; the Neuron's diagnostic characteristics end in `b010` and `b011`. Diagnostics report separately whether a sample is present, connected, fresh and valid. Freshness expires after 90 seconds.

## Reporting to the computer

The Neuron caches each half's last valid percentage and reports the lower one through the Bluetooth Battery Service. It keeps the value through half sleep, disconnection and invalid samples. A charging half is left out until it settles a new estimate, and a finished charge counts as 100%. When both halves charge, the lower earlier value is reported. With nothing cached, or with a non-charging half that has no value, the read returns an ATT error. The cache resets when the Neuron restarts.

This kept value is separate from the live diagnostic flags and from the fresh battery display. A computer may also keep its own cached percentage. macOS has shown the Defy's percentage on the tested keyboard; that does not show it is accurate.

Notifications skip unchanged values for each connection and retry temporary buffer failures up to three times, 250 ms apart. USB HID and Studio have no battery display in this port.

The [half updater](half-updater.md)'s opcode 9 reads the Neuron's cached battery records without a session. It does not reset or wake the halves and does not request a new measurement; the battery key does that. Reading faster than the radio publishes returns the same cached samples. The payload is version 1, 104 bytes, little-endian: an 8-byte header with the version, the record count 2, the record size 48 as a 16-bit value and the Neuron's uptime in milliseconds, then one record per half:

| Bytes | Field |
| --- | --- |
| 0 | Side: 0 left, 1 right |
| 1 | Flags: bit 0 present, bit 1 connected, bit 2 fresh, bit 3 valid |
| 2 | Links the sample is live on: bit 0 cable, bit 1 Bluetooth |
| 3 | Source: 0 unknown, 1 gauge, 2 analogue |
| 4..7 | Sample age in milliseconds, `0xFFFFFFFF` without a sample |
| 8..11 | Validation error, signed, 0 for a valid sample |
| 12 | Level in percent, 255 when not valid |
| 13 | Reserved, 0 |
| 14..15 | Millivolts, `0xFFFF` when not valid |
| 16..47 | The half's 32-byte battery frame, as received |

`defy_battery_central_snapshot()` in `battery/defy_battery_central.c` writes it. For a filtered analogue sample the level is the filtered percentage. While the radio has no estimate, or while the charger reports charging, charge complete, boost or most faults, the record is not valid and its validation error is `-EAGAIN`. So while a half's charge is complete, its record is not valid, although the Battery Service counts that half as 100%. A filtered percentage from 101 to 254 is malformed and gives `-EPROTO`.

## Updating

Routine `left-hybrid` and `right-hybrid` builds keep the installed radio firmware. Battery filter and charger changes live in the radio, so install them with the radio-installing builds:

```sh
scripts/build left-hybrid-radio right-hybrid-radio neuron-hybrid
```

Write the halves through the [half updater](half-updater.md), then the Neuron, [keeping its settings](neuron-flash.md#keeping-the-settings). Each half installs its radio image at its next start, after checking the radio's version; see [radio install](radio-install.md). Radio writes keep Dygma's radio bootloader and S113. The battery format has changed together across the radio, the halves and the Neuron before, so update all of them when the battery code changes.
