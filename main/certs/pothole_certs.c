/*
 * pothole_certs.c — NVS-backed TLS certificate loader
 *
 * Reads device certificate and private key from NVS namespace "fleet_certs".
 * Keys are written by tools/provision_device.py at provisioning time.
 */

#include "pothole_certs.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "pothole_certs";

static esp_err_t load_blob_from_nvs(const char *nvs_key,
                                     char       *buf,
                                     size_t      buf_len)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(POTHOLE_CERTS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open(\"%s\") failed: %s",
                 POTHOLE_CERTS_NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    size_t len = buf_len - 1;
    err = nvs_get_blob(handle, nvs_key, buf, &len);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS key \"%s\" not found — run provision_device.py", nvs_key);
        return err;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Buffer too small for \"%s\"", nvs_key);
        return err;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_get_blob(\"%s\") failed: %s", nvs_key, esp_err_to_name(err));
        return err;
    }

    buf[len] = '\0';
    ESP_LOGI(TAG, "Loaded \"%s\" from NVS (%u bytes)", nvs_key, (unsigned)len);
    return ESP_OK;
}

esp_err_t cert_load_from_nvs(char *buf, size_t buf_len)
{
    return load_blob_from_nvs(POTHOLE_CERTS_NVS_KEY_CERT, buf, buf_len);
}

esp_err_t key_load_from_nvs(char *buf, size_t buf_len)
{
    return load_blob_from_nvs(POTHOLE_CERTS_NVS_KEY_KEY, buf, buf_len);
}
