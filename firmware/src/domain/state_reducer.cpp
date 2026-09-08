#include "state_reducer.h"

namespace qm {
Screen select_screen(const AppSnapshot &state)
{
    if (state.hardware_error) return Screen::FatalHardwareError;
    if (state.wifi != WifiState::Connected) return Screen::WifiSetup;
    if (state.openai.auth == AuthState::Authenticated || state.claude.auth == AuthState::Authenticated ||
        state.openai.auth == AuthState::Refreshing || state.claude.auth == AuthState::Refreshing ||
        state.openai.auth == AuthState::Expired || state.claude.auth == AuthState::Expired) return Screen::Dashboard;
    return Screen::TokenImport;
}

bool has_cached_usage(const ProviderStatus &status)
{
    return status.window_count > 0 || status.extra_usage.present || status.reset_credits.present;
}

QuotaState quota_after_failure(bool has_last_good)
{
    return has_last_good ? QuotaState::Stale : QuotaState::Error;
}

ErrorCode map_http_error(int status, ErrorCode transport_error)
{
    if (status == 401) return ErrorCode::Unauthorized;
    if (status == 403) return ErrorCode::Forbidden;
    if (status == 429) return ErrorCode::Throttled;
    if (transport_error != ErrorCode::None) return transport_error;
    if (status >= 200 && status < 300) return ErrorCode::None;
    if (status <= 0) return ErrorCode::Network;
    return ErrorCode::InvalidResponse;
}

void apply_credential_failure(ProviderStatus &status, ErrorCode error)
{
    status.auth = error == ErrorCode::Unauthorized ? AuthState::Expired : AuthState::Authenticated;
    status.error = error;
    status.quota = quota_after_failure(has_cached_usage(status));
}

bool can_fetch_quota(const ProviderStatus &status)
{
    return status.auth == AuthState::Authenticated || status.auth == AuthState::Refreshing;
}
}
