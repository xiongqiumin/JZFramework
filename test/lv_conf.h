#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 32

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

#define LV_STDINT_INCLUDE       <stdint.h>
#define LV_STDDEF_INCLUDE       <stddef.h>
#define LV_STDBOOL_INCLUDE      <stdbool.h>
#define LV_INTTYPES_INCLUDE     <inttypes.h>
#define LV_LIMITS_INCLUDE       <limits.h>
#define LV_STDARG_INCLUDE       <stdarg.h>

#define LV_DEF_REFR_PERIOD      33
#define LV_DPI_DEF              130

#define LV_USE_TICK              1
#define LV_USE_TIMER             1
#define LV_USE_TASK_MANAGER      1
#define LV_USE_DRAW_SW           1
#define LV_USE_DRAW_SW_ASM       LV_DRAW_SW_ASM_NONE

#define LV_FONT_MONTSERRAT_14    1
#define LV_FONT_MONTSERRAT_20    1
#define LV_FONT_MONTSERRAT_24    1

#endif
