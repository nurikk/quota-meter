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
    if (suffix[0] != 'Z' || suffix[1] != 0) return 0;
    int adjusted_year = year - (month <= 2);
    int era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
    unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
    unsigned adjusted_month = static_cast<unsigned>(month + (month > 2 ? -3 : 9));
    unsigned day_of_year = (153 * adjusted_month + 2) / 5 + static_cast<unsigned>(day) - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

static void add_usage(ProviderStatus *status, const char *label, cJSON *window)
{
    if (!cJSON_IsObject(window) || status->window_count >= 8) return; cJSON *used = cJSON_GetObjectItemCaseSensitive(window, "utilization");
    if (!cJSON_IsNumber(used)) used = cJSON_GetObjectItemCaseSensitive(window, "percent"); if (!cJSON_IsNumber(used)) return;
    QuotaWindow &target = status->windows[status->window_count++]; target = {}; snprintf(target.label, sizeof(target.label), "%s", label);
    target.used_percent = static_cast<float>(used->valuedouble); target.present = true;
    cJSON *reset = cJSON_GetObjectItemCaseSensitive(window, "resets_at");
    if (cJSON_IsString(reset)) target.resets_at = parse_utc_timestamp(reset->valuestring);
    else if (cJSON_IsNumber(reset)) target.resets_at = static_cast<int64_t>(reset->valuedouble);
}
bool parse_claude_usage(const char *json, size_t length, ProviderStatus *status)
{
    if (!json || !status || length == 0 || length > 16384) return false; cJSON *root = cJSON_ParseWithLength(json, length); if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    status->window_count = 0; const char *keys[] = {"five_hour", "seven_day", "seven_day_sonnet", "seven_day_opus", "seven_day_oauth_apps"};
    const char *labels[] = {"5 hour", "7 day", "7 day Sonnet", "7 day Opus", "7 day OAuth apps"};
    for (size_t i = 0; i < 5; ++i) add_usage(status, labels[i], cJSON_GetObjectItemCaseSensitive(root, keys[i]));
    cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, root) {
        bool known = false;
        for (const char *key : keys) {
            if (entry->string && strcmp(entry->string, key) == 0) {
                known = true;
                break;
            }
        }
        if (!known && entry->string && status->window_count < 8) add_usage(status, entry->string, entry);
    }
    cJSON_Delete(root); return status->window_count > 0;
}
}
