/*
 * HXESP8080HC0340MIC-A BLE test
 *
 * Hardware: ESP32-P4 + ESP32-C6
 * Arduino board: ESP32P4 Dev Module (esp32:esp32:esp32p4)
 *
 * The ESP32-P4 has no Bluetooth radio. BLEDevice::init() initializes the
 * ESP-Hosted SDIO link and uses the ESP32-C6 as the Bluetooth controller.
 * The C6 must contain compatible ESP-Hosted firmware and C6_IO9 must not be
 * connected to GND during normal operation.
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "Select Tools > Board > ESP32 Arduino > ESP32P4 Dev Module"
#endif

#if !defined(CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE)
#error "This ESP32 Arduino core does not enable ESP-Hosted BLE for ESP32-P4"
#endif

static constexpr char DEVICE_NAME[] = "HXESP_P4C6_BLE";
static constexpr char SERVICE_UUID[] =
    "7a1e0001-5a7d-4f1b-9c5e-1d49b7a10001";
static constexpr char CHARACTERISTIC_UUID[] =
    "7a1e0002-5a7d-4f1b-9c5e-1d49b7a10001";

static BLEServer *bleServer = nullptr;
static BLECharacteristic *testCharacteristic = nullptr;
static volatile bool deviceConnected = false;
static bool previousConnectionState = false;
static uint32_t notificationCount = 0;
static uint32_t lastNotificationMs = 0;

class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer *server) override {
    (void)server;
    deviceConnected = true;
    Serial.println("[BLE] Phone connected");
  }

  void onDisconnect(BLEServer *server) override {
    (void)server;
    deviceConnected = false;
    Serial.println("[BLE] Phone disconnected");
  }
};

class CharacteristicCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();

    Serial.printf("[BLE] Received %u byte(s): ",
                  static_cast<unsigned>(value.length()));
    for (size_t i = 0; i < value.length(); ++i) {
      const uint8_t byteValue = static_cast<uint8_t>(value[i]);
      if (byteValue >= 32 && byteValue <= 126) {
        Serial.write(byteValue);
      } else {
        Serial.printf("\\x%02X", byteValue);
      }
    }
    Serial.println();
  }
};

static bool startBleServer() {
  Serial.println("[BLE] Initializing ESP-Hosted and BLE controller...");

  if (!BLEDevice::init(DEVICE_NAME)) {
    Serial.println("[BLE] Initialization failed");
    Serial.println("Check board selection, C6 Hosted firmware, C6_IO9 and SDIO link");
    return false;
  }

  bleServer = BLEDevice::createServer();
  if (bleServer == nullptr) {
    Serial.println("[BLE] Failed to create GATT server");
    return false;
  }
  bleServer->setCallbacks(new ServerCallbacks());

  BLEService *service = bleServer->createService(SERVICE_UUID);
  if (service == nullptr) {
    Serial.println("[BLE] Failed to create GATT service");
    return false;
  }

  testCharacteristic = service->createCharacteristic(
      CHARACTERISTIC_UUID,
      BLECharacteristic::PROPERTY_READ |
          BLECharacteristic::PROPERTY_WRITE |
          BLECharacteristic::PROPERTY_NOTIFY);
  if (testCharacteristic == nullptr) {
    Serial.println("[BLE] Failed to create GATT characteristic");
    return false;
  }

  testCharacteristic->setCallbacks(new CharacteristicCallbacks());
  testCharacteristic->setValue("BLE ready");
  service->start();

  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising started");
  Serial.printf("[BLE] Device name: %s\n", DEVICE_NAME);
  Serial.printf("[BLE] Service UUID: %s\n", SERVICE_UUID);
  Serial.println("[BLE] Use nRF Connect or LightBlue to connect/read/write/subscribe");
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("=== ESP32-P4 + ESP32-C6 BLE test ===");

  if (!startBleServer()) {
    Serial.println("[BLE] Test stopped because initialization did not complete");
  }
}

void loop() {
  if (testCharacteristic == nullptr || bleServer == nullptr) {
    delay(1000);
    return;
  }

  if (deviceConnected && millis() - lastNotificationMs >= 1000) {
    lastNotificationMs = millis();

    char message[20];
    snprintf(message, sizeof(message), "count=%lu",
             static_cast<unsigned long>(notificationCount++));
    testCharacteristic->setValue(message);
    testCharacteristic->notify();
    Serial.printf("[BLE] Notify: %s\n", message);
  }

  // Advertising stops after a client connection. Restart it after disconnect
  // so that the phone can find the board again.
  if (!deviceConnected && previousConnectionState) {
    delay(300);
    bleServer->startAdvertising();
    Serial.println("[BLE] Advertising restarted");
  }

  previousConnectionState = deviceConnected;
  delay(10);
}
