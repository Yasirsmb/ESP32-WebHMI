#include "gpio_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"   // ESP-IDF v5 ADC API
#include "cJSON.h"

#define TAG          "GPIO_MGR"
#define MAX_WIDGETS  32
#define CFG_PATH     "/spiffs/config.json"
#define CFG_MAX_SIZE 8192

// ─────────────────────────────────────────────
//  Internal types
// ─────────────────────────────────────────────
typedef enum {
    MODE_OUTPUT,
    MODE_DIGITAL_IN,
    MODE_ADC,
} pin_mode_t;

typedef struct {
    char         id[32];
    int          pin;
    pin_mode_t   mode;
    bool         active_high;
    float        adc_scale;
    adc_channel_t adc_channel;   // -1 cast if not ADC
} widget_hw_t;

static widget_hw_t              g_hw[MAX_WIDGETS];
static int                      g_hw_count  = 0;
static adc_oneshot_unit_handle_t g_adc_handle = NULL;

// ─────────────────────────────────────────────
//  GPIO → ADC1 channel (ESP32: GPIO 32-39 only)
//  ADC2 is unavailable while WiFi is active.
// ─────────────────────────────────────────────
static int gpio_to_adc_ch(int pin) {
    switch (pin) {
        case 36: return ADC_CHANNEL_0;   // VP  (input-only)
        case 37: return ADC_CHANNEL_1;
        case 38: return ADC_CHANNEL_2;
        case 39: return ADC_CHANNEL_3;   // VN  (input-only)
        case 32: return ADC_CHANNEL_4;
        case 33: return ADC_CHANNEL_5;
        case 34: return ADC_CHANNEL_6;   // input-only
        case 35: return ADC_CHANNEL_7;   // input-only
        default: return -1;
    }
}

// ─────────────────────────────────────────────
//  gpio_mgr_load
// ─────────────────────────────────────────────
void gpio_mgr_load(void) {
    g_hw_count = 0;

    FILE *f = fopen(CFG_PATH, "r");
    if (!f) { ESP_LOGE(TAG, "Cannot open %s", CFG_PATH); return; }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0 || flen >= CFG_MAX_SIZE) { fclose(f); return; }
    char *raw = malloc(flen + 1);
    if (!raw) { fclose(f); return; }
    fread(raw, 1, flen, f);
    raw[flen] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) { ESP_LOGE(TAG, "JSON parse error in %s", CFG_PATH); return; }

    cJSON *widgets = cJSON_GetObjectItem(root, "widgets");
    if (!cJSON_IsArray(widgets)) { cJSON_Delete(root); return; }

    // Create ADC unit on first use (shared across all ADC channels)
    bool adc_unit_needed = false;
    cJSON *w;
    cJSON_ArrayForEach(w, widgets) {
        cJSON *j_mode = cJSON_GetObjectItem(w, "pin_mode");
        if (cJSON_IsString(j_mode) && strcmp(j_mode->valuestring, "adc") == 0)
            adc_unit_needed = true;
    }
    if (adc_unit_needed && g_adc_handle == NULL) {
        adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &g_adc_handle));
    }

    cJSON_ArrayForEach(w, widgets) {
        if (g_hw_count >= MAX_WIDGETS) break;

        cJSON *j_id   = cJSON_GetObjectItem(w, "id");
        cJSON *j_pin  = cJSON_GetObjectItem(w, "pin");
        cJSON *j_mode = cJSON_GetObjectItem(w, "pin_mode");

        if (!cJSON_IsString(j_id) || !cJSON_IsNumber(j_pin) || !cJSON_IsString(j_mode))
            continue;

        widget_hw_t *hw = &g_hw[g_hw_count];
        strncpy(hw->id, j_id->valuestring, sizeof(hw->id) - 1);
        hw->id[sizeof(hw->id) - 1] = '\0';
        hw->pin         = (int)j_pin->valuedouble;
        hw->adc_channel = (adc_channel_t)-1;

        cJSON *j_ahi   = cJSON_GetObjectItem(w, "active_high");
        cJSON *j_scale = cJSON_GetObjectItem(w, "adc_scale");
        hw->active_high = !cJSON_IsFalse(j_ahi);
        hw->adc_scale   = j_scale ? (float)j_scale->valuedouble : 3.3f;

        const char *mode = j_mode->valuestring;

        if (strcmp(mode, "output") == 0) {
            hw->mode = MODE_OUTPUT;
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << hw->pin,
                .mode         = GPIO_MODE_OUTPUT,
                .pull_up_en   = GPIO_PULLUP_DISABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&cfg);
            gpio_set_level(hw->pin, hw->active_high ? 0 : 1);

        } else if (strcmp(mode, "digital_in") == 0) {
            hw->mode = MODE_DIGITAL_IN;
            gpio_config_t cfg = {
                .pin_bit_mask = 1ULL << hw->pin,
                .mode         = GPIO_MODE_INPUT,
                .pull_up_en   = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type    = GPIO_INTR_DISABLE,
            };
            gpio_config(&cfg);

        } else if (strcmp(mode, "adc") == 0) {
            int ch = gpio_to_adc_ch(hw->pin);
            if (ch < 0) {
                ESP_LOGE(TAG, "Pin %d is not an ADC1 pin (use GPIO 32-39)", hw->pin);
                continue;
            }
            hw->mode        = MODE_ADC;
            hw->adc_channel = (adc_channel_t)ch;
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten    = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, hw->adc_channel, &chan_cfg));

        } else {
            ESP_LOGW(TAG, "Unknown pin_mode '%s' for widget '%s'", mode, hw->id);
            continue;
        }

        ESP_LOGI(TAG, "  '%s'  pin=%-2d  mode=%s", hw->id, hw->pin, mode);
        g_hw_count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "%d widget pin(s) configured", g_hw_count);
}

// ─────────────────────────────────────────────
//  gpio_mgr_control
// ─────────────────────────────────────────────
bool gpio_mgr_control(const char *widget_id, int value) {
    for (int i = 0; i < g_hw_count; i++) {
        if (strcmp(g_hw[i].id, widget_id) == 0 && g_hw[i].mode == MODE_OUTPUT) {
            int level = g_hw[i].active_high ? (value ? 1 : 0) : (value ? 0 : 1);
            gpio_set_level(g_hw[i].pin, level);
            ESP_LOGI(TAG, "GPIO %d → %d  (%s)", g_hw[i].pin, level, widget_id);
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
//  gpio_mgr_status_json
// ─────────────────────────────────────────────
int gpio_mgr_status_json(char *buf, int buflen) {
    int  pos   = 0;
    bool first = true;

    pos += snprintf(buf + pos, buflen - pos, "{");

    for (int i = 0; i < g_hw_count && pos < buflen - 32; i++) {
        widget_hw_t *hw = &g_hw[i];
        float val;

        if (hw->mode == MODE_DIGITAL_IN) {
            val = (float)gpio_get_level(hw->pin);
        } else if (hw->mode == MODE_ADC && g_adc_handle) {
            int raw = 0;
            adc_oneshot_read(g_adc_handle, hw->adc_channel, &raw);
            val = (raw / 4095.0f) * hw->adc_scale;
        } else {
            continue;
        }

        pos += snprintf(buf + pos, buflen - pos,
                        "%s\"%s\":%.2f", first ? "" : ",", hw->id, val);
        first = false;
    }

    pos += snprintf(buf + pos, buflen - pos, "}");
    return pos;
}
