idf_component_register(
    SRCS
        "main.cpp"
        "app/app_state.cpp"
        "auth/auth_manager.cpp"
        "auth/pkce.cpp"
        "auth/token_store.cpp"
        "console/wifi_console.cpp"
        "domain/state_reducer.cpp"
        "net/http_client.cpp"
        "net/time_sync.cpp"
        "portal/portal_logic.cpp"
        "portal/wifi_portal.cpp"
        "providers/claude_protocol.cpp"
        "providers/openai_protocol.cpp"
        "ui/assets/claude_logo.c"
        "ui/assets/codex_logo.c"
        "ui/ui.cpp"
        "ui/view_model.cpp"
        "util/base64url.cpp"
    INCLUDE_DIRS "." "app" "auth" "console" "domain" "net" "portal" "providers" "ui" "util"
    REQUIRES esp32_s3_touch_amoled_1_75c espressif__cjson lvgl
    PRIV_REQUIRES console esp_event esp_http_client esp_netif esp_wifi lwip mbedtls nvs_flash esp_timer
    EMBED_TXTFILES "portal/portal.html" "portal/portal.js"
)
