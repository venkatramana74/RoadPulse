/*
 * pothole_config.h — Pothole LTE Firmware Configuration v1.0.0
 *
 * Board  : 7Semi ESP32-S3 + EC200U-CN (Quectel Cat-1 LTE)
 * Sensor : GY-61 (ADXL335) analog accelerometer
 *
 * WIRING:
 *   GY-61 Vcc  → 3.3V
 *   GY-61 GND  → GND
 *   GY-61 XOUT → GPIO4   (ADC1_CH3)
 *   GY-61 YOUT → GPIO5   (ADC1_CH4)
 *   GY-61 ZOUT → GPIO6   (ADC1_CH5)
 *
 * PROVISIONING (run once per device before first boot):
 *   python tools/provision_device.py \
 *       --port <COM_PORT> \
 *       --cert device.pem.crt \
 *       --key  private.pem.key \
 *       --device-id   pothole-unit-01 \
 *       --org-id      ORG-001 \
 *       --apn         airtelgprs.com \
 *       --endpoint    <your-iot-endpoint>.iot.ap-south-1.amazonaws.com
 *
 * IMPORTANT GPIO RULES (7Semi board):
 *   GPIO 9  — NEVER USE. Causes EC200U hardware reset.
 *   GPIO 10 — PWRKEY (leave alone, modem auto-powers from USB).
 *   GPIO 12 — EC200U RX (modem UART)
 *   GPIO 13 — EC200U TX (modem UART)
 */

#ifndef POTHOLE_CONFIG_H
#define POTHOLE_CONFIG_H

#include "esp_err.h"

// ── Firmware ──────────────────────────────────────────────────────────────────
#define FW_VERSION          "1.0.0"

// ── Device Identity — loaded from NVS at boot ─────────────────────────────────
// Written by tools/provision_device.py  (namespace "fleet_id")
#define IDENTITY_NVS_NS         "fleet_id"
#define IDENTITY_KEY_DEVICE_ID  "device_id"
#define IDENTITY_KEY_ORG_ID     "org_id"
#define IDENTITY_KEY_APN        "apn"
#define IDENTITY_KEY_ENDPOINT   "iot_endpoint"

extern char DEVICE_ID[64];
extern char ORG_ID[64];
extern char CELLULAR_APN[64];
extern char AWS_IOT_ENDPOINT[128];
extern char MQTT_TOPIC[192];   /* built at runtime: potholes/detection/{DEVICE_ID} */

esp_err_t config_load_from_nvs(void);

// ── AWS IoT Core ──────────────────────────────────────────────────────────────
// Port 443 — MQTT over TLS with ALPN "x-amzn-mqtt-ca"
// Port 8883 is silently blocked by Airtel CGNAT (TCP SYN timeout, misreported as SSL error 565)
#define AWS_IOT_PORT            443

// ── Modem UART (EC200U-CN) ────────────────────────────────────────────────────
#define MODEM_TX_PIN            13
#define MODEM_RX_PIN            12
#define MODEM_UART_NUM          UART_NUM_1
#define MODEM_BAUD              115200

// ── Accelerometer ADC Pins (GY-61 / ADXL335) ─────────────────────────────────
// GPIO4 = ADC1_CHANNEL_3 (X axis)
// GPIO5 = ADC1_CHANNEL_4 (Y axis)
// GPIO6 = ADC1_CHANNEL_5 (Z axis)
#define ACCEL_ADC_UNIT          ADC_UNIT_1
#define ACCEL_CH_X              ADC_CHANNEL_3   // GPIO4
#define ACCEL_CH_Y              ADC_CHANNEL_4   // GPIO5
#define ACCEL_CH_Z              ADC_CHANNEL_5   // GPIO6

// ADXL335 at 3.3V supply:
//   Zero-G output ≈ Vcc/2 = 1650 mV  →  raw ≈ 2048 (12-bit, 0-3.3V)
//   Sensitivity  ≈ 330 mV/g  (300-360 mV/g range — calibrate per unit)
#define ADXL_ZERO_G_MV          1650
#define ADXL_SENS_MV_PER_G      330
#define ADC_VREF_MV             3300
#define ADC_MAX_RAW             4095

// ── Pothole Detection Thresholds ──────────────────────────────────────────────
#define THRESHOLD_LOW_G         1.5f   // magnitude ≥ 1.5g → LOW severity
#define THRESHOLD_MEDIUM_G      2.5f   // magnitude ≥ 2.5g → MEDIUM severity
#define THRESHOLD_HIGH_G        3.5f   // magnitude ≥ 3.5g → HIGH severity
#define DETECTION_COOLDOWN_MS   2000   // min ms between detections
#define SAMPLING_RATE_HZ        100    // accelerometer sample rate

// ── Offline Buffer ────────────────────────────────────────────────────────────
#define OFFLINE_BUF_MAX         50     // max queued events
#define OFFLINE_MSG_SIZE        512    // max bytes per payload
#define POTHOLE_LOG_NVS_NS      "pothole_log"
#define POTHOLE_LOG_MAX         20     // persistent rolling event log in NVS

// ── Watchdog ──────────────────────────────────────────────────────────────────
#define WDT_TIMEOUT_SEC         90
#define MODEM_HEALTH_TIMEOUT_MS 60000

// ── MQTT Reconnection ─────────────────────────────────────────────────────────
#define MQTT_RETRY_BASE_MS      5000
#define MQTT_RETRY_MAX_MS       120000

#endif // POTHOLE_CONFIG_H
