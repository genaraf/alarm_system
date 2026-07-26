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
#define CONFIG_ALARM_SIREN_START_DS "V5"
#endif

#ifndef CONFIG_ALARM_SIREN_STOP_DS
#define CONFIG_ALARM_SIREN_STOP_DS "V6"
#endif

#ifndef CONFIG_ALARM_SIREN_STATE_DS
#define CONFIG_ALARM_SIREN_STATE_DS "V7"
#endif

#define MAX_HOURS_VALUE      99U
#define MAX_MINSEC_VALUE     59U
#define MAX_SIREN_SECONDS    ((MAX_HOURS_VALUE * 3600U) + (MAX_MINSEC_VALUE * 60U) + MAX_MINSEC_VALUE)
#define TIMER_POLL_INTERVAL_MS 1000U
#define DEFAULT_PRESET_INDEX 0U

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

static const char *TAG = "alarm_system";

static siren_state_t s_siren_state;
static SemaphoreHandle_t s_siren_mutex;
static TaskHandle_t s_siren_task_handle;
static esp_timer_handle_t s_siren_timer;

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

static void decompose_seconds(uint32_t total_seconds, uint32_t *hours, uint32_t *minutes, uint32_t *seconds)
{
    const uint32_t safe_seconds = clamp_u32(total_seconds, MAX_SIREN_SECONDS);

    if (hours != NULL) {
        *hours = safe_seconds / 3600U;
    }

    if (minutes != NULL) {
        *minutes = (safe_seconds % 3600U) / 60U;
    }

    if (seconds != NULL) {
        *seconds = safe_seconds % 60U;
    }
}

static void format_hhmmss(uint32_t total_seconds, char value[9])
{
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;

    decompose_seconds(total_seconds, &hours, &minutes, &seconds);
    snprintf(value, 9, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32, hours, minutes, seconds);
}

static void siren_set_gpio(bool enabled)
{
    const int on_level = CONFIG_ALARM_SIREN_ACTIVE_LEVEL ? 1 : 0;
    const int off_level = on_level ? 0 : 1;

    gpio_set_level(CONFIG_ALARM_SIREN_GPIO, enabled ? on_level : off_level);
}

static void publish_remaining_time(uint32_t remaining_seconds)
{
    char value[9];

    format_hhmmss(remaining_seconds, value);
    edgent_publish_ds_str(CONFIG_ALARM_SIREN_TIME_DS, value);
}

static void publish_selected_preset(uint32_t preset_index)
{
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_PRESET_DS, preset_index);
}

static void publish_running_state(bool enabled)
{
    edgent_publish_ds_int(CONFIG_ALARM_SIREN_STATE_DS, enabled ? 1 : 0);
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
    siren_publish_full_state();
}

static void on_reboot_request(void)
{
    esp_restart();
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

void app_main(void)
{
    const edgent_config_t edgent_config = {
        .downlink_ds_callback = handle_downlink_datastream,
        .initial_connection_callback = on_initial_connection,
        .reboot_request_callback = on_reboot_request,
    };

    init_nvs();
    init_default_event_loop();
    siren_init();
    edgent_init(&edgent_config);
    edgent_start();
}
