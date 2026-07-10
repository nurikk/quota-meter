#include "time_sync.h"
#ifdef ESP_PLATFORM
#include "esp_netif_sntp.h"
#include <time.h>
namespace qm {
bool time_is_plausible() { return time(nullptr) >= 1735689600; }
esp_err_t time_sync_start_and_wait()
{
    if (time_is_plausible()) return ESP_OK;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t result = esp_netif_sntp_init(&config); if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000)); return result == ESP_OK && time_is_plausible() ? ESP_OK : ESP_ERR_TIMEOUT;
}
}
#endif
