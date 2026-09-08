#pragma once

#include <stddef.h>
#include <stdint.h>

namespace qm {

enum class WifiState : uint8_t { Idle, Connecting, Connected, Provisioning, Failed };
enum class AuthState : uint8_t { SignedOut, Authenticated, Refreshing, Error, Expired };
enum class QuotaState : uint8_t { Idle, Loading, Fresh, Stale, Error };
enum class Screen : uint8_t { WifiSetup, TokenImport, Dashboard, FatalHardwareError };
enum class Provider : uint8_t { OpenAI, Claude };
enum class ErrorCode : uint8_t { None, Network, InvalidResponse, Unauthorized, Forbidden, Throttled, Storage };

struct QuotaWindow {
    char label[40]{};
    float used_percent{};
    int64_t resets_at{};
    bool present{};
    bool model_limit{};
    int32_t window_minutes{};
};

struct ClaudeExtraUsage {
    bool present{};
    bool is_enabled{};
    bool has_utilization{};
    float utilization{};
    bool has_used_credits{};
    double used_credits{};
    bool has_monthly_limit{};
    double monthly_limit{};
    bool has_spend{};
    double spend{};
    bool has_spend_percent{};
    float spend_percent{};
    uint8_t decimal_places{};
    uint8_t spend_decimal_places{};
    uint8_t limit_decimal_places{};
    char currency[8]{};
};

struct OpenAiResetCredits {
    bool present{};
    uint32_t available_count{};
};

struct ProviderStatus {
    AuthState auth;
    QuotaState quota;
    ErrorCode error;
    char plan[32];
    QuotaWindow windows[8];
    uint8_t window_count;
    ClaudeExtraUsage extra_usage;
    OpenAiResetCredits reset_credits;
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
