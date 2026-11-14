/*
 * zmk-autoswitch over Bluetooth LE: a second HID service (next to ZMK's own) carrying the HID++
 * collection. Over HID-over-GATT a report travels without its ID byte (the Report Reference
 * descriptor says which report it is), so this side adds and strips it.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "autoswitch.h"

LOG_MODULE_DECLARE(zmk_autoswitch, CONFIG_ZMK_LOG_LEVEL);

#define REPORT_BODY_LEN (AS_HIDPP_LEN - 1)

struct hids_info {
    uint16_t version;
    uint8_t code;
    uint8_t flags;
} __packed;

struct hids_report {
    uint8_t id;
    uint8_t type;
} __packed;

enum {
    HIDS_REMOTE_WAKE = BIT(0),
    HIDS_NORMALLY_CONNECTABLE = BIT(1),
};

enum {
    HIDS_INPUT = 0x01,
    HIDS_OUTPUT = 0x02,
};

static const struct hids_info info = {
    .version = 0x0000,
    .code = 0x00,
    .flags = HIDS_NORMALLY_CONNECTABLE | HIDS_REMOTE_WAKE,
};

static const struct hids_report input_ref = {.id = AS_HIDPP_REPORT_ID, .type = HIDS_INPUT};
static const struct hids_report output_ref = {.id = AS_HIDPP_REPORT_ID, .type = HIDS_OUTPUT};

static uint8_t ctrl_point;

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                         uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_info));
}

static ssize_t read_report_map(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                               uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, as_report_desc, as_report_desc_len);
}

static ssize_t read_report_ref(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                               uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attr, buf, len, offset, attr->user_data,
                             sizeof(struct hids_report));
}

/* Reading a report characteristic directly returns an empty report. */
static ssize_t read_report(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset) {
    static const uint8_t empty[REPORT_BODY_LEN];
    return bt_gatt_attr_read(conn, attr, buf, len, offset, empty, sizeof(empty));
}

static ssize_t write_report(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                            const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    const uint8_t *data = buf;
    uint16_t body = len;
    if (body == AS_HIDPP_LEN && data[0] == AS_HIDPP_REPORT_ID) {
        data++; /* tolerate a host that sends the ID too */
        body--;
    }
    if (body > REPORT_BODY_LEN) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    uint8_t report[AS_HIDPP_LEN] = {AS_HIDPP_REPORT_ID};
    memcpy(&report[1], data, body);
    as_hidpp_received(AS_TRANSPORT_BLE, conn, report);
    return len;
}

static ssize_t write_ctrl_point(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                                const void *buf, uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset + len > sizeof(ctrl_point)) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    memcpy((uint8_t *)attr->user_data + offset, buf, len);
    return len;
}

/* Attribute indexes: 0 service, 1-2 info, 3-4 report map, 5-6 input report (5 is what notify
 * takes), 7 its CCC, 8 its report reference, 9-10 output report, 11 its report reference,
 * 12-13 control point. */
BT_GATT_SERVICE_DEFINE(
    as_hids, BT_GATT_PRIMARY_SERVICE(BT_UUID_HIDS),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_info,
                           NULL, (void *)&info),
    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT_MAP, BT_GATT_CHRC_READ, BT_GATT_PERM_READ_ENCRYPT,
                           read_report_map, NULL, NULL),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                           BT_GATT_PERM_READ_ENCRYPT, read_report, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL,
                       (void *)&input_ref),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_REPORT,
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_report,
                           write_report, NULL),
    BT_GATT_DESCRIPTOR(BT_UUID_HIDS_REPORT_REF, BT_GATT_PERM_READ_ENCRYPT, read_report_ref, NULL,
                       (void *)&output_ref),

    BT_GATT_CHARACTERISTIC(BT_UUID_HIDS_CTRL_POINT, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl_point, &ctrl_point));

void as_ble_send(struct bt_conn *conn, const uint8_t *report) {
    if (conn == NULL) {
        return;
    }
    struct bt_gatt_notify_params params = {
        .attr = &as_hids.attrs[5],
        .data = &report[1],
        .len = REPORT_BODY_LEN,
    };
    int err = bt_gatt_notify_cb(conn, &params);
    if (err) {
        LOG_WRN("AutoSwitch: sending the reply over Bluetooth failed: %d", err);
    }
}
