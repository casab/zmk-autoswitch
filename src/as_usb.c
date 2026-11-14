/*
 * zmk-autoswitch over USB: a second HID interface (ZMK's own is HID_0) carrying the HID++
 * collection. Needs CONFIG_USB_HID_DEVICE_COUNT >= 2 (the module's Kconfig defaults it to 2).
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/usb/usb_device.h>

#include "autoswitch.h"

LOG_MODULE_DECLARE(zmk_autoswitch, CONFIG_ZMK_LOG_LEVEL);

#define HID_REPORT_TYPE_INPUT 0x01
#define HID_REPORT_TYPE_OUTPUT 0x02

static const struct device *hid_dev;

static K_SEM_DEFINE(in_sem, 1, 1);

static void in_ready_cb(const struct device *dev) { k_sem_give(&in_sem); }

static int get_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    static uint8_t empty[AS_HIDPP_LEN] = {AS_HIDPP_REPORT_ID};
    if ((setup->wValue >> 8) != HID_REPORT_TYPE_INPUT ||
        (setup->wValue & 0xFF) != AS_HIDPP_REPORT_ID) {
        return -ENOTSUP;
    }
    *data = empty;
    *len = sizeof(empty);
    return 0;
}

static int set_report_cb(const struct device *dev, struct usb_setup_packet *setup, int32_t *len,
                         uint8_t **data) {
    if ((setup->wValue >> 8) != HID_REPORT_TYPE_OUTPUT) {
        return -ENOTSUP;
    }
    const uint8_t *d = *data;
    int32_t n = *len;
    if (n > 0 && d[0] == AS_HIDPP_REPORT_ID) {
        d++; /* with report IDs, the data stage starts with the ID */
        n--;
    }
    uint8_t report[AS_HIDPP_LEN] = {AS_HIDPP_REPORT_ID};
    memcpy(&report[1], d, MIN(MAX(n, 0), AS_HIDPP_LEN - 1));
    as_hidpp_received(AS_TRANSPORT_USB, NULL, report);
    return 0;
}

static const struct hid_ops ops = {
    .int_in_ready = in_ready_cb,
    .get_report = get_report_cb,
    .set_report = set_report_cb,
};

void as_usb_send(const uint8_t *report) {
    if (hid_dev == NULL) {
        return;
    }
    if (k_sem_take(&in_sem, K_MSEC(30)) != 0) {
        LOG_WRN("AutoSwitch: previous USB reply still in flight");
    }
    /* Static: the USB controller reads it after this returns; in_sem guards it until then. */
    static uint8_t buf[AS_HIDPP_LEN];
    memcpy(buf, report, sizeof(buf));
    int err = hid_int_ep_write(hid_dev, buf, sizeof(buf), NULL);
    if (err) {
        LOG_WRN("AutoSwitch: sending the reply over USB failed: %d", err);
        k_sem_give(&in_sem);
    }
}

static int as_usb_init(void) {
    hid_dev = device_get_binding("HID_1");
    if (hid_dev == NULL) {
        LOG_ERR("AutoSwitch: no second USB HID device (CONFIG_USB_HID_DEVICE_COUNT must be 2+)");
        return -ENODEV;
    }
    usb_hid_register_device(hid_dev, as_report_desc, as_report_desc_len, &ops);
    usb_hid_init(hid_dev);
    return 0;
}

SYS_INIT(as_usb_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
