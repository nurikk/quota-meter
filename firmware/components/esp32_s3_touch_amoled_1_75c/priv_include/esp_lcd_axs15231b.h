#pragma once

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch.h"

#define ESP_LCD_AXS15231B_VER_MAJOR (1)
#define ESP_LCD_AXS15231B_VER_MINOR (0)
#define ESP_LCD_AXS15231B_VER_PATCH (0)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int cmd;
    const void *data;
    size_t data_bytes;
    unsigned int delay_ms;
} axs15231b_lcd_init_cmd_t;

typedef struct {
    const axs15231b_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface : 1;
    } flags;
} axs15231b_vendor_config_t;

esp_err_t esp_lcd_new_panel_axs15231b(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t *panel_dev_config,
                                      esp_lcd_panel_handle_t *ret_panel);

#define AXS15231B_PANEL_BUS_QSPI_CONFIG(sclk, d0, d1, d2, d3, max_trans_sz)                                            \
    {                                                                                                                  \
        .sclk_io_num = sclk,                                                                                           \
        .data0_io_num = d0,                                                                                            \
        .data1_io_num = d1,                                                                                            \
        .data2_io_num = d2,                                                                                            \
        .data3_io_num = d3,                                                                                            \
        .max_transfer_sz = max_trans_sz,                                                                               \
    }

#define AXS15231B_PANEL_IO_QSPI_CONFIG(cs, cb, cb_ctx)                                                                 \
    {                                                                                                                  \
        .cs_gpio_num = cs,                                                                                             \
        .dc_gpio_num = -1,                                                                                             \
        .spi_mode = 3,                                                                                                 \
        .pclk_hz = 50 * 1000 * 1000,                                                                                   \
        .trans_queue_depth = 10,                                                                                       \
        .on_color_trans_done = cb,                                                                                     \
        .user_ctx = cb_ctx,                                                                                            \
        .lcd_cmd_bits = 32,                                                                                            \
        .lcd_param_bits = 8,                                                                                           \
        .flags = {.quad_mode = true},                                                                                  \
    }

esp_err_t esp_lcd_touch_new_i2c_axs15231b(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                          esp_lcd_touch_handle_t *tp);

#define ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS (0x3B)
#define ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG()                                                                        \
    {                                                                                                                  \
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS,                                                            \
        .control_phase_bytes = 1,                                                                                      \
        .dc_bit_offset = 0,                                                                                            \
        .lcd_cmd_bits = 8,                                                                                             \
        .flags = {.disable_control_phase = 1},                                                                         \
    }

#ifdef __cplusplus
}
#endif
