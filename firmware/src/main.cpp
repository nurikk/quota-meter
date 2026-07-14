#ifdef ESP_PLATFORM
#include "app/app_state.h"
#include "auth/auth_manager.h"
#include "console/wifi_console.h"
#include "net/time_sync.h"
#include "portal/wifi_portal.h"
#include "ui/ui.h"
#include "bsp/esp-bsp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>

static void start_task(TaskFunction_t task, const char *name, uint32_t stack_size, UBaseType_t priority)
{
    if (xTaskCreate(task, name, stack_size, nullptr, priority, nullptr) != pdPASS) {
        ESP_LOGE("quota_meter", "Could not create %s task", name);
        abort();
    }
}

extern "C" void app_main(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
    qm::configure_london_timezone();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    qm::app_state_init();
    qm::auth_manager_init();
    qm::wifi_portal_init();
    qm::wifi_console_init();
    lv_display_t *display = bsp_display_start();
    if (!display) {
        ESP_LOGE("quota_meter", "Display initialization failed");
        abort();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(55));
    qm::ui_create();
    start_task(qm::wifi_task, "wifi_owner", 6144, 5);
    start_task(qm::auth_worker_task, "cloud_owner", 32768, 4);
    start_task(qm::ui_task, "quota_ui", 16384, 3);
}
#endif
