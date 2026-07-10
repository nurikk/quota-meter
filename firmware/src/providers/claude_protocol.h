#pragma once
#include <stddef.h>
#include "openai_protocol.h"

namespace qm {
inline constexpr char CLAUDE_CLIENT_ID[] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
inline constexpr char CLAUDE_REDIRECT_URI[] = "https://platform.claude.com/oauth/code/callback";
inline constexpr char CLAUDE_SCOPES[] = "org:create_api_key user:profile user:inference user:sessions:claude_code user:mcp_servers user:file_upload";
inline constexpr char CLAUDE_BETA[] = "oauth-2025-04-20";

bool claude_authorize_url(const char *challenge, const char *state, char *output, size_t output_len);
bool claude_parse_callback(const char *input, const char *expected_state, char *code, size_t code_len);
bool claude_exchange_request(const char *code, const char *state, const char *verifier, char *output, size_t output_len);
bool claude_refresh_request(const char *refresh_token, char *output, size_t output_len);
bool parse_claude_usage(const char *json, size_t length, ProviderStatus *status);
}
