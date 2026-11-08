# Power, sleep and wake

The halves cut their polling after 30 s without a key change and go into dormant sleep after five idle minutes. A key press wakes a half by restarting it. The Neuron never sleeps: ZMK's own sleep stays off, and it has no key wake source of its own. Once a half has been gone a minute, the Neuron's [scan for it](ble-input-recovery.md#scan-pace) listens about 3% of the time instead of half. With no half connected, its cable worker and its split service wait for an event instead of waking every 10 ms and 100 ms. On the dock the halves sleep even with the cable link up; see [on the dock](#on-the-dock).

Current draw, battery runtime and the delay from a wake key to the first typed key have not been measured.

## Connected idle

After 30 s without a key change and with no key held, a half polls less often:

| Work | Active | Idle |
| --- | ---: | ---: |
| Cable poll, connected | 2 ms | 20 ms |
| Cable probe, nothing connected | 20 ms | 100 ms |
| Transport status check | 20 ms | 100 ms |
| Bluetooth key snapshot | 100 ms | 400 ms |

A real key change wakes the cable thread at once and schedules transport work. Duplicate or invalid events do not count as activity. A held key keeps the half active, also across a transport disable and re-enable. A backend status change schedules transport work at once. The status check never waits past the next Bluetooth snapshot. The first key change after idle brings the next snapshot forward to within 100 ms, so a lost notification of it is put right as quickly as before. Idle snapshots stay inside the Neuron's 1,000 ms Bluetooth timeout even when one is lost.

`CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS` sets the timeout, 30,000 ms by default. `CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS`, `CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS` and `CONFIG_DEFY_HYBRID_IDLE_STATUS_POLL_MS` set the Idle column. `CONFIG_DEFY_WIRED_POLL_INTERVAL_MS`, `CONFIG_DEFY_HYBRID_CABLE_PROBE_MS` and `CONFIG_DEFY_HYBRID_PERIPHERAL_POLL_MS` set the Active column. `CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS` and `CONFIG_DEFY_HYBRID_IDLE_SNAPSHOT_INTERVAL_MS` set the snapshot row.

A keymap sets these in its `config/defy.conf`, which lists the idle timeout. A change there reaches the halves when `defy-installer update` writes them. A copy to the Neuron's [drive](uf2-loader.md) writes only the Neuron.

## Clocks

Whenever a half is awake, active or idle, it runs the RP2040's system and peripheral clocks at 48 MHz instead of Zephyr's 125 MHz, with the system PLL's VCO at 768 MHz (`boards/dygma/defy/defy.dtsi`). The RP2040 datasheet's current figures grow roughly with the clock, so an awake half draws less; this has not been measured. The SPI links at 1 and 2 MHz and the underglow's 8 MHz PIO cycles divide 48 MHz evenly, and SysTick follows clk_sys through `CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC`.

The halves also turn off the RP2040's USB PLL, at 48 MHz, and the USB, ADC and RTC clocks it drives, once Zephyr's clock driver has started them. The halves use none of them. `CONFIG_DEFY_RP2040_USB_CLOCKS_OFF` is on by default and off in a build that enables USB, the ADC, a counter or the RTC.

## Half sleep

Dormant sleep, `CONFIG_DEFY_HALF_SLEEP`, is on by default. The `*-hybrid-awake` builds turn it off. `CONFIG_DEFY_HALF_SLEEP_TIMEOUT_MS` sets the idle time, 300,000 ms by default. The keymap's `config/defy.conf` lists it, and a change reaches the halves as the [connected idle](#connected-idle) settings do.

The policy checks once a second. A half stays awake while:

- a key is held, or the last key change is newer than the timeout;
- its cable link to the Neuron is up, unless the Neuron [runs on battery](#on-the-dock);
- the battery display is showing;
- its last battery sample, if under 90 s old, reports charging or charge complete. The radio services the charger only while the half is awake, and without that service the charger stops charging 20.5 to 32 s later; see [battery](battery.md#charging-stops-when-servicing-stops).

### On the dock

The dock powers the Neuron from the left half's battery. Stock firmware reaches the docked left half over the cable lines, so its cable link can be up while docked. The Neuron counts as running on battery once its place has read docked or alone, with no USB host, for 5 s; see [cable and dock detection](automatic-transports.md#cable-and-dock-detection). It stops at the first sample, every 250 ms, that finds a computer, or a USB charger with both cables in or none. The 5 s wait covers the Neuron's start, when it reads alone for up to about a second even on a computer. Bit 1 of the [lighting state's flags](automatic-transports.md#lighting-messages) carries the result to both halves, whether the lights are on or off.

While a half's last lighting state says the Neuron runs on battery, its cable does not keep it awake, and the half sleeps after its idle time as on Bluetooth alone. Bluetooth stops first while the cable keeps carrying keys. If the Neuron has by then acknowledged every key snapshot sent on the cable, the half stops polling it, and the Neuron drops the silent cable 250 ms later. Otherwise the half cancels the attempt, restarts Bluetooth and tries again at the policy's next check, a second later. A half that has no lighting state yet, as just after it wakes, stays awake for its cable. The flag travels only in the lighting state. A half therefore also stays awake for its cable when either build has `CONFIG_DEFY_LIGHTING` off, when the Neuron's build has `CONFIG_DEFY_NEURON_DOCK` off, or when the halves' and the Neuron's lighting versions differ, as they can after copying only a new Neuron image to the [drive](uf2-loader.md); see [matching builds](lighting.md#matching-builds).

A half asleep on its cable wakes only on a key, also after the Neuron reaches a computer or a USB charger, and does not charge until then. One cable with the Neuron on a USB charger reads as docked too; that half still stays awake while its charger reports charging or charge complete. On the keyboard, the Neuron kept its power from the left battery through a 12-hour night while the docked left half slept.

### Going to sleep

1. **Bluetooth first.** The half queues a Bluetooth shutdown on the peripheral's service work queue, whose stack the halves' defconfig raises to 1,024 bytes. The shutdown blocks new advertising, keeps references to existing and pending connections, stops advertising and disconnects. Key scanning and cable polling go on meanwhile, and the policy checks progress every 20 ms. Sleep continues only once the worker has finished, the backend is disabled and disconnected, and no connection is connecting, connected or disconnecting.
2. **Transports.** The half stops transport polling and snapshots, waits for a cable transfer in flight and checks for activity again.
3. **Matrix.** It checks that the key scanner is in its idle row and column state and masks interrupts without clearing a pending key edge.
4. **Devices.** It suspends the radio and the LED drivers, then checks that they suspended while the matrix wake stays enabled. The radio must acknowledge its power-off within 80 ms of the start of the power-off write. That budget covers the write, any messages queued ahead of the acknowledgement, and the acknowledgement. The radio powers off once the host has read the acknowledgement, or after 200 ms without it. An acknowledgement read before the deadline counts even if the host finishes handling it later. The 100 ms radio lock timeout limits the wait for the lock, not a transfer that the hardware stalls.
5. **DORMANT.** The RP2040 stops its clocks and waits for a key.

A key press, a cable connection while the Neuron does not run on battery, the battery display or a shutdown error cancels sleep, and so does a one-second deadline on the Bluetooth step. If a Bluetooth command is still in flight, the half stays awake and waits for that worker to finish before it restarts advertising, so a late shutdown step cannot disable a restored connection. A cancel always restarts the backend, even when a failed advertising stop left its enabled flag set. Before the devices suspend, a cancelled attempt restores scanning and transport work. A failure after suspension has begun restarts the half cold rather than leave it half stopped.

The advertising gate shares the identity wrapper's mutex, so an advertising start cannot slip past the gate and survive the stop. Sleep therefore requires `CONFIG_DEFY_BT_PERSISTENT_ID`. Disconnecting first lets the Neuron free the half's slot at once. Without it, the Neuron would wait out its four-second supervision timeout before accepting the half again.

### Waking

Dormant wake watches the eight matrix columns for a low level and a falling edge. A key pressed during the shutdown stays latched through the clock change. Waking restarts the half, which then reconnects. A short first tap can be lost; wait for the reconnection before typing. Plugging in a cable has not been confirmed as a wake source.

## USB report recovery

The Neuron recovers held keys and mouse buttons across a USB suspend. `drivers/usb/defy_usb_report_recovery.c` wraps ZMK's USB keyboard and consumer report senders, its mouse report sender when `CONFIG_ZMK_POINTING` is on, `usb_wakeup_request` and `usb_enable`. `CONFIG_DEFY_USB_REPORT_RECOVERY` enables it on Neuron builds with USB, without boot protocol.

- **After a resume**, the Neuron sends the current keyboard, consumer and mouse reports again. That covers modifiers held before the suspend and keys released during it. It keeps the current state, not a queue of past taps, so a short wake tap can be lost.
- **Failed sends.** A send that fails with `-EAGAIN` or `-EIO` is retried after 5 ms, with the delay doubling up to 80 ms. A successful send resets the delay.
- **Remote wake.** ZMK's own wake requests are blocked. The Neuron asks the host to wake only when a half reports a newly pressed key after the suspend, while USB is the selected output and configured. It tries at most three times, 100 ms apart, and stops when the host refuses.
- **Generations.** A send that spans a suspend and resume cannot cancel the refresh that the resume needs, because a real resume changes the acceptance generation. A USB reset, disconnect or error drops pending recovery. A reset and re-enumeration is not a resume, so a key held across one may need a release and a new press.
- **Mouse reports.** ZMK sends each movement or scroll step once and then clears it from the report, so a resend carries the mouse buttons alone. Steps lost to a suspend or a failed send are not replayed. A resend that interrupts ZMK's input thread between setting a step and clearing it sends that step a second time. Mouse keys wake the host as other keys do. The input thread sends mouse reports, while keyboard reports and output changes run on the system work queue. If the output leaves USB while a mouse send is on its way, that report can reach the old host after its release, so the Neuron sends the host one more mouse release.
- **Output switch.** Switching output drops pending USB reports. If the old USB host is still configured, fixed all-up reports in RAM release its keys and mouse buttons. Switching back cancels that release before new input. ZMK's HID senders, completion callback and semaphore stay in use. Endpoint changes and the retry worker share the system work queue, and the pinned Nordic USB driver refuses a write while another transfer is active.

On the tested Mac, holding Shift to wake the computer and typing while still holding it produced lowercase, although the Neuron's reports carried the Shift modifier. Release the wake key and press it again before using it as a modifier.

`CONFIG_DEFY_USB_REPORT_TRACE` and `CONFIG_DEFY_USB_POSITION_TRACE` log USB status, endpoint changes, report modifiers and key positions. `neuron-hybrid-log` turns both on through the `defy-ble-debug` snippet. ZMK's verbose log can include keycodes, so capture only controlled tests.
