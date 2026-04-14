#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_engine.h"
#include "mode_manager.h"
#include "nvs_flash.h"

static const char *TAG = "main";

#define WIFI_STA_SSID           "ran_gen_gang"
#define WIFI_STA_PASS           "coinplusfire"
#define WIFI_CONNECTED_BIT      BIT0
#define WIFI_FAIL_BIT           BIT1
#define WIFI_MAX_RETRY          10
#define WIFI_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_event_group;
static int  s_wifi_retry_num  = 0;
static bool s_webserver_started = false;
static esp_event_handler_instance_t s_wifi_any_id_handler;
static esp_event_handler_instance_t s_wifi_got_ip_handler;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static esp_err_t send_file(httpd_req_t *req, const char *path, const char *content_type)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type);
    char buf[512];
    while (1) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0) httpd_resp_send_chunk(req, buf, n);
        if (n < sizeof(buf)) break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static bool extract_json_string(const char *json, const char *key, char *out, size_t out_len)
{
    char key_pat[32];
    snprintf(key_pat, sizeof(key_pat), "\"%s\"", key);
    const char *p = strstr(json, key_pat);
    if (!p) return false;
    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\"') p++;
    size_t idx = 0;
    while (*p && *p != '\"' && *p != ',' && *p != '}' && idx + 1 < out_len) {
        out[idx++] = *p++;
    }
    out[idx] = '\0';
    return idx > 0;
}

static bool extract_json_bool(const char *json, const char *key, bool *out_value)
{
    char value[12];
    if (!extract_json_string(json, key, value, sizeof(value))) return false;
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0)  { *out_value = true;  return true; }
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0) { *out_value = false; return true; }
    return false;
}

static uint8_t palette_name_to_id(const char *name)
{
    if (strcmp(name, "sunset") == 0) return 0;
    if (strcmp(name, "ocean")  == 0) return 1;
    if (strcmp(name, "forest") == 0) return 2;
    if (strcmp(name, "neon")   == 0) return 3;
    return 0;
}

static const char *palette_id_to_name(uint8_t id)
{
    switch (id) {
    case 0: return "sunset";
    case 1: return "ocean";
    case 2: return "forest";
    case 3: return "neon";
    default: return "sunset";
    }
}

static void hsv_to_rgb(const hsv_t *hsv, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float h  = (float)(hsv->hue % 360);
    float s  = hsv->sat / 255.0f;
    float v  = hsv->val / 255.0f;
    float c  = v * s;
    float x  = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m  = v - c;
    float rp = 0, gp = 0, bp = 0;
    if (h < 60)       { rp = c; gp = x; }
    else if (h < 120) { rp = x; gp = c; }
    else if (h < 180) { gp = c; bp = x; }
    else if (h < 240) { gp = x; bp = c; }
    else if (h < 300) { rp = x; bp = c; }
    else              { rp = c; bp = x; }
    *r = (uint8_t)((rp + m) * 255.0f);
    *g = (uint8_t)((gp + m) * 255.0f);
    *b = (uint8_t)((bp + m) * 255.0f);
}

static hsv_t rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b)
{
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float maxv = fmaxf(rf, fmaxf(gf, bf));
    float minv = fminf(rf, fminf(gf, bf));
    float delta = maxv - minv;
    float h = 0.0f;
    if (delta > 0.0001f) {
        if (maxv == rf)      h = 60.0f * fmodf(((gf - bf) / delta), 6.0f);
        else if (maxv == gf) h = 60.0f * (((bf - rf) / delta) + 2.0f);
        else                 h = 60.0f * (((rf - gf) / delta) + 4.0f);
        if (h < 0) h += 360.0f;
    }
    float s = (maxv <= 0.0f) ? 0.0f : (delta / maxv);
    return (hsv_t){.hue = (uint16_t)h, .sat = (uint8_t)(s * 255.0f), .val = (uint8_t)(maxv * 255.0f)};
}

static bool parse_hex_color(const char *hex, hsv_t *out_color)
{
    if (!hex || !out_color) return false;
    if (hex[0] == '#') hex++;
    if (strlen(hex) != 6) return false;
    for (size_t i = 0; i < 6; i++) {
        if (!isxdigit((int)hex[i])) return false;
    }
    char c[3] = {0};
    c[0] = hex[0]; c[1] = hex[1]; uint8_t rv = (uint8_t)strtoul(c, NULL, 16);
    c[0] = hex[2]; c[1] = hex[3]; uint8_t gv = (uint8_t)strtoul(c, NULL, 16);
    c[0] = hex[4]; c[1] = hex[5]; uint8_t bv = (uint8_t)strtoul(c, NULL, 16);
    *out_color = rgb_to_hsv(rv, gv, bv);
    return true;
}

static void color_to_hex(const hsv_t *color, char *out_hex, size_t out_hex_len)
{
    uint8_t r = 0, g = 0, b = 0;
    hsv_to_rgb(color, &r, &g, &b);
    snprintf(out_hex, out_hex_len, "#%02X%02X%02X", r, g, b);
}

static esp_err_t post_event(const app_event_t *event)
{
    QueueHandle_t queue = mode_manager_get_event_queue();
    if (!queue) return ESP_ERR_INVALID_STATE;
    return (xQueueSend(queue, event, pdMS_TO_TICKS(50)) == pdTRUE) ? ESP_OK : ESP_FAIL;
}

/* ── HTTP handlers ───────────────────────────────────────────────────────── */

static esp_err_t index_handler(httpd_req_t *req)  { return send_file(req, "/littlefs/index.html", "text/html"); }
static esp_err_t style_handler(httpd_req_t *req)  { return send_file(req, "/littlefs/style.css",  "text/css"); }
static esp_err_t script_handler(httpd_req_t *req) { return send_file(req, "/littlefs/app.js", "application/javascript"); }

static esp_err_t api_state_handler(httpd_req_t *req)
{
    app_state_t state;
    ESP_ERROR_CHECK(mode_manager_get_state_snapshot(&state));
    char color_hex[10];
    color_to_hex(&state.manual.color, color_hex, sizeof(color_hex));
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"mode\":\"%s\",\"power_on\":%s,\"color\":\"%s\",\"palette\":\"%s\","
             "\"brightness_cap\":%u,\"led_count\":%u}",
             state.mode == MODE_PALETTE_BREATHING ? "palette_breathing" : "single_color",
             state.flags.power_on ? "true" : "false",
             color_hex,
             palette_id_to_name(state.breathing.palette_id),
             state.led.brightness_cap,
             state.led.led_count_total);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, payload);
}

static esp_err_t api_control_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int max_len = req->content_len < (int)(sizeof(body) - 1) ? req->content_len : (int)(sizeof(body) - 1);
    int received = httpd_req_recv(req, body, max_len);
    if (received <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    body[received] = '\0';

    char mode_name[32] = {0};
    if (extract_json_string(body, "mode", mode_name, sizeof(mode_name))) {
        app_event_t evt = {.type = APP_EVT_SET_MODE};
        evt.data.mode = (strcmp(mode_name, "palette_breathing") == 0) ? MODE_PALETTE_BREATHING : MODE_SINGLE_COLOR;
        ESP_ERROR_CHECK(post_event(&evt));
    }
    char color_hex[12] = {0};
    hsv_t color;
    if (extract_json_string(body, "color", color_hex, sizeof(color_hex)) && parse_hex_color(color_hex, &color)) {
        app_event_t evt = {.type = APP_EVT_SET_MANUAL_COLOR};
        evt.data.manual_color = color;
        ESP_ERROR_CHECK(post_event(&evt));
    }
    char palette_name[24] = {0};
    if (extract_json_string(body, "palette", palette_name, sizeof(palette_name))) {
        app_event_t evt = {.type = APP_EVT_SET_PALETTE};
        evt.data.palette_id = palette_name_to_id(palette_name);
        ESP_ERROR_CHECK(post_event(&evt));
    }
    bool power_on = false;
    if (extract_json_bool(body, "power_on", &power_on)) {
        app_event_t evt = {.type = APP_EVT_SET_POWER};
        evt.data.flag = power_on;
        ESP_ERROR_CHECK(post_event(&evt));
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void start_webserver(void)
{
    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = 80;
    httpd_handle_t server  = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t uris[] = {
        {.uri = "/",           .method = HTTP_GET,  .handler = index_handler,   .user_ctx = NULL},
        {.uri = "/style.css",  .method = HTTP_GET,  .handler = style_handler,   .user_ctx = NULL},
        {.uri = "/app.js",     .method = HTTP_GET,  .handler = script_handler,  .user_ctx = NULL},
        {.uri = "/api/state",  .method = HTTP_GET,  .handler = api_state_handler,   .user_ctx = NULL},
        {.uri = "/api/control",.method = HTTP_POST, .handler = api_control_handler, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &uris[i]));
    }
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA start, connecting to '%s'", WIFI_STA_SSID);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%u", d ? d->reason : 0);
        if (s_wifi_retry_num < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry_num++;
            ESP_LOGW(TAG, "Reconnect attempt %d/%d", s_wifi_retry_num, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry_num = 0;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        if (!s_webserver_started) {
            start_webserver();
            s_webserver_started = true;
            ESP_LOGI(TAG, "Webserver ready at http://" IPSTR, IP2STR(&event->ip_info.ip));
        }
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_status = esp_event_loop_create_default();
    if (loop_status != ESP_OK && loop_status != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(loop_status);
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &s_wifi_any_id_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &s_wifi_got_ip_handler));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid,     sizeof(wifi_config.sta.ssid),     "%s", WIFI_STA_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", WIFI_STA_PASS);
    wifi_config.sta.scan_method          = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method          = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.authmode   = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable      = true;
    wifi_config.sta.pmf_cfg.required     = false;
    wifi_config.sta.failure_retry_cnt    = WIFI_MAX_RETRY;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected.");
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to '%s'", WIFI_STA_SSID);
    } else {
        ESP_LOGE(TAG, "WiFi connection timed out");
    }
}

static void littlefs_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path          = "/littlefs",
        .partition_label    = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount         = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));
}

/* ── LED render task ─────────────────────────────────────────────────────── */

static void led_render_task(void *arg)
{
    (void)arg;
    while (1) {
        app_state_t state;
        if (mode_manager_get_state_snapshot(&state) == ESP_OK) {
            led_engine_render(&state);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ── app_main ────────────────────────────────────────────────────────────── */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(led_engine_init());
    ESP_ERROR_CHECK(led_engine_test_pattern());

    littlefs_mount();
    wifi_init_sta();
    ESP_ERROR_CHECK(mode_manager_init());

    BaseType_t task_ok = xTaskCreate(led_render_task, "led_render", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_FAIL);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
