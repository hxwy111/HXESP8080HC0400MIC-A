#include "sd_card_bsp.h"

static const char FILE_PATH[] = "/sd_card/hello.txt";
static const char WRITE_DATA[] = "Hello,World!";

void setup()
{
  Serial.begin(115200);
  unsigned long serial_wait_start = millis();
  while (!Serial && (millis() - serial_wait_start < 3000)) {
    delay(10);
  }

  esp_err_t ret = SD_card_Init();
  if (ret != ESP_OK) {
    Serial.printf("SD card init failed: %s (0x%x)\n",
                  esp_err_to_name(ret), ret);
    return;
  }

  ret = s_example_write_file(FILE_PATH, WRITE_DATA);
  if (ret != ESP_OK) {
    Serial.printf("Write failed: %s (0x%x)\n",
                  esp_err_to_name(ret), ret);
    return;
  }

  char read_buffer[64] = {};
  size_t read_length = 0;
  ret = s_example_read_file(FILE_PATH,
                            read_buffer,
                            sizeof(read_buffer),
                            &read_length);
  if (ret != ESP_OK) {
    Serial.printf("Read failed: %s (0x%x)\n",
                  esp_err_to_name(ret), ret);
    return;
  }

 printf("Read %u bytes from %s:\n%s\n",
                (unsigned int)read_length,
                FILE_PATH,
                read_buffer);
}

void loop()
{
}
