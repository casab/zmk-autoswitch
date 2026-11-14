/*
 * zmk-autoswitch: a small HID++ 2.0 device, so the AutoSwitch KVM app (or any HID++ host) can read
 * and change this keyboard's Bluetooth profile the way it does on a Logitech Easy-Switch keyboard.
 *
 * Features (index: id, version):
 *
 *   0: 0x0000 Root         [0] getFeature(id hi, id lo) -> index, type, version (index 0: absent)
 *                          [1] getProtocolVersion(0, 0, ping) -> 4, 2, ping
 *   1: 0x0001 FeatureSet   [0] getCount -> features after the root
 *                          [1] getFeatureId(index) -> id hi, id lo, type, version
 *   2: 0x0005 DeviceName   [0] getNameLength  [1] getName(offset) -> up to 16 bytes  [2] getType -> 0
 *   3: 0x1814 ChangeHost   [0] getHostInfo -> host count, current host, flags (0)
 *      v1                  [1] setCurrentHost(host): replies, then switches
 *                              CONFIG_ZMK_AUTOSWITCH_SELECT_DELAY_MS later
 *                          [2] getCookies -> one byte per host
 *                          [3] setCookie(host, cookie)
 *   4: 0x1815 HostsInfo    [0] getFeatureInfo -> caps (bit 0 names readable, bit 1 writable),
 *                              0, host count, current host
 *                          [1] getHostInfo(host) -> host, status (1 paired), bus 0, pages 0,
 *                              name length, name max length
 *                          [3] getHostFriendlyName(host, offset) -> host, offset, up to 14 bytes
 *                          [4] setHostFriendlyName(host, offset, up to 14 bytes) -> host, offset
 *                          host 0xFF means the current one.
 *
 * Hosts are the ZMK Bluetooth profiles. A profile's cookie and name are stored with the address
 * of the computer it was bonded to when they were written, and read as empty once the profile is
 * bonded to another computer. Writing either for an unpaired profile is an invalid argument.
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include <zmk/ble.h>

#include "autoswitch.h"

LOG_MODULE_REGISTER(zmk_autoswitch, CONFIG_ZMK_LOG_LEVEL);

#define AS_MAX_HOSTS 16 /* getCookies fits 16 in one reply */
#define AS_HOSTS MIN(ZMK_BLE_PROFILE_COUNT, AS_MAX_HOSTS)
#define AS_NAME_MAX 32
#define AS_CURRENT_HOST 0xFF

/* HID++ 2.0 error codes */
#define ERR_NONE 0x00
#define ERR_INVALID_ARGUMENT 0x02
#define ERR_OUT_OF_RANGE 0x03
#define ERR_INVALID_FEATURE_INDEX 0x06
#define ERR_INVALID_FUNCTION 0x07

const uint8_t as_report_desc[] = {
    0x06, 0x43, 0xFF, /* Usage Page (Vendor 0xFF43) */
    0x0A, 0x02, 0x02, /* Usage (0x0202) */
    0xA1, 0x01,       /* Collection (Application) */
    0x85, AS_HIDPP_REPORT_ID,
    0x75, 0x08,             /*   Report Size (8) */
    0x95, AS_HIDPP_LEN - 1, /*   Report Count (19) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x26, 0xFF, 0x00,       /*   Logical Maximum (255) */
    0x09, 0x02,             /*   Usage (0x02) */
    0x81, 0x00,             /*   Input (Data, Array, Absolute) */
    0x09, 0x02,             /*   Usage (0x02) */
    0x91, 0x00,             /*   Output (Data, Array, Absolute) */
    0xC0,                   /* End Collection */
};
const size_t as_report_desc_len = sizeof(as_report_desc);

enum {
    IDX_ROOT,
    IDX_FEATURE_SET,
    IDX_DEVICE_NAME,
    IDX_CHANGE_HOST,
    IDX_HOSTS_INFO,
    IDX_COUNT,
};

static const struct {
    uint16_t id;
    uint8_t type;
    uint8_t version;
} features[IDX_COUNT] = {
    [IDX_ROOT] = {0x0000, 0, 0},        [IDX_FEATURE_SET] = {0x0001, 0, 0},
    [IDX_DEVICE_NAME] = {0x0005, 0, 0}, [IDX_CHANGE_HOST] = {0x1814, 0, 1},
    [IDX_HOSTS_INFO] = {0x1815, 0, 0},
};

/* ---- Per-host data, kept in settings ---- */

struct as_host {
    bt_addr_le_t addr; /* the bond this entry belongs to */
    uint8_t cookie;
    uint8_t name_len;
    uint8_t name[AS_NAME_MAX];
};

static struct as_host hosts[AS_HOSTS];

/* Protocol v1 (module 1.x) stored only marks; they become cookies. */
struct as_v1_mark {
    bt_addr_le_t addr;
    uint8_t value;
};

static struct as_v1_mark v1_marks[AS_HOSTS];
static bool hosts_loaded, v1_loaded;

/* Host data is only written after a host changed it (setCookie, setHostFriendlyName), never
 * while settings load at boot: 2.0's first build saved from the settings commit and the keyboard
 * didn't start. The delay folds a cookie and a name written together into one flash write. */
#define AS_SAVE_DELAY K_SECONDS(1)

static void save_work_handler(struct k_work *work) {
    int err = settings_save_one("autoswitch/hosts", hosts, sizeof(hosts));
    if (err < 0) {
        LOG_ERR("AutoSwitch: saving host data failed: %d", err);
        return;
    }
    if (v1_loaded) {
        settings_delete("autoswitch/marks"); /* migrated into the data just saved */
        v1_loaded = false;
    }
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

static void schedule_save(void) { k_work_reschedule(&save_work, AS_SAVE_DELAY); }

static int as_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "hosts", &next) && !next) {
        if (len != sizeof(hosts)) {
            LOG_WRN("AutoSwitch: ignoring saved host data (%d bytes, expected %d)", len,
                    sizeof(hosts));
            return 0;
        }
        int rc = read_cb(cb_arg, hosts, sizeof(hosts));
        hosts_loaded = rc >= 0;
        return rc < 0 ? rc : 0;
    }
    if (settings_name_steq(name, "marks", &next) && !next) {
        if (len == sizeof(v1_marks)) {
            v1_loaded = read_cb(cb_arg, v1_marks, sizeof(v1_marks)) >= 0;
        }
        return 0;
    }
    return -ENOENT;
}

/* In memory only: nothing is written or scheduled while settings load. The migrated cookies are
 * saved (and the 1.x key deleted) with the first change a host makes. */
static int as_settings_commit(void) {
    if (v1_loaded && !hosts_loaded) {
        for (int i = 0; i < AS_HOSTS; i++) {
            bt_addr_le_copy(&hosts[i].addr, &v1_marks[i].addr);
            hosts[i].cookie = v1_marks[i].value;
        }
        hosts_loaded = true; /* a later commit (settings_load_subtree) doesn't redo it */
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_autoswitch, "autoswitch", NULL, as_settings_set,
                               as_settings_commit, NULL);

static bool host_paired(int h) { return !zmk_ble_profile_is_open(h); }

/* The entry for host h, if it still belongs to the computer that profile is bonded to. */
static const struct as_host *host_entry(int h) {
    if (!host_paired(h)) {
        return NULL;
    }
    const bt_addr_le_t *bonded = zmk_ble_profile_address(h);
    if (bonded == NULL || bt_addr_le_cmp(bonded, &hosts[h].addr) != 0) {
        return NULL;
    }
    return &hosts[h];
}

/* The entry for host h, taken over for its current bond (cleared if it belonged to another).
 * NULL for an unpaired profile. */
static struct as_host *claim_host(int h) {
    if (!host_paired(h)) {
        return NULL;
    }
    const bt_addr_le_t *bonded = zmk_ble_profile_address(h);
    if (bonded == NULL) {
        return NULL;
    }
    if (bt_addr_le_cmp(bonded, &hosts[h].addr) != 0) {
        memset(&hosts[h], 0, sizeof(hosts[h]));
        bt_addr_le_copy(&hosts[h].addr, bonded);
    }
    return &hosts[h];
}

static uint8_t current_host(void) {
    int active = zmk_ble_active_profile_index();
    return active < 0 ? 0 : (uint8_t)active;
}

/* 0..AS_HOSTS-1, or -1 */
static int resolve_host(uint8_t h) {
    if (h == AS_CURRENT_HOST) {
        return current_host();
    }
    return h < AS_HOSTS ? h : -1;
}

/* ---- Profile switch, deferred so the reply leaves first ---- */

static uint8_t pending_profile;

static void select_work_handler(struct k_work *work) {
    LOG_INF("AutoSwitch: selecting BLE profile %d", pending_profile);
    int err = zmk_ble_prof_select(pending_profile);
    if (err < 0) {
        LOG_ERR("AutoSwitch: selecting profile %d failed: %d", pending_profile, err);
    }
}

static K_WORK_DELAYABLE_DEFINE(select_work, select_work_handler);

/* ---- Features. Each fills `out` (16 bytes, zeroed) and returns an error code. ---- */

static uint8_t root(uint8_t fn, const uint8_t *p, uint8_t *out) {
    switch (fn) {
    case 0: {
        uint16_t id = (p[0] << 8) | p[1];
        for (int i = 0; i < IDX_COUNT; i++) {
            if (features[i].id == id) {
                out[0] = i;
                out[1] = features[i].type;
                out[2] = features[i].version;
                break;
            }
        }
        return ERR_NONE; /* not found: index 0 */
    }
    case 1:
        out[0] = 4;
        out[1] = 2;
        out[2] = p[2];
        return ERR_NONE;
    default:
        return ERR_INVALID_FUNCTION;
    }
}

static uint8_t feature_set(uint8_t fn, const uint8_t *p, uint8_t *out) {
    switch (fn) {
    case 0:
        out[0] = IDX_COUNT - 1;
        return ERR_NONE;
    case 1:
        if (p[0] >= IDX_COUNT) {
            return ERR_OUT_OF_RANGE;
        }
        out[0] = features[p[0]].id >> 8;
        out[1] = features[p[0]].id & 0xFF;
        out[2] = features[p[0]].type;
        out[3] = features[p[0]].version;
        return ERR_NONE;
    default:
        return ERR_INVALID_FUNCTION;
    }
}

static uint8_t device_name(uint8_t fn, const uint8_t *p, uint8_t *out) {
    static const char name[] = CONFIG_ZMK_KEYBOARD_NAME;
    const size_t len = sizeof(name) - 1;
    switch (fn) {
    case 0:
        out[0] = len;
        return ERR_NONE;
    case 1:
        if (p[0] > len) {
            return ERR_OUT_OF_RANGE;
        }
        memcpy(out, &name[p[0]], MIN(16, len - p[0]));
        return ERR_NONE;
    case 2:
        out[0] = 0; /* keyboard */
        return ERR_NONE;
    default:
        return ERR_INVALID_FUNCTION;
    }
}

static uint8_t change_host(uint8_t fn, const uint8_t *p, uint8_t *out) {
    switch (fn) {
    case 0:
        out[0] = AS_HOSTS;
        out[1] = current_host();
        out[2] = 0;
        return ERR_NONE;
    case 1:
        if (p[0] >= AS_HOSTS) {
            return ERR_INVALID_ARGUMENT;
        }
        pending_profile = p[0];
        k_work_reschedule(&select_work, K_MSEC(CONFIG_ZMK_AUTOSWITCH_SELECT_DELAY_MS));
        return ERR_NONE;
    case 2:
        for (int i = 0; i < AS_HOSTS; i++) {
            const struct as_host *e = host_entry(i);
            out[i] = e ? e->cookie : 0;
        }
        return ERR_NONE;
    case 3: {
        struct as_host *e = p[0] < AS_HOSTS ? claim_host(p[0]) : NULL;
        if (e == NULL) {
            return ERR_INVALID_ARGUMENT;
        }
        if (e->cookie != p[1]) {
            e->cookie = p[1];
            schedule_save();
        }
        return ERR_NONE;
    }
    default:
        return ERR_INVALID_FUNCTION;
    }
}

static uint8_t hosts_info(uint8_t fn, const uint8_t *p, uint8_t *out) {
    switch (fn) {
    case 0:
        out[0] = 0x03; /* names readable and writable */
        out[1] = 0;
        out[2] = AS_HOSTS;
        out[3] = current_host();
        return ERR_NONE;
    case 1: {
        int h = resolve_host(p[0]);
        if (h < 0) {
            return ERR_INVALID_ARGUMENT;
        }
        const struct as_host *e = host_entry(h);
        out[0] = h;
        out[1] = host_paired(h) ? 1 : 0;
        out[2] = 0; /* bus type: not reported */
        out[3] = 0; /* descriptor pages */
        out[4] = e ? e->name_len : 0;
        out[5] = AS_NAME_MAX;
        return ERR_NONE;
    }
    case 3: {
        int h = resolve_host(p[0]);
        if (h < 0) {
            return ERR_INVALID_ARGUMENT;
        }
        const struct as_host *e = host_entry(h);
        uint8_t len = e ? e->name_len : 0;
        out[0] = h;
        out[1] = p[1];
        if (p[1] < len) {
            memcpy(&out[2], &e->name[p[1]], MIN(14, len - p[1]));
        }
        return ERR_NONE;
    }
    case 4: {
        int h = resolve_host(p[0]);
        uint8_t offset = p[1];
        struct as_host *e = h < 0 ? NULL : claim_host(h);
        if (e == NULL) {
            return ERR_INVALID_ARGUMENT;
        }
        if (offset >= AS_NAME_MAX || offset > (offset == 0 ? 0 : e->name_len)) {
            return ERR_OUT_OF_RANGE; /* chunks are written in order, from offset 0 */
        }
        uint8_t n = MIN(strnlen((const char *)&p[2], 14), AS_NAME_MAX - offset);
        uint8_t name[AS_NAME_MAX] = {0};
        memcpy(name, e->name, offset);
        memcpy(&name[offset], &p[2], n);
        if (e->name_len != offset + n || memcmp(e->name, name, sizeof(name)) != 0) {
            memcpy(e->name, name, sizeof(name));
            e->name_len = offset + n;
            schedule_save();
        }
        out[0] = h;
        out[1] = offset;
        return ERR_NONE;
    }
    default:
        return ERR_INVALID_FUNCTION;
    }
}

/* ---- Requests: handled on the system work queue, one at a time ---- */

static struct {
    enum as_transport transport;
    struct bt_conn *conn;
    uint8_t report[AS_HIDPP_LEN];
} request;

static atomic_t busy;

static void request_work_handler(struct k_work *work) {
    const uint8_t *req = request.report;
    static uint8_t reply[AS_HIDPP_LEN];
    uint8_t out[16] = {0};
    uint8_t fn = req[3] >> 4;
    uint8_t err;

    switch (req[2]) {
    case IDX_ROOT:
        err = root(fn, &req[4], out);
        break;
    case IDX_FEATURE_SET:
        err = feature_set(fn, &req[4], out);
        break;
    case IDX_DEVICE_NAME:
        err = device_name(fn, &req[4], out);
        break;
    case IDX_CHANGE_HOST:
        err = change_host(fn, &req[4], out);
        break;
    case IDX_HOSTS_INFO:
        err = hosts_info(fn, &req[4], out);
        break;
    default:
        err = ERR_INVALID_FEATURE_INDEX;
        break;
    }

    memset(reply, 0, sizeof(reply));
    reply[0] = AS_HIDPP_REPORT_ID;
    reply[1] = req[1];
    if (err == ERR_NONE) {
        reply[2] = req[2];
        reply[3] = req[3];
        memcpy(&reply[4], out, sizeof(out));
    } else {
        reply[2] = 0xFF;
        reply[3] = req[2];
        reply[4] = req[3];
        reply[5] = err;
        LOG_DBG("AutoSwitch: feature %d function %d: error %d", req[2], fn, err);
    }

    if (request.transport == AS_TRANSPORT_BLE) {
        as_ble_send(request.conn, reply);
    }
#if IS_ENABLED(CONFIG_ZMK_AUTOSWITCH_USB)
    else {
        as_usb_send(reply);
    }
#endif
    if (request.conn != NULL) {
        bt_conn_unref(request.conn);
        request.conn = NULL;
    }
    atomic_clear(&busy);
}

static K_WORK_DEFINE(request_work, request_work_handler);

void as_hidpp_received(enum as_transport transport, struct bt_conn *conn, const uint8_t *report) {
    if (report[0] != AS_HIDPP_REPORT_ID) {
        return;
    }
    /* The host waits for each reply before it sends the next request; drop anything that
     * arrives while one is in progress (the host times out and retries). */
    if (!atomic_cas(&busy, 0, 1)) {
        LOG_WRN("AutoSwitch: request dropped, still handling the previous one");
        return;
    }
    request.transport = transport;
    request.conn = conn ? bt_conn_ref(conn) : NULL;
    memcpy(request.report, report, AS_HIDPP_LEN);
    k_work_submit(&request_work);
}
