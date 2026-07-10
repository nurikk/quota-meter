#include "portal_logic.h"
#include <ctype.h>
#include <string.h>

namespace qm {
static bool put(char *out, size_t cap, size_t *used, char c) { if (*used + 1 >= cap) return false; out[(*used)++] = c; out[*used] = 0; return true; }
bool wifi_qr_payload(const char *ssid, const char *password, char *out, size_t cap)
{
    if (!ssid || !password || !out || cap == 0) return false; size_t used = 0; out[0] = 0; const char *prefix = "WIFI:T:WPA;S:";
    for (const char *p = prefix; *p; ++p) if (!put(out, cap, &used, *p)) return false;
    auto escaped = [&](const char *value) { for (const char *p = value; *p; ++p) { if (strchr("\\;,\":", *p) && !put(out, cap, &used, '\\')) return false; if (!put(out, cap, &used, *p)) return false; } return true; };
    if (!escaped(ssid)) return false; for (const char *p = ";P:"; *p; ++p) if (!put(out, cap, &used, *p)) return false;
    if (!escaped(password)) return false; return put(out, cap, &used, ';') && put(out, cap, &used, ';');
}
static int hex(char c) { if (c >= '0' && c <= '9') return c - '0'; c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }
bool form_value(const char *body, size_t body_len, const char *key, char *out, size_t cap)
{
    if (!body || !key || !out || cap == 0 || body_len > 4096) return false; out[0] = 0; size_t key_len = strlen(key); const char *cursor = body, *limit = body + body_len;
    while (cursor < limit) { const char *end = static_cast<const char *>(memchr(cursor, '&', static_cast<size_t>(limit - cursor))); if (!end) end = limit;
        const char *equal = static_cast<const char *>(memchr(cursor, '=', static_cast<size_t>(end - cursor)));
        if (equal && static_cast<size_t>(equal - cursor) == key_len && memcmp(cursor, key, key_len) == 0) { size_t used = 0;
            for (const char *p = equal + 1; p < end; ++p) { unsigned char value = *p; if (value == '+') value = ' '; else if (value == '%' && p + 2 < end) { int high = hex(p[1]), low = hex(p[2]); if (high < 0 || low < 0) return false; value = static_cast<unsigned char>((high << 4) | low); p += 2; }
                if (value < 0x20 || used + 1 >= cap) return false; out[used++] = static_cast<char>(value); } out[used] = 0; return true; }
        cursor = end < limit ? end + 1 : end;
    }
    return false;
}
bool same_origin(const char *origin, const char *host)
{
    if (!origin || !host) return false; const char *prefix = "http://"; if (strncmp(origin, prefix, strlen(prefix)) != 0) return false;
    const char *authority = origin + strlen(prefix); const char *end = strchr(authority, '/'); size_t len = end ? static_cast<size_t>(end - authority) : strlen(authority);
    return len == strlen(host) && memcmp(authority, host, len) == 0;
}
}
