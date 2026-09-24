#include <Arduino.h>
#include <WiFi.h>

static const char *AP_SSID = "waveshare_esp32";
static const char *AP_PASSWORD = "wav123456";

void setup()
{
    Serial.begin(115200);

    // GPIO53连接C6的GPIO9启动脚。保持高阻，正常启动由板载10k上拉保证；
    // 给C6串口烧录时，U3 pin 3才能安全地临时接到GND。
    //pinMode(53, INPUT);

    WiFi.mode(WIFI_AP);

    if (!WiFi.softAP(AP_SSID, AP_PASSWORD, 1, false, 4)) {
        Serial.println("SoftAP start failed");
        return;
    }

    Serial.println("SoftAP started");
    Serial.printf("SSID: %s\n", AP_SSID);
    Serial.printf("IP: %s\n",
                  WiFi.softAPIP().toString().c_str());
}

void loop()
{
}
