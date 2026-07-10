#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/aipi_lite.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "bsp_aipi_lite";

static i2c_master_bus_handle_t i2c_handle;
static bool i2c_initialized;
static bool brightness_initialized;
static uint8_t brightness_percent;
static lv_indev_t *disp_indev;
static esp_lcd_panel_io_handle_t io_handle;
static i2s_chan_handle_t i2s_tx_chan;
static i2s_chan_handle_t i2s_rx_chan;
static const audio_codec_data_if_t *i2s_data_if;
static const audio_codec_if_t *es8311_dev;
static SemaphoreHandle_t lcd_trans_done;
static SemaphoreHandle_t lvgl_mutex;
static void *lvgl_buffer;
static void *lcd_trans_buffer;
static size_t lcd_trans_buffer_size;

#define BSP_LCD_LEDC_TIMER (LEDC_TIMER_0)
#define BSP_LCD_LEDC_CHANNEL (LEDC_CHANNEL_0)

#define ST7735_SWRESET (0x01)
#define ST7735_SLPOUT (0x11)
#define ST7735_NORON (0x13)
#define ST7735_INVON (0x21)
#define ST7735_CASET (0x2A)
#define ST7735_RASET (0x2B)
#define ST7735_RAMWR (0x2C)
#define ST7735_MADCTL (0x36)
#define ST7735_COLMOD (0x3A)
#define ST7735_DISPON (0x29)

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

typedef struct {
    uint8_t cmd;
    uint8_t data[16];
    uint8_t data_bytes;
    uint16_t delay_ms;
} st7735_init_cmd_t;

static const st7735_init_cmd_t lcd_init_cmds[] = {
    {ST7735_SWRESET, {0}, 0, 150},  {ST7735_SLPOUT, {0}, 0, 120}, {ST7735_COLMOD, {0x05}, 1, 10},
    {ST7735_MADCTL, {0x00}, 1, 10}, {ST7735_INVON, {0}, 0, 0},    {ST7735_NORON, {0}, 0, 10},
    {ST7735_DISPON, {0}, 0, 100},
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

static esp_err_t bsp_audio_codec_ensure(void)
{
    if (es8311_dev) {
        return ESP_OK;
    }
    if (i2s_data_if == NULL) {
        BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());
        BSP_ERROR_CHECK_RETURN_ERR(bsp_audio_init(NULL));
    }

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(i2c_ctrl_if, ESP_FAIL, TAG, "ES8311 I2C control unavailable");

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    es8311_dev = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_dev, ESP_FAIL, TAG, "ES8311 codec unavailable");
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_codec_ensure());
    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_codec_ensure());
    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

static esp_err_t brightness_apply(int percent)
{
    uint32_t duty_cycle = (1023 * percent) / 100;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CHANNEL, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, BSP_LCD_LEDC_CHANNEL));
    brightness_percent = (uint8_t)percent;
    return ESP_OK;
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
    return brightness_apply(100);
}

esp_err_t bsp_display_brightness_set(int percent)
{
    if (percent < 0 || percent > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!brightness_initialized) {
        ESP_RETURN_ON_ERROR(bsp_display_brightness_init(), TAG, "brightness init");
    }
    return brightness_apply(percent);
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

static void copy_rgb565_to_lcd(uint8_t *dst, const uint8_t *src, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
#if BSP_LCD_BIGENDIAN
        dst[i * 2] = src[i * 2 + 1];
        dst[i * 2 + 1] = src[i * 2];
#else
        dst[i * 2] = src[i * 2];
        dst[i * 2 + 1] = src[i * 2 + 1];
#endif
    }
}

static esp_err_t lcd_send_cmd(uint8_t cmd, const uint8_t *data, size_t data_len)
{
    return esp_lcd_panel_io_tx_param(io_handle, cmd, data, data_len);
}

static esp_err_t lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t col_data[] = {x0 >> 8, x0 & 0xff, x1 >> 8, x1 & 0xff};
    uint8_t row_data[] = {y0 >> 8, y0 & 0xff, y1 >> 8, y1 & 0xff};
    ESP_RETURN_ON_ERROR(lcd_send_cmd(ST7735_CASET, col_data, sizeof(col_data)), TAG, "set columns");
    return lcd_send_cmd(ST7735_RASET, row_data, sizeof(row_data));
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    const size_t pixel_count = (size_t)width * height;
    const size_t color_bytes = pixel_count * sizeof(uint16_t);
    if (!lcd_trans_buffer || color_bytes > lcd_trans_buffer_size) {
        ESP_LOGE(TAG, "LCD transfer buffer too small: %u", (unsigned)color_bytes);
        lv_display_flush_ready(disp);
        return;
    }

    copy_rgb565_to_lcd((uint8_t *)lcd_trans_buffer, px_map, pixel_count);
    while (xSemaphoreTake(lcd_trans_done, 0) == pdTRUE) {
    }

    esp_err_t ret = lcd_set_window(area->x1, area->y1, area->x2, area->y2);
    if (ret == ESP_OK) {
        ret = esp_lcd_panel_io_tx_color(io_handle, ST7735_RAMWR, lcd_trans_buffer, color_bytes);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(ret));
    } else if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "LCD flush timed out");
        // DMA may still be reading lcd_trans_buffer; releasing it to LVGL now would let
        // the next flush overwrite the buffer mid-transfer. Wait a bounded extra time for
        // the transfer to finish; only give up (accepting possible tearing) if it still
        // has not completed.
        if (xSemaphoreTake(lcd_trans_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGE(TAG, "LCD flush still pending after extended wait");
        }
    }
    lv_display_flush_ready(disp);
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

static void lcd_hardware_reset(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BSP_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
}

static esp_err_t lcd_init_panel(void)
{
    lcd_hardware_reset();
    for (size_t i = 0; i < sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]); i++) {
        const st7735_init_cmd_t *cmd = &lcd_init_cmds[i];
        ESP_RETURN_ON_ERROR(lcd_send_cmd(cmd->cmd, cmd->data_bytes ? cmd->data : NULL, cmd->data_bytes), TAG,
                            "LCD init command");
        if (cmd->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(cmd->delay_ms));
        }
    }
    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel,
                          esp_lcd_panel_io_handle_t *ret_io)
{
    assert(config && config->max_transfer_sz > 0);

    spi_bus_config_t buscfg = {
        .mosi_io_num = BSP_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = BSP_LCD_PCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = config->max_transfer_sz,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init");

    lcd_trans_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(lcd_trans_done, ESP_ERR_NO_MEM, TAG, "LCD semaphore allocation failed");

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BSP_LCD_DC,
        .cs_gpio_num = BSP_LCD_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = lcd_trans_done_cb,
        .user_ctx = lcd_trans_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &io_handle),
                        TAG, "panel IO");
    ESP_RETURN_ON_ERROR(lcd_init_panel(), TAG, "panel init");

    if (ret_panel) {
        *ret_panel = NULL;
    }
    if (ret_io) {
        *ret_io = io_handle;
    }
    return ESP_OK;
}

esp_err_t bsp_touch_new(const bsp_touch_config_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    (void)cfg;
    if (ret_touch) {
        *ret_touch = NULL;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    const size_t buffer_size = BSP_LCD_H_RES * 32 * BSP_LCD_BITS_PER_PIXEL / 8;
    const bsp_display_config_t disp_config = {
        .max_transfer_sz = buffer_size,
    };
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, NULL, &io_handle));

    lvgl_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lvgl_buffer) {
        lvgl_buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!lvgl_buffer) {
        ESP_LOGE(TAG, "LVGL draw buffer allocation failed");
        return NULL;
    }
    memset(lvgl_buffer, 0, buffer_size);

    lcd_trans_buffer_size = buffer_size;
    lcd_trans_buffer = heap_caps_malloc(lcd_trans_buffer_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
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
    lv_display_set_buffers(disp, lvgl_buffer, NULL, buffer_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_rotation(disp, cfg->rotation);
    return disp;
}

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .rotation = LV_DISPLAY_ROTATION_0,
        .touch_flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
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
    (void)rotation;
    return ESP_ERR_NOT_SUPPORTED;
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
