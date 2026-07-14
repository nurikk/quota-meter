#include "claude_protocol.h"
#include "../auth/pkce.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace qm {
static void secure_zero(void *value, size_t length)
{
    volatile unsigned char *bytes = static_cast<volatile unsigned char *>(value);
    while (length-- > 0) *bytes++ = 0;
}

static bool append(char *out, size_t cap, size_t *used, const char *text)
{
    size_t len = strlen(text); if (*used + len >= cap) return false; memcpy(out + *used, text, len); *used += len; out[*used] = 0; return true;
}
static bool url_encode(char *out, size_t cap, size_t *used, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') { char c[2] = {static_cast<char>(*p), 0}; if (!append(out, cap, used, c)) return false; }
        else { char encoded[4] = {'%', hex[*p >> 4], hex[*p & 15], 0}; if (!append(out, cap, used, encoded)) return false; }
    }
    return true;
}
static bool decode_component(const char *begin, size_t len, char *out, size_t cap)
{
    size_t used = 0; for (size_t i = 0; i < len; ++i) {
        unsigned char value = static_cast<unsigned char>(begin[i]);
        if (value == '%' && i + 2 < len && isxdigit(begin[i + 1]) && isxdigit(begin[i + 2])) {
            auto digit = [](char c) { return c <= '9' ? c - '0' : (tolower(c) - 'a' + 10); };
            value = static_cast<unsigned char>((digit(begin[i + 1]) << 4) | digit(begin[i + 2])); i += 2;
        } else if (value == '+') value = ' ';
        if (value < 0x20 || used + 1 >= cap) return false; out[used++] = static_cast<char>(value);
    }
    out[used] = 0; return used > 0;
}

bool claude_authorize_url(const char *challenge, const char *state, char *out, size_t cap)
{
    if (!challenge || !state || strlen(challenge) > 128 || strlen(state) > 128 || cap == 0) return false;
    size_t used = 0; out[0] = 0;
    return append(out, cap, &used, "https://claude.ai/oauth/authorize?code=true&client_id=") && append(out, cap, &used, CLAUDE_CLIENT_ID) &&
        append(out, cap, &used, "&response_type=code&redirect_uri=") && url_encode(out, cap, &used, CLAUDE_REDIRECT_URI) &&
        append(out, cap, &used, "&scope=") && url_encode(out, cap, &used, CLAUDE_SCOPES) && append(out, cap, &used, "&code_challenge=") &&
        url_encode(out, cap, &used, challenge) && append(out, cap, &used, "&code_challenge_method=S256&state=") && url_encode(out, cap, &used, state);
}

bool claude_parse_callback(const char *input, const char *expected, char *code, size_t cap)
{
    if (!input || !expected || !code || strlen(input) > 2048) return false; code[0] = 0;
    char state[129] = {}; const char *hash = strchr(input, '#');
    if (hash && !strchr(hash + 1, '#')) {
        if (!decode_component(input, static_cast<size_t>(hash - input), code, cap) || !decode_component(hash + 1, strlen(hash + 1), state, sizeof(state))) return false;
    } else {
        const char *query = strchr(input, '?'); query = query ? query + 1 : input;
        const char *cursor = query; bool got_code = false, got_state = false;
        while (*cursor) {
            const char *end = strchr(cursor, '&'); if (!end) end = cursor + strlen(cursor);
            const char *equal = static_cast<const char *>(memchr(cursor, '=', static_cast<size_t>(end - cursor)));
            if (equal) {
                size_t key_len = static_cast<size_t>(equal - cursor);
                if (key_len == 4 && strncmp(cursor, "code", 4) == 0) got_code = decode_component(equal + 1, static_cast<size_t>(end - equal - 1), code, cap);
                if (key_len == 5 && strncmp(cursor, "state", 5) == 0) got_state = decode_component(equal + 1, static_cast<size_t>(end - equal - 1), state, sizeof(state));
            }
            cursor = *end ? end + 1 : end;
        }
        if (!got_code || !got_state) return false;
    }
    if (strlen(code) > 1024 || !constant_time_equal(state, expected)) { secure_zero(code, cap); return false; }
    return true;
}

static bool token_body(const char *grant, const char *value_key, const char *value, const char *state, const char *verifier, char *out, size_t cap)
{
    if (!value || strlen(value) > 4096) return false; cJSON *root = cJSON_CreateObject(); if (!root) return false;
    cJSON_AddStringToObject(root, "grant_type", grant); cJSON_AddStringToObject(root, "client_id", CLAUDE_CLIENT_ID); cJSON_AddStringToObject(root, value_key, value);
    if (state) { cJSON_AddStringToObject(root, "state", state); cJSON_AddStringToObject(root, "redirect_uri", CLAUDE_REDIRECT_URI); cJSON_AddStringToObject(root, "code_verifier", verifier); }
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root); if (!text || strlen(text) >= cap) { cJSON_free(text); return false; }
    memcpy(out, text, strlen(text) + 1); cJSON_free(text); return true;
}
bool claude_exchange_request(const char *code, const char *state, const char *verifier, char *out, size_t cap)
{ return token_body("authorization_code", "code", code, state, verifier, out, cap); }
bool claude_refresh_request(const char *token, char *out, size_t cap)
{ return token_body("refresh_token", "refresh_token", token, nullptr, nullptr, out, cap); }

static int64_t parse_utc_timestamp(const char *value)
{
    if (!value) return 0;
    int year, month, day, hour, minute, second;
    if (sscanf(value, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6 ||
        year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) return 0;

    const char *suffix = value + 19;
    if (*suffix == '.') {
        ++suffix;
        if (!isdigit(static_cast<unsigned char>(*suffix))) return 0;
        while (isdigit(static_cast<unsigned char>(*suffix))) ++suffix;
    }

    int offset_seconds = 0;
    if (suffix[0] == 'Z' && suffix[1] == 0) {
        offset_seconds = 0;
    } else if (suffix[0] == '+' || suffix[0] == '-') {
        int offset_hours, offset_minutes, consumed = 0;
        if (sscanf(suffix + 1, "%2d:%2d%n", &offset_hours, &offset_minutes, &consumed) != 2 ||
            consumed != 5 || suffix[6] != 0 || offset_hours > 23 || offset_minutes > 59) return 0;
        offset_seconds = (offset_hours * 60 + offset_minutes) * 60;
        if (suffix[0] == '-') offset_seconds = -offset_seconds;
    } else {
        return 0;
    }

    int adjusted_year = year - (month <= 2);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    unsigned adjusted_month = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    unsigned day_of_year = (153 * adjusted_month + 2) / 5 + static_cast<unsigned>(day) - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
}

static void add_usage(ProviderStatus *status, const char *label, cJSON *window, bool model_limit = false,
                      int32_t window_minutes = 0)
{
    if (!cJSON_IsObject(window) || status->window_count >= 8) return;
    cJSON *used = cJSON_GetObjectItemCaseSensitive(window, "utilization");
    if (!cJSON_IsNumber(used)) used = cJSON_GetObjectItemCaseSensitive(window, "percent");
    if (!cJSON_IsNumber(used)) return;
    QuotaWindow &target = status->windows[status->window_count++];
    target = {};
    snprintf(target.label, sizeof(target.label), "%s", label);
    target.used_percent = static_cast<float>(used->valuedouble);
    target.present = true;
    target.model_limit = model_limit;
    target.window_minutes = window_minutes;
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(window, "resets_at");
    if (cJSON_IsString(reset)) target.resets_at = parse_utc_timestamp(reset->valuestring);
    else if (cJSON_IsNumber(reset)) target.resets_at = static_cast<int64_t>(reset->valuedouble);
}

static const char *model_name(cJSON *model)
{
    if (cJSON_IsString(model)) return model->valuestring;
    if (!cJSON_IsObject(model)) return nullptr;
    const char *keys[] = {"display_name", "name", "id"};
    for (const char *key : keys) {
        cJSON *value = cJSON_GetObjectItemCaseSensitive(model, key);
        if (cJSON_IsString(value)) return value->valuestring;
    }
    return nullptr;
}

static const char *limit_display_name(cJSON *limit)
{
    cJSON *scope = cJSON_GetObjectItemCaseSensitive(limit, "scope");
    if (cJSON_IsObject(scope)) {
        const char *name = model_name(cJSON_GetObjectItemCaseSensitive(scope, "model"));
        if (name) return name;
    }
    const char *name = model_name(cJSON_GetObjectItemCaseSensitive(limit, "model"));
    if (name) return name;
    cJSON *display = cJSON_GetObjectItemCaseSensitive(limit, "display_name");
    return cJSON_IsString(display) ? display->valuestring : nullptr;
}

static double decimal_amount(double minor, uint8_t exponent)
{
    while (exponent-- > 0) minor /= 10.0;
    return minor;
}

static bool parse_money(cJSON *object, double *amount, uint8_t *exponent, char *currency, size_t currency_size)
{
    if (!cJSON_IsObject(object)) return false;
    cJSON *minor = cJSON_GetObjectItemCaseSensitive(object, "amount_minor");
    cJSON *power = cJSON_GetObjectItemCaseSensitive(object, "exponent");
    if (!cJSON_IsNumber(minor) || !cJSON_IsNumber(power) || power->valuedouble < 0 || power->valuedouble > 6) return false;
    *exponent = static_cast<uint8_t>(power->valuedouble);
    *amount = decimal_amount(minor->valuedouble, *exponent);
    cJSON *code = cJSON_GetObjectItemCaseSensitive(object, "currency");
    if (cJSON_IsString(code)) snprintf(currency, currency_size, "%s", code->valuestring);
    return true;
}

static void parse_extra_usage(cJSON *root, ClaudeExtraUsage *extra)
{
    *extra = {};
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, "extra_usage");
    if (cJSON_IsObject(object)) {
        extra->present = true;
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(object, "is_enabled");
        extra->is_enabled = cJSON_IsTrue(enabled);
        cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "utilization");
        if (cJSON_IsNumber(value)) { extra->has_utilization = true; extra->utilization = static_cast<float>(value->valuedouble); }
        value = cJSON_GetObjectItemCaseSensitive(object, "decimal_places");
        if (cJSON_IsNumber(value) && value->valuedouble >= 0 && value->valuedouble <= 6) extra->decimal_places = static_cast<uint8_t>(value->valuedouble);
        value = cJSON_GetObjectItemCaseSensitive(object, "used_credits");
        if (cJSON_IsNumber(value)) {
            extra->has_used_credits = true;
            extra->used_credits = decimal_amount(value->valuedouble, extra->decimal_places);
            extra->spend_decimal_places = extra->decimal_places;
        }
        value = cJSON_GetObjectItemCaseSensitive(object, "monthly_limit");
        if (cJSON_IsNumber(value)) {
            extra->has_monthly_limit = true;
            extra->monthly_limit = decimal_amount(value->valuedouble, extra->decimal_places);
            extra->limit_decimal_places = extra->decimal_places;
        }
        value = cJSON_GetObjectItemCaseSensitive(object, "currency");
        if (cJSON_IsString(value)) snprintf(extra->currency, sizeof(extra->currency), "%s", value->valuestring);
    }

    cJSON *spend = cJSON_GetObjectItemCaseSensitive(root, "spend");
    if (!cJSON_IsObject(spend)) return;
    extra->present = true;
    cJSON *percent = cJSON_GetObjectItemCaseSensitive(spend, "percent");
    if (cJSON_IsNumber(percent)) {
        extra->has_spend_percent = true;
        extra->spend_percent = static_cast<float>(percent->valuedouble);
    }
    uint8_t exponent = 0;
    if (parse_money(cJSON_GetObjectItemCaseSensitive(spend, "used"), &extra->spend, &exponent,
                    extra->currency, sizeof(extra->currency))) {
        extra->has_spend = true;
        extra->spend_decimal_places = exponent;
    }
    if (parse_money(cJSON_GetObjectItemCaseSensitive(spend, "limit"), &extra->monthly_limit, &exponent,
                    extra->currency, sizeof(extra->currency))) {
        extra->has_monthly_limit = true;
        extra->limit_decimal_places = exponent;
    }
}

bool parse_claude_usage(const char *json, size_t length, ProviderStatus *status)
{
    if (!json || !status || length == 0 || length > 16384) return false;
    cJSON *root = cJSON_ParseWithLength(json, length);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    status->window_count = 0;
    parse_extra_usage(root, &status->extra_usage);
    const char *keys[] = {"five_hour", "seven_day", "seven_day_sonnet", "seven_day_opus", "seven_day_oauth_apps"};
    const char *labels[] = {"5 hour", "7 day", "7 day Sonnet", "7 day Opus", "7 day OAuth apps"};
    const int32_t durations[] = {300, 10080, 10080, 10080, 10080};
    for (size_t i = 0; i < 5; ++i) {
        add_usage(status, labels[i], cJSON_GetObjectItemCaseSensitive(root, keys[i]), false, durations[i]);
    }

    cJSON *limits = cJSON_GetObjectItemCaseSensitive(root, "limits");
    cJSON *limit = nullptr;
    cJSON_ArrayForEach(limit, limits) {
        const char *name = limit_display_name(limit);
        if (name) add_usage(status, name, limit, true, 10080);
    }
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        bool known = !entry->string || strcmp(entry->string, "limits") == 0 ||
                     strcmp(entry->string, "extra_usage") == 0 || strcmp(entry->string, "spend") == 0;
        for (const char *key : keys) if (entry->string && strcmp(entry->string, key) == 0) known = true;
        if (!known) add_usage(status, entry->string, entry);
    }
    cJSON_Delete(root);
    return status->window_count > 0 || status->extra_usage.present;
}
}
