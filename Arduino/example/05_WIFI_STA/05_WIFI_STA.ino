#include <WiFi.h>

const char* ssid = "nova6";          // Change to your WiFi name
const char* password = "asdfghjkl";         // Change to your WiFi password
void setup()
{
  WiFi.mode(WIFI_STA); // Set to STA mode
  WiFi.begin(ssid, password);
  
  printf("Connecting to WiFi\n");
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }
  printf("IP Address: %s\n",WiFi.localIP().toString().c_str());
}
void loop()
{
  // Your code
}
