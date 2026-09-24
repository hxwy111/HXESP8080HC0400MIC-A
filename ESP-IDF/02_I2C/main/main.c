#include <stdio.h>
#include "i2c_bsp.h"


void app_main(void)
{
      I2C_master_Init();

      I2C_scan_devices();

}