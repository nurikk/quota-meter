#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../domain/models.h"
namespace qm {
enum class HttpMethod { Get, Post };
struct HttpRequest {
    HttpMethod method;
    const char *url;
    const char *body;
    const char *content_type;
    const char *authorization;
    const char *account_id;
    const char *beta;
    size_t max_response;
};
bool parse_retry_after(const char *value, int64_t now, uint32_t *delay_seconds);
void https_request(const HttpRequest &request, HttpResult *output);
}
