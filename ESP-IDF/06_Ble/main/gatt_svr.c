#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "bleprph.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* NimBLE encodes 128-bit UUIDs least-significant byte first. */
const ble_uuid128_t test_service_uuid =
    BLE_UUID128_INIT(0x01,0x00,0xa1,0xb7,0x49,0x1d,0x5e,0x9c,
                    0x1b,0x4f,0x7d,0x5a,0x01,0x00,0x1e,0x7a);
static const ble_uuid128_t characteristic_uuid =
    BLE_UUID128_INIT(0x01,0x00,0xa1,0xb7,0x49,0x1d,0x5e,0x9c,
                    0x1b,0x4f,0x7d,0x5a,0x02,0x00,0x1e,0x7a);
static uint8_t value[512] = "BLE ready";
static uint16_t value_len = sizeof("BLE ready") - 1;
static uint16_t value_handle;

/* Access callbacks and notification callouts both run in the host task. */
static int access_value(uint16_t conn, uint16_t attr,
                        struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, value, value_len) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t incoming[sizeof(value)];
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(value))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, incoming, sizeof(incoming), &len))
            return BLE_ATT_ERR_UNLIKELY;
        memcpy(value, incoming, len);
        value_len = len;
        printf("[BLE] Received %u byte(s): ", (unsigned)len);
        for (uint16_t i = 0; i < len; ++i) {
            if (value[i] >= 32 && value[i] <= 126) putchar(value[i]);
            else printf("\\x%02X", value[i]);
        }
        putchar('\n');
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &test_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &characteristic_uuid.u,
                .access_cb = access_value,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &value_handle,
            },
            {0},
        },
    },
    {0},
};

int gatt_svr_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(services);
    return rc ? rc : ble_gatts_add_svcs(services);
}

void gatt_svr_notify_count(uint32_t count)
{
    value_len = snprintf((char *)value, sizeof(value), "count=%" PRIu32, count);
    /* NimBLE sends notifications only to clients that enabled the CCCD. */
    ble_gatts_chr_updated(value_handle);
    printf("[BLE] Notify: %s\n", value);
}
