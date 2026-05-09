#pragma once
#include <stdbool.h>

/**
 * gpio_manager — config-driven GPIO / ADC for WebHMI
 *
 * Reads "widgets" from /spiffs/config.json.
 * Any widget with "pin", "pin_mode", and optionally "active_high" / "adc_scale"
 * is registered and its GPIO is initialised automatically.
 *
 * Supported pin_mode values:
 *   "output"      — digital output (toggle, button, led)
 *   "digital_in"  — digital input  (display, led indicator)
 *   "adc"         — ADC1 analog input, GPIO 32-39 only (gauge, display, chart)
 *                   NOTE: ADC2 is unavailable while WiFi is active.
 *
 * Config.json example:
 *   { "id": "relay1",      "type": "toggle",  "pin": 2,  "pin_mode": "output",     "active_high": true  }
 *   { "id": "temperature", "type": "gauge",   "pin": 34, "pin_mode": "adc",        "adc_scale": 150.0   }
 *   { "id": "door",        "type": "led",     "pin": 15, "pin_mode": "digital_in"                       }
 */

// Load widget pin configs from config.json and initialise GPIO.
// Safe to call multiple times — re-reads and reconfigures every time.
void gpio_mgr_load(void);

// Set an output widget's GPIO pin.
// value 1 = ON, 0 = OFF (respects active_high from config).
// Returns true if the widget was found and the GPIO was driven.
bool gpio_mgr_control(const char *widget_id, int value);

// Write all input widget values as a JSON object into buf (e.g. {"temp":46.9,"door":1}).
// Returns the number of bytes written (excluding the null terminator).
int gpio_mgr_status_json(char *buf, int buflen);
