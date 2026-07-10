#include "base64url.h"
#include <string.h>

namespace qm {
static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

bool base64url_encode(const uint8_t *in, size_t len, char *out, size_t cap)
{
    if (!out || cap == 0 || (!in && len)) return false;
    size_t needed = (len * 4 + 2) / 3;
    if (needed + 1 > cap) { out[0] = 0; return false; }
    size_t i = 0, j = 0;
    while (i + 2 < len) {
        uint32_t value = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
        out[j++] = alphabet[(value >> 18) & 63]; out[j++] = alphabet[(value >> 12) & 63];
        out[j++] = alphabet[(value >> 6) & 63]; out[j++] = alphabet[value & 63]; i += 3;
    }
    if (i < len) {
        uint32_t value = static_cast<uint32_t>(in[i]) << 16;
        if (i + 1 < len) value |= static_cast<uint32_t>(in[i + 1]) << 8;
        out[j++] = alphabet[(value >> 18) & 63]; out[j++] = alphabet[(value >> 12) & 63];
        if (i + 1 < len) out[j++] = alphabet[(value >> 6) & 63];
    }
    out[j] = 0;
    return true;
}

static int decode_char(char c)
{
    const char *found = strchr(alphabet, c);
    return found ? static_cast<int>(found - alphabet) : -1;
}

bool base64url_decode(const char *in, uint8_t *out, size_t cap, size_t *written)
{
    if (written) *written = 0;
    if (!in || !out) return false;
    size_t len = strlen(in);
    if ((len & 3) == 1 || (len * 3 / 4) > cap) return false;
    uint32_t acc = 0; int bits = 0; size_t used = 0;
    for (size_t i = 0; i < len; ++i) {
        int value = decode_char(in[i]); if (value < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(value); bits += 6;
        if (bits >= 8) { bits -= 8; if (used >= cap) return false; out[used++] = static_cast<uint8_t>(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (written) *written = used;
    return true;
}
}
