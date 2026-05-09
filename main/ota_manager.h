#pragma once
#include "esp_http_server.h"

/**
 * ota_manager — browser-triggered firmware update
 *
 * The browser POSTs a raw .bin firmware file to /api/ota.
 * The handler writes it to the next OTA partition, validates it,
 * marks it as the boot partition, then restarts.
 *
 * Requires a partition table with ota_0 / ota_1 partitions.
 * See partitions.csv in the project root.
 */

// Receive a firmware .bin upload and flash it.
// On success, device restarts automatically.
esp_err_t ota_mgr_handle_upload(httpd_req_t *req);
