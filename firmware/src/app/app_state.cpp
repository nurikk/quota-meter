#include "app_state.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
namespace qm {
static AppSnapshot state{}; static SemaphoreHandle_t mutex;
void app_state_init() { mutex = xSemaphoreCreateMutex(); state = {}; state.wifi = WifiState::Idle; }
AppSnapshot app_state_get() { xSemaphoreTake(mutex, portMAX_DELAY); AppSnapshot copy = state; xSemaphoreGive(mutex); return copy; }
void app_state_set_wifi(WifiState value, const char *ip) { xSemaphoreTake(mutex, portMAX_DELAY); ++state.revision; state.wifi = value; if (ip) snprintf(state.sta_ip, sizeof(state.sta_ip), "%s", ip); xSemaphoreGive(mutex); }
void app_state_set_account(const Account &account, const ProviderStatus &value)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (state.find(account.id)) state.status(account.id) = value;
    else state.accounts.push_back({account, value});
    ++state.revision;
    xSemaphoreGive(mutex);
}
void app_state_set_ap(const char *ssid, const char *password) { xSemaphoreTake(mutex, portMAX_DELAY); ++state.revision; snprintf(state.ap_ssid, sizeof(state.ap_ssid), "%s", ssid); snprintf(state.ap_password, sizeof(state.ap_password), "%s", password); xSemaphoreGive(mutex); }
}
#endif
