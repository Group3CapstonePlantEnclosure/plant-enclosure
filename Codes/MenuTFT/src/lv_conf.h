#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_CONF_SKIP  // Tells LVGL to use the settings below
#define LV_COLOR_DEPTH 16
#define LV_MEM_SIZE (128U * 1024U)
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Standard font for SquareLine */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_18 0

#endif
