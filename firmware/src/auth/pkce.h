#pragma once
#include <stddef.h>
#include <stdint.h>
namespace qm {
bool pkce_challenge(const char *verifier, char *challenge, size_t challenge_len);
bool constant_time_equal(const char *left, const char *right);
#ifdef ESP_PLATFORM
bool pkce_generate(char *verifier, size_t verifier_len, char *state, size_t state_len, char *challenge, size_t challenge_len);
#endif
}
