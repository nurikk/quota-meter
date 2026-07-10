#pragma once

#include "bsp/config.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "lvgl.h"
#include "sdkconfig.h"

#define BSP_CAPS_DISPLAY 1
#define BSP_CAPS_TOUCH 1
#define BSP_CAPS_BUTTONS 0
#define BSP_CAPS_AUDIO 1
#define BSP_CAPS_AUDIO_SPEAKER 1
#define BSP_CAPS_AUDIO_MIC 1
#define BSP_CAPS_SDCARD 0
#define BSP_CAPS_IMU 0

#define BSP_I2C_SCL (GPIO_NUM_14)
#define BSP_I2C_SDA (GPIO_NUM_15)
#define BSP_I2S_SCLK (GPIO_NUM_9)
#define BSP_I2S_MCLK (GPIO_NUM_16)
#define BSP_I2S_LCLK (GPIO_NUM_45)
#define BSP_I2S_DOUT (GPIO_NUM_8)
#define BSP_I2S_DSIN (GPIO_NUM_10)
#define BSP_POWER_AMP_IO (GPIO_NUM_46)

#define BSP_LCD_CS (GPIO_NUM_12)
#define BSP_LCD_PCLK (GPIO_NUM_38)
#define BSP_LCD_DATA0 (GPIO_NUM_4)
#define BSP_LCD_DATA1 (GPIO_NUM_5)
#define BSP_LCD_DATA2 (GPIO_NUM_6)
#define BSP_LCD_DATA3 (GPIO_NUM_7)
#define BSP_LCD_BACKLIGHT (GPIO_NUM_NC)
#define BSP_LCD_RST (GPIO_NUM_39)
#define BSP_LCD_TOUCH_RST (GPIO_NUM_40)
#define BSP_LCD_TOUCH_INT (GPIO_NUM_11)
#define BSP_I2C_NUM CONFIG_BSP_I2C_NUM
#define BSP_LCD_SPI_NUM (SPI2_HOST)

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_i2c_init(void);
esp_err_t bsp_i2c_deinit(void);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
esp_err_t bsp_spiffs_mount(void);
esp_err_t bsp_spiffs_unmount(void);

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
typedef struct {
    lv_display_rotation_t rotation;
    struct {
        unsigned int swap_xy;
        unsigned int mirror_x;
        unsigned int mirror_y;
    } touch_flags;
} bsp_display_cfg_t;

lv_display_t *bsp_display_start(void);
lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg);
lv_indev_t *bsp_display_get_input_dev(void);
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);
void bsp_display_rotate(lv_display_t *disp, lv_display_rotation_t rotation);
#endif

#ifdef __cplusplus
}
#endif
