#pragma once
#include <stddef.h>
#include <stdint.h>
namespace qm {
bool base64url_encode(const uint8_t *input, size_t input_len, char *output, size_t output_len);
bool base64url_decode(const char *input, uint8_t *output, size_t output_len, size_t *written);
}
