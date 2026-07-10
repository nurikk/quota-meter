#pragma once
#include "models.h"
namespace qm {
Screen select_screen(const AppSnapshot &state);
QuotaState quota_after_failure(bool has_last_good);
ErrorCode map_http_error(int status);
}
