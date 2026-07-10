#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/jc3248w535en.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "bsp_jc3248w535en";

static i2c_master_bus_handle_t i2c_handle;
static bool i2c_initialized;
static bool brightness_initialized;
static uint8_t brightness_percent;
static lv_indev_t *disp_indev;
static esp_lcd_touch_handle_t touch_handle;
static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_panel_io_handle_t io_handle;
static SemaphoreHandle_t lcd_trans_done;
static SemaphoreHandle_t lvgl_mutex;
static void *lvgl_buffer;
static void *lcd_trans_buffer;

#define BSP_LCD_LEDC_TIMER (LEDC_TIMER_1)
#define BSP_LCD_LEDC_CHANNEL (LEDC_CHANNEL_1)

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0,
     (uint8_t[]){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00},
     17, 0},
    {0xA2, (uint8_t[]){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xF9, 0x10,
                       0x02, 0xFF, 0xFF, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A},
     31, 0},
    {0xD0, (uint8_t[]){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA,
                       0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12},
     30, 0},
    {0xA3, (uint8_t[]){0xA0, 0x06, 0xAA, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04,
                       0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55},
     22, 0},
    {0xC1, (uint8_t[]){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF,
                       0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0D, 0x00, 0xFF, 0x40},
     30, 0},
    {0xC3, (uint8_t[]){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t[]){0x00, 0x24, 0x33, 0x80, 0x00, 0xEA, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11,
                       0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50},
     29, 0},
    {0xC5, (uint8_t[]){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE,
                       0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00},
     23, 0},
    {0xC6, (uint8_t[]){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22,
                       0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22},
     20, 0},
    {0xC7, (uint8_t[]){0x50, 0x32, 0x28, 0x00, 0xA2, 0x80, 0x8F, 0x00, 0x80, 0xFF,
                       0x07, 0x11, 0x9C, 0x67, 0xFF, 0x24, 0x0C, 0x0D, 0x0E, 0x0F},
     20, 0},
    {0xC9, (uint8_t[]){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t[]){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09,
                       0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08},
     27, 0},
    {0xD5, (uint8_t[]){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04,
                       0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00},
     30, 0},
    {0xD6, (uint8_t[]){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00,
                       0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00},
     30, 0},
    {0xD7,
     (uint8_t[]){0x03, 0x01, 0x0B, 0x09, 0x0F, 0x0D, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20,
                 0xA0, 0x1F},
     19, 0},
    {0xD8, (uint8_t[]){0x02, 0x00, 0x0A, 0x08, 0x0E, 0x0C, 0x1E, 0x1F, 0x18, 0x1D, 0x1F, 0x19}, 12, 0},
    {0xD9, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t[]){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t[]){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8, 0},
    {0xE0,
     (uint8_t[]){0x3B, 0x28, 0x10, 0x16, 0x0C, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D},
     17, 0},
    {0xE1,
     (uint8_t[]){0x37, 0x28, 0x10, 0x16, 0x0B, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F},
     17, 0},
    {0xE2,
     (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D},
     17, 0},
    {0xE3,
     (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F},
     17, 0},
    {0xE4,
     (uint8_t[]){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D},
     17, 0},
    {0xE5,
     (uint8_t[]){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F},
     17, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30},
     16, 0},
    {0xA4, (uint8_t[]){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, NULL, 0, 0},
    {0x11, NULL, 0, 120},
    {0x2C, (uint8_t[]){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

esp_err_t bsp_i2c_init(void)
{
    if (i2c_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {.enable_internal_pullup = true},
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));
    i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    if (!i2c_initialized) {
        return ESP_OK;
    }
    BSP_ERROR_CHECK_RETURN_ERR(i2c_del_master_bus(i2c_handle));
    i2c_initialized = false;
    i2c_handle = NULL;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };
    return esp_vfs_spiffs_register(&conf);
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    (void)i2s_config;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    return NULL;
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    return NULL;
}

esp_err_t bsp_display_brightness_init(void)
{
    if (!brightness_initialized) {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = BSP_LCD_LEDC_TIMER,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&timer));

        const ledc_channel_config_t channel = {
            .gpio_num = BSP_LCD_BACKLIGHT,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = BSP_LCD_LEDC_CHANNEL,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = BSP_LCD_LEDC_TIMER,
            .duty = 0,
            .hpoint = 0,
        };
        BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&channel));
        brightness_initialized = true;
    }
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_brightness_set(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!brightness_initialized) {
        ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness init");
    }

    uint32_t duty_cycle = (1023 * percent) / 100;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CHANNEL, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CHANNEL));
    brightness_percent = (uint8_t)percent;
    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
    return brightness_percent;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static bool lcd_trans_done_cb(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    return task_woken == pdTRUE;
}

static void copy_lcd_rows(uint8_t *dst, const uint8_t *src, int native_y_start, int native_rows)
{
    for (int y = 0; y < native_rows; y++) {
        int native_y = native_y_start + y;
        for (int native_x = 0; native_x < BSP_LCD_NATIVE_H_RES; native_x++) {
#if BSP_LCD_H_RES == BSP_LCD_NATIVE_H_RES && BSP_LCD_V_RES == BSP_LCD_NATIVE_V_RES
            int logical_x = native_x;
            int logical_y = native_y;
#elif BSP_LCD_H_RES == BSP_LCD_NATIVE_V_RES && BSP_LCD_V_RES == BSP_LCD_NATIVE_H_RES
            int logical_x = native_y;
            int logical_y = BSP_LCD_V_RES - native_x - 1;
#else
#error "Unsupported jc3248w535en logical/native LCD geometry"
#endif
            const uint8_t *src_pixel = src + ((size_t)logical_y * BSP_LCD_H_RES + logical_x) * sizeof(uint16_t);
            uint8_t *dst_pixel = dst + ((size_t)y * BSP_LCD_NATIVE_H_RES + native_x) * sizeof(uint16_t);
            if (BSP_LCD_BIGENDIAN) {
                dst_pixel[0] = src_pixel[1];
                dst_pixel[1] = src_pixel[0];
            } else {
                memcpy(dst_pixel, src_pixel, sizeof(uint16_t));
            }
        }
    }
}

static void touch_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                      uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    uint8_t count = *point_num > max_point_num ? max_point_num : *point_num;
    for (uint8_t i = 0; i < count; i++) {
        uint16_t raw_x = x[i];
        uint16_t raw_y = y[i];
        if (raw_x >= BSP_LCD_NATIVE_H_RES) {
            raw_x = BSP_LCD_NATIVE_H_RES - 1;
        }
        if (raw_y >= BSP_LCD_NATIVE_V_RES) {
            raw_y = BSP_LCD_NATIVE_V_RES - 1;
        }
#if BSP_LCD_H_RES == BSP_LCD_NATIVE_H_RES && BSP_LCD_V_RES == BSP_LCD_NATIVE_V_RES
        x[i] = raw_x;
        y[i] = raw_y;
#elif BSP_LCD_H_RES == BSP_LCD_NATIVE_V_RES && BSP_LCD_V_RES == BSP_LCD_NATIVE_H_RES
        x[i] = raw_y;
        y[i] = BSP_LCD_NATIVE_H_RES - raw_x - 1;
#else
#error "Unsupported jc3248w535en logical/native touch geometry"
#endif
    }
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    const int width = BSP_LCD_NATIVE_H_RES;
    const int height = BSP_LCD_NATIVE_V_RES;
    const int max_rows = 40;

    for (int y = 0; y < height; y += max_rows) {
        int rows = height - y;
        if (rows > max_rows) {
            rows = max_rows;
        }

        copy_lcd_rows(lcd_trans_buffer, px_map, y, rows);

        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, width, y + rows, lcd_trans_buffer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(ret));
            break;
        }
        if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "LCD flush timed out");
            break;
        }
    }
    lv_display_flush_ready(disp);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    uint16_t x = 0;
    uint16_t y = 0;
    uint8_t point_count = 0;

    if (touch_handle && esp_lcd_touch_read_data(touch_handle) == ESP_OK &&
        esp_lcd_touch_get_coordinates(touch_handle, &x, &y, NULL, &point_count, 1) && point_count > 0) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay_ms = 10;
        if (xSemaphoreTakeRecursive(lvgl_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            delay_ms = lv_timer_handler();
            xSemaphoreGiveRecursive(lvgl_mutex);
        }
        if (delay_ms < 5) {
            delay_ms = 5;
        } else if (delay_ms > 20) {
            delay_ms = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io)
{
    assert(config && config->max_transfer_sz > 0);

    const spi_bus_config_t buscfg = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3, config->max_transfer_sz);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init");

    lcd_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(lcd_trans_done, ESP_ERR_NO_MEM, TAG, "LCD semaphore allocation failed");

    const esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, lcd_trans_done_cb, lcd_trans_done);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle),
                        TAG, "panel IO");

    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io_handle, &panel_config, &panel_handle), TAG, "panel create");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "panel on");

    if (ret_panel) {
        *ret_panel = panel_handle;
    }
    if (ret_io) {
        *ret_io = io_handle;
    }
    return ESP_OK;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    assert(cfg);
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_NATIVE_H_RES,
        .y_max = BSP_LCD_NATIVE_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .process_coordinates = touch_process_coordinates,
        .levels = {.reset = 0, .interrupt = 0},
        .flags =
            {
                .swap_xy = cfg->flags.swap_xy,
                .mirror_x = cfg->flags.mirror_x,
                .mirror_y = cfg->flags.mirror_y,
            },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "touch IO");
    return esp_lcd_touch_new_i2c_axs15231b(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    const size_t trans_buffer_size = BSP_LCD_NATIVE_H_RES * 40 * BSP_LCD_BITS_PER_PIXEL / 8;
    const size_t draw_buffer_size = BSP_LCD_H_RES * BSP_LCD_V_RES * BSP_LCD_BITS_PER_PIXEL / 8;

    const bsp_display_config_t disp_config = {
        .max_transfer_sz = trans_buffer_size,
    };
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    lvgl_buffer = heap_caps_malloc(draw_buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lvgl_buffer) {
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed");
        return NULL;
    }
    memset(lvgl_buffer, 0, draw_buffer_size);
    lcd_trans_buffer = heap_caps_malloc(trans_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lcd_trans_buffer) {
        ESP_LOGE(TAG, "LCD transfer buffer allocation failed");
        return NULL;
    }

    lv_display_t *disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (!disp) {
        return NULL;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_buffers(disp, lvgl_buffer, NULL, draw_buffer_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_rotation(disp, cfg->rotation);
    return disp;
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp)
{
    bsp_touch_config_t touch_cfg = {
        .flags =
            {
                .swap_xy = cfg->touch_flags.swap_xy,
                .mirror_x = cfg->touch_flags.mirror_x,
                .mirror_y = cfg->touch_flags.mirror_y,
            },
    };
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(&touch_cfg, &touch_handle));

    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        return NULL;
    }
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev, disp);
    lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
    return indev;
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .rotation = LV_DISPLAY_ROTATION_0,
        .touch_flags =
            {
                .swap_xy = 0,
                .mirror_x = 0,
                .mirror_y = 0,
            },
    };
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg)
{
    assert(cfg);

    lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    BSP_NULL_CHECK(lvgl_mutex, NULL);

    lv_init();

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    BSP_ERROR_CHECK_RETURN_NULL(esp_timer_create(&tick_timer_args, &tick_timer));
    BSP_ERROR_CHECK_RETURN_NULL(esp_timer_start_periodic(tick_timer, 1000));

    lv_display_t *disp = bsp_display_lcd_init(cfg);
    BSP_NULL_CHECK(disp, NULL);
    disp_indev = bsp_display_indev_init(cfg, disp);
    BSP_NULL_CHECK(disp_indev, NULL);
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());
    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "LVGL task creation failed");
        return NULL;
    }
    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation)
{
    if (!panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    switch (rotation) {
    case BSP_DISPLAY_ROTATE_0:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, false), TAG, "swap xy");
        return esp_lcd_panel_mirror(panel_handle, false, false);
    case BSP_DISPLAY_ROTATE_90:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), TAG, "swap xy");
        return esp_lcd_panel_mirror(panel_handle, true, false);
    case BSP_DISPLAY_ROTATE_180:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, false), TAG, "swap xy");
        return esp_lcd_panel_mirror(panel_handle, true, true);
    case BSP_DISPLAY_ROTATE_270:
        ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(panel_handle, true), TAG, "swap xy");
        return esp_lcd_panel_mirror(panel_handle, false, true);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    if (!lvgl_mutex) {
        return false;
    }
    TickType_t timeout_ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mutex, timeout_ticks) == pdTRUE;
}

void bsp_display_unlock(void)
{
    if (lvgl_mutex) {
        xSemaphoreGiveRecursive(lvgl_mutex);
    }
}

void bsp_display_rotate(lv_display_t *disp, lv_display_rotation_t rotation)
{
    lv_display_set_rotation(disp, rotation);
}
