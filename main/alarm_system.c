#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blynk_edgent.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_ALARM_SIREN_GPIO
#define CONFIG_ALARM_SIREN_GPIO 4
#endif

#ifndef CONFIG_ALARM_SIREN_ACTIVE_LEVEL
#define CONFIG_ALARM_SIREN_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_ALARM_SIREN_CONTROL_DS
#define CONFIG_ALARM_SIREN_CONTROL_DS "V0"
#endif

#ifndef CONFIG_ALARM_SIREN_TIME_DS
#define CONFIG_ALARM_SIREN_TIME_DS "V1"
#endif

#ifndef CONFIG_ALARM_SIREN_PRESET_DS
#define CONFIG_ALARM_SIREN_PRESET_DS "V2"
#endif

#ifndef CONFIG_ALARM_SIREN_START_DS
#define CONFIG_ALARM_SIREN_START_DS "V3"
#endif

#ifndef CONFIG_ALARM_SIREN_STOP_DS
#define CONFIG_ALARM_SIREN_STOP_DS "V4"
#endif

#ifndef CONFIG_ALARM_SIREN_STATE_DS
#define CONFIG_ALARM_SIREN_STATE_DS "V5"
#endif

#ifndef CONFIG_ALARM_RECONFIG_BUTTON_GPIO
#define CONFIG_ALARM_RECONFIG_BUTTON_GPIO 9
#endif

#ifndef CONFIG_ALARM_RECONFIG_BUTTON_ACTIVE_LEVEL
#define CONFIG_ALARM_RECONFIG_BUTTON_ACTIVE_LEVEL 0
#endif

#ifndef CONFIG_ALARM_STATUS_LED_GPIO
#define CONFIG_ALARM_STATUS_LED_GPIO 8
#endif

#ifndef CONFIG_ALARM_STATUS_LED_ACTIVE_LEVEL
#define CONFIG_ALARM_STATUS_LED_ACTIVE_LEVEL 1
#endif

#ifndef CONFIG_ALARM_WIFI_RSSI_DS
#define CONFIG_ALARM_WIFI_RSSI_DS "V6"
#endif

#ifndef CONFIG_ALARM_WIFI_RSSI_PUBLISH_INTERVAL_SECONDS
#define CONFIG_ALARM_WIFI_RSSI_PUBLISH_INTERVAL_SECONDS 15
#endif

#define MAX_HOURS_VALUE      99U
#define MAX_MINSEC_VALUE     59U
#define MAX_SIREN_SECONDS    ((MAX_HOURS_VALUE * 3600U) + (MAX_MINSEC_VALUE * 60U) + MAX_MINSEC_VALUE)
#define TIMER_POLL_INTERVAL_MS 1000U
#define DEFAULT_PRESET_INDEX 0U
#define BUTTON_POLL_INTERVAL_MS 20U
#define BUTTON_DEBOUNCE_MS 30U
#define BUTTON_DOUBLE_CLICK_TIMEOUT_MS 400U
#define BUTTON_LONG_PRESS_MS 5000U
#define STATUS_LED_FAST_BLINK_MS 125U
#define STATUS_LED_SLOW_BLINK_MS 500U
#define STATUS_LED_ERROR_BLINK_MS 75U

static const uint32_t SIREN_PRESET_SECONDS[] = {
    5U,
    10U,
    30U,
    60U,
    300U,
    1800U,
};

typedef struct {
    uint32_t selected_preset_index;
    uint32_t last_published_remaining_seconds;
    int64_t stop_time_us;
    bool siren_enabled;
} siren_state_t;

typedef enum {
    STATUS_LED_PATTERN_OFF = 0,
    STATUS_LED_PATTERN_ON,
    STATUS_LED_PATTERN_BLINK_FAST,
    STATUS_LED_PATTERN_BLINK_SLOW,
    STATUS_LED_PATTERN_BLINK_ERROR,
} status_led_pattern_t;

static const char *TAG = "alarm_system";

static siren_state_t s_siren_state;
static SemaphoreHandle_t s_siren_mutex;
static TaskHandle_t s_siren_task_handle;
static esp_timer_handle_t s_siren_timer;
static volatile status_led_pattern_t s_status_led_pattern = STATUS_LED_PATTERN_BLINK_FAST;
static bool s_edgent_connected;
static bool s_wifi_rssi_published;
static int8_t s_last_wifi_rssi;
static TickType_t s_last_wifi_rssi_publish_tick;

static size_t siren_preset_count(void)
{
    return sizeof(SIREN_PRESET_SECONDS) / sizeof(SIREN_PRESET_SECONDS[0]);
}

static bool siren_preset_index_is_valid(uint32_t index)
{
    return index < siren_preset_count();
}

static uint32_t siren_preset_seconds_from_index(uint32_t index)
{
    if (!siren_preset_index_is_valid(index)) {
        index = DEFAULT_PRESET_INDEX;
    }

    return SIREN_PRESET_SECONDS[index];
}

static bool siren_find_preset_index(uint32_t seconds, uint32_t *index)
{
    size_t i;

    for (i = 0; i < siren_preset_count(); ++i) {
        if (SIREN_PRESET_SECONDS[i] == seconds) {
            if (index != NULL) {
                *index = (uint32_t)i;
            }
            return true;
        }
    }

    return false;
}

static uint32_t clamp_u32(uint32_t value, uint32_t max_value)
{
    return value > max_value ? max_value : value;
}

static bool topic_matches_datastream(const char *topic, int topic_len, const char *datastream)
{
    const size_t datastream_len = strlen(datastream);

    if (topic_len < (int)datastream_len) {
        return false;
    }

    return memcmp(topic + topic_len - datastream_len, datastream, datastream_len) == 0;
}

static bool parse_non_negative_u32(const char *data, int data_len, uint32_t *value)
{
    char buffer[24];
    char *start = buffer;
    char *end = NULL;
    unsigned long parsed;

    if (data_len <= 0 || data_len >= (int)sizeof(buffer) || value == NULL) {
        return false;
    }

    memcpy(buffer, data, data_len);
    buffer[data_len] = '\0';

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start++;
    }

    if (*start == '\0') {
        return false;
    }

    end = start + strlen(start) - 1;
    while (end >= start && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    errno = 0;
    parsed = strtoul(start, &end, 10);
    if (errno != 0 || end == start || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static void siren_set_gpio(bool enabled)
{
    const int on_level = CONFIG_ALARM_SIREN_ACTIVE_LEVEL ? 1 : 0;
    const int off_level = on_level ? 0 : 1;

    gpio_set_level(CONFIG_ALARM_SIREN_GPIO, enabled ? on_level : off_level);
}

static void status_led_set_gpio(bool enabled)
{
    const int on_level = CONFIG_ALARM_STATUS_LED_ACTIVE_LEVEL ? 1 : 0;
    const int off_level = on_level ? 0 : 1;

    gpio_set_level(CONFIG_ALARM_STATUS_LED_GPIO, enabled ? on_level : off_level);
}

static const char *edgent_state_to_str(edgent_state_t state)
{
    switch (state) {
    case EDGENT_STATE_UNKNOWN:
        return "UNKNOWN";
    case EDGENT_STATE_IDLE:
        return "IDLE";
    case EDGENT_STATE_CONNECTING_NET:
        return "CONNECTING_NET";
    case EDGENT_STATE_CONNECTING_CLOUD:
        return "CONNECTING_CLOUD";
    case EDGENT_STATE_CONNECTED:
        return "CONNECTED";
    case EDGENT_STATE_WAIT_CONFIG:
        return "WAIT_CONFIG";
    case EDGENT_STATE_OTA_UPGRADE:
        return "OTA_UPGRADE";
    case EDGENT_STATE_ERROR:
        return "ERROR";
    default:
        return "INVALID";
    }
}

static status_led_pattern_t status_led_pattern_from_state(edgent_state_t state)
{
    switch (state) {
    case EDGENT_STATE_CONNECTED:
        return STATUS_LED_PATTERN_ON;
    case EDGENT_STATE_WAIT_CONFIG:
        return STATUS_LED_PATTERN_BLINK_SLOW;
    case EDGENT_STATE_ERROR:
        return STATUS_LED_PATTERN_BLINK_ERROR;
    case EDGENT_STATE_IDLE:
    case EDGENT_STATE_CONNECTING_NET:
    case EDGENT_STATE_CONNECTING_CLOUD:
    case EDGENT_STATE_OTA_UPGRADE:
    case EDGENT_STATE_UNKNOWN:
    default:
        return STATUS_LED_PATTERN_BLINK_FAST;
    }
}

static void status_led_set_pattern(status_led_pattern_t pattern)
{
    s_status_led_pattern = pattern;
}

static void publish_remaining_time(uint32_t remaining_seconds)
{
    const edgent_err rc = edgent_publish_ds_int(CONFIG_ALARM_SIREN_TIME_DS, remaining_seconds);

    if (rc != EDGENT_OK) {
        ESP_LOGW(TAG, "Failed to publish remaining time: %" PRIu32 " s, rc=%d", remaining_seconds, rc);
        return;
    }

    ESP_LOGI(TAG, "Published remaining time: %" PRIu32 " s", remaining_seconds);
}

static void publish_selected_preset(uint32_t preset_index)
{
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_PRESET_DS, preset_index);
}

static void publish_running_state(bool enabled)
{
    const int state_value = enabled ? 1 : 0;
    const edgent_err rc = edgent_publish_ds_int(CONFIG_ALARM_SIREN_STATE_DS, state_value);

    if (rc != EDGENT_OK) {
        ESP_LOGW(TAG, "Failed to publish siren state: %d, rc=%d", state_value, rc);
        return;
    }

    ESP_LOGI(TAG, "Published siren state: %d", state_value);
}

static void publish_wifi_rssi(int8_t rssi)
{
    const edgent_err rc = edgent_publish_ds_int(CONFIG_ALARM_WIFI_RSSI_DS, rssi);

    if (rc != EDGENT_OK) {
        ESP_LOGW(TAG, "Failed to publish Wi-Fi RSSI: %d dBm, rc=%d", rssi, rc);
        return;
    }

    s_last_wifi_rssi = rssi;
    s_last_wifi_rssi_publish_tick = xTaskGetTickCount();
    s_wifi_rssi_published = true;

    ESP_LOGI(TAG, "Published Wi-Fi RSSI: %d dBm", rssi);
}

static void maybe_publish_wifi_rssi(bool force)
{
    wifi_ap_record_t ap_info;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t min_interval_ticks = pdMS_TO_TICKS(CONFIG_ALARM_WIFI_RSSI_PUBLISH_INTERVAL_SECONDS * 1000U);
    esp_err_t err;

    if (!s_edgent_connected) {
        return;
    }

    if (!force && s_wifi_rssi_published &&
        (now - s_last_wifi_rssi_publish_tick) < min_interval_ticks) {
        return;
    }

    err = esp_wifi_sta_get_ap_info(&ap_info);

    if (err == ESP_OK) {
        if (force || !s_wifi_rssi_published || ap_info.rssi != s_last_wifi_rssi) {
            publish_wifi_rssi(ap_info.rssi);
        } else {
            s_last_wifi_rssi_publish_tick = now;
        }
        return;
    }

    if (err != ESP_ERR_WIFI_CONN && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Failed to read Wi-Fi RSSI: %s", esp_err_to_name(err));
    }
}

static void reset_button_datastreams(void)
{
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_START_DS, 0);
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_STOP_DS, 0);
}

static void stop_timer_if_running(void)
{
    const esp_err_t err = esp_timer_stop(s_siren_timer);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to stop timer: %s", esp_err_to_name(err));
    }
}

static void siren_stop(void)
{
    stop_timer_if_running();

    xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
    s_siren_state.siren_enabled = false;
    s_siren_state.stop_time_us = 0;
    s_siren_state.last_published_remaining_seconds = 0;
    xSemaphoreGive(s_siren_mutex);

    siren_set_gpio(false);
    publish_remaining_time(0);
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_CONTROL_DS, 0);
    publish_running_state(false);
    reset_button_datastreams();

    ESP_LOGI(TAG, "Siren stopped");
}

static void siren_start(uint32_t seconds)
{
    const uint32_t safe_seconds = clamp_u32(seconds, MAX_SIREN_SECONDS);
    uint32_t matched_preset_index = DEFAULT_PRESET_INDEX;
    const bool preset_matched = siren_find_preset_index(safe_seconds, &matched_preset_index);

    if (safe_seconds == 0) {
        siren_stop();
        return;
    }

    stop_timer_if_running();

    xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
    if (preset_matched) {
        s_siren_state.selected_preset_index = matched_preset_index;
    }
    s_siren_state.last_published_remaining_seconds = safe_seconds;
    s_siren_state.stop_time_us = esp_timer_get_time() + ((int64_t)safe_seconds * 1000000LL);
    s_siren_state.siren_enabled = true;
    xSemaphoreGive(s_siren_mutex);

    siren_set_gpio(true);
    ESP_ERROR_CHECK(esp_timer_start_once(s_siren_timer, (uint64_t)safe_seconds * 1000000ULL));

    if (preset_matched) {
        publish_selected_preset(matched_preset_index);
    }
    publish_remaining_time(safe_seconds);
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_CONTROL_DS, safe_seconds);
    publish_running_state(true);
    reset_button_datastreams();

    ESP_LOGI(TAG, "Siren started for %" PRIu32 " s", safe_seconds);
}

static void siren_timer_callback(void *arg)
{
    (void)arg;

    if (s_siren_task_handle != NULL) {
        xTaskNotifyGive(s_siren_task_handle);
    }
}

static void siren_publish_full_state(void)
{
    bool enabled;
    uint32_t preset_index;
    uint32_t remaining_seconds = 0;

    xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
    enabled = s_siren_state.siren_enabled;
    preset_index = s_siren_state.selected_preset_index;

    if (enabled && s_siren_state.stop_time_us > 0) {
        const int64_t remaining_us = s_siren_state.stop_time_us - esp_timer_get_time();
        remaining_seconds = remaining_us > 0 ? (uint32_t)((remaining_us + 999999LL) / 1000000LL) : 0;
    }
    xSemaphoreGive(s_siren_mutex);

    publish_selected_preset(preset_index);
    publish_remaining_time(remaining_seconds);
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_CONTROL_DS, remaining_seconds);
    publish_running_state(enabled && remaining_seconds > 0);
    reset_button_datastreams();
}

static void handle_preset_selection_update(uint32_t preset_index)
{
    if (!siren_preset_index_is_valid(preset_index)) {
        uint32_t current_preset_index;

        xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
        current_preset_index = s_siren_state.selected_preset_index;
        xSemaphoreGive(s_siren_mutex);

        publish_selected_preset(current_preset_index);
        ESP_LOGW(TAG, "Unsupported preset index: %" PRIu32, preset_index);
        return;
    }

    xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
    s_siren_state.selected_preset_index = preset_index;
    xSemaphoreGive(s_siren_mutex);

    publish_selected_preset(preset_index);
    ESP_LOGI(TAG, "Preset selected: index=%" PRIu32 ", seconds=%" PRIu32,
             preset_index, siren_preset_seconds_from_index(preset_index));
}

static void handle_downlink_datastream(const char *topic, int topic_len, const char *data, int data_len)
{
    uint32_t value;

    if (!parse_non_negative_u32(data, data_len, &value)) {
        ESP_LOGW(TAG, "Invalid downlink payload");
        return;
    }

    if (topic_matches_datastream(topic, topic_len, CONFIG_ALARM_SIREN_CONTROL_DS)) {
        siren_start(value);
        return;
    }

    if (topic_matches_datastream(topic, topic_len, CONFIG_ALARM_SIREN_PRESET_DS)) {
        handle_preset_selection_update(value);
        return;
    }

    if (topic_matches_datastream(topic, topic_len, CONFIG_ALARM_SIREN_START_DS)) {
        if (value != 0) {
            uint32_t preset_index;

            xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
            preset_index = s_siren_state.selected_preset_index;
            xSemaphoreGive(s_siren_mutex);

            siren_start(siren_preset_seconds_from_index(preset_index));
        } else {
            reset_button_datastreams();
        }
        return;
    }

    if (topic_matches_datastream(topic, topic_len, CONFIG_ALARM_SIREN_STOP_DS)) {
        if (value != 0) {
            siren_stop();
        } else {
            reset_button_datastreams();
        }
        return;
    }
}

static void on_initial_connection(void)
{
    s_edgent_connected = true;
    siren_publish_full_state();
    maybe_publish_wifi_rssi(true);
}

static void on_edgent_state_changed(void *handler_arg, esp_event_base_t base, int32_t id, void *event_data)
{
    const edgent_state_evt_t *state_event = (const edgent_state_evt_t *)event_data;

    (void)handler_arg;
    (void)base;

    if (id != EDGENT_EVENT_STATE_CHANGED || state_event == NULL) {
        return;
    }

    s_edgent_connected = state_event->curr == EDGENT_STATE_CONNECTED;
    if (!s_edgent_connected) {
        s_wifi_rssi_published = false;
        s_last_wifi_rssi_publish_tick = 0;
    }

    status_led_set_pattern(status_led_pattern_from_state(state_event->curr));
    ESP_LOGI(TAG, "Edgent state: %s -> %s",
             edgent_state_to_str(state_event->prev),
             edgent_state_to_str(state_event->curr));
}

static void on_reboot_request(void)
{
    esp_restart();
}

static void status_led_task(void *arg)
{
    status_led_pattern_t active_pattern = STATUS_LED_PATTERN_OFF;
    bool led_on = false;

    (void)arg;

    while (true) {
        const status_led_pattern_t next_pattern = s_status_led_pattern;

        if (next_pattern != active_pattern) {
            active_pattern = next_pattern;
            led_on = false;
        }

        switch (active_pattern) {
        case STATUS_LED_PATTERN_OFF:
            status_led_set_gpio(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case STATUS_LED_PATTERN_ON:
            status_led_set_gpio(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        case STATUS_LED_PATTERN_BLINK_FAST:
            led_on = !led_on;
            status_led_set_gpio(led_on);
            vTaskDelay(pdMS_TO_TICKS(STATUS_LED_FAST_BLINK_MS));
            break;
        case STATUS_LED_PATTERN_BLINK_SLOW:
            led_on = !led_on;
            status_led_set_gpio(led_on);
            vTaskDelay(pdMS_TO_TICKS(STATUS_LED_SLOW_BLINK_MS));
            break;
        case STATUS_LED_PATTERN_BLINK_ERROR:
            led_on = !led_on;
            status_led_set_gpio(led_on);
            vTaskDelay(pdMS_TO_TICKS(STATUS_LED_ERROR_BLINK_MS));
            break;
        default:
            status_led_set_gpio(false);
            vTaskDelay(pdMS_TO_TICKS(100));
            break;
        }
    }
}

static void reconfig_button_task(void *arg)
{
    const int active_level = CONFIG_ALARM_RECONFIG_BUTTON_ACTIVE_LEVEL ? 1 : 0;
    const TickType_t poll_interval = pdMS_TO_TICKS(BUTTON_POLL_INTERVAL_MS);
    const TickType_t debounce_ticks = pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS);
    const TickType_t double_click_ticks = pdMS_TO_TICKS(BUTTON_DOUBLE_CLICK_TIMEOUT_MS);
    const TickType_t long_press_ticks = pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS);
    TickType_t now = xTaskGetTickCount();
    bool raw_pressed = gpio_get_level(CONFIG_ALARM_RECONFIG_BUTTON_GPIO) == active_level;
    bool stable_pressed = raw_pressed;
    TickType_t raw_changed_at = now;
    TickType_t pressed_at = now;
    TickType_t last_release_at = 0;
    uint32_t click_count = 0;
    bool long_press_handled = false;

    (void)arg;

    while (true) {
        const bool sampled_pressed = gpio_get_level(CONFIG_ALARM_RECONFIG_BUTTON_GPIO) == active_level;

        now = xTaskGetTickCount();

        if (sampled_pressed != raw_pressed) {
            raw_pressed = sampled_pressed;
            raw_changed_at = now;
        }

        if (stable_pressed != raw_pressed && (now - raw_changed_at) >= debounce_ticks) {
            stable_pressed = raw_pressed;

            if (stable_pressed) {
                pressed_at = now;
                long_press_handled = false;
            } else if (!long_press_handled) {
                click_count++;
                last_release_at = now;
            }
        }

        if (stable_pressed && !long_press_handled && (now - pressed_at) >= long_press_ticks) {
            long_press_handled = true;
            click_count = 0;
            ESP_LOGI(TAG, "BOOT long-press detected, resetting Blynk.Edgent config");
            edgent_config_reset();
        } else if (!stable_pressed && click_count > 0 && (now - last_release_at) >= double_click_ticks) {
            if (click_count >= 2U) {
                ESP_LOGI(TAG, "BOOT double-click detected, starting Blynk.Edgent reconfigure");
                edgent_config_start();
            }
            click_count = 0;
        }

        vTaskDelay(poll_interval);
    }
}

static void siren_task(void *arg)
{
    (void)arg;

    while (true) {
        bool should_stop = false;
        bool should_publish = false;
        uint32_t remaining_seconds = 0;

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TIMER_POLL_INTERVAL_MS));

        xSemaphoreTake(s_siren_mutex, portMAX_DELAY);
        if (s_siren_state.siren_enabled && s_siren_state.stop_time_us > 0) {
            const int64_t remaining_us = s_siren_state.stop_time_us - esp_timer_get_time();

            if (remaining_us <= 0) {
                should_stop = true;
            } else {
                remaining_seconds = (uint32_t)((remaining_us + 999999LL) / 1000000LL);
                if (remaining_seconds != s_siren_state.last_published_remaining_seconds) {
                    s_siren_state.last_published_remaining_seconds = remaining_seconds;
                    should_publish = true;
                }
            }
        }
        xSemaphoreGive(s_siren_mutex);

        if (should_stop) {
            siren_stop();
        } else if (should_publish) {
            publish_remaining_time(remaining_seconds);
        }

        maybe_publish_wifi_rssi(false);
    }
}

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(err);
}

static void init_default_event_loop(void)
{
    const esp_err_t err = esp_event_loop_create_default();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

static void init_edgent_event_handler(void)
{
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        EDGENT_EVENT_BASE,
        EDGENT_EVENT_STATE_CHANGED,
        on_edgent_state_changed,
        NULL,
        NULL));
}

static void siren_init(void)
{
    const gpio_config_t gpio_config_siren = {
        .pin_bit_mask = 1ULL << CONFIG_ALARM_SIREN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const esp_timer_create_args_t timer_args = {
        .callback = siren_timer_callback,
        .name = "siren_timer",
    };

    s_siren_mutex = xSemaphoreCreateMutex();
    configASSERT(s_siren_mutex != NULL);

    s_siren_state.selected_preset_index = DEFAULT_PRESET_INDEX;
    s_siren_state.last_published_remaining_seconds = UINT32_MAX;

    ESP_ERROR_CHECK(gpio_config(&gpio_config_siren));
    siren_set_gpio(false);

    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_siren_timer));
    xTaskCreate(siren_task, "siren_task", 4096, NULL, 5, &s_siren_task_handle);
}

static void status_led_init(void)
{
    const gpio_config_t gpio_config_led = {
        .pin_bit_mask = 1ULL << CONFIG_ALARM_STATUS_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&gpio_config_led));
    status_led_set_gpio(false);
    xTaskCreate(status_led_task, "status_led_task", 2048, NULL, 4, NULL);
}

static void reconfig_button_init(void)
{
    const bool active_low = CONFIG_ALARM_RECONFIG_BUTTON_ACTIVE_LEVEL == 0;
    const gpio_config_t gpio_config_button = {
        .pin_bit_mask = 1ULL << CONFIG_ALARM_RECONFIG_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&gpio_config_button));
    xTaskCreate(reconfig_button_task, "reconfig_button_task", 3072, NULL, 4, NULL);
}

void app_main(void)
{
    const edgent_config_t edgent_config = {
        .downlink_ds_callback = handle_downlink_datastream,
        .initial_connection_callback = on_initial_connection,
        .reboot_request_callback = on_reboot_request,
    };

    init_nvs();
    init_default_event_loop();
    init_edgent_event_handler();
    siren_init();
    status_led_init();
    reconfig_button_init();
    edgent_init(&edgent_config);
    edgent_start();
}
