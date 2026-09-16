#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

namespace qm {

enum class WifiState : uint8_t { Idle, Connecting, Connected, Provisioning, Failed };
enum class AuthState : uint8_t { SignedOut, Authenticated, Refreshing, Error, Expired };
enum class QuotaState : uint8_t { Idle, Loading, Fresh, Stale, Error };
enum class Screen : uint8_t { WifiSetup, TokenImport, Dashboard, FatalHardwareError };
enum class Provider : uint8_t { OpenAI, Claude };
using AccountId = uint32_t;
constexpr size_t ACCOUNT_NAME_CAPACITY = 33;
struct Account {
    AccountId id{};
    Provider provider{};
    char name[ACCOUNT_NAME_CAPACITY]{};
    uint32_t generation{};
};
inline bool valid_account_name(const char *name)
{
    const size_t length = strnlen(name, ACCOUNT_NAME_CAPACITY);
    if (!length || length >= ACCOUNT_NAME_CAPACITY || name[0] == ' ' || name[length - 1] == ' ') return false;
    for (size_t i = 0; i < length; ++i) {
        if (static_cast<unsigned char>(name[i]) < 32 || static_cast<unsigned char>(name[i]) > 126) return false;
    }
    return true;
}
inline bool same_account_target(const Account &account, Provider provider, const char *name)
{
    return account.provider == provider && strcmp(account.name, name) == 0;
}
inline const char *provider_name(Provider provider) { return provider == Provider::OpenAI ? "codex" : "claude"; }
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

struct AccountStatus {
    Account account;
    ProviderStatus status{};
};

struct AppSnapshot {
    uint64_t revision{};
    WifiState wifi{};
    std::vector<AccountStatus> accounts;
    bool hardware_error{};
    char ap_ssid[33]{};
    char ap_password[16]{};
    char sta_ip[16]{};

    const AccountStatus *find(AccountId id) const
    {
        for (const auto &entry : accounts) if (entry.account.id == id) return &entry;
        return nullptr;
    }
    ProviderStatus &status(AccountId id)
    {
        for (auto &entry : accounts) if (entry.account.id == id) return entry.status;
        abort();
    }
    const ProviderStatus &status(AccountId id) const
    {
        const auto *entry = find(id);
        if (!entry) abort();
        return entry->status;
    }
};

struct HttpResult {
    int status{};
    ErrorCode error{ErrorCode::None};
    char body[16385]{};
    size_t body_len = 0;
    uint32_t retry_after{};
};

} // namespace qm
