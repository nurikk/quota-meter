#include "http_client.h"
#include "../domain/state_reducer.h" // IWYU pragma: keep
#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include <stdlib.h>
#include <string.h>
namespace qm {
struct ResponseContext { HttpResult *result; size_t maximum; };
static esp_err_t event_handler(esp_http_client_event_t *event)
{
    auto *context = static_cast<ResponseContext *>(event->user_data);
    HttpResult &result = *context->result;
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && strcasecmp(event->header_key, "Retry-After") == 0) {
        result.retry_after = static_cast<uint32_t>(strtoul(event->header_value, nullptr, 10));
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
    esp_http_client_set_method(client, request.method == HttpMethod::Post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "quota-meter-firmware/0.1.0");
    if (request.content_type) esp_http_client_set_header(client, "Content-Type", request.content_type);
    if (request.authorization) esp_http_client_set_header(client, "Authorization", request.authorization);
    if (request.account_id) esp_http_client_set_header(client, "ChatGPT-Account-Id", request.account_id);
    if (request.beta) esp_http_client_set_header(client, "anthropic-beta", request.beta);
    if (request.body) esp_http_client_set_post_field(client, request.body, static_cast<int>(strlen(request.body)));
    esp_err_t result = esp_http_client_perform(client);
    output->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK && output->error == ErrorCode::None) output->error = ErrorCode::Network;
    if (output->error == ErrorCode::None) output->error = map_http_error(output->status);
}
}
#endif
