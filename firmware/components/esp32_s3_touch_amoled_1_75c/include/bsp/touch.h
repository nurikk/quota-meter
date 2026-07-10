#pragma once

#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct {
        unsigned int swap_xy;
        unsigned int mirror_x;
        unsigned int mirror_y;
    } flags;
} bsp_touch_config_t;

esp_err_t bsp_touch_new(const bsp_touch_config_t *cfg, esp_lcd_touch_handle_t *ret_touch);

#ifdef __cplusplus
}
#endif
