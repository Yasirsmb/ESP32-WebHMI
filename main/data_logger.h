#pragma once
#include <stdbool.h>
#include "esp_http_server.h"

/**
 * data_logger — periodic CSV logging of sensor values to SPIFFS
 *
 * Reads sensor values from gpio_manager at a configurable interval
 * and appends them as CSV rows to /spiffs/log.csv.
 *
 * Config in config.json:
 *   "logging": {
 *     "enabled":     true,
 *     "interval_s":  60,
 *     "max_entries": 1000
 *   }
 *
 * Endpoints (registered by main.c):
 *   GET  /api/log       — download log.csv
 *   POST /api/log/clear — erase the log
 *   GET  /api/log/info  — JSON stats { entries, size_bytes, interval_s }
 *
 * When max_entries is reached the log is cleared automatically to
 * prevent SPIFFS from filling up.
 */

void      data_logger_init(void);
esp_err_t data_logger_get_csv(httpd_req_t *req);
esp_err_t data_logger_clear(httpd_req_t *req);
esp_err_t data_logger_info(httpd_req_t *req);
