#include "data_logger.h"
#include "gpio_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#define TAG        "LOGGER"
#define LOG_PATH   "/spiffs/log.csv"
#define CFG_PATH   "/spiffs/config.json"
#define CFG_MAX    8192

// ─────────────────────────────────────────────
//  State
// ─────────────────────────────────────────────
static bool  g_enabled     = false;
static int   g_interval_s  = 60;
static int   g_max_entries = 1000;
static int   g_entry_count = 0;   // entries in current log (excluding header)

// ─────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────
static int count_lines(void) {
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return 0;
    int lines = 0, ch;
    while ((ch = fgetc(f)) != EOF)
        if (ch == '\n') lines++;
    fclose(f);
    return lines;
}

static long file_size(void) {
    FILE *f = fopen(LOG_PATH, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

// ─────────────────────────────────────────────
//  Append one CSV row
// ─────────────────────────────────────────────
static void append_entry(void) {
    char json_buf[512];
    gpio_mgr_status_json(json_buf, sizeof(json_buf));

    cJSON *data = cJSON_Parse(json_buf);
    if (!data) { ESP_LOGW(TAG, "No status data to log"); return; }

    // Roll log when full
    if (g_entry_count >= g_max_entries) {
        FILE *f = fopen(LOG_PATH, "w");
        if (f) fclose(f);
        g_entry_count = 0;
        ESP_LOGW(TAG, "Log full — cleared (max %d entries)", g_max_entries);
    }

    FILE *f = fopen(LOG_PATH, "a");
    if (!f) { cJSON_Delete(data); return; }

    // Write CSV header on first entry
    if (g_entry_count == 0) {
        fprintf(f, "uptime_s");
        cJSON *col;
        cJSON_ArrayForEach(col, data) fprintf(f, ",%s", col->string);
        fprintf(f, "\r\n");
    }

    // Write data row
    uint64_t uptime_s = esp_timer_get_time() / 1000000ULL;
    fprintf(f, "%llu", (unsigned long long)uptime_s);
    cJSON *col;
    cJSON_ArrayForEach(col, data) fprintf(f, ",%.2f", col->valuedouble);
    fprintf(f, "\r\n");

    fclose(f);
    cJSON_Delete(data);
    g_entry_count++;
    ESP_LOGI(TAG, "Entry %d logged (uptime %llus)", g_entry_count, (unsigned long long)uptime_s);
}

// ─────────────────────────────────────────────
//  Logger task
// ─────────────────────────────────────────────
static void logger_task(void *arg) {
    ESP_LOGI(TAG, "Logger started — interval %ds, max %d entries", g_interval_s, g_max_entries);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)g_interval_s * 1000));
        append_entry();
    }
}

// ─────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────
void data_logger_init(void) {
    // Read logging config from config.json
    FILE *cf = fopen(CFG_PATH, "r");
    if (cf) {
        fseek(cf, 0, SEEK_END); long len = ftell(cf); fseek(cf, 0, SEEK_SET);
        if (len > 0 && len < CFG_MAX) {
            char *buf = malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, cf); buf[len] = '\0';
                cJSON *root = cJSON_Parse(buf);
                if (root) {
                    cJSON *lg = cJSON_GetObjectItem(root, "logging");
                    if (cJSON_IsObject(lg)) {
                        cJSON *en  = cJSON_GetObjectItem(lg, "enabled");
                        cJSON *iv  = cJSON_GetObjectItem(lg, "interval_s");
                        cJSON *mx  = cJSON_GetObjectItem(lg, "max_entries");
                        g_enabled     = !cJSON_IsFalse(en);
                        if (cJSON_IsNumber(iv) && iv->valueint > 0) g_interval_s  = iv->valueint;
                        if (cJSON_IsNumber(mx) && mx->valueint > 0) g_max_entries = mx->valueint;
                    }
                    cJSON_Delete(root);
                }
                free(buf);
            }
        }
        fclose(cf);
    }

    if (!g_enabled) { ESP_LOGI(TAG, "Logging disabled"); return; }

    // Count existing entries (lines minus header)
    int lines = count_lines();
    g_entry_count = (lines > 1) ? (lines - 1) : 0;
    ESP_LOGI(TAG, "Resuming log — %d existing entries", g_entry_count);

    xTaskCreate(logger_task, "data_logger", 4096, NULL, 3, NULL);
}

esp_err_t data_logger_get_csv(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"webhmi_log.csv\"");

    FILE *f = fopen(LOG_PATH, "r");
    if (!f) {
        httpd_resp_sendstr(req, "uptime_s\r\n");
        return ESP_OK;
    }
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        httpd_resp_send_chunk(req, buf, n);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

esp_err_t data_logger_clear(httpd_req_t *req) {
    FILE *f = fopen(LOG_PATH, "w");
    if (f) fclose(f);
    g_entry_count = 0;
    ESP_LOGI(TAG, "Log cleared");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

esp_err_t data_logger_info(httpd_req_t *req) {
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"enabled\":%s,\"entries\":%d,\"max_entries\":%d,"
             "\"interval_s\":%d,\"size_bytes\":%ld}",
             g_enabled ? "true" : "false",
             g_entry_count, g_max_entries,
             g_interval_s, file_size());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}
