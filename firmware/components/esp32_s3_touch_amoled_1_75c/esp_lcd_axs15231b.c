#include "esp_lcd_axs15231b.h"

#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AXS_MAX_TOUCH_NUMBER (1)
#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x0BULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const char *TAG = "lcd_panel.axs15231b";

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const axs15231b_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
    struct {
        unsigned int use_qspi_interface : 1;
        unsigned int reset_level : 1;
    } flags;
} axs15231b_panel_t;

static esp_err_t panel_axs15231b_del(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_init(esp_lcd_panel_t *panel);
static esp_err_t panel_axs15231b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                             const void *color_data);
static esp_err_t panel_axs15231b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_axs15231b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_axs15231b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_axs15231b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_axs15231b_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
static esp_err_t touch_axs15231b_read_data(esp_lcd_touch_handle_t tp);
static bool touch_axs15231b_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                   uint8_t *point_num, uint8_t max_point_num);
static esp_err_t touch_axs15231b_del(esp_lcd_touch_handle_t tp);
static esp_err_t touch_axs15231b_reset(esp_lcd_touch_handle_t tp);
static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, int reg, uint8_t *data, uint8_t len);
static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, int reg, const uint8_t *data, uint8_t len);

static esp_err_t tx_param(axs15231b_panel_t *axs15231b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                          size_t param_size)
{
    if (axs15231b->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
    }
    return esp_lcd_panel_io_tx_param(io, lcd_cmd, param, param_size);
}

static esp_err_t tx_color(axs15231b_panel_t *axs15231b, esp_lcd_panel_io_handle_t io, int lcd_cmd, const void *param,
                          size_t param_size)
{
    if (axs15231b->flags.use_qspi_interface) {
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_COLOR << 24;
    }
    return esp_lcd_panel_io_tx_color(io, lcd_cmd, param, param_size);
}

esp_err_t esp_lcd_new_panel_axs15231b(const esp_lcd_panel_io_handle_t io,
                                      const esp_lcd_panel_dev_config_t *panel_dev_config,
                                      esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    axs15231b_panel_t *axs15231b = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    axs15231b = calloc(1, sizeof(axs15231b_panel_t));
    ESP_GOTO_ON_FALSE(axs15231b, ESP_ERR_NO_MEM, err, TAG, "no mem for axs15231b panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .pin_bit_mask = BIT64(panel_dev_config->reset_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->rgb_ele_order) {
    case LCD_RGB_ELEMENT_ORDER_RGB:
        axs15231b->madctl_val = 0;
        break;
    case LCD_RGB_ELEMENT_ORDER_BGR:
        axs15231b->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported RGB element order");
    }

    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        axs15231b->colmod_val = 0x55;
        axs15231b->fb_bits_per_pixel = 16;
        break;
    case 18:
        axs15231b->colmod_val = 0x66;
        axs15231b->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
    }

    axs15231b->io = io;
    axs15231b->reset_gpio_num = panel_dev_config->reset_gpio_num;
    axs15231b->flags.reset_level = panel_dev_config->flags.reset_active_high;
    if (panel_dev_config->vendor_config) {
        const axs15231b_vendor_config_t *vendor_config =
            (const axs15231b_vendor_config_t *)panel_dev_config->vendor_config;
        axs15231b->init_cmds = vendor_config->init_cmds;
        axs15231b->init_cmds_size = vendor_config->init_cmds_size;
        axs15231b->flags.use_qspi_interface = vendor_config->flags.use_qspi_interface;
    }

    axs15231b->base.del = panel_axs15231b_del;
    axs15231b->base.reset = panel_axs15231b_reset;
    axs15231b->base.init = panel_axs15231b_init;
    axs15231b->base.draw_bitmap = panel_axs15231b_draw_bitmap;
    axs15231b->base.invert_color = panel_axs15231b_invert_color;
    axs15231b->base.set_gap = panel_axs15231b_set_gap;
    axs15231b->base.mirror = panel_axs15231b_mirror;
    axs15231b->base.swap_xy = panel_axs15231b_swap_xy;
    axs15231b->base.disp_on_off = panel_axs15231b_disp_on_off;
    *ret_panel = &axs15231b->base;

    ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_AXS15231B_VER_MAJOR,
             ESP_LCD_AXS15231B_VER_MINOR, ESP_LCD_AXS15231B_VER_PATCH);
    return ESP_OK;

err:
    if (axs15231b) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(axs15231b);
    }
    return ret;
}

static esp_err_t panel_axs15231b_del(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    if (axs15231b->reset_gpio_num >= 0) {
        gpio_reset_pin(axs15231b->reset_gpio_num);
    }
    free(axs15231b);
    return ESP_OK;
}

static esp_err_t panel_axs15231b_reset(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;

    if (axs15231b->reset_gpio_num >= 0) {
        ESP_RETURN_ON_ERROR(gpio_set_level(axs15231b->reset_gpio_num, !axs15231b->flags.reset_level), TAG,
                            "set reset inactive");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(axs15231b->reset_gpio_num, axs15231b->flags.reset_level), TAG,
                            "set reset active");
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_RETURN_ON_ERROR(gpio_set_level(axs15231b->reset_gpio_num, !axs15231b->flags.reset_level), TAG,
                            "set reset inactive");
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_SWRESET, NULL, 0), TAG, "software reset");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_axs15231b_init(esp_lcd_panel_t *panel)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;

    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_SLPOUT, NULL, 0), TAG, "sleep out");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_MADCTL, (uint8_t[]){axs15231b->madctl_val}, 1), TAG, "madctl");
    ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, LCD_CMD_COLMOD, (uint8_t[]){axs15231b->colmod_val}, 1), TAG, "colmod");

    for (int i = 0; i < axs15231b->init_cmds_size; i++) {
        ESP_RETURN_ON_ERROR(tx_param(axs15231b, io, axs15231b->init_cmds[i].cmd, axs15231b->init_cmds[i].data,
                                     axs15231b->init_cmds[i].data_bytes),
                            TAG, "init command");
        vTaskDelay(pdMS_TO_TICKS(axs15231b->init_cmds[i].delay_ms));
    }

    return ESP_OK;
}

static esp_err_t panel_axs15231b_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                             const void *color_data)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    esp_lcd_panel_io_handle_t io = axs15231b->io;
    assert((x_start < x_end) && (y_start < y_end));

    x_start += axs15231b->x_gap;
    x_end += axs15231b->x_gap;
    y_start += axs15231b->y_gap;
    y_end += axs15231b->y_gap;

    ESP_RETURN_ON_ERROR(
        tx_param(axs15231b, io, LCD_CMD_CASET,
                 (uint8_t[]){(x_start >> 8) & 0xff, x_start & 0xff, ((x_end - 1) >> 8) & 0xff, (x_end - 1) & 0xff}, 4),
        TAG, "column address");
    if (!axs15231b->flags.use_qspi_interface) {
        ESP_RETURN_ON_ERROR(
            tx_param(axs15231b, io, LCD_CMD_RASET,
                     (uint8_t[]){(y_start >> 8) & 0xff, y_start & 0xff, ((y_end - 1) >> 8) & 0xff, (y_end - 1) & 0xff},
                     4),
            TAG, "row address");
    }

    size_t len = (x_end - x_start) * (y_end - y_start) * axs15231b->fb_bits_per_pixel / 8;
    int cmd = (axs15231b->flags.use_qspi_interface && y_start > 0) ? LCD_CMD_RAMWRC : LCD_CMD_RAMWR;
    return tx_color(axs15231b, io, cmd, color_data, len);
}

static esp_err_t panel_axs15231b_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    return tx_param(axs15231b, axs15231b->io, invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF, NULL, 0);
}

static esp_err_t panel_axs15231b_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    if (mirror_x) {
        axs15231b->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y) {
        axs15231b->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    return tx_param(axs15231b, axs15231b->io, LCD_CMD_MADCTL, (uint8_t[]){axs15231b->madctl_val}, 1);
}

static esp_err_t panel_axs15231b_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    if (swap_axes) {
        axs15231b->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        axs15231b->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    return tx_param(axs15231b, axs15231b->io, LCD_CMD_MADCTL, (uint8_t[]){axs15231b->madctl_val}, 1);
}

static esp_err_t panel_axs15231b_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    axs15231b->x_gap = x_gap;
    axs15231b->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_axs15231b_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    axs15231b_panel_t *axs15231b = __containerof(panel, axs15231b_panel_t, base);
    return tx_param(axs15231b, axs15231b->io, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF, NULL, 0);
}

esp_err_t esp_lcd_touch_new_i2c_axs15231b(const esp_lcd_panel_io_handle_t io, const esp_lcd_touch_config_t *config,
                                          esp_lcd_touch_handle_t *tp)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(io && config && tp, ESP_ERR_INVALID_ARG, TAG, "invalid touch argument");

    esp_lcd_touch_handle_t axs15231b = calloc(1, sizeof(esp_lcd_touch_t));
    ESP_GOTO_ON_FALSE(axs15231b, ESP_ERR_NO_MEM, err, TAG, "touch handle malloc failed");

    axs15231b->io = io;
    axs15231b->read_data = touch_axs15231b_read_data;
    axs15231b->get_xy = touch_axs15231b_get_xy;
    axs15231b->del = touch_axs15231b_del;
    axs15231b->data.lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    memcpy(&axs15231b->config, config, sizeof(esp_lcd_touch_config_t));

    if (axs15231b->config.int_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t int_gpio_config = {
            .pin_bit_mask = BIT64(axs15231b->config.int_gpio_num),
            .mode = GPIO_MODE_INPUT,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_gpio_config), err, TAG, "GPIO intr config failed");
        if (axs15231b->config.interrupt_callback) {
            ESP_GOTO_ON_ERROR(
                esp_lcd_touch_register_interrupt_callback(axs15231b, axs15231b->config.interrupt_callback), err, TAG,
                "register interrupt callback");
        }
    }

    if (axs15231b->config.rst_gpio_num != GPIO_NUM_NC) {
        const gpio_config_t rst_gpio_config = {
            .pin_bit_mask = BIT64(axs15231b->config.rst_gpio_num),
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_gpio_config), err, TAG, "GPIO reset config failed");
    }

    ESP_GOTO_ON_ERROR(touch_axs15231b_reset(axs15231b), err, TAG, "touch reset failed");
    *tp = axs15231b;
    return ESP_OK;

err:
    if (axs15231b) {
        touch_axs15231b_del(axs15231b);
    }
    return ret;
}

static esp_err_t touch_axs15231b_read_data(esp_lcd_touch_handle_t tp)
{
    typedef struct {
        uint8_t gesture;
        uint8_t num;
        uint8_t x_h : 4;
        uint8_t : 2;
        uint8_t event : 2;
        uint8_t x_l;
        uint8_t y_h : 4;
        uint8_t : 4;
        uint8_t y_l;
    } __attribute__((packed)) touch_record_t;

    uint8_t data[AXS_MAX_TOUCH_NUMBER * 6 + 2] = {0};
    const uint8_t read_cmd[11] = {
        0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, (AXS_MAX_TOUCH_NUMBER * 6 + 2) >> 8, (AXS_MAX_TOUCH_NUMBER * 6 + 2) & 0xff,
        0x00, 0x00, 0x00};

    ESP_RETURN_ON_ERROR(i2c_write_bytes(tp, -1, read_cmd, sizeof(read_cmd)), TAG, "I2C write failed");
    ESP_RETURN_ON_ERROR(i2c_read_bytes(tp, -1, data, sizeof(data)), TAG, "I2C read failed");

    touch_record_t *touch_data = (touch_record_t *)data;
    portENTER_CRITICAL(&tp->data.lock);
    if (touch_data->num > 0 && touch_data->num <= AXS_MAX_TOUCH_NUMBER) {
        tp->data.points = touch_data->num;
        tp->data.coords[0].x = ((touch_data->x_h & 0x0f) << 8) | touch_data->x_l;
        tp->data.coords[0].y = ((touch_data->y_h & 0x0f) << 8) | touch_data->y_l;
        tp->data.coords[0].strength = 0;
    } else {
        tp->data.points = 0;
    }
    portEXIT_CRITICAL(&tp->data.lock);

    return ESP_OK;
}

static bool touch_axs15231b_get_xy(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y, uint16_t *strength,
                                   uint8_t *point_num, uint8_t max_point_num)
{
    portENTER_CRITICAL(&tp->data.lock);
    *point_num = tp->data.points > max_point_num ? max_point_num : tp->data.points;
    for (size_t i = 0; i < *point_num; i++) {
        x[i] = tp->data.coords[i].x;
        y[i] = tp->data.coords[i].y;
        if (strength) {
            strength[i] = tp->data.coords[i].strength;
        }
    }
    tp->data.points = 0;
    portEXIT_CRITICAL(&tp->data.lock);
    return *point_num > 0;
}

static esp_err_t touch_axs15231b_del(esp_lcd_touch_handle_t tp)
{
    if (tp->config.int_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.int_gpio_num);
    }
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        gpio_reset_pin(tp->config.rst_gpio_num);
    }
    free(tp);
    return ESP_OK;
}

static esp_err_t touch_axs15231b_reset(esp_lcd_touch_handle_t tp)
{
    if (tp->config.rst_gpio_num != GPIO_NUM_NC) {
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, tp->config.levels.reset), TAG,
                            "touch reset active");
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_RETURN_ON_ERROR(gpio_set_level(tp->config.rst_gpio_num, !tp->config.levels.reset), TAG,
                            "touch reset inactive");
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return ESP_OK;
}

static esp_err_t i2c_read_bytes(esp_lcd_touch_handle_t tp, int reg, uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "invalid data");
    return esp_lcd_panel_io_rx_param(tp->io, reg, data, len);
}

static esp_err_t i2c_write_bytes(esp_lcd_touch_handle_t tp, int reg, const uint8_t *data, uint8_t len)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "invalid data");
    return esp_lcd_panel_io_tx_param(tp->io, reg, data, len);
}
