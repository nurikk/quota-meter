#pragma once
#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
using esp_err_t = int;
#endif
namespace qm {
void configure_london_timezone();

esp_err_t time_sync_start_and_wait();
bool time_is_plausible();
}
