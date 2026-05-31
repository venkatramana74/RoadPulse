/*
 * pothole_certs.h — TLS certificate loader interface
 *
 * Provides NVS-backed loading of device certificate and private key,
 * plus the embedded Amazon Root CA 1 for AWS IoT Core mutual TLS.
 */

#pragma once

#include "esp_err.h"
#include <stddef.h>

/* ── NVS namespace & key constants ── */
#define POTHOLE_CERTS_NVS_NAMESPACE   "fleet_certs"
#define POTHOLE_CERTS_NVS_KEY_CERT    "client_cert"
#define POTHOLE_CERTS_NVS_KEY_KEY     "client_key"

/* ── Buffer sizes (must fit PEM + null terminator) ── */
#define POTHOLE_CERT_MAX_LEN          2048
#define POTHOLE_KEY_MAX_LEN           2048

/* ── Amazon Root CA 1 — embedded in firmware ──
   Source: https://www.amazontrust.com/repository/AmazonRootCA1.pem
   Used for AWS IoT Core mutual TLS on port 8883.                    */
static const char ROOT_CA_PEM[] =
    "-----BEGIN CERTIFICATE-----\r\n"
    "MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\r\n"
    "ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\r\n"
    "b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\r\n"
    "MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\r\n"
    "b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\r\n"
    "ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\r\n"
    "9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\r\n"
    "IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\r\n"
    "VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\r\n"
    "93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\r\n"
    "jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\r\n"
    "AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\r\n"
    "A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\r\n"
    "U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\r\n"
    "N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\r\n"
    "o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\r\n"
    "5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\r\n"
    "rqXRfboQnoZsG4q5WTP468SQvvG5\r\n"
    "-----END CERTIFICATE-----\r\n";

/* ── Public API ── */

/**
 * @brief Load device certificate PEM from NVS.
 *
 * @param buf     Output buffer (null-terminated PEM on success).
 * @param buf_len Size of buf in bytes (must be >= POTHOLE_CERT_MAX_LEN).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t cert_load_from_nvs(char *buf, size_t buf_len);

/**
 * @brief Load device private key PEM from NVS.
 *
 * @param buf     Output buffer (null-terminated PEM on success).
 * @param buf_len Size of buf in bytes (must be >= POTHOLE_KEY_MAX_LEN).
 * @return ESP_OK on success, or an esp_err_t error code.
 */
esp_err_t key_load_from_nvs(char *buf, size_t buf_len);
