# Split connection recovery

The Neuron recovers stalled Bluetooth links to the halves by itself, without a restart and without pairing again. The recovery lives in `drivers/split/defy_hybrid_ble.c` and in the patch to ZMK's split central, [zmk-split-central.patch](../drivers/split/zmk-split-central.patch). Each mechanism below works on its own.

| Problem | Recovery | Limit |
| --- | --- | --- |
| A connected half sends no key reports | Disconnect that half after 10 s | Three disconnect attempts, at least 1 s apart |
| The key subscription is refused for encryption | Retry it once encryption reaches level 2 | Three retries per connection |
| A scan operation or connection attempt fails | Retry the scan after 500 ms | None while split Bluetooth is enabled |
| A connection attempt never completes | Cancel it after 5 s | Cancel retried every second |
| The controller drops a half's advertising report | Restart the scan | One scan stop and start per restart |
| Two halves claim the same side | Hold the rejected half off | 1 s, doubling to 60 s |

The split status shows the per-slot state these act on; see [split diagnostics](split-diagnostics.md).

## Missing key reports

ZMK's split central has no recovery for a key subscription that fails or is removed. Lighting uses a separate GATT path, so working lights do not show that key reports flow.

The Neuron tracks when each split peer last sent a valid key snapshot. Discovery starts a 10 s grace period. Valid all-up snapshots count, and so do valid snapshots kept as the latest state when the event queue is full. Malformed snapshots, a removed subscription and repeated discovery do not.

After 10 s without a valid snapshot, the Neuron asks to disconnect that peer. ZMK's split callbacks then scan again, reconnect, rediscover and subscribe with the existing bond. Healthy peers and host connections are left alone. The stale-key cleanup releases a lost half's keys on its own schedule.

The disconnect runs on ZMK's split command queue, not the system work queue, because HCI command buffers need the system work queue to drain. That queue's stack is 1,024 bytes. A held connection reference and an epoch check protect against slot reuse during the call. A failed disconnect request is retried at most twice, at least one second apart. A new valid report or a new connection rearms the watchdog.

Ten seconds is the detection threshold, not a promise of when the half reconnects. A half that sends valid but wrong all-up snapshots is not detected.

## Subscription refused for encryption

In pinned Zephyr, an ATT `0x0f` (insufficient encryption) error that arrives after local security has reached level 2 makes `att_change_security` return `-EALREADY`. The ATT error handler then passes the failure to the application, and the subscription is lost. This looks like a race with the encryption change.

The Neuron keeps the key characteristic's handle before Zephyr removes the refused subscription. After forwarding the final subscription callback, it restores the handle and repeats the known CCC notification request. If encryption has not reached level 2, it waits for a successful security-change callback first. It retries at most three times per connection.

Each retry checks the connection epoch, the subscription state, the maintenance state and the CCC request. A disconnect discards a pending retry, and the half updater's maintenance mode cancels it. Other errors, exhausted retries and failed allocations fall back to the missing-report watchdog. The retry runs in the Bluetooth callback. In the pinned host, the refused request is freed before the final callback, and allocating a new one from the Bluetooth RX work queue does not wait.

## Scan recovery

The patched central retries 500 ms after a failed scan start, scan stop, connection creation or asynchronous connection completion. A failed attempt releases its reserved slot and clears stale scanning state. An atomic flag keeps scan start and stop from overlapping. Retries stop when split Bluetooth is disabled or both slots are taken, and never interrupt a pending connection.

The retry checks the remaining delay of its work item, not whether the item is pending, because the retry handler itself can be the caller. An error for a scanner that is already running stops the scan before the delayed retry.

## Pending connection attempts

The Neuron builds with controller privacy off, `CONFIG_BT_CTLR_PRIVACY=n` in [the Neuron's defconfig](../boards/dygma/defy_neuron/defy_neuron_nrf52833_zmk_defconfig), which saves about 7 KiB of flash. The controller then has no resolving list. Every bonded device that shared an identity key still counts against that list, and a Mac does. Once the count exceeds the list size, Zephyr sends every connection attempt through a scan first (`rl_entries > rl_size` in Zephyr's `subsys/bluetooth/host/conn.c`). It holds the attempt until the target advertises again and starts its 3 s create timeout only once the controller is initiating.

The halves advertise from fixed addresses, but their attempts take the same path. A half that was seen once and then stopped advertising, because it was switched off or asleep, would leave its slot connecting forever. `start_scanning()` does not scan while any slot is connecting, so the other half could not reconnect either.

`create_connection()` in the patch arms a 5 s timeout for each attempt, which covers a half's advertising interval plus Zephyr's 3 s. When it expires, the Neuron cancels the attempt with `bt_conn_disconnect()`. The cancellation reaches `split_central_connected()` with an error, which releases the slot and retries the scan. A failed cancel is retried every second. A connection that completes just before the timeout is kept.

## Dropped advertising reports

The scan uses `BT_LE_SCAN_PASSIVE`, which turns on the controller's duplicate filter, so each address is reported once per scan. The Neuron drops a report when a half advertises while its old connection still holds the slot, when the report arrives during another scan operation, and while a rejected half is held off. Each of these sets `scan_restart`, and the next `start_scanning()` stops and restarts a running scan, which clears the controller's duplicate list. Turning the filter off would also work, but then every advertisement from every nearby device would reach the host while a half is missing.

## Identity conflicts

Two halves can report the same side, for example with left firmware on both. Without a limit, the Neuron would refuse one after every connection and it would reconnect at once, forever, costing battery and airtime. The Neuron holds the refused slot off instead. The hold starts at 1 s and doubles with each refusal, up to 60 s. It survives reconnects. A refusal more than a minute after the last hold ended starts again at 1 s. ZMK binds each slot to a stored peripheral address, so the hold applies to one half. A [split pairing reset](half-updater.md#split-pairing) empties the slots for new halves and ends both holds, so the next refusal holds for 1 s again. While a hold lasts, the scanner drops that half's reports. When it ends, the scanner restarts the scan. After the firmware is fixed, the half may take up to a minute to reconnect over Bluetooth.

## Disabling split Bluetooth

ZMK's `split_central_bt_set_enabled(false)` disconnects only connected slots. The patch cancels pending attempts too. An attempt whose create call was still running during the disable is cancelled as soon as `bt_conn_le_create()` returns. Advertising reports that arrive while split Bluetooth is disabled are ignored.

## The patch

[zmk_split_central.cmake](../drivers/split/zmk_split_central.cmake) checks the SHA-256 of ZMK's `app/src/split/bluetooth/central.c`, `46455685cbaa16e4508a70fdd27ca5301f0288165efd6b343598eb963c9b0f3b` at ZMK `4dd95d7`. It copies the file into the build directory and applies the patch with `git apply`, so the ZMK checkout stays unchanged. Any other version of the file stops the build until the patch is reviewed. Every change carries a `Defy:` comment, except the added `<zephyr/sys/atomic.h>` include and the call to `retry_scanning()` in place of `start_scanning()` after a failed connection. Besides the recovery above, the patch:

- sends the key subscription and discovery through the hybrid central, which follows them for its health check;
- hands each peripheral event to the hybrid central, which maps the slot to its side;
- initialises the split central one priority after ZMK's Bluetooth, at `UTIL_INC(CONFIG_ZMK_BLE_INIT_PRIORITY)`. The patched copy is a module source, and module sources link first. At the same priority it would register its settings before Bluetooth set up the settings subsystem, which would discard them.

`tests/drivers/defy_ble_scan` runs the patched scanner against a fake controller that filters duplicates, one report per address per scan start. `tests/drivers/defy_hybrid_central` covers the watchdog and the encryption retry.
