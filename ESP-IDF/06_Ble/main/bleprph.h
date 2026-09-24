#ifndef H_BLEPRPH_
#define H_BLEPRPH_
#include "host/ble_hs.h"
#define DEVICE_NAME "HXESP_P4C6_BLE"
#define SERVICE_UUID "7a1e0001-5a7d-4f1b-9c5e-1d49b7a10001"
#define CHARACTERISTIC_UUID "7a1e0002-5a7d-4f1b-9c5e-1d49b7a10001"
extern const ble_uuid128_t test_service_uuid;
int gatt_svr_init(void);
void gatt_svr_notify_count(uint32_t count);
#endif
