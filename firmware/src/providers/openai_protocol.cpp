#include "openai_protocol.h"
#include "../util/base64url.h"
#include "../domain/state_reducer.h"
#include "cJSON.h"
#include <ctype.h>
#include <math.h>
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
    const char *end = nullptr;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length, &end, false);
    while (end && end < json + length && isspace(static_cast<unsigned char>(*end))) ++end;
    if (!root || end != json + length) {
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

static bool integer_in_range(double value, double minimum, double maximum)
{
    return isfinite(value) && value >= minimum && value <= maximum && floor(value) == value;
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

bool openai_refresh_request(const char *refresh, char *out, size_t cap)
{
    if (!refresh || strlen(refresh) > 4096) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "client_id", OPENAI_CLIENT_ID);
    cJSON_AddStringToObject(root, "grant_type", "refresh_token");
    cJSON_AddStringToObject(root, "refresh_token", refresh);
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text || strlen(text) >= cap) {
        cJSON_free(text);
        return false;
    }
    memcpy(out, text, strlen(text) + 1);
    cJSON_free(text);
    return true;
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
    out->expires_in = cJSON_IsNumber(expires) && integer_in_range(expires->valuedouble, 1, UINT32_MAX)
        ? static_cast<uint32_t>(expires->valuedouble) : 3600;
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
    cJSON *root = decoded_ok ? parse_bounded(reinterpret_cast<char *>(decoded), written) : nullptr;
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
    if (!cJSON_IsNumber(used) || !isfinite(used->valuedouble)) return;
    QuotaWindow &target = status->windows[status->window_count++]; memset(&target, 0, sizeof(target));
    snprintf(target.label, sizeof(target.label), "%s", label); target.used_percent = static_cast<float>(used->valuedouble); target.present = true;
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(window, "reset_at");
    if (!cJSON_IsNumber(reset)) reset = cJSON_GetObjectItemCaseSensitive(window, "resets_at");
    if (!cJSON_IsNumber(reset)) reset = cJSON_GetObjectItemCaseSensitive(window, "resetsAt");
    if (cJSON_IsNumber(reset) && integer_in_range(reset->valuedouble, 0, 253402300799.0)) {
        target.resets_at = static_cast<int64_t>(reset->valuedouble);
    }
    cJSON *minutes = cJSON_GetObjectItemCaseSensitive(window, "window_duration_mins");
    if (!cJSON_IsNumber(minutes)) minutes = cJSON_GetObjectItemCaseSensitive(window, "windowDurationMins");
    if (cJSON_IsNumber(minutes) && integer_in_range(minutes->valuedouble, 1, INT32_MAX)) {
        target.window_minutes = static_cast<int32_t>(minutes->valuedouble);
    } else {
        cJSON *seconds = cJSON_GetObjectItemCaseSensitive(window, "limit_window_seconds");
        if (cJSON_IsNumber(seconds) && integer_in_range(seconds->valuedouble, 60,
                                                        static_cast<double>(INT32_MAX) * 60)) {
            target.window_minutes = static_cast<int32_t>(seconds->valuedouble / 60);
        }
    }
}

bool parse_openai_usage(const char *json, size_t length, ProviderStatus *status)
{
    if (!status) return false; cJSON *root = parse_bounded(json, length); if (!root || !cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    status->window_count = 0;
    status->plan[0] = 0;
    cJSON *plan = cJSON_GetObjectItemCaseSensitive(root, "plan_type");
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
    if (cJSON_IsNumber(available) && integer_in_range(available->valuedouble, 0, UINT32_MAX)) {
        status->reset_credits = {true, static_cast<uint32_t>(available->valuedouble)};
    }
    cJSON_Delete(root); return status->window_count > 0;
}
}
