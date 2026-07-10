#include "app_state.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
namespace qm {
static AppSnapshot state{}; static SemaphoreHandle_t mutex;
void app_state_init() { mutex = xSemaphoreCreateMutex(); state = {}; state.wifi = WifiState::Idle; state.openai.auth = AuthState::SignedOut; state.claude.auth = AuthState::SignedOut; }
AppSnapshot app_state_get() { xSemaphoreTake(mutex, portMAX_DELAY); AppSnapshot copy = state; xSemaphoreGive(mutex); return copy; }
void app_state_set_wifi(WifiState value, const char *ip) { xSemaphoreTake(mutex, portMAX_DELAY); state.wifi = value; if (ip) snprintf(state.sta_ip, sizeof(state.sta_ip), "%s", ip); xSemaphoreGive(mutex); }
void app_state_set_provider(Provider provider, const ProviderStatus &value) { xSemaphoreTake(mutex, portMAX_DELAY); (provider == Provider::OpenAI ? state.openai : state.claude) = value; xSemaphoreGive(mutex); }
void app_state_set_ap(const char *ssid, const char *password) { xSemaphoreTake(mutex, portMAX_DELAY); snprintf(state.ap_ssid, sizeof(state.ap_ssid), "%s", ssid); snprintf(state.ap_password, sizeof(state.ap_password), "%s", password); xSemaphoreGive(mutex); }
}
#endif
