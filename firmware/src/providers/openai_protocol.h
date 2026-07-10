#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../domain/models.h"

namespace qm {
inline constexpr char OPENAI_CLIENT_ID[] = "app_EMoamEEZ73f0CkXaXp7hrann";
inline constexpr char OPENAI_DEVICE_URL[] = "https://auth.openai.com/codex/device";
inline constexpr char OPENAI_REDIRECT_URI[] = "https://auth.openai.com/deviceauth/callback";

struct OpenAiDeviceCode {
    char device_auth_id[256];
    char user_code[32];
    uint32_t interval;
};
struct OAuthTokens {
    char access_token[4097];
    char refresh_token[4097];
    char id_token[4097];
    uint32_t expires_in;
};

bool openai_user_code_request(char *output, size_t output_len);
bool openai_poll_request(const OpenAiDeviceCode &code, char *output, size_t output_len);
bool openai_exchange_request(const char *authorization_code, const char *verifier, char *output, size_t output_len);
bool openai_refresh_request(const char *refresh_token, char *output, size_t output_len);
bool parse_openai_device_code(const char *json, size_t length, OpenAiDeviceCode *output);
bool parse_oauth_tokens(const char *json, size_t length, OAuthTokens *output, bool require_refresh);
bool openai_account_id_from_jwt(const char *jwt, char *output, size_t output_len);
bool parse_openai_usage(const char *json, size_t length, ProviderStatus *status);
}
