#pragma once

#include <stddef.h>
#include "openai_protocol.h"

namespace qm {
inline constexpr char CLAUDE_CLIENT_ID[] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
inline constexpr char CLAUDE_BETA[] = "oauth-2025-04-20";

bool claude_refresh_request(const char *refresh_token, char *output, size_t output_len);
bool parse_claude_usage(const char *json, size_t length, ProviderStatus *status);
}
