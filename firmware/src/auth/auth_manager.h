#pragma once
#include "../domain/models.h"
namespace qm {
void auth_manager_init();
void auth_worker_task(void *argument);
bool auth_enqueue_start(Provider provider);
bool auth_enqueue_callback(const char *callback);
bool auth_enqueue_refresh(Provider provider);
bool auth_enqueue_logout(Provider provider);
}
