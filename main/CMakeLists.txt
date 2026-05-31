idf_component_register(
    SRCS "main.c" "certs/pothole_certs.c"
    INCLUDE_DIRS "." "certs"
    REQUIRES driver nvs_flash esp_timer esp_adc  esp_system esp_psram heap
)
