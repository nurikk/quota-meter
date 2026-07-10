if(NOT DEFINED BSP_BOARD_SOURCE)
    set(BSP_PIOENV "")
    if(DEFINED PIOENV)
        set(BSP_PIOENV "${PIOENV}")
    elseif(DEFINED ENV{PIOENV})
        set(BSP_PIOENV "$ENV{PIOENV}")
    endif()
    get_filename_component(BSP_SDKCONFIG_NAME "${SDKCONFIG}" NAME)
    if(BSP_PIOENV STREQUAL "aipi-lite" OR BSP_SDKCONFIG_NAME STREQUAL "sdkconfig.aipi-lite")
        set(BSP_BOARD_SOURCE "aipi_lite.c")
    elseif(BSP_PIOENV STREQUAL "jc3248w535en" OR BSP_SDKCONFIG_NAME STREQUAL "sdkconfig.jc3248w535en")
        set(BSP_BOARD_SOURCE "jc3248w535en.c")
    else()
        set(BSP_BOARD_SOURCE "esp32_s3_touch_amoled_1_75c.c")
    endif()
endif()

idf_component_register(
    SRCS "${BSP_BOARD_SOURCE}" "esp_lcd_axs15231b.c"
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES driver esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_ledc esp_driver_spi esp_lcd esp_codec_dev esp_lcd_co5300 esp_lcd_touch_cst9217 esp_lcd_touch lvgl esp_lcd_panel_io_additions
    PRIV_REQUIRES esp_timer esp_psram spiffs fatfs
)
