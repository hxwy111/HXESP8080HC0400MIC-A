#include "i2c_bsp.h"
#include "stdio.h"
#include <Arduino.h>

void setup() {
  I2C_master_Init();

  I2C_scan_devices();
}

void loop() {
 
} 