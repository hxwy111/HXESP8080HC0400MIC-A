#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "bleprph.h"

static const char *TAG = "BLE_PERIPH";
static uint8_t own_addr_type;
static bool connected;
static uint32_t notification_count;
static struct ble_npl_callout notify_timer;
static struct ble_npl_callout advertise_timer;
static int gap_event(struct ble_gap_event *event, void *arg);

static void advertise(struct ble_npl_event *event)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&test_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) goto failed;
    /* The name and 128-bit UUID do not fit together in 31 bytes. */
    struct ble_hs_adv_fields response = {0};
    response.name = (const uint8_t *)DEVICE_NAME;
    response.name_len = strlen(DEVICE_NAME);
    response.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc) goto failed;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER,
                          &params, gap_event, NULL);
    if (rc) goto failed;
    printf("[BLE] Advertising started\n");
    printf("[BLE] Device name: %s\n", DEVICE_NAME);
    printf("[BLE] Service UUID: %s\n", SERVICE_UUID);
    printf("[BLE] Use nRF Connect or LightBlue to connect/read/write/subscribe\n");
    return;
failed:
    ESP_LOGE(TAG, "Advertising failed; rc=%d", rc);
}

static void notify_tick(struct ble_npl_event *event)
{
    if (connected) {
        gatt_svr_notify_count(notification_count++);
        ble_npl_callout_reset(&notify_timer, ble_npl_time_ms_to_ticks32(1000));
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            connected = true;
            printf("[BLE] Phone connected\n");
            ble_npl_callout_reset(&notify_timer, ble_npl_time_ms_to_ticks32(1000));
        } else {
            advertise(NULL);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connected = false;
        ble_npl_callout_stop(&notify_timer);
        printf("[BLE] Phone disconnected\n");
        ble_npl_callout_reset(&advertise_timer, ble_npl_time_ms_to_ticks32(300));
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!connected) advertise(NULL);
        break;
    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc) {
        ESP_LOGE(TAG, "BLE address setup failed; rc=%d", rc);
        return;
    }
    advertise(NULL);
}

static void on_reset(int reason)
{
    connected = false;
    ble_npl_callout_stop(&notify_timer);
    ble_npl_callout_stop(&advertise_timer);
    ESP_LOGE(TAG, "BLE host reset; reason=%d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    printf("\n=== ESP32-P4 + ESP32-C6 BLE test ===\n");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    printf("[BLE] Initializing ESP-Hosted and BLE controller...\n");
    ESP_ERROR_CHECK(esp_hosted_init());
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE initialization failed: %s", esp_err_to_name(ret));
        return;
    }
    int rc = gatt_svr_init();
    if (!rc) rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc) {
        ESP_LOGE(TAG, "GATT initialization failed; rc=%d", rc);
        return;
    }
    ble_npl_callout_init(&notify_timer, nimble_port_get_dflt_eventq(), notify_tick, NULL);
    ble_npl_callout_init(&advertise_timer, nimble_port_get_dflt_eventq(), advertise, NULL);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
