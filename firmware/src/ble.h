// Vendor GATT server for image push, Zephyr side.
//
// GATT callbacks run on the Bluetooth RX thread and must not block, so they do
// nothing heavier than handing the bytes to the receiver and setting a flag.
// The main thread waits on ble_wait() and does the CRC pass, the panel refresh
// and the sleeping.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings the stack up. `info` is the 8-byte INFO value, copied. Returns 0 or a
// negative errno from bt_enable().
int ble_begin(const char *name, uint16_t adv_min, uint16_t adv_max, const uint8_t info[8]);

// Starts advertising until a connection; BT_LE_ADV_OPT_CONN does not restart
// it after a disconnect, so the caller does. Returns bt_le_adv_start()'s errno.
int ble_adv_start(void);

bool ble_connected(void);
void ble_disconnect(void);

// Blocks until a GATT write or connection event, or the timeout. Returns true
// if an event arrived.
bool ble_wait(uint32_t timeout_ms);

// k_uptime of the last GATT write or connection, for the session watchdog.
int64_t ble_last_activity(void);

// Pushes the receiver's state to STATUS and notifies a subscribed client.
void ble_publish_status(void);

// Implemented by main.cpp around the shared ImageReceiver; called from the
// Bluetooth RX thread. rx_data returns non-zero on failure so the caller can
// notify only then.
void rx_ctrl(const uint8_t *payload, size_t len);
int rx_data(const uint8_t *payload, size_t len);
void rx_status(uint8_t out[6]);

#ifdef __cplusplus
}
#endif
