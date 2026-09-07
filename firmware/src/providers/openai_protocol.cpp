#include "openai_protocol.h"
#include "../util/base64url.h"
#include "../domain/state_reducer.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace qm {
static bool json_string(cJSON *root, const char *key, char *out, size_t cap, bool required = true)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || strlen(item->valuestring) >= cap) {
        if (cap) out[0] = 0;
        return !required && (!item || cJSON_IsNull(item));
    }
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static cJSON *parse_bounded(const char *json, size_t length)
{
    if (!json || length == 0 || length > 16384) return nullptr;
    return cJSON_ParseWithLength(json, length);
}

ErrorCode oauth_refresh_error(const HttpResult &response)
{
    const ErrorCode error = map_http_error(response.status, response.error);
    if (response.status != 400 && response.status != 403) return error;
    cJSON *root = parse_bounded(response.body, response.body_len);
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(code)) code = cJSON_GetObjectItemCaseSensitive(code, "code");
    const bool rejected = cJSON_IsString(code) && code->valuestring &&
                          strcmp(code->valuestring, "invalid_grant") == 0;
    cJSON_Delete(root);
    return rejected ? ErrorCode::Unauthorized : error;
}

bool openai_user_code_request(char *out, size_t cap)
{
    int count = snprintf(out, cap, "{\"client_id\":\"%s\"}", OPENAI_CLIENT_ID);
    return count > 0 && static_cast<size_t>(count) < cap;
}

bool openai_poll_request(const OpenAiDeviceCode &code, char *out, size_t cap)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "device_auth_id", code.device_auth_id);
    cJSON_AddStringToObject(root, "user_code", code.user_code);
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!text || strlen(text) >= cap) { cJSON_free(text); return false; }
    memcpy(out, text, strlen(text) + 1); cJSON_free(text); return true;
}

static bool token_request(const char *grant, const char *value_key, const char *value, const char *verifier, char *out, size_t cap)
{
    if (!value || strlen(value) > 4096) return false;
    cJSON *root = cJSON_CreateObject(); if (!root) return false;
    cJSON_AddStringToObject(root, "client_id", OPENAI_CLIENT_ID); cJSON_AddStringToObject(root, "grant_type", grant);
    cJSON_AddStringToObject(root, value_key, value);
    if (verifier) { cJSON_AddStringToObject(root, "redirect_uri", OPENAI_REDIRECT_URI); cJSON_AddStringToObject(root, "code_verifier", verifier); }
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!text || strlen(text) >= cap) { cJSON_free(text); return false; }
    memcpy(out, text, strlen(text) + 1); cJSON_free(text); return true;
}

static bool form_append(char *out, size_t cap, size_t *used, const char *value, bool encode)
{
    static constexpr char HEX[] = "0123456789ABCDEF";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
        bool unreserved = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~';
        size_t needed = encode && !unreserved ? 3 : 1;
        if (*used + needed >= cap) return false;
        if (needed == 1) out[(*used)++] = static_cast<char>(*p);
        else {
            out[(*used)++] = '%';
            out[(*used)++] = HEX[*p >> 4];
            out[(*used)++] = HEX[*p & 15];
        }
    }
    out[*used] = 0;
    return true;
}

bool openai_exchange_request(const char *code, const char *verifier, char *out, size_t cap)
{
    if (!code || !verifier || !out || cap == 0 || strlen(code) > 4096 || strlen(verifier) > 128) return false;
    size_t used = 0;
    out[0] = 0;
    return form_append(out, cap, &used, "grant_type=authorization_code&code=", false) &&
        form_append(out, cap, &used, code, true) &&
        form_append(out, cap, &used, "&redirect_uri=", false) &&
        form_append(out, cap, &used, OPENAI_REDIRECT_URI, true) &&
        form_append(out, cap, &used, "&client_id=", false) &&
        form_append(out, cap, &used, OPENAI_CLIENT_ID, true) &&
        form_append(out, cap, &used, "&code_verifier=", false) &&
        form_append(out, cap, &used, verifier, true);
}
bool openai_refresh_request(const char *refresh, char *out, size_t cap)
{ return token_request("refresh_token", "refresh_token", refresh, nullptr, out, cap); }

bool parse_openai_device_code(const char *json, size_t length, OpenAiDeviceCode *out)
{
    if (!out) return false; memset(out, 0, sizeof(*out)); cJSON *root = parse_bounded(json, length); if (!root) return false;
    bool ok = json_string(root, "device_auth_id", out->device_auth_id, sizeof(out->device_auth_id));
    if (!json_string(root, "user_code", out->user_code, sizeof(out->user_code))) ok = json_string(root, "usercode", out->user_code, sizeof(out->user_code));
    cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval");
    if (cJSON_IsNumber(interval)) out->interval = static_cast<uint32_t>(interval->valuedouble);
    else if (cJSON_IsString(interval)) out->interval = static_cast<uint32_t>(strtoul(interval->valuestring, nullptr, 10));
    if (out->interval < 1 || out->interval > 60) ok = false;
    cJSON_Delete(root); return ok;
}

bool parse_oauth_tokens(const char *json, size_t length, OAuthTokens *out, bool require_refresh)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = parse_bounded(json, length);
    if (!root) return false;
    bool has_access = json_string(root, "access_token", out->access_token, sizeof(out->access_token), false) && out->access_token[0];
    bool has_refresh = json_string(root, "refresh_token", out->refresh_token, sizeof(out->refresh_token), false) && out->refresh_token[0];
    bool has_id = json_string(root, "id_token", out->id_token, sizeof(out->id_token), false) && out->id_token[0];
    cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    out->expires_in = cJSON_IsNumber(expires) && expires->valuedouble > 0 ? static_cast<uint32_t>(expires->valuedouble) : 3600;
    cJSON_Delete(root);
    return require_refresh ? has_access && has_refresh : has_access || has_refresh || has_id;
}

bool openai_account_id_from_jwt(const char *jwt, char *out, size_t cap)
{
    if (!jwt || !out || cap == 0 || strlen(jwt) > 8192) return false;
    out[0] = 0;
    const char *first = strchr(jwt, '.');
    if (!first) return false;
    const char *second = strchr(first + 1, '.');
    if (!second) return false;
    size_t encoded_len = static_cast<size_t>(second - first - 1);
    if (encoded_len == 0 || encoded_len > 6000) return false;
    char *encoded = static_cast<char *>(malloc(encoded_len + 1));
    uint8_t *decoded = static_cast<uint8_t *>(malloc(4097));
    if (!encoded || !decoded) {
        free(encoded);
        free(decoded);
        return false;
    }
    memcpy(encoded, first + 1, encoded_len);
    encoded[encoded_len] = 0;
    size_t written = 0;
    bool decoded_ok = base64url_decode(encoded, decoded, 4096, &written);
    if (decoded_ok) decoded[written] = 0;
    cJSON *root = decoded_ok ? cJSON_ParseWithLength(reinterpret_cast<char *>(decoded), written) : nullptr;
    cJSON *auth = root ? cJSON_GetObjectItemCaseSensitive(root, "https://api.openai.com/auth") : nullptr;
    bool ok = cJSON_IsObject(auth) && json_string(auth, "chatgpt_account_id", out, cap);
    cJSON_Delete(root);
    volatile char *encoded_bytes = encoded;
    for (size_t i = 0; i < encoded_len + 1; ++i) encoded_bytes[i] = 0;
    volatile uint8_t *decoded_bytes = decoded;
    for (size_t i = 0; i < 4097; ++i) decoded_bytes[i] = 0;
    free(encoded);
    free(decoded);
    return ok;
}

static void add_window(ProviderStatus *status, const char *label, cJSON *window)
{
    if (!cJSON_IsObject(window) || status->window_count >= 8) return;
    cJSON *used = cJSON_GetObjectItemCaseSensitive(window, "used_percent");
    if (!cJSON_IsNumber(used)) used = cJSON_GetObjectItemCaseSensitive(window, "usedPercent");
    if (!cJSON_IsNumber(used)) return;
    QuotaWindow &target = status->windows[status->window_count++]; memset(&target, 0, sizeof(target));
    snprintf(target.label, sizeof(target.label), "%s", label); target.used_percent = static_cast<float>(used->valuedouble); target.present = true;
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(window, "reset_at");
    if (!cJSON_IsNumber(reset)) reset = cJSON_GetObjectItemCaseSensitive(window, "resets_at");
    if (!cJSON_IsNumber(reset)) reset = cJSON_GetObjectItemCaseSensitive(window, "resetsAt");
    if (cJSON_IsNumber(reset)) target.resets_at = static_cast<int64_t>(reset->valuedouble);
    cJSON *minutes = cJSON_GetObjectItemCaseSensitive(window, "window_duration_mins");
    if (!cJSON_IsNumber(minutes)) minutes = cJSON_GetObjectItemCaseSensitive(window, "windowDurationMins");
    if (cJSON_IsNumber(minutes) && minutes->valuedouble > 0) {
        target.window_minutes = static_cast<int32_t>(minutes->valuedouble);
    } else {
        cJSON *seconds = cJSON_GetObjectItemCaseSensitive(window, "limit_window_seconds");
        if (cJSON_IsNumber(seconds) && seconds->valuedouble > 0) {
            target.window_minutes = static_cast<int32_t>(seconds->valuedouble / 60);
        }
    }
}

bool parse_openai_usage(const char *json, size_t length, ProviderStatus *status)
{
    if (!status) return false; cJSON *root = parse_bounded(json, length); if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    status->window_count = 0; cJSON *plan = cJSON_GetObjectItemCaseSensitive(root, "plan_type");
    if (!cJSON_IsString(plan)) plan = cJSON_GetObjectItemCaseSensitive(root, "planType");
    if (cJSON_IsString(plan) && strlen(plan->valuestring) < sizeof(status->plan)) strcpy(status->plan, plan->valuestring);
    cJSON *rate = cJSON_GetObjectItemCaseSensitive(root, "rate_limit"); if (!cJSON_IsObject(rate)) rate = root;
    cJSON *primary = cJSON_GetObjectItemCaseSensitive(rate, "primary_window");
    if (!cJSON_IsObject(primary)) primary = cJSON_GetObjectItemCaseSensitive(rate, "primary");
    cJSON *secondary = cJSON_GetObjectItemCaseSensitive(rate, "secondary_window");
    if (!cJSON_IsObject(secondary)) secondary = cJSON_GetObjectItemCaseSensitive(rate, "secondary");
    add_window(status, "Primary", primary);
    add_window(status, "Secondary", secondary);
    status->reset_credits = {};
    cJSON *credits = cJSON_GetObjectItemCaseSensitive(root, "rate_limit_reset_credits");
    cJSON *available = cJSON_GetObjectItemCaseSensitive(credits, "available_count");
    if (cJSON_IsNumber(available) && available->valuedouble >= 0 && available->valuedouble <= UINT32_MAX) {
        const uint32_t count = static_cast<uint32_t>(available->valuedouble);
        if (available->valuedouble == count) status->reset_credits = {true, count};
    }
    cJSON_Delete(root); return status->window_count > 0;
}
}
