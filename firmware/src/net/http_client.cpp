#include "http_client.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "../domain/state_reducer.h" // IWYU pragma: keep
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <time.h>
#endif

namespace qm {
namespace {

bool parse_digits(const char *value, size_t count, int *output)
{
    int parsed = 0;
    for (size_t index = 0; index < count; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        if (!isdigit(character)) return false;
        parsed = parsed * 10 + character - '0';
    }
    *output = parsed;
    return true;
}

bool leap_year(int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

int days_in_month(int year, int month)
{
    static constexpr int DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : DAYS[month - 1];
}

int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = month + (month > 2 ? -3 : 9);
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return static_cast<int64_t>(era) * 146097 + day_of_era - 719468;
}

int month_number(const char *value)
{
    static constexpr const char *MONTHS[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    for (int month = 0; month < 12; ++month) {
        if (memcmp(value, MONTHS[month], 3) == 0) return month + 1;
    }
    return 0;
}

} // namespace

bool parse_retry_after(const char *value, int64_t now, uint32_t *delay_seconds)
{
    if (!value || !delay_seconds || !value[0] || now < 0) return false;
    uint64_t delta = 0;
    bool digits_only = true;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(value); *cursor; ++cursor) {
        if (!isdigit(*cursor)) {
            digits_only = false;
            break;
        }
        const uint32_t digit = *cursor - '0';
        if (delta > (UINT32_MAX - digit) / 10) return false;
        delta = delta * 10 + digit;
    }
    if (digits_only) {
        *delay_seconds = static_cast<uint32_t>(delta);
        return true;
    }

    if (strlen(value) != 29 || value[3] != ',' || value[4] != ' ' || value[7] != ' ' ||
        value[11] != ' ' || value[16] != ' ' || value[19] != ':' || value[22] != ':' ||
        strcmp(value + 25, " GMT") != 0) return false;
    for (size_t index = 0; index < 3; ++index) {
        if (!isalpha(static_cast<unsigned char>(value[index]))) return false;
    }

    int day, year, hour, minute, second;
    const int month = month_number(value + 8);
    if (!parse_digits(value + 5, 2, &day) || !parse_digits(value + 12, 4, &year) ||
        !parse_digits(value + 17, 2, &hour) || !parse_digits(value + 20, 2, &minute) ||
        !parse_digits(value + 23, 2, &second) || month == 0 || year < 1970 ||
        day < 1 || day > days_in_month(year, month) || hour > 23 || minute > 59 || second > 59) return false;

    const int64_t target = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400 +
                           hour * 3600 + minute * 60 + second;
    if (target <= now) {
        *delay_seconds = 0;
        return true;
    }
    const uint64_t difference = static_cast<uint64_t>(target - now);
    if (difference > UINT32_MAX) return false;
    *delay_seconds = static_cast<uint32_t>(difference);
    return true;
}

#ifdef ESP_PLATFORM
struct ResponseContext { HttpResult *result; size_t maximum; };

static esp_err_t event_handler(esp_http_client_event_t *event)
{
    auto *context = static_cast<ResponseContext *>(event->user_data);
    HttpResult &result = *context->result;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value &&
        strcasecmp(event->header_key, "Retry-After") == 0) {
        uint32_t delay = 0;
        if (parse_retry_after(event->header_value, time(nullptr), &delay)) result.retry_after = delay;
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        if (result.body_len + static_cast<size_t>(event->data_len) > context->maximum) {
            result.error = ErrorCode::InvalidResponse;
            return ESP_FAIL;
        }
        memcpy(result.body + result.body_len, event->data, event->data_len);
        result.body_len += event->data_len;
        result.body[result.body_len] = 0;
    }
    return ESP_OK;
}

void https_request(const HttpRequest &request, HttpResult *output)
{
    if (!output) return;
    *output = {};
    ResponseContext context{output, request.max_response > 0 && request.max_response <= 16384 ? request.max_response : 16384};
    esp_http_client_config_t config{};
    config.url = request.url;
    config.event_handler = event_handler;
    config.user_data = &context;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;
    config.buffer_size = 2048;
    config.buffer_size_tx = 6144;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        output->error = ErrorCode::Network;
        return;
    }

    esp_err_t configured = esp_http_client_set_method(
        client, request.method == HttpMethod::Post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    if (configured == ESP_OK) configured = esp_http_client_set_header(client, "Accept", "application/json");
    if (configured == ESP_OK) configured = esp_http_client_set_header(client, "User-Agent", "quota-meter-firmware/0.1.0");
    if (configured == ESP_OK && request.content_type) configured = esp_http_client_set_header(client, "Content-Type", request.content_type);
    if (configured == ESP_OK && request.authorization) configured = esp_http_client_set_header(client, "Authorization", request.authorization);
    if (configured == ESP_OK && request.account_id) configured = esp_http_client_set_header(client, "ChatGPT-Account-Id", request.account_id);
    if (configured == ESP_OK && request.beta) configured = esp_http_client_set_header(client, "anthropic-beta", request.beta);
    if (configured == ESP_OK && request.body) {
        const size_t body_length = strlen(request.body);
        configured = body_length <= INT_MAX
            ? esp_http_client_set_post_field(client, request.body, static_cast<int>(body_length))
            : ESP_ERR_INVALID_SIZE;
    }
    if (configured != ESP_OK) {
        esp_http_client_cleanup(client);
        output->error = ErrorCode::InvalidResponse;
        return;
    }

    const esp_err_t result = esp_http_client_perform(client);
    output->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK && output->error == ErrorCode::None) output->error = ErrorCode::Network;
    output->error = map_http_error(output->status, output->error);
}
#endif

} // namespace qm
