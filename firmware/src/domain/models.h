#pragma once

#include <stddef.h>
#include <stdint.h>

namespace qm {

enum class WifiState : uint8_t { Idle, Connecting, Connected, Provisioning, Failed };
enum class AuthState : uint8_t { SignedOut, Starting, AwaitingUser, Exchanging, Authenticated, Refreshing, Error };
enum class QuotaState : uint8_t { Idle, Loading, Fresh, Stale, Error };
enum class Screen : uint8_t { WifiSetup, ProviderLogin, OpenAiCode, ClaudeManualCode, Dashboard, FatalHardwareError };
enum class Provider : uint8_t { OpenAI, Claude };
enum class ErrorCode : uint8_t { None, Network, InvalidResponse, Unauthorized, Forbidden, Throttled, Storage, StateMismatch, Timeout };

struct QuotaWindow {
    char label[40]{};
    float used_percent{};
    int64_t resets_at{};
    bool present{};
};

struct ProviderStatus {
    AuthState auth;
    QuotaState quota;
    ErrorCode error;
    char plan[32];
    char login_url[768];
    char user_code[32];
    QuotaWindow windows[8];
    uint8_t window_count;
    int64_t fetched_at;
};

struct AppSnapshot {
    WifiState wifi;
    ProviderStatus openai;
    ProviderStatus claude;
    bool hardware_error;
    char ap_ssid[33];
    char ap_password[16];
    char sta_ip[16];
};

struct HttpResult {
    int status{};
    ErrorCode error{ErrorCode::None};
    char body[16385]{};
    size_t body_len = 0;
    uint32_t retry_after{};
};

} // namespace qm
