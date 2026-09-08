#pragma once

#include <stddef.h>
#include <stdint.h>
#include "../domain/models.h"

namespace qm {
inline constexpr char OPENAI_CLIENT_ID[] = "app_EMoamEEZ73f0CkXaXp7hrann";

struct OAuthTokens {
    char access_token[4097];
    char refresh_token[4097];
    char id_token[4097];
    uint32_t expires_in;
};

bool openai_refresh_request(const char *refresh_token, char *output, size_t output_len);
ErrorCode oauth_refresh_error(const HttpResult &response);
bool parse_oauth_tokens(const char *json, size_t length, OAuthTokens *output, bool require_refresh);
bool openai_account_id_from_jwt(const char *jwt, char *output, size_t output_len);
bool parse_openai_usage(const char *json, size_t length, ProviderStatus *status);
}
