#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/display.h"
#include "bsp/esp32_s3_touch_amoled_1_75c.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_amoled_1_75c";

static i2c_master_bus_handle_t i2c_handle;
static bool i2c_initialized;
static lv_indev_t *disp_indev;
static esp_lcd_touch_handle_t touch_handle;
static esp_lcd_panel_handle_t panel_handle;
static esp_lcd_panel_io_handle_t io_handle;
static i2s_chan_handle_t i2s_tx_chan;
static i2s_chan_handle_t i2s_rx_chan;
static const audio_codec_data_if_t *i2s_data_if;
static uint8_t brightness;
static SemaphoreHandle_t lcd_trans_done;
static SemaphoreHandle_t lvgl_mutex;
static void *lvgl_buffers[2];

typedef struct {
    lv_display_t *disp;
    unsigned int swap_xy;
    unsigned int mirror_x;
    unsigned int mirror_y;
} touch_init_context_t;

#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR

#define BSP_I2S_GPIO_CFG                                                                                               \
    {                                                                                                                  \
        .mclk = BSP_I2S_MCLK,                                                                                          \
        .bclk = BSP_I2S_SCLK,                                                                                          \
        .ws = BSP_I2S_LCLK,                                                                                            \
        .dout = BSP_I2S_DOUT,                                                                                          \
        .din = BSP_I2S_DSIN,                                                                                           \
        .invert_flags =                                                                                                \
            {                                                                                                          \
                .mclk_inv = false,                                                                                     \
                .bclk_inv = false,                                                                                     \
                .ws_inv = false,                                                                                       \
            },                                                                                                         \
    }

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                                          \
    {                                                                                                                  \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                                           \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),                  \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                                  \
    }

static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
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
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));
    i2c_initialized = true;
    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
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

    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "register spiffs");
    size_t total = 0;
    size_t used = 0;
    esp_err_t ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: total=%u used=%u", (unsigned)total, (unsigned)used);
    }
    return ret;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(24000);
    const i2s_std_config_t *p_i2s_cfg = i2s_config ? i2s_config : &std_cfg_default;

    if (i2s_tx_chan) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg), err, TAG, "i2s tx init");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "i2s tx enable");
    }
    if (i2s_rx_chan) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg), err, TAG, "i2s rx init");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "i2s rx enable");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK_GOTO(i2s_data_if, err);
    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
    }
    return ret;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    if (i2s_data_if == NULL) {
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        ESP_LOGW(TAG, "Speaker codec I2C control unavailable");
        return NULL;
    }

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    if (!es8311_dev) {
        ESP_LOGW(TAG, "Speaker codec unavailable");
        return NULL;
    }

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    if (i2s_data_if == NULL) {
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!i2c_ctrl_if) {
        ESP_LOGW(TAG, "Microphone codec I2C control unavailable");
        return NULL;
    }

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    if (!es7210_dev) {
        ESP_LOGW(TAG, "Microphone codec unavailable");
        return NULL;
    }

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

esp_err_t bsp_display_brightness_init(void)
{
    return bsp_display_brightness_set(100);
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (!io_handle || !panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness_percent < 0 || brightness_percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    brightness = (uint8_t)(brightness_percent * 255 / 100);
    uint32_t lcd_cmd = 0x51;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    bool locked = false;
    if (lvgl_mutex) {
        if (xSemaphoreTakeRecursive(lvgl_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        locked = true;
    }
    esp_err_t ret = esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &brightness, 1);
    if (locked) {
        xSemaphoreGiveRecursive(lvgl_mutex);
    }
    return ret;
}

int bsp_display_brightness_get(void)
{
    return brightness * 100 / 255;
}

esp_err_t bsp_display_backlight_off(void)
{
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    return bsp_display_brightness_set(100);
}

static void rounder_event_cb(lv_event_t *event)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(event);
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
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

#ifdef CONFIG_BSP_DEBUG_DIAGNOSTICS
static void fill_rgb565_be(uint8_t *dst, size_t pixel_count, uint16_t color)
{
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xff;
    for (size_t i = 0; i < pixel_count; i++) {
        dst[i * 2] = hi;
        dst[i * 2 + 1] = lo;
    }
}

static void draw_diagnostic_bars(void)
{
    if (!lvgl_buffers[0] || !lcd_trans_done) {
        ESP_LOGE(TAG, "LCD diagnostic resources unavailable");
        return;
    }

    const int rows_per_chunk = 20;
    const uint16_t colors[] = {0xffff, 0xf800, 0x07e0, 0x001f};
    for (int y = 0; y < BSP_LCD_V_RES; y += rows_per_chunk) {
        int rows = BSP_LCD_V_RES - y;
        if (rows > rows_per_chunk) {
            rows = rows_per_chunk;
        }
        uint16_t color = colors[(size_t)y * (sizeof(colors) / sizeof(colors[0])) / BSP_LCD_V_RES];
        size_t pixel_count = (size_t)BSP_LCD_H_RES * rows;
        fill_rgb565_be((uint8_t *)lvgl_buffers[0], pixel_count, color);
        while (xSemaphoreTake(lcd_trans_done, 0) == pdTRUE) {
        }
        esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, 0, y, BSP_LCD_H_RES, y + rows, lvgl_buffers[0]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD diagnostic draw failed: %s", esp_err_to_name(ret));
            return;
        }
        if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "LCD diagnostic draw timed out");
            return;
        }
    }
}
#endif

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    if (!lcd_trans_done) {
        ESP_LOGE(TAG, "LCD flush semaphore unavailable");
        lv_display_flush_ready(disp);
        return;
    }

    const size_t pixel_count = (size_t)(area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, pixel_count);
    while (xSemaphoreTake(lcd_trans_done, 0) == pdTRUE) {
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(ret));
        lv_display_flush_ready(disp);
    }
}

static void lvgl_flush_wait_cb(lv_display_t *disp)
{
    (void)disp;
    if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "LCD flush timed out; waiting for DMA completion");
        xSemaphoreTake(lcd_trans_done, portMAX_DELAY);
    }
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

    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1,
                                                                 BSP_LCD_DATA2, BSP_LCD_DATA3, config->max_transfer_sz);
    ESP_ERROR_CHECK(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO));

    lcd_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(lcd_trans_done, ESP_ERR_NO_MEM, TAG, "LCD semaphore allocation failed");

    esp_lcd_panel_io_spi_config_t io_config =
        CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, lcd_trans_done_cb, lcd_trans_done);
    io_config.trans_queue_depth = 10;
    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags =
            {
                .use_qspi_interface = 1,
            },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0x06, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

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
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels =
            {
                .reset = 0,
                .interrupt = 0,
            },
        .flags =
            {
                .swap_xy = cfg->flags.swap_xy,
                .mirror_x = cfg->flags.mirror_x,
                .mirror_y = cfg->flags.mirror_y,
            },
    };
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "touch io");
    return esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, ret_touch);
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = BSP_LCD_H_RES * 20 * BSP_LCD_BITS_PER_PIXEL / 8,
    };
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    const size_t buffer_size = BSP_LCD_H_RES * 20 * BSP_LCD_BITS_PER_PIXEL / 8;
    lvgl_buffers[0] = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lvgl_buffers[1] = heap_caps_malloc(buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!lvgl_buffers[0] || !lvgl_buffers[1]) {
        ESP_LOGE(TAG, "LVGL DMA draw buffer allocation failed");
        heap_caps_free(lvgl_buffers[0]);
        heap_caps_free(lvgl_buffers[1]);
        lvgl_buffers[0] = NULL;
        lvgl_buffers[1] = NULL;
        return NULL;
    }
    memset(lvgl_buffers[0], 0, buffer_size);
    memset(lvgl_buffers[1], 0, buffer_size);

    lv_display_t *disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display creation failed");
        heap_caps_free(lvgl_buffers[0]);
        heap_caps_free(lvgl_buffers[1]);
        lvgl_buffers[0] = NULL;
        lvgl_buffers[1] = NULL;
        return NULL;
    }

    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    lv_display_set_flush_wait_cb(disp, lvgl_flush_wait_cb);
    lv_display_set_buffers(disp, lvgl_buffers[0], lvgl_buffers[1], buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_rotation(disp, cfg->rotation);
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    return disp;
}

static void touch_init_task(void *arg)
{
    touch_init_context_t *ctx = (touch_init_context_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));

    bsp_touch_config_t touch_cfg = {
        .flags =
            {
                .swap_xy = ctx->swap_xy,
                .mirror_x = ctx->mirror_x,
                .mirror_y = ctx->mirror_y,
            },
    };

    ESP_LOGI(TAG, "Starting touch init");
    esp_lcd_touch_handle_t next_touch = NULL;
    esp_err_t ret = bsp_touch_new(&touch_cfg, &next_touch);
    if (ret != ESP_OK || !next_touch) {
        ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(ret));
        free(ctx);
        vTaskDelete(NULL);
        return;
    }

    lv_indev_t *indev = NULL;
    if (xSemaphoreTakeRecursive(lvgl_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        touch_handle = next_touch;
        indev = lv_indev_create();
        if (indev) {
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_display(indev, ctx->disp);
            lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
        }
        xSemaphoreGiveRecursive(lvgl_mutex);
    }

    if (!indev) {
        ESP_LOGW(TAG, "Touch input device creation failed");
    } else {
        disp_indev = indev;
        ESP_LOGI(TAG, "Touch input ready");
    }
    free(ctx);
    vTaskDelete(NULL);
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .rotation = LV_DISPLAY_ROTATION_0,
        .touch_flags =
            {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
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
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());
#ifdef CONFIG_BSP_DEBUG_DIAGNOSTICS
    ESP_LOGI(TAG, "Drawing LCD diagnostic bars");
    draw_diagnostic_bars();
    vTaskDelay(pdMS_TO_TICKS(5000));
#endif
    BaseType_t task_ok = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "LVGL task creation failed");
        return NULL;
    }

    touch_init_context_t *touch_ctx = calloc(1, sizeof(*touch_ctx));
    if (touch_ctx) {
        touch_ctx->disp = disp;
        touch_ctx->swap_xy = cfg->touch_flags.swap_xy;
        touch_ctx->mirror_x = cfg->touch_flags.mirror_x;
        touch_ctx->mirror_y = cfg->touch_flags.mirror_y;
        task_ok = xTaskCreate(touch_init_task, "touch_init", 4096, touch_ctx, 4, NULL);
        if (task_ok != pdPASS) {
            ESP_LOGW(TAG, "Touch init task creation failed");
            free(touch_ctx);
        }
    } else {
        ESP_LOGW(TAG, "Touch init context allocation failed");
    }

    ESP_LOGI(TAG, "Display start complete");
    return disp;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

esp_err_t bsp_display_rotation_set(bsp_display_rotation_t rotation)
{
    if (!io_handle || !panel_handle) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t madctl = 0x00;
    switch (rotation) {
    case BSP_DISPLAY_ROTATE_0:
        madctl = 0x00;
        break;
    case BSP_DISPLAY_ROTATE_90:
        madctl = 0x60;
        break;
    case BSP_DISPLAY_ROTATE_180:
        madctl = 0xC0;
        break;
    case BSP_DISPLAY_ROTATE_270:
        madctl = 0xA0;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t lcd_cmd = 0x36;
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= 0x02 << 24;
    return esp_lcd_panel_io_tx_param(io_handle, lcd_cmd, &madctl, 1);
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
