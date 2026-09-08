#pragma once
#include <stddef.h>
namespace qm {
bool wifi_qr_payload(const char *ssid, const char *password, char *output, size_t output_len);
bool form_value(const char *body, size_t body_len, const char *key, char *output, size_t output_len);
bool same_origin(const char *origin, const char *host);
bool constant_time_equal(const char *left, const char *right);
}
