#pragma once
#include "../domain/models.h"
namespace qm {
void app_state_init();
AppSnapshot app_state_get();
void app_state_set_wifi(WifiState state, const char *ip = nullptr);
void app_state_set_provider(Provider provider, const ProviderStatus &status);
void app_state_set_ap(const char *ssid, const char *password);
}
