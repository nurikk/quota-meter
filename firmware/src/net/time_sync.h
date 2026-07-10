#pragma once
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
using esp_err_t = int;
#endif
namespace qm {
esp_err_t time_sync_start_and_wait();
bool time_is_plausible();
}
