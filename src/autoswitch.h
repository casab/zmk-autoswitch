/*
 * zmk-autoswitch internals shared by the HID++ handler and the transports.
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <zephyr/bluetooth/conn.h>

/* HID++ long report: report ID 0x11, then device index, feature index, function | software ID and
 * 16 parameter bytes. Logitech's Bluetooth LE devices use only this one. */
#define AS_HIDPP_REPORT_ID 0x11
#define AS_HIDPP_LEN 20

enum as_transport {
    AS_TRANSPORT_BLE,
    AS_TRANSPORT_USB,
};

/* The vendor collection both transports present: usage page 0xFF43, usage 0x0202, report 0x11
 * (19 bytes after the ID) in and out. */
extern const uint8_t as_report_desc[];
extern const size_t as_report_desc_len;

/* A transport received a long report (report ID first, AS_HIDPP_LEN bytes). `conn` is the
 * Bluetooth connection it came from (BLE) or NULL (USB). Callable from any thread: the request
 * is handled on the system work queue. */
void as_hidpp_received(enum as_transport transport, struct bt_conn *conn, const uint8_t *report);

/* Send a reply (report ID first, AS_HIDPP_LEN bytes) back the way the request came. */
void as_ble_send(struct bt_conn *conn, const uint8_t *report);
void as_usb_send(const uint8_t *report);
