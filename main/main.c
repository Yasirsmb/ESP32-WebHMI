#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include "gpio_manager.h"
#include "ota_manager.h"
#include "data_logger.h"

#define CONFIG_MAX_SIZE  8192
#define WIFI_MAX_RETRY   5
static const char *TAG = "WEBHMI";

// ─────────────────────────────────────────────
//  Runtime settings (loaded from config.json)
// ─────────────────────────────────────────────
static char s_wifi_mode[4]  = "ap";
static char s_ap_ssid[32]   = "ESP32-WebHMI";
static char s_ap_pass[64]   = "12345678";
static char s_sta_ssid[32]  = "";
static char s_sta_pass[64]  = "";
static bool s_static_ip     = false;
static char s_ip_addr[16]   = "192.168.1.100";
static char s_ip_gw[16]     = "192.168.1.1";
static char s_ip_nm[16]     = "255.255.255.0";
static char s_auth_user[32] = "admin";
static char s_auth_pass[64] = "admin123";
static char s_session_token[33] = {0};

static void load_main_config(void) {
    FILE *f = fopen("/spiffs/config.json", "r");
    if (!f) return;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len >= CONFIG_MAX_SIZE) { fclose(f); return; }
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, len, f); buf[len] = '\0'; fclose(f);

    cJSON *root = cJSON_Parse(buf); free(buf);
    if (!root) return;

    cJSON *wifi = cJSON_GetObjectItem(root, "wifi");
    if (cJSON_IsObject(wifi)) {
        // mode
        cJSON *m = cJSON_GetObjectItem(wifi, "mode");
        if (cJSON_IsString(m)) strncpy(s_wifi_mode, m->valuestring, sizeof(s_wifi_mode)-1);
        // AP
        cJSON *as = cJSON_GetObjectItem(wifi, "ap_ssid");
        cJSON *ap = cJSON_GetObjectItem(wifi, "ap_password");
        if (!as) as = cJSON_GetObjectItem(wifi, "ssid");      // legacy key
        if (!ap) ap = cJSON_GetObjectItem(wifi, "password");  // legacy key
        if (cJSON_IsString(as)) strncpy(s_ap_ssid, as->valuestring, sizeof(s_ap_ssid)-1);
        if (cJSON_IsString(ap)) strncpy(s_ap_pass, ap->valuestring, sizeof(s_ap_pass)-1);
        // STA
        cJSON *ss = cJSON_GetObjectItem(wifi, "sta_ssid");
        cJSON *sp = cJSON_GetObjectItem(wifi, "sta_password");
        if (cJSON_IsString(ss)) strncpy(s_sta_ssid, ss->valuestring, sizeof(s_sta_ssid)-1);
        if (cJSON_IsString(sp)) strncpy(s_sta_pass, sp->valuestring, sizeof(s_sta_pass)-1);
        // Static IP
        s_static_ip = cJSON_IsTrue(cJSON_GetObjectItem(wifi, "static_ip"));
        cJSON *ip = cJSON_GetObjectItem(wifi, "ip");
        cJSON *gw = cJSON_GetObjectItem(wifi, "gateway");
        cJSON *nm = cJSON_GetObjectItem(wifi, "netmask");
        if (cJSON_IsString(ip)) strncpy(s_ip_addr, ip->valuestring, sizeof(s_ip_addr)-1);
        if (cJSON_IsString(gw)) strncpy(s_ip_gw,   gw->valuestring, sizeof(s_ip_gw)-1);
        if (cJSON_IsString(nm)) strncpy(s_ip_nm,    nm->valuestring, sizeof(s_ip_nm)-1);
    }
    cJSON *auth = cJSON_GetObjectItem(root, "auth");
    if (cJSON_IsObject(auth)) {
        cJSON *u = cJSON_GetObjectItem(auth, "username");
        cJSON *p = cJSON_GetObjectItem(auth, "password");
        if (cJSON_IsString(u)) strncpy(s_auth_user, u->valuestring, sizeof(s_auth_user)-1);
        if (cJSON_IsString(p)) strncpy(s_auth_pass, p->valuestring, sizeof(s_auth_pass)-1);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Config loaded — mode=%s  AP=%s  auth=%s", s_wifi_mode, s_ap_ssid, s_auth_user);
}

static void generate_session_token(void) {
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        sprintf(s_session_token + i * 8, "%08" PRIx32, r);
    }
    s_session_token[32] = '\0';
}


// ─────────────────────────────────────────────
//  Auth helpers
// ─────────────────────────────────────────────
static bool check_auth(httpd_req_t *req) {
    char cookies[256] = {0};
    httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies));
    char expected[64];
    snprintf(expected, sizeof(expected), "HMI_SESSION=%s", s_session_token);
    return strstr(cookies, expected) != NULL;
}
static void redirect_to_login(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_send(req, NULL, 0);
}
static void get_form_field(const char *body, const char *field,
                           char *out, size_t outlen) {
    char key[48]; snprintf(key, sizeof(key), "%s=", field);
    const char *p = strstr(body, key);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(key);
    size_t i = 0;
    while (*p && *p != '&' && i < outlen - 1) {
        if (*p == '+')                    { out[i++] = ' '; p++; }
        else if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            out[i++] = (char)strtol(hex, NULL, 16); p += 3;
        } else { out[i++] = *p++; }
    }
    out[i] = '\0';
}


// ─────────────────────────────────────────────
//  WiFi — Access Point
// ─────────────────────────────────────────────
static void ap_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data) {
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "Client connected    %02x:%02x:%02x:%02x:%02x:%02x",
                 ev->mac[0],ev->mac[1],ev->mac[2],ev->mac[3],ev->mac[4],ev->mac[5]);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
        ESP_LOGI(TAG, "Client disconnected %02x:%02x:%02x:%02x:%02x:%02x",
                 ev->mac[0],ev->mac[1],ev->mac[2],ev->mac[3],ev->mac[4],ev->mac[5]);
    }
}
static void init_wifi_ap(void) {
    esp_netif_create_default_wifi_ap();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        ap_event_handler, NULL, NULL);
    wifi_config_t ap_cfg = { .ap = { .channel = 1, .max_connection = 4,
                                     .authmode = WIFI_AUTH_WPA2_PSK } };
    strncpy((char *)ap_cfg.ap.ssid,     s_ap_ssid, sizeof(ap_cfg.ap.ssid)-1);
    strncpy((char *)ap_cfg.ap.password, s_ap_pass, sizeof(ap_cfg.ap.password)-1);
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    if (strlen(s_ap_pass) == 0) ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "AP started — SSID: \"%s\"  IP: 192.168.4.1", s_ap_ssid);
}


// ─────────────────────────────────────────────
//  WiFi — Station
// ─────────────────────────────────────────────
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_count = 0;

static void sta_event_handler(void *arg, esp_event_base_t base,
                              int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect(); s_retry_count++;
            ESP_LOGI(TAG, "STA retry %d/%d...", s_retry_count, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA connected — IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}
static bool init_wifi_sta(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

    if (s_static_ip) {
        esp_netif_dhcpc_stop(netif);
        esp_netif_ip_info_t ip_info = {0};
        ip_info.ip.addr      = inet_addr(s_ip_addr);
        ip_info.gw.addr      = inet_addr(s_ip_gw);
        ip_info.netmask.addr = inet_addr(s_ip_nm);
        esp_netif_set_ip_info(netif, &ip_info);
        ESP_LOGI(TAG, "Static IP: %s  GW: %s", s_ip_addr, s_ip_gw);
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        sta_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        sta_event_handler, NULL, NULL);

    wifi_config_t sta_cfg = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK }
    };
    strncpy((char *)sta_cfg.sta.ssid,     s_sta_ssid, sizeof(sta_cfg.sta.ssid)-1);
    strncpy((char *)sta_cfg.sta.password, s_sta_pass, sizeof(sta_cfg.sta.password)-1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
                       WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                       pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}


// ─────────────────────────────────────────────
//  SPIFFS
// ─────────────────────────────────────────────
static void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs", .partition_label = NULL,
        .max_files = 8, .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SPIFFS: %s", esp_err_to_name(ret)); return; }
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: %d / %d bytes", used, total);
}


// ─────────────────────────────────────────────
//  File helper
// ─────────────────────────────────────────────
static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *ctype) {
    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found"); return ESP_FAIL; }
    char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}


// ─────────────────────────────────────────────
//  Login / Logout (public)
// ─────────────────────────────────────────────
static esp_err_t handler_login_get(httpd_req_t *req) {
    return serve_file(req, "/spiffs/login.html", "text/html");
}
static esp_err_t handler_login_post(httpd_req_t *req) {
    char body[256] = {0};
    int n = httpd_req_recv(req, body, sizeof(body)-1);
    if (n > 0) body[n] = '\0';

    char username[32]={0}, password[64]={0};
    get_form_field(body, "username", username, sizeof(username));
    get_form_field(body, "password", password, sizeof(password));

    if (strcmp(username, s_auth_user) == 0 && strcmp(password, s_auth_pass) == 0) {
        char cookie[96];
        snprintf(cookie, sizeof(cookie), "HMI_SESSION=%s; Path=/; HttpOnly", s_session_token);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/");
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        httpd_resp_send(req, NULL, 0);
        ESP_LOGI(TAG, "Login OK: %s", username);
    } else {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/login?err=1");
        httpd_resp_send(req, NULL, 0);
        ESP_LOGW(TAG, "Login FAILED: %s", username);
    }
    return ESP_OK;
}
static esp_err_t handler_logout(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/login");
    httpd_resp_set_hdr(req, "Set-Cookie", "HMI_SESSION=; Path=/; Max-Age=0");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}


// ─────────────────────────────────────────────
//  Protected handlers
// ─────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t *req) {
    if (!check_auth(req)) return serve_file(req, "/spiffs/login.html", "text/html");
    return serve_file(req, "/spiffs/index.html", "text/html");
}
static esp_err_t handler_js(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return serve_file(req, "/spiffs/hmi.js", "application/javascript");
}
static esp_err_t handler_settings(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return serve_file(req, "/spiffs/settings.html", "text/html");
}
static esp_err_t handler_config_get(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return serve_file(req, "/spiffs/config.json", "application/json");
}
static esp_err_t handler_config_post(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    int total = req->content_len;
    if (total <= 0 || total >= CONFIG_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length"); return ESP_FAIL;
    }
    char *buf = malloc(total + 1);
    if (!buf) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, buf + received, total - received);
        if (r <= 0) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv"); return ESP_FAIL; }
        received += r;
    }
    buf[received] = '\0';
    FILE *f = fopen("/spiffs/config.json", "w");
    if (!f) { free(buf); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write"); return ESP_FAIL; }
    fwrite(buf, 1, received, f); fclose(f); free(buf);
    load_main_config();
    gpio_mgr_load();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
static esp_err_t handler_status(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    char resp[1024];
    gpio_mgr_status_json(resp, sizeof(resp));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
static esp_err_t handler_control(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    char buf[256];
    int n = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (n <= 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty"); return ESP_FAIL; }
    buf[n] = '\0';
    cJSON *body = cJSON_Parse(buf);
    if (!body) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON"); return ESP_FAIL; }
    cJSON *j_id  = cJSON_GetObjectItem(body, "id");
    cJSON *j_val = cJSON_GetObjectItem(body, "value");
    if (cJSON_IsString(j_id) && cJSON_IsString(j_val))
        gpio_mgr_control(j_id->valuestring, atoi(j_val->valuestring));
    cJSON_Delete(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
static esp_err_t handler_restart(httpd_req_t *req) {
    // Log auth status but do NOT block restart — worst case someone reboots the device
    if (!check_auth(req))
        ESP_LOGW(TAG, ">>> Restart: no valid session (proceeding anyway)");
    ESP_LOGI(TAG, ">>> RESTART TRIGGERED — next mode: %s", s_wifi_mode);
    if (strcmp(s_wifi_mode, "sta") == 0)
        ESP_LOGI(TAG, ">>> Will join router SSID: \"%s\"", s_sta_ssid);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(800));
    ESP_LOGI(TAG, ">>> Restarting now...");
    esp_restart();
    return ESP_OK;
}
static esp_err_t handler_ota(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return ota_mgr_handle_upload(req);
}
static esp_err_t handler_log_get(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return data_logger_get_csv(req);
}
static esp_err_t handler_log_clear(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return data_logger_clear(req);
}
static esp_err_t handler_log_info(httpd_req_t *req) {
    if (!check_auth(req)) { redirect_to_login(req); return ESP_OK; }
    return data_logger_info(req);
}

static esp_err_t catch_all_handler(httpd_req_t *req) {
    if (!check_auth(req)) return serve_file(req, "/spiffs/login.html", "text/html");
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_OK;
}


// ─────────────────────────────────────────────
//  Web server
// ─────────────────────────────────────────────
static void start_webserver(void) {
    httpd_config_t cfg     = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.max_uri_handlers   = 20;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Server start failed"); return;
    }
    httpd_uri_t uris[] = {
        // Public
        { "/login",        HTTP_GET,  handler_login_get,  NULL },
        { "/login",        HTTP_POST, handler_login_post, NULL },
        { "/logout",       HTTP_GET,  handler_logout,     NULL },
        // Protected pages
        { "/",             HTTP_GET,  index_handler,       NULL },
        { "/hmi.js",       HTTP_GET,  handler_js,          NULL },
        { "/settings",     HTTP_GET,  handler_settings,    NULL },
        // Protected API
        { "/api/config",   HTTP_GET,  handler_config_get,  NULL },
        { "/api/config",   HTTP_POST, handler_config_post, NULL },
        { "/api/status",   HTTP_GET,  handler_status,      NULL },
        { "/api/control",  HTTP_POST, handler_control,     NULL },
        { "/api/restart",    HTTP_POST, handler_restart,    NULL },
        // OTA + logging
        { "/api/ota",        HTTP_POST, handler_ota,       NULL },
        { "/api/log",        HTTP_GET,  handler_log_get,   NULL },
        { "/api/log/clear",  HTTP_POST, handler_log_clear, NULL },
        { "/api/log/info",   HTTP_GET,  handler_log_info,  NULL },
        // Catch-all captive portal (must be last)
        { "/*",              HTTP_GET,  catch_all_handler, NULL },
    };
    for (int i = 0; i < (int)(sizeof(uris)/sizeof(uris[0])); i++)
        httpd_register_uri_handler(server, &uris[i]);
    ESP_LOGI(TAG, "Web server started  (%d routes)", (int)(sizeof(uris)/sizeof(uris[0])));
}


// ─────────────────────────────────────────────
//  Main
// ─────────────────────────────────────────────
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_spiffs();
    load_main_config();
    generate_session_token();
    gpio_mgr_load();
    data_logger_init();

    // Shared WiFi init (mode-independent)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    bool connected = false;
    if (strcmp(s_wifi_mode, "sta") == 0 && strlen(s_sta_ssid) > 0) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "WiFi mode: STATION");
        ESP_LOGI(TAG, "Connecting to: \"%s\"", s_sta_ssid);
        if (s_static_ip)
            ESP_LOGI(TAG, "Static IP: %s", s_ip_addr);
        ESP_LOGI(TAG, "========================================");
        connected = init_wifi_sta();
        if (!connected) {
            ESP_LOGW(TAG, "STA failed — falling back to AP mode");
            ESP_LOGW(TAG, "Check SSID and password in Settings page");
        }
    }
    if (!connected) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "WiFi mode: ACCESS POINT");
        ESP_LOGI(TAG, "SSID: \"%s\"   IP: 192.168.4.1", s_ap_ssid);
        ESP_LOGI(TAG, "========================================");
        init_wifi_ap();
    }

    start_webserver();
}
