#include "time_sync.h"
#ifdef ESP_PLATFORM
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include <stdlib.h>
#include <time.h>
namespace qm {

static constexpr char TAG[] = "time_sync";
void configure_london_timezone()
{
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0/2", 1);
    tzset();
}
bool time_is_plausible() { return time(nullptr) >= 1735689600; }
esp_err_t time_sync_start_and_wait()
{
    static bool initialized;
    static bool synchronized_this_boot;
    if (!initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        const esp_err_t result = esp_netif_sntp_init(&config);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "SNTP initialization failed error=%d", result);
            return result;
        }
        initialized = true;
        ESP_LOGI(TAG, "SNTP service started");
    }
    if (synchronized_this_boot && time_is_plausible()) return ESP_OK;
    const esp_err_t result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));
    synchronized_this_boot = result == ESP_OK && time_is_plausible();
    if (synchronized_this_boot) {
        ESP_LOGI(TAG, "Clock synchronized epoch=%lld", static_cast<long long>(time(nullptr)));
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Clock synchronization timed out error=%d epoch=%lld", result,
             static_cast<long long>(time(nullptr)));
    return ESP_ERR_TIMEOUT;
}
}
#endif
