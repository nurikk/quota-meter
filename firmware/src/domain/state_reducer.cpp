#include "state_reducer.h"

namespace qm {
Screen select_screen(const AppSnapshot &state)
{
    if (state.hardware_error) return Screen::FatalHardwareError;
    if (state.wifi != WifiState::Connected) return Screen::WifiSetup;
    if (state.openai.auth == AuthState::AwaitingUser) return Screen::OpenAiCode;
    if (state.claude.auth == AuthState::AwaitingUser) return Screen::ClaudeManualCode;
    if (state.openai.auth == AuthState::Authenticated || state.claude.auth == AuthState::Authenticated ||
        state.openai.auth == AuthState::Refreshing || state.claude.auth == AuthState::Refreshing) return Screen::Dashboard;
    return Screen::ProviderLogin;
}

QuotaState quota_after_failure(bool has_last_good)
{
    return has_last_good ? QuotaState::Stale : QuotaState::Error;
}

ErrorCode map_http_error(int status)
{
    if (status >= 200 && status < 300) return ErrorCode::None;
    if (status == 401) return ErrorCode::Unauthorized;
    if (status == 403) return ErrorCode::Forbidden;
    if (status == 429) return ErrorCode::Throttled;
    if (status <= 0) return ErrorCode::Network;
    return ErrorCode::InvalidResponse;
}
}
