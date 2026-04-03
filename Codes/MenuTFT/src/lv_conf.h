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
#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 0
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 0
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 0
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 0
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 0
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 0
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 0
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 0
#define LV_FONT_DEFAULT &lv_font_montserrat_14

/* Other Features */
#define LV_USE_LOG 0
#define LV_LOG_LEVEL LV_LOG_LEVEL_NONE
#define LV_LOG_PRINTF 0

#endif
