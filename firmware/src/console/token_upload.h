#pragma once
#include "../domain/models.h"
#include "../util/base64url.h"
#include <string.h>

namespace qm {
inline bool parse_upload_account(const char *provider, const char *encoded_name, Account *account)
{
    if (!provider || !encoded_name || !account) return false;
    Account parsed{};
    if (strcmp(provider, "codex") == 0) parsed.provider = Provider::OpenAI;
    else if (strcmp(provider, "claude") == 0) parsed.provider = Provider::Claude;
    else return false;
    size_t length = 0;
    if (!base64url_decode(encoded_name, reinterpret_cast<uint8_t *>(parsed.name),
                          sizeof(parsed.name) - 1, &length) ||
        memchr(parsed.name, 0, length) || !valid_account_name(parsed.name)) return false;
    char canonical[45];
    if (!base64url_encode(reinterpret_cast<const uint8_t *>(parsed.name), length, canonical, sizeof(canonical)) ||
        strcmp(canonical, encoded_name) != 0) return false;
    *account = parsed;
    return true;
}
}
