#include "token_store.h"
#ifdef ESP_PLATFORM
#include "nvs.h"
#include <string.h>

namespace qm {
static const char *name_space(Provider provider) { return provider == Provider::OpenAI ? "qm_openai" : "qm_claude"; }
void secure_clear(void *value, size_t length) { volatile unsigned char *p = static_cast<volatile unsigned char *>(value); while (length--) *p++ = 0; }
static bool terminated(const char *value, size_t cap) { return memchr(value, 0, cap) != nullptr; }

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

esp_err_t token_store_remove(Provider provider)
{
    nvs_handle_t handle; esp_err_t result = nvs_open(name_space(provider), NVS_READWRITE, &handle); if (result != ESP_OK) return result;
    result = nvs_erase_all(handle); if (result == ESP_OK) result = nvs_commit(handle); nvs_close(handle); return result;
}
}
#endif
