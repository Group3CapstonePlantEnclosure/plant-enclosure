#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color Settings */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0 // Set to 1 only if your colors look "inverted" (Blue is Red)

/* Memory Settings - Boosted for S3 PSRAM */
#define LV_MEM_CUSTOM 0
#define LV_MEM_SIZE (48U * 1024U) 
#define LV_MEM_ADR 0
#define LV_MEM_POOL_INCLUDE_FREE 0

/* HAL Settings */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Font Usage - Standard SquareLine Fonts */
// Enable these so SquareLine text actually shows up
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1

/* Other Features */
#define LV_USE_LOG 1        // Set to 1 if you need to debug LVGL errors in Serial
#define LV_LOG_LEVEL LV_LOG_LEVEL_NONE
#define LV_LOG_PRINTF 0

#endif