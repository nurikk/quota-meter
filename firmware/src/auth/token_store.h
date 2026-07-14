#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
using esp_err_t = int;
#endif
#include "../providers/openai_protocol.h"
namespace qm {
struct TokenBundle {
    static constexpr uint32_t VERSION = 1;
    uint32_t version;
    OAuthTokens oauth;
    char account_id[128];
    int64_t expires_at;
    int64_t refreshed_at;
};
esp_err_t token_store_save(Provider provider, const TokenBundle &bundle);
esp_err_t token_store_load(Provider provider, TokenBundle *bundle);
esp_err_t quota_store_save(Provider provider, const ProviderStatus &status);
esp_err_t quota_store_load(Provider provider, ProviderStatus *status);
esp_err_t token_store_remove(Provider provider);
void secure_clear(void *value, size_t length);
}
