idf_component_register(
    SRCS "alarm_system.c"
    INCLUDE_DIRS "."
    REQUIRES blynk__edgent driver esp_event esp_timer freertos nvs_flash
)
