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
struct AccountRecord {
    static constexpr uint32_t VERSION = 1;
    uint32_t version{VERSION};
    Account account;
    TokenBundle bundle;
};
bool valid_token_bundle(Provider provider, const TokenBundle &bundle);
bool decode_account_record(const void *data, size_t length, AccountRecord *record);
esp_err_t token_store_init();
esp_err_t token_store_clear();
esp_err_t token_store_accounts(std::vector<Account> *accounts);
esp_err_t token_store_upload(Provider provider, const char *name, const TokenBundle &bundle);
esp_err_t token_store_save(const Account &account, const TokenBundle &bundle);
esp_err_t token_store_load(const Account &account, TokenBundle *bundle);
esp_err_t quota_store_save(const Account &account, const ProviderStatus &status);
esp_err_t quota_store_load(const Account &account, ProviderStatus *status);
void secure_clear(void *value, size_t length);
}
