#pragma once
namespace qm {
void wifi_portal_init();
void wifi_task(void *argument);
bool clear_wifi_credentials();
bool wifi_connect_and_save(const char *ssid, const char *password);
const char *portal_password();
}
