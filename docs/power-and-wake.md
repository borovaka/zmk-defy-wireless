# Power, sleep and wake

The halves cut their polling after 30 s without a key change and go into dormant sleep after five idle minutes. A key press wakes a half by restarting it. The Neuron never sleeps: ZMK's own sleep stays off, and it has no key wake source of its own.

Current draw, battery runtime and the delay from a wake key to the first typed key have not been measured.

## Connected idle

After 30 s without a key change and with no key held, a half polls less often:

| Work | Active | Idle |
| --- | ---: | ---: |
| Cable poll, connected | 2 ms | 20 ms |
| Cable probe, nothing connected | 20 ms | 100 ms |
| Transport status check | 20 ms | 100 ms |
| Bluetooth key snapshot | 100 ms | 100 ms |

A real key change wakes the cable thread at once and schedules transport work. Duplicate or invalid events do not count as activity. A held key keeps the half active, also across a transport disable and re-enable. A backend status change schedules transport work at once. The status check never waits past the next Bluetooth snapshot, so idle polling does not slow the snapshots.

`CONFIG_DEFY_HYBRID_IDLE_TIMEOUT_MS` sets the timeout, 30,000 ms by default. `CONFIG_DEFY_HYBRID_IDLE_CABLE_POLL_MS`, `CONFIG_DEFY_HYBRID_IDLE_CABLE_PROBE_MS` and `CONFIG_DEFY_HYBRID_IDLE_STATUS_POLL_MS` set the Idle column. `CONFIG_DEFY_WIRED_POLL_INTERVAL_MS`, `CONFIG_DEFY_HYBRID_CABLE_PROBE_MS` and `CONFIG_DEFY_HYBRID_PERIPHERAL_POLL_MS` set the Active column, and `CONFIG_DEFY_HYBRID_SNAPSHOT_INTERVAL_MS` sets the snapshot interval in both.

A keymap sets these in its `config/defy.conf`, which lists the idle timeout. A change there reaches the halves when `defy-installer update` writes them. A copy to the Neuron's [drive](uf2-loader.md) writes only the Neuron.

## Half sleep

Dormant sleep, `CONFIG_DEFY_HALF_SLEEP`, is on by default. The `*-hybrid-awake` builds turn it off. `CONFIG_DEFY_HALF_SLEEP_TIMEOUT_MS` sets the idle time, 300,000 ms by default. The keymap's `config/defy.conf` lists it, and a change reaches the halves as the [connected idle](#connected-idle) settings do.

The policy checks once a second. A half stays awake while:

- a key is held, or the last key change is newer than the timeout;
- its cable link to the Neuron is up;
- the battery display is showing;
- its last battery sample, if under 90 s old, reports charging or charge complete. The radio services the charger only while the half is awake, and without that service the charger stops charging 20.5 to 32 s later; see [battery](battery.md#charging-stops-when-servicing-stops).

The policy does not try to detect the dock.

### Going to sleep

1. **Bluetooth first.** The half queues a Bluetooth shutdown on the peripheral's service work queue, whose stack the halves' defconfig raises to 1,024 bytes. The shutdown blocks new advertising, keeps references to existing and pending connections, stops advertising and disconnects. Key scanning and cable polling go on meanwhile, and the policy checks progress every 20 ms. Sleep continues only once the worker has finished, the backend is disabled and disconnected, and no connection is connecting, connected or disconnecting.
2. **Transports.** The half stops transport polling and snapshots, waits for a cable transfer in flight and checks for activity again.
3. **Matrix.** It checks that the key scanner is in its idle row and column state and masks interrupts without clearing a pending key edge.
4. **Devices.** It suspends the radio and the LED drivers, then checks that they suspended while the matrix wake stays enabled. The radio must acknowledge its power-off within 80 ms of the start of the power-off write. That budget covers the write, any messages queued ahead of the acknowledgement, and the acknowledgement. The radio powers off once the host has read the acknowledgement, or after 200 ms without it. An acknowledgement read before the deadline counts even if the host finishes handling it later. The 100 ms radio lock timeout limits the wait for the lock, not a transfer that the hardware stalls.
5. **DORMANT.** The RP2040 stops its clocks and waits for a key.

A key press, a cable connection, the battery display or a shutdown error cancels sleep, and so does a one-second deadline on the Bluetooth step. If a Bluetooth command is still in flight, the half stays awake and waits for that worker to finish before it restarts advertising, so a late shutdown step cannot disable a restored connection. A cancel always restarts the backend, even when a failed advertising stop left its enabled flag set. Before the devices suspend, a cancelled attempt restores scanning and transport work. A failure after suspension has begun restarts the half cold rather than leave it half stopped.

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
