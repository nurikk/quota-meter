#include "pkce.h"
#include "../util/base64url.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "mbedtls/sha256.h"
#else
#include <CommonCrypto/CommonDigest.h>
#endif

namespace qm {
bool pkce_challenge(const char *verifier, char *challenge, size_t cap)
{
    if (!verifier || !challenge || strlen(verifier) < 43 || strlen(verifier) > 128) return false;
    uint8_t digest[32];
#ifdef ESP_PLATFORM
    if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(verifier), strlen(verifier), digest, 0) != 0) return false;
#else
    CC_SHA256(verifier, static_cast<CC_LONG>(strlen(verifier)), digest);
#endif
    return base64url_encode(digest, sizeof(digest), challenge, cap);
}

bool constant_time_equal(const char *left, const char *right)
{
    if (!left || !right) return false;
    size_t left_len = strlen(left), right_len = strlen(right), maximum = left_len > right_len ? left_len : right_len;
    unsigned difference = static_cast<unsigned>(left_len ^ right_len);
    for (size_t i = 0; i < maximum; ++i) {
        unsigned char a = i < left_len ? static_cast<unsigned char>(left[i]) : 0;
        unsigned char b = i < right_len ? static_cast<unsigned char>(right[i]) : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

#ifdef ESP_PLATFORM
bool pkce_generate(char *verifier, size_t verifier_len, char *state, size_t state_len, char *challenge, size_t challenge_len)
{
    uint8_t verifier_bytes[64], state_bytes[32];
    esp_fill_random(verifier_bytes, sizeof(verifier_bytes));
    esp_fill_random(state_bytes, sizeof(state_bytes));
    if (!base64url_encode(verifier_bytes, sizeof(verifier_bytes), verifier, verifier_len) ||
        !base64url_encode(state_bytes, sizeof(state_bytes), state, state_len)) return false;
    return pkce_challenge(verifier, challenge, challenge_len);
}
#endif
}
