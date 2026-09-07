#pragma once
#include "models.h"
namespace qm {
Screen select_screen(const AppSnapshot &state);
QuotaState quota_after_failure(bool has_last_good);
ErrorCode map_http_error(int status, ErrorCode transport_error = ErrorCode::None);
void apply_credential_failure(ProviderStatus &status, ErrorCode error);
bool can_fetch_quota(const ProviderStatus &status);
}
