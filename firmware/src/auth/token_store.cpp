#include "token_store.h"
#ifdef ESP_PLATFORM
#include "nvs.h"
#include <string.h>

namespace qm {
namespace {
constexpr uint32_t QUOTA_VERSION = 3;

struct QuotaBundle {
    uint32_t version;
    char plan[32];
    QuotaWindow windows[8];
    uint8_t window_count;
    ClaudeExtraUsage extra_usage;
    OpenAiResetCredits reset_credits;
    int64_t fetched_at;
};

const char *name_space(Provider provider) { return provider == Provider::OpenAI ? "qm_openai" : "qm_claude"; }
bool terminated(const char *value, size_t cap) { return memchr(value, 0, cap) != nullptr; }

bool valid_quota(const QuotaBundle &quota)
{
    if (quota.version != QUOTA_VERSION || quota.window_count > 8 || !terminated(quota.plan, sizeof(quota.plan)) ||
        !terminated(quota.extra_usage.currency, sizeof(quota.extra_usage.currency))) return false;
    for (uint8_t i = 0; i < quota.window_count; ++i) {
        if (!terminated(quota.windows[i].label, sizeof(quota.windows[i].label))) return false;
    }
    return true;
}
}

void secure_clear(void *value, size_t length) { volatile unsigned char *p = static_cast<volatile unsigned char *>(value); while (length--) *p++ = 0; }

esp_err_t token_store_save(Provider provider, const TokenBundle &input)
{
    if (input.version != TokenBundle::VERSION || !input.oauth.access_token[0] || !input.oauth.refresh_token[0] ||
        !terminated(input.oauth.access_token, sizeof(input.oauth.access_token)) ||
        !terminated(input.oauth.refresh_token, sizeof(input.oauth.refresh_token)) ||
        !terminated(input.oauth.id_token, sizeof(input.oauth.id_token)) ||
        !terminated(input.account_id, sizeof(input.account_id))) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(name_space(provider), NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "bundle", &input, sizeof(input));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t token_store_load(Provider provider, TokenBundle *bundle)
{
    if (!bundle) return ESP_ERR_INVALID_ARG; secure_clear(bundle, sizeof(*bundle)); nvs_handle_t handle;
    esp_err_t result = nvs_open(name_space(provider), NVS_READONLY, &handle); if (result != ESP_OK) return result;
    size_t length = sizeof(*bundle); result = nvs_get_blob(handle, "bundle", bundle, &length); nvs_close(handle);
    if (result != ESP_OK) return result;
    if (length != sizeof(*bundle) || bundle->version != TokenBundle::VERSION || !bundle->oauth.access_token[0] || !bundle->oauth.refresh_token[0] ||
        !terminated(bundle->oauth.access_token, sizeof(bundle->oauth.access_token)) || !terminated(bundle->oauth.refresh_token, sizeof(bundle->oauth.refresh_token)) ||
        !terminated(bundle->oauth.id_token, sizeof(bundle->oauth.id_token)) || !terminated(bundle->account_id, sizeof(bundle->account_id))) {
        secure_clear(bundle, sizeof(*bundle)); return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t quota_store_save(Provider provider, const ProviderStatus &status)
{
    if (status.window_count > 8 || !terminated(status.plan, sizeof(status.plan)) ||
        !terminated(status.extra_usage.currency, sizeof(status.extra_usage.currency))) return ESP_ERR_INVALID_ARG;
    QuotaBundle quota{};
    quota.version = QUOTA_VERSION;
    memcpy(quota.plan, status.plan, sizeof(quota.plan));
    quota.window_count = status.window_count;
    for (uint8_t i = 0; i < status.window_count; ++i) {
        if (!terminated(status.windows[i].label, sizeof(status.windows[i].label))) return ESP_ERR_INVALID_ARG;
        quota.windows[i] = status.windows[i];
    }
    quota.extra_usage = status.extra_usage;
    quota.reset_credits = status.reset_credits;
    quota.fetched_at = status.fetched_at;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(name_space(provider), NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "quota", &quota, sizeof(quota));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

esp_err_t quota_store_load(Provider provider, ProviderStatus *status)
{
    if (!status) return ESP_ERR_INVALID_ARG;
    QuotaBundle quota{};
    nvs_handle_t handle;
    esp_err_t result = nvs_open(name_space(provider), NVS_READONLY, &handle);
    if (result != ESP_OK) return result;
    size_t length = sizeof(quota);
    result = nvs_get_blob(handle, "quota", &quota, &length);
    nvs_close(handle);
    if (result != ESP_OK) return result;
    if (length != sizeof(quota) || !valid_quota(quota)) return ESP_ERR_INVALID_VERSION;
    memcpy(status->plan, quota.plan, sizeof(status->plan));
    for (QuotaWindow &window : status->windows) window = {};
    memcpy(status->windows, quota.windows, sizeof(status->windows));
    status->window_count = quota.window_count;
    status->extra_usage = quota.extra_usage;
    status->reset_credits = quota.reset_credits;
    status->fetched_at = quota.fetched_at;
    return ESP_OK;
}

esp_err_t token_store_remove(Provider provider)
{
    nvs_handle_t handle; esp_err_t result = nvs_open(name_space(provider), NVS_READWRITE, &handle); if (result != ESP_OK) return result;
    result = nvs_erase_all(handle); if (result == ESP_OK) result = nvs_commit(handle); nvs_close(handle); return result;
}
}
#endif
