#include "wifi_portal.h"
#ifdef ESP_PLATFORM
#include "portal_logic.h"
#include "../app/app_state.h"
#include "../auth/auth_manager.h"
#include "../auth/pkce.h"
#include "../auth/token_store.h"
#include "../providers/cJSON.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace qm {
static constexpr char WIFI_NS[] = "qm_wifi";
static constexpr char SETUP_NS[] = "qm_setup";
static constexpr size_t MAX_NETWORKS = 20;
static const char *TAG = "quota_portal";
static char ap_ssid[33] = "QuotaMeter-Setup";
static char ap_password[16]{};
static char saved_ssid[33]{};
static char saved_password[65]{};
static std::atomic_bool connected;
static std::atomic_bool reconnect_needed;
static std::atomic_bool provisioning;
static std::atomic<int64_t> connection_failed_since_us;
static httpd_handle_t server;
static SemaphoreHandle_t credentials_mutex;
static uint32_t reconnect_delay = 1000;
extern const uint8_t portal_html_start[] asm("_binary_portal_html_start");
extern const uint8_t portal_html_end[] asm("_binary_portal_html_end");
extern const uint8_t portal_js_start[] asm("_binary_portal_js_start");
extern const uint8_t portal_js_end[] asm("_binary_portal_js_end");

const char *portal_password() { return ap_password; }
static void load_setup()
{
    uint8_t mac[6]{}; esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP); snprintf(ap_ssid, sizeof(ap_ssid), "QuotaMeter-%02X%02X%02X", mac[3], mac[4], mac[5]);
    nvs_handle_t handle; if (nvs_open(SETUP_NS, NVS_READWRITE, &handle) != ESP_OK) return; size_t len = sizeof(ap_password);
    if (nvs_get_str(handle, "password", ap_password, &len) != ESP_OK || strlen(ap_password) != 8) {
        snprintf(ap_password, sizeof(ap_password), "%08lu", static_cast<unsigned long>(esp_random() % 100000000));
        if (nvs_set_str(handle, "password", ap_password) == ESP_OK) nvs_commit(handle);
    }
    nvs_close(handle); app_state_set_ap(ap_ssid, ap_password);
}
static bool load_wifi()
{
    nvs_handle_t handle; if (nvs_open(WIFI_NS, NVS_READONLY, &handle) != ESP_OK) return false; size_t ssid_len = sizeof(saved_ssid), password_len = sizeof(saved_password);
    bool valid = nvs_get_str(handle, "ssid", saved_ssid, &ssid_len) == ESP_OK && nvs_get_str(handle, "password", saved_password, &password_len) == ESP_OK && saved_ssid[0]; nvs_close(handle); return valid;
}
static bool save_wifi(const char *ssid, const char *password)
{
    nvs_handle_t handle; if (nvs_open(WIFI_NS, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t result = nvs_set_str(handle, "ssid", ssid); if (result == ESP_OK) result = nvs_set_str(handle, "password", password); if (result == ESP_OK) result = nvs_commit(handle); nvs_close(handle); return result == ESP_OK;
}
void clear_wifi_credentials()
{
    nvs_handle_t handle; if (nvs_open(WIFI_NS, NVS_READWRITE, &handle) == ESP_OK) { if (nvs_erase_all(handle) == ESP_OK) nvs_commit(handle); nvs_close(handle); }
    secure_clear(saved_ssid, sizeof(saved_ssid)); secure_clear(saved_password, sizeof(saved_password));
}
static void configure_ap()
{
    wifi_config_t config{};
    snprintf(reinterpret_cast<char *>(config.ap.ssid), sizeof(config.ap.ssid), "%s", ap_ssid);
    snprintf(reinterpret_cast<char *>(config.ap.password), sizeof(config.ap.password), "%s", ap_password);
    config.ap.ssid_len = strlen(ap_ssid);
    config.ap.channel = 1;
    config.ap.max_connection = 4;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    provisioning = true;
    app_state_set_wifi(WifiState::Provisioning, "0.0.0.0");
}
static void connect_sta()
{
    wifi_config_t config{};
    size_t ssid_length = strlen(saved_ssid);
    size_t password_length = strlen(saved_password);
    memcpy(config.sta.ssid, saved_ssid, ssid_length);
    memcpy(config.sta.password, saved_password, password_length);
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    if (connection_failed_since_us.load() == 0) connection_failed_since_us = esp_timer_get_time();
    app_state_set_wifi(provisioning ? WifiState::Provisioning : WifiState::Connecting, "0.0.0.0");
    esp_wifi_connect();
}

bool wifi_connect_and_save(const char *ssid, const char *password)
{
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 64 || !credentials_mutex) return false;
    if (xSemaphoreTake(credentials_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    bool saved = save_wifi(ssid, password);
    if (saved) {
        snprintf(saved_ssid, sizeof(saved_ssid), "%s", ssid);
        snprintf(saved_password, sizeof(saved_password), "%s", password);
        connection_failed_since_us = esp_timer_get_time();
        saved = esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK;
        if (saved) {
            configure_ap();
            connect_sta();
        }
    }
    xSemaphoreGive(credentials_mutex);
    return saved;
}
static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false;
        reconnect_needed = true;
        if (connection_failed_since_us.load() == 0) connection_failed_since_us = esp_timer_get_time();
        app_state_set_wifi(provisioning ? WifiState::Provisioning : WifiState::Connecting, "0.0.0.0");
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(data);
        char ip[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));
        connected = true;
        reconnect_needed = false;
        provisioning = false;
        connection_failed_since_us = 0;
        reconnect_delay = 1000;
        app_state_set_wifi(WifiState::Connected, ip);
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
}

static bool cookie_authorized(httpd_req_t *request)
{
    char cookie[128]; if (httpd_req_get_hdr_value_str(request, "Cookie", cookie, sizeof(cookie)) != ESP_OK) return false;
    char expected[64]; snprintf(expected, sizeof(expected), "qm_settings=%s", ap_password); const char *found = strstr(cookie, expected);
    return found && (found == cookie || found[-1] == ' ' || found[-1] == ';') && (found[strlen(expected)] == 0 || found[strlen(expected)] == ';');
}
static bool mutation_allowed(httpd_req_t *request, bool allow_without_cookie = false)
{
    if (!allow_without_cookie && !cookie_authorized(request)) {
        httpd_resp_set_status(request, "401 Unauthorized");
        httpd_resp_set_hdr(request, "WWW-Authenticate", "Basic realm=\"Quota Meter settings\"");
        httpd_resp_sendstr(request, "{\"error\":\"authentication required\"}");
        return false;
    }
    char origin[96], host[64];
    if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK || !same_origin(origin, host)) {
        httpd_resp_set_status(request, "403 Forbidden");
        httpd_resp_sendstr(request, "{\"error\":\"same-origin request required\"}");
        return false;
    }
    return true;
}
static esp_err_t root_handler(httpd_req_t *request)
{
    char query[64], password[16];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK && httpd_query_key_value(query, "p", password, sizeof(password)) == ESP_OK && constant_time_equal(password, ap_password)) {
        char cookie[96]; snprintf(cookie, sizeof(cookie), "qm_settings=%s; Path=/; HttpOnly; SameSite=Strict", ap_password); httpd_resp_set_hdr(request, "Set-Cookie", cookie); httpd_resp_set_status(request, "302 Found"); httpd_resp_set_hdr(request, "Location", "/"); return httpd_resp_send(request, nullptr, 0);
    }
    httpd_resp_set_type(request, "text/html"); httpd_resp_set_hdr(request, "Cache-Control", "no-store"); size_t length = static_cast<size_t>(portal_html_end - portal_html_start); if (length && portal_html_start[length - 1] == 0) --length;
    return httpd_resp_send(request, reinterpret_cast<const char *>(portal_html_start), length);
}
static esp_err_t js_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/javascript"); httpd_resp_set_hdr(request, "Cache-Control", "no-store"); size_t length = static_cast<size_t>(portal_js_end - portal_js_start); if (length && portal_js_start[length - 1] == 0) --length;
    return httpd_resp_send(request, reinterpret_cast<const char *>(portal_js_start), length);
}
static const char *auth_name(AuthState state) { const char *names[] = {"signed_out","starting","awaiting_user","exchanging","authenticated","refreshing","error","expired"}; size_t value = static_cast<size_t>(state); return value < sizeof(names) / sizeof(names[0]) ? names[value] : "error"; }
static const char *quota_name(QuotaState state) { const char *names[] = {"idle","loading","fresh","stale","error"}; size_t value = static_cast<size_t>(state); return value < 5 ? names[value] : "error"; }
static void provider_json(cJSON *root, const char *name, const ProviderStatus &status)
{
    cJSON *provider = cJSON_CreateObject(); cJSON_AddStringToObject(provider, "auth", auth_name(status.auth)); cJSON_AddStringToObject(provider, "quota", quota_name(status.quota)); cJSON_AddStringToObject(provider, "plan", status.plan);
    cJSON_AddNumberToObject(provider, "window_count", status.window_count); cJSON_AddNumberToObject(provider, "fetched_at", static_cast<double>(status.fetched_at)); cJSON_AddItemToObject(root, name, provider);
}
static esp_err_t status_handler(httpd_req_t *request)
{
    AppSnapshot snapshot = app_state_get(); cJSON *root = cJSON_CreateObject(); cJSON_AddNumberToObject(root, "wifi", static_cast<int>(snapshot.wifi)); cJSON_AddStringToObject(root, "ip", snapshot.sta_ip);
    provider_json(root, "openai", snapshot.openai); provider_json(root, "claude", snapshot.claude); char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root); if (!text) return ESP_FAIL;
    httpd_resp_set_type(request, "application/json"); httpd_resp_set_hdr(request, "Cache-Control", "no-store"); esp_err_t result = httpd_resp_sendstr(request, text); cJSON_free(text); return result;
}
static esp_err_t scan_handler(httpd_req_t *request)
{
    uint16_t count = MAX_NETWORKS; wifi_ap_record_t records[MAX_NETWORKS]{}; esp_wifi_scan_start(nullptr, true); if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) count = 0;
    cJSON *root = cJSON_CreateObject(), *networks = cJSON_CreateArray(); for (uint16_t i = 0; i < count; ++i) { cJSON *network = cJSON_CreateObject(); cJSON_AddStringToObject(network, "ssid", reinterpret_cast<char *>(records[i].ssid)); cJSON_AddNumberToObject(network, "rssi", records[i].rssi); cJSON_AddBoolToObject(network, "secure", records[i].authmode != WIFI_AUTH_OPEN); cJSON_AddItemToArray(networks, network); }
    cJSON_AddItemToObject(root, "networks", networks); char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root); httpd_resp_set_type(request, "application/json"); esp_err_t result = text ? httpd_resp_sendstr(request, text) : ESP_FAIL; cJSON_free(text); return result;
}
static bool read_body(httpd_req_t *request, char *body, size_t cap)
{
    if (request->content_len <= 0 || static_cast<size_t>(request->content_len) >= cap) return false; int total = 0;
    while (total < request->content_len) { int read = httpd_req_recv(request, body + total, request->content_len - total); if (read <= 0) return false; total += read; }
    body[total] = 0; return true;
}
static esp_err_t wifi_save_handler(httpd_req_t *request)
{
    if (!mutation_allowed(request, provisioning.load())) return ESP_OK;
    char body[384], ssid[33], password[65];
    if (!read_body(request, body, sizeof(body)) ||
        !form_value(body, strlen(body), "ssid", ssid, sizeof(ssid)) ||
        !form_value(body, strlen(body), "password", password, sizeof(password))) {
        secure_clear(body, sizeof(body));
        httpd_resp_set_status(request, "400 Bad Request");
        return httpd_resp_sendstr(request, "{\"ok\":false}");
    }
    bool saved = wifi_connect_and_save(ssid, password);
    secure_clear(password, sizeof(password));
    secure_clear(body, sizeof(body));
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, saved ? "{\"ok\":true}" : "{\"ok\":false}");
}
static esp_err_t provider_handler(httpd_req_t *request)
{
    if (!mutation_allowed(request)) return ESP_OK; const char *uri = request->uri; bool ok = false;
    if (strcmp(uri, "/api/auth/openai/start") == 0) ok = auth_enqueue_start(Provider::OpenAI);
    else if (strcmp(uri, "/api/auth/claude/start") == 0) ok = auth_enqueue_start(Provider::Claude);
    else if (strcmp(uri, "/api/providers/openai/refresh") == 0) ok = auth_enqueue_refresh(Provider::OpenAI);
    else if (strcmp(uri, "/api/providers/claude/refresh") == 0) ok = auth_enqueue_refresh(Provider::Claude);
    else if (request->method == HTTP_DELETE && strcmp(uri, "/api/providers/openai") == 0) ok = auth_enqueue_logout(Provider::OpenAI);
    else if (request->method == HTTP_DELETE && strcmp(uri, "/api/providers/claude") == 0) ok = auth_enqueue_logout(Provider::Claude);
    else if (strcmp(uri, "/api/auth/claude/code") == 0) { char body[2300], callback[2049]; if (read_body(request, body, sizeof(body)) && form_value(body, strlen(body), "code", callback, sizeof(callback))) ok = auth_enqueue_callback(callback); secure_clear(callback, sizeof(callback)); secure_clear(body, sizeof(body)); }
    httpd_resp_set_type(request, "application/json"); httpd_resp_set_status(request, ok ? "202 Accepted" : "400 Bad Request"); return httpd_resp_sendstr(request, ok ? "{\"accepted\":true}" : "{\"accepted\":false}");
}
static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *)) { httpd_uri_t config{}; config.uri = uri; config.method = method; config.handler = handler; httpd_register_uri_handler(server, &config); }
static esp_err_t captive_handler(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", provisioning ? "http://192.168.4.1/" : "/");
    return httpd_resp_send(request, nullptr, 0);
}

static void start_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.max_req_hdr_len = 1024;
    config.max_uri_len = 256;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    register_uri("/", HTTP_GET, root_handler);
    register_uri("/portal.js", HTTP_GET, js_handler);
    register_uri("/api/status", HTTP_GET, status_handler);
    register_uri("/api/wifi/scan", HTTP_GET, scan_handler);
    register_uri("/api/wifi", HTTP_POST, wifi_save_handler);
    const char *posts[] = {"/api/auth/openai/start","/api/auth/claude/start","/api/auth/claude/code","/api/providers/openai/refresh","/api/providers/claude/refresh"};
    for (const char *uri : posts) register_uri(uri, HTTP_POST, provider_handler);
    register_uri("/api/providers/openai", HTTP_DELETE, provider_handler);
    register_uri("/api/providers/claude", HTTP_DELETE, provider_handler);
    register_uri("/*", HTTP_GET, captive_handler);
}
static void dns_task(void *)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(53);
    address.sin_addr.s_addr = INADDR_ANY;
    if (socket_fd < 0 || bind(socket_fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ESP_LOGE(TAG, "Could not start captive DNS");
        vTaskDelete(nullptr);
    }
    uint8_t packet[512];
    while (true) {
        sockaddr_in source{};
        socklen_t source_len = sizeof(source);
        int length = recvfrom(socket_fd, packet, sizeof(packet) - 16, 0, reinterpret_cast<sockaddr *>(&source), &source_len);
        if (length < 12 || !provisioning) continue;
        packet[2] = 0x81;
        packet[3] = 0x80;
        packet[6] = 0;
        packet[7] = 1;
        uint8_t answer[] = {0xC0,0x0C,0,1,0,1,0,0,0,60,0,4,192,168,4,1};
        if (length + static_cast<int>(sizeof(answer)) > static_cast<int>(sizeof(packet))) continue;
        memcpy(packet + length, answer, sizeof(answer));
        sendto(socket_fd, packet, length + sizeof(answer), 0, reinterpret_cast<sockaddr *>(&source), source_len);
    }
}
void wifi_portal_init()
{
    credentials_mutex = xSemaphoreCreateMutex();
    if (!credentials_mutex) abort();
    load_setup();
    bool saved = load_wifi();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(saved ? WIFI_MODE_STA : WIFI_MODE_APSTA));
    if (!saved) configure_ap();
    ESP_ERROR_CHECK(esp_wifi_start());
    start_server();
    if (xTaskCreate(dns_task, "captive_dns", 3072, nullptr, 3, nullptr) != pdPASS) abort();
    if (saved) connect_sta();
}
void wifi_task(void *)
{
    while (true) {
        if (reconnect_needed && saved_ssid[0]) {
            int64_t failed_since = connection_failed_since_us.load();
            if (!provisioning && failed_since > 0 && esp_timer_get_time() - failed_since >= 30000000) {
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
                configure_ap();
            }
            vTaskDelay(pdMS_TO_TICKS(reconnect_delay));
            if (!connected) esp_wifi_connect();
            reconnect_delay = reconnect_delay < 60000 ? reconnect_delay * 2 : 60000;
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}
}
#endif
