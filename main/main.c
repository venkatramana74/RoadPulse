/*
 * main.c — Pothole LTE Firmware v1.0.0
 *
 * Board  : 7Semi ESP32-S3 + EC200U-CN (Quectel Cat-1 LTE)
 * Sensor : GY-61 (ADXL335) analog accelerometer
 *
 * Architecture (single-core FreeRTOS tasks):
 *   accel_task   — samples ADXL335 @ 100 Hz, detects pothole events, pushes to queue
 *   modem_task   — owns all AT-command I/O: LTE attach, GNSS, NTP, MQTT
 *   publish_task — drains event queue; publishes to AWS IoT Core via modem_task MQTT
 *
 * MQTT payload (JSON):
 * {
 *   "device_id"   : "pothole-unit-01",
 *   "org_id"      : "ORG-001",
 *   "timestamp_ms": 1700000000123,
 *   "severity"    : "HIGH",
 *   "peak_g"      : 3.87,
 *   "x_g"         : 0.12,
 *   "y_g"         : -0.09,
 *   "z_g"         : 3.85,
 *   "magnitude_g" : 3.87,
 *   "lat"         : 17.385044,
 *   "lng"         : 78.486671,
 *   "speed_kmh"   : 32.4,
 *   "has_gps"     : true
 * }
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

#include "pothole_config.h"
#include "certs/pothole_certs.h"

/* ── Tags ─────────────────────────────────────────────────────────────────── */
static const char *TAG_MAIN   = "MAIN";
static const char *TAG_ACCEL  = "ACCEL";
static const char *TAG_MODEM  = "MODEM";
static const char *TAG_PUB    = "PUB";

/* ── Global identity (filled from NVS at boot) ───────────────────────────── */
char DEVICE_ID[64]         = {0};
char ORG_ID[64]            = {0};
char CELLULAR_APN[64]      = {0};
char AWS_IOT_ENDPOINT[128] = {0};
char MQTT_TOPIC[192]       = {0};

/* ── Pothole event ───────────────────────────────────────────────────────── */
typedef struct {
    char     event_id[16];     /* "event_00", "event_01", ... */
    int64_t  timestamp_ms;   /* Unix epoch ms from modem NTP */
    float    x_g, y_g, z_g;
    float    peak_g;
    float    magnitude_g;
    char     severity[8];    /* "LOW", "MEDIUM", "HIGH" */
    double   lat, lng;
    float    speed_kmh;
    bool     has_gps;
} pothole_event_t;

/* ── Queues / semaphores ─────────────────────────────────────────────────── */
static QueueHandle_t     s_event_queue   = NULL;  /* accel → publish */
static SemaphoreHandle_t s_uart_mutex    = NULL;  /* guard modem UART */
static SemaphoreHandle_t s_modem_ready  = NULL;  /* modem_task signals ready */

/* ── Event-group bits ────────────────────────────────────────────────────── */
static EventGroupHandle_t s_modem_eg = NULL;
#define BIT_LTE_CONNECTED   BIT0
#define BIT_MQTT_CONNECTED  BIT1
#define BIT_TIME_SYNCED     BIT2

/* ── Current GPS state (updated by modem_task) ───────────────────────────── */
static volatile double  s_lat       = 0.0;
static volatile double  s_lng       = 0.0;
static volatile float   s_speed_kmh = 0.0f;
static volatile bool    s_has_gps   = false;
static SemaphoreHandle_t s_gps_mutex = NULL;

/* ── Current epoch time (ms) provided by modem NTP ──────────────────────── */
static volatile int64_t s_epoch_base_ms = 0;   /* set once after NTP sync   */
static volatile int64_t s_epoch_timer_us = 0;  /* esp_timer_get_time() snap */

/* ── Offline buffer (stored in PSRAM if available) ───────────────────────── */
static pothole_event_t  *s_offline_buf  = NULL;
static int               s_offline_head = 0;
static int               s_offline_tail = 0;
static int               s_offline_cnt  = 0;
static SemaphoreHandle_t s_offline_mutex = NULL;

/* ════════════════════════════════════════════════════════════════════════════
 *  NVS CONFIG LOADER
 * ════════════════════════════════════════════════════════════════════════════ */
esp_err_t config_load_from_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(IDENTITY_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_MAIN, "nvs_open(%s) failed: %s — device not provisioned?",
                 IDENTITY_NVS_NS, esp_err_to_name(err));
        return err;
    }

    size_t len;
#define NVS_GET_STR(key, buf) \
    len = sizeof(buf); \
    err = nvs_get_str(h, key, buf, &len); \
    if (err != ESP_OK) { \
        ESP_LOGE(TAG_MAIN, "NVS missing key '%s': %s", key, esp_err_to_name(err)); \
        nvs_close(h); return err; \
    }

    NVS_GET_STR(IDENTITY_KEY_DEVICE_ID, DEVICE_ID)
    NVS_GET_STR(IDENTITY_KEY_ORG_ID,    ORG_ID)
    NVS_GET_STR(IDENTITY_KEY_APN,       CELLULAR_APN)
    NVS_GET_STR(IDENTITY_KEY_ENDPOINT,  AWS_IOT_ENDPOINT)
    nvs_close(h);

    snprintf(MQTT_TOPIC, sizeof(MQTT_TOPIC), "potholes/detection/%s", DEVICE_ID);

    ESP_LOGI(TAG_MAIN, "Identity loaded — device=%s org=%s apn=%s",
             DEVICE_ID, ORG_ID, CELLULAR_APN);
    ESP_LOGI(TAG_MAIN, "Endpoint=%s  Topic=%s", AWS_IOT_ENDPOINT, MQTT_TOPIC);
    return ESP_OK;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  MODEM UART HELPERS
 * ════════════════════════════════════════════════════════════════════════════ */
#define MODEM_RX_BUF   2048
#define AT_TIMEOUT_MS  5000
#define AT_LONG_MS    30000

static void modem_uart_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    uart_driver_install(MODEM_UART_NUM, MODEM_RX_BUF, 0, 0, NULL, 0);
    uart_param_config(MODEM_UART_NUM, &cfg);
    uart_set_pin(MODEM_UART_NUM, MODEM_TX_PIN, MODEM_RX_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

/**
 * Send AT command and wait for expected response (or "ERROR").
 * Returns true if expected_resp found in modem reply within timeout_ms.
 */
static bool at_send(const char *cmd, const char *expected_resp,
                    int timeout_ms, char *rx_buf, size_t rx_buf_len)
{
    if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(35000)) != pdTRUE) {
        ESP_LOGE(TAG_MODEM, "UART mutex timeout for: %s", cmd);
        return false;
    }

    /* Flush RX */
    uart_flush_input(MODEM_UART_NUM);

    /* Send command */
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    /* Read response */
    if (!rx_buf || rx_buf_len == 0) {
        /* caller doesn't want the response — use a local buffer */
        static char tmp[512];
        rx_buf     = tmp;
        rx_buf_len = sizeof(tmp);
    }

    int  total  = 0;
    int  remain = (int)rx_buf_len - 1;
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline && remain > 0) {
        int n = uart_read_bytes(MODEM_UART_NUM,
                                (uint8_t *)(rx_buf + total),
                                remain,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            total  += n;
            remain -= n;
            rx_buf[total] = '\0';
            if (strstr(rx_buf, expected_resp) ||
                strstr(rx_buf, "ERROR")) {
                break;
            }
        }
    }
    rx_buf[total] = '\0';

    xSemaphoreGive(s_uart_mutex);

    bool ok = (strstr(rx_buf, expected_resp) != NULL);
    if (!ok) {
        ESP_LOGD(TAG_MODEM, "AT '%s' failed. Got: %.80s", cmd, rx_buf);
    }
    return ok;
}

/* Convenience: send AT, wait for OK */
static bool at_ok(const char *cmd)
{
    return at_send(cmd, "OK", AT_TIMEOUT_MS, NULL, 0);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  MODEM INIT — power-on sequence and basic configuration
 * ════════════════════════════════════════════════════════════════════════════ */
static bool modem_init(void)
{
    ESP_LOGI(TAG_MODEM, "Waiting for modem to boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Synchronise baud — try a few times */
    for (int i = 0; i < 10; i++) {
        if (at_ok("AT")) {
            ESP_LOGI(TAG_MODEM, "Modem responding to AT");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        if (i == 9) {
            ESP_LOGE(TAG_MODEM, "Modem not responding after 10 tries");
            return false;
        }
    }

    at_ok("ATE0");           /* disable echo */
    at_ok("AT+CMEE=2");      /* verbose error codes */
    at_ok("AT+QURCCFG=\"urcport\",\"uart1\""); /* URC → UART1 */

    /* Log SIM identity for verification */
    char rx_sim[128];
    at_send("AT+CIMI",  "OK", AT_TIMEOUT_MS, rx_sim, sizeof(rx_sim));
    ESP_LOGI(TAG_MODEM, "SIM IMSI  : %.40s", rx_sim);
    at_send("AT+QCCID", "OK", AT_TIMEOUT_MS, rx_sim, sizeof(rx_sim));
    ESP_LOGI(TAG_MODEM, "SIM ICCID : %.40s", rx_sim);
    at_send("AT+CNUM",  "OK", AT_TIMEOUT_MS, rx_sim, sizeof(rx_sim));
    ESP_LOGI(TAG_MODEM, "SIM Number: %.40s", rx_sim);

    ESP_LOGI(TAG_MODEM, "Modem init OK");
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  LTE ATTACH
 * ════════════════════════════════════════════════════════════════════════════ */
static bool lte_attach(void)
{
    char cmd[256], rx[256];

    /* Standard 3GPP PDP context config (required alongside QICSGP for EC200U-CN) */
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", CELLULAR_APN);
    at_ok(cmd);

    /* Quectel extended APN config */
    snprintf(cmd, sizeof(cmd),
             "AT+QICSGP=1,1,\"%s\",\"\",\"\",1", CELLULAR_APN);
    at_ok(cmd);

    /* Wait for LTE registration — CEREG stat=1 (home) or 5 (roaming) */
    ESP_LOGI(TAG_MODEM, "Waiting for LTE registration...");
    for (int i = 0; i < 60; i++) {
        if (at_send("AT+CEREG?", "+CEREG:", AT_TIMEOUT_MS, rx, sizeof(rx))) {
            if (strstr(rx, ",1") || strstr(rx, ",5")) {
                ESP_LOGI(TAG_MODEM, "LTE registered: %.60s", rx);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (i == 59) {
            ESP_LOGE(TAG_MODEM, "LTE registration timeout — check SIM/antenna");
            return false;
        }
    }

    /* ── Activate PDP and fix DNS ─────────────────────────────────────────
     * Read the DNS servers that Airtel actually assigns via CGCONTRDP,
     * then write them back with QIDNSCFG so MQTT uses the carrier's own
     * DNS (not the stale 8.8.8.8 retained from a previous session). */
    if (!at_send("AT+QIACT=1", "OK", 30000, rx, sizeof(rx))) {
        ESP_LOGW(TAG_MODEM, "QIACT activate failed, checking existing PDP state: %.80s", rx);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));

    memset(rx, 0, sizeof(rx));
    if (!at_send("AT+QIACT?", "+QIACT:", AT_TIMEOUT_MS, rx, sizeof(rx))) {
        ESP_LOGE(TAG_MODEM, "PDP context is not active after QIACT");
        return false;
    }
    ESP_LOGI(TAG_MODEM, "IP: %.80s", rx);

    /* Read network-assigned PDP parameters — includes carrier DNS */
    char pdp_rx[512] = {0};
    at_send("AT+CGCONTRDP=1", "+CGCONTRDP:", AT_TIMEOUT_MS, pdp_rx, sizeof(pdp_rx));
    ESP_LOGI(TAG_MODEM, "PDP params: %.200s", pdp_rx);

    /* Parse DNS from CGCONTRDP response and apply it.
     * Format: +CGCONTRDP: cid,bearer,"apn","laddr","gw","dns1"[,"dns2"] */
    char dns1[48] = {0}, dns2[48] = {0};
    char *pp = strstr(pdp_rx, "+CGCONTRDP:");
    if (pp) {
        sscanf(pp,
               "+CGCONTRDP: %*d,%*d,\"%*[^\"]\",\"%*[^\"]\",\"%*[^\"]\",\"%47[^\"]\",\"%47[^\"]\"",
               dns1, dns2);
    }

    if (strlen(dns1) > 6) {   /* got a real IP */
        /* Set DNS on both context 0 (global) and context 1 (PDP) so all
         * modem subsystems (MQTT, SSL, QPING) use the carrier's DNS */
        snprintf(cmd, sizeof(cmd), "AT+QIDNSCFG=0,\"%s\",\"%s\"", dns1, dns2);
        at_send(cmd, "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
        snprintf(cmd, sizeof(cmd), "AT+QIDNSCFG=1,\"%s\",\"%s\"", dns1, dns2);
        at_send(cmd, "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
        ESP_LOGI(TAG_MODEM, "DNS set to carrier: %s / %s", dns1, dns2);
    } else {
        /* Fallback: known Airtel India DNS servers — set on both contexts */
        at_ok("AT+QIDNSCFG=0,\"121.242.190.150\",\"115.249.168.190\"");
        at_ok("AT+QIDNSCFG=1,\"121.242.190.150\",\"115.249.168.190\"");
        ESP_LOGI(TAG_MODEM, "DNS set to Airtel fallback (both ctx)");
    }

    /* ── Network diagnostics ─────────────────────────────────────────────────
     * +QPING: 0,"x.x.x.x",ttl,rtt  → SUCCESS  (rtt in ms)
     * +QPING: <err>                 → FAILURE  (no IP field means error)
     * Common errors: 1=timeout, 2=no route, 3=DNS fail, 565=data not working */

    /* Signal quality */
    memset(rx, 0, sizeof(rx));
    at_send("AT+CSQ", "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
    ESP_LOGI(TAG_MODEM, "Signal (CSQ): %.40s", rx);

    /* Operator */
    memset(rx, 0, sizeof(rx));
    at_send("AT+COPS?", "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
    ESP_LOGI(TAG_MODEM, "Operator: %.60s", rx);

    /* Ping 8.8.8.8 by IP — no DNS needed, proves raw IP routing works */
    memset(rx, 0, sizeof(rx));
    at_send("AT+QPING=1,\"8.8.8.8\",5,1", "+QPING:", 15000, rx, sizeof(rx));
    ESP_LOGI(TAG_MODEM, "PING 8.8.8.8 (IP): %.100s", rx);
    bool ip_reachable = (strstr(rx, "+QPING: 0,") != NULL);

    /* Ping by hostname — proves DNS works */
    memset(rx, 0, sizeof(rx));
    at_send("AT+QPING=1,\"amazon.com\",5,1", "+QPING:", 15000, rx, sizeof(rx));
    ESP_LOGI(TAG_MODEM, "PING amazon.com (DNS): %.100s", rx);
    bool dns_ok = (strstr(rx, "+QPING: 0,") != NULL);

    if (!dns_ok) {
        ESP_LOGW(TAG_MODEM, "Carrier DNS failed, trying public DNS");
        at_send("AT+QIDNSCFG=0,\"8.8.8.8\",\"1.1.1.1\"", "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
        at_send("AT+QIDNSCFG=1,\"8.8.8.8\",\"1.1.1.1\"", "OK", AT_TIMEOUT_MS, rx, sizeof(rx));
        vTaskDelay(pdMS_TO_TICKS(500));
        memset(rx, 0, sizeof(rx));
        at_send("AT+QPING=1,\"amazon.com\",5,1", "+QPING:", 15000, rx, sizeof(rx));
        ESP_LOGI(TAG_MODEM, "PING amazon.com after DNS fallback: %.100s", rx);
    }

    /* Ping AWS endpoint */
    memset(rx, 0, sizeof(rx));
    snprintf(cmd, sizeof(cmd), "AT+QPING=1,\"%s\",5,1", AWS_IOT_ENDPOINT);
    at_send(cmd, "+QPING:", 15000, rx, sizeof(rx));
    ESP_LOGI(TAG_MODEM, "PING AWS endpoint: %.100s", rx);

    if (!ip_reachable) {
        ESP_LOGE(TAG_MODEM, "DATA NOT WORKING — IP routing failed. "
                             "Check SIM data plan and APN setting.");
        /* Still continue — try MQTT anyway in case QPING is restricted */
    } else {
        ESP_LOGI(TAG_MODEM, "IP routing OK — data plan active");
    }

    /* Keep context active — MQTT will use it (no pdpcid conflict when
     * context is already up and SSL is configured on same context). */
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  GNSS — start and read location
 * ════════════════════════════════════════════════════════════════════════════ */
static void gnss_start(void)
{
    at_ok("AT+QGPS=1");                /* enable GNSS */
    at_ok("AT+QGPSCFG=\"nmeasrc\",1"); /* enable NMEA output */
    ESP_LOGI(TAG_MODEM, "GNSS started");
}

/**
 * Parse AT+QGPSLOC=2 response:
 * +QGPSLOC: <UTC>,<lat>,<lng>,<hdop>,<alt>,<fix>,<cog>,<spkm>,<spkn>,<date>,<nsat>
 * Returns true if fix obtained.
 */
static bool gnss_read(double *lat, double *lng, float *speed_kmh)
{
    char rx[256];
    if (!at_send("AT+QGPSLOC=2", "+QGPSLOC:", AT_TIMEOUT_MS, rx, sizeof(rx))) {
        return false;
    }

    /* Find start of response line */
    char *p = strstr(rx, "+QGPSLOC:");
    if (!p) return false;
    p += strlen("+QGPSLOC:");

    /* Fields: UTC,lat,lon,hdop,alt,fix,cog,spkm,spkn,date,nsat */
    char utc[16], slat[16], slng[16], hdop[8], alt[10],
         fix[4], cog[8], spkm[10], spkn[10], date[10], nsat[4];

    int parsed = sscanf(p, "%15[^,],%15[^,],%15[^,],%7[^,],%9[^,],"
                           "%3[^,],%7[^,],%9[^,],%9[^,],%9[^,],%3s",
                        utc, slat, slng, hdop, alt, fix, cog, spkm, spkn, date, nsat);
    if (parsed < 3) return false;

    *lat       = atof(slat);
    *lng       = atof(slng);
    *speed_kmh = atof(spkm);  /* spkm = speed in km/h */
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  NETWORK TIME — get Unix timestamp from modem
 * ════════════════════════════════════════════════════════════════════════════ */
/**
 * AT+QLTS=2 returns local time adjusted with timezone info:
 * +QLTS: "2024/01/15,10:30:00+22,0"   ("+22" = +5:30 IST in 15-min units)
 * We want UTC epoch ms.  Use AT+CCLK? instead which gives UTC.
 */
static bool ntp_sync(void)
{
    char rx[128];

    /* Try to get network time */
    if (!at_send("AT+QLTS=2", "+QLTS:", AT_TIMEOUT_MS, rx, sizeof(rx))) {
        ESP_LOGW(TAG_MODEM, "QLTS failed, will use uptime as timestamp");
        return false;
    }

    /*
     * +QLTS: "2024/01/15,10:30:00+22,0"
     *  parse: year month day hour min sec tz_quarters
     */
    char *p = strstr(rx, "\"");
    if (!p) return false;
    p++;  /* skip opening quote */

    int yr = 0, mo = 0, dy = 0, hr = 0, mn = 0, sc = 0, tz_q = 0;
    int tz_sign = 1;
    if (sscanf(p, "%d/%d/%d,%d:%d:%d+%d",
               &yr, &mo, &dy, &hr, &mn, &sc, &tz_q) < 6) {
        /* Try negative TZ */
        if (sscanf(p, "%d/%d/%d,%d:%d:%d-%d",
                   &yr, &mo, &dy, &hr, &mn, &sc, &tz_q) < 6) {
            ESP_LOGW(TAG_MODEM, "Could not parse QLTS response: %.80s", rx);
            return false;
        }
        tz_sign = -1;
    }

    /*
     * Quick epoch approximation (±1s is fine for our use-case).
     * Use days_from_epoch formula:  days since 1970-01-01.
     */
    static const int days_per_month[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int64_t days = (yr - 1970) * 365 + (yr - 1969) / 4
                   - (yr - 1901) / 100 + (yr - 1601) / 400;
    for (int m = 1; m < mo; m++) {
        days += days_per_month[m-1];
        if (m == 2 && ((yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0))
            days++;
    }
    days += dy - 1;

    /* tz_q is in units of 15 min; subtract to get UTC */
    int64_t epoch_s = days * 86400LL
                    + (int64_t)hr * 3600
                    + (int64_t)mn * 60
                    + sc
                    - (int64_t)tz_sign * tz_q * 15 * 60;

    s_epoch_base_ms  = epoch_s * 1000LL;
    s_epoch_timer_us = esp_timer_get_time();

    xEventGroupSetBits(s_modem_eg, BIT_TIME_SYNCED);
    ESP_LOGI(TAG_MODEM, "Time synced — epoch=%lld ms", (long long)s_epoch_base_ms);
    return true;
}

/**
 * Get current Unix timestamp in ms.
 * Uses NTP-synced base + elapsed esp_timer ticks.
 * Falls back to uptime ms if NTP not yet synced.
 */
static int64_t get_timestamp_ms(void)
{
    if (s_epoch_base_ms == 0) {
        return (int64_t)(esp_timer_get_time() / 1000);
    }
    int64_t elapsed_us = esp_timer_get_time() - s_epoch_timer_us;
    return s_epoch_base_ms + elapsed_us / 1000;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  SSL/MQTT — uses AT+QSSLOPEN instead of AT+QMTOPEN
 *
 *  AT+QMTOPEN has a broken internal DNS resolver that ignores QIDNSCFG.
 *  AT+QSSLOPEN takes an explicit pdpctxID parameter, so it uses PDP context 1
 *  and its DNS — the same path as QPING which resolves correctly.
 *
 *  We speak MQTT 3.1.1 manually over the raw SSL socket:
 *    AT+QSSLOPEN  — open SSL/TLS socket on PDP context 1
 *    AT+QSSLSEND  — send raw MQTT packet bytes
 *    AT+QSSLRECV  — receive raw bytes (CONNACK / PINGRESP)
 *    AT+QSSLCLOSE — close socket
 * ════════════════════════════════════════════════════════════════════════════ */

/* TLS cert / key buffers — allocated at runtime to avoid stack overflow */
static char *s_tls_cert = NULL;
static char *s_tls_key  = NULL;
static int   s_mqtt_port = AWS_IOT_PORT;

#define SSL_CONN_ID  0   /* QSSLOPEN connection ID (0-4) */

/* ── SSL low-level send/receive ────────────────────────────────────────── */

static bool ssl_send_bytes(const uint8_t *data, size_t len)
{
    char cmd[64], rx[128] = {0};
    snprintf(cmd, sizeof(cmd), "AT+QSSLSEND=%d,%u", SSL_CONN_ID, (unsigned)len);

    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_flush_input(MODEM_UART_NUM);
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    /* Wait for '>' prompt */
    int total = 0;
    int64_t dl = esp_timer_get_time() + 10000000LL;
    while (esp_timer_get_time() < dl && total < (int)sizeof(rx)-1) {
        int n = uart_read_bytes(MODEM_UART_NUM, (uint8_t*)(rx+total),
                                sizeof(rx)-total-1, pdMS_TO_TICKS(50));
        if (n > 0) { total += n; rx[total] = '\0'; if (strstr(rx, ">")) break; }
    }
    if (!strstr(rx, ">")) {
        xSemaphoreGive(s_uart_mutex);
        ESP_LOGE(TAG_MODEM, "QSSLSEND no prompt: %.60s", rx);
        return false;
    }

    uart_write_bytes(MODEM_UART_NUM, data, len);

    /* Wait for SEND OK */
    total = 0; memset(rx, 0, sizeof(rx));
    dl = esp_timer_get_time() + 10000000LL;
    while (esp_timer_get_time() < dl && total < (int)sizeof(rx)-1) {
        int n = uart_read_bytes(MODEM_UART_NUM, (uint8_t*)(rx+total),
                                sizeof(rx)-total-1, pdMS_TO_TICKS(50));
        if (n > 0) { total += n; rx[total] = '\0';
            if (strstr(rx, "SEND OK") || strstr(rx, "ERROR")) break; }
    }
    xSemaphoreGive(s_uart_mutex);
    bool ok = strstr(rx, "SEND OK") != NULL;
    if (!ok) ESP_LOGE(TAG_MODEM, "QSSLSEND no SEND OK: %.60s", rx);
    return ok;
}

/* Returns bytes received (0 = none ready within timeout) */
static int ssl_recv_bytes(uint8_t *data, size_t max_len, int timeout_ms)
{
    char cmd[64], rx[512];
    snprintf(cmd, sizeof(cmd), "AT+QSSLRECV=%d,%u", SSL_CONN_ID, (unsigned)max_len);

    int64_t outer_dl = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < outer_dl) {
        memset(rx, 0, sizeof(rx));
        xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
        uart_flush_input(MODEM_UART_NUM);
        uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
        uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
        int total = 0;
        int64_t rd_dl = esp_timer_get_time() + 3000000LL;
        while (esp_timer_get_time() < rd_dl && total < (int)sizeof(rx)-1) {
            int n = uart_read_bytes(MODEM_UART_NUM, (uint8_t*)(rx+total),
                                    sizeof(rx)-total-1, pdMS_TO_TICKS(50));
            if (n > 0) { total += n; rx[total] = '\0';
                if (strstr(rx, "\r\nOK") || strstr(rx, "ERROR")) break; }
        }
        xSemaphoreGive(s_uart_mutex);

        char *p = strstr(rx, "+QSSLRECV:");
        if (p) {
            int recv_len = 0;
            sscanf(p + 10, "%d", &recv_len);
            if (recv_len > 0) {
                char *ds = strstr(p + 10, "\r\n");
                if (ds) {
                    ds += 2;
                    int avail = (int)(rx + total - ds);
                    int cp = (recv_len < avail) ? recv_len : avail;
                    if (cp > (int)max_len) cp = (int)max_len;
                    if (cp > 0) { memcpy(data, ds, cp); return cp; }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return 0;
}

/* ── MQTT 3.1.1 packet builders ────────────────────────────────────────── */

static int mqtt_encode_remaining(uint8_t *buf, int len)
{
    int i = 0;
    do {
        uint8_t b = len % 128;
        len /= 128;
        if (len > 0) b |= 0x80;
        buf[i++] = b;
    } while (len > 0);
    return i;
}

static int mqtt_build_connect_pkt(uint8_t *buf, const char *client_id)
{
    static const uint8_t vh[] = {
        0x00,0x04,'M','Q','T','T', /* Protocol Name */
        0x04,                      /* Level 3.1.1   */
        0x02,                      /* Flags: clean  */
        0x00,0x78                  /* Keepalive 120s */
    };
    uint16_t cid = (uint16_t)strlen(client_id);
    int remaining = (int)sizeof(vh) + 2 + cid;
    int pos = 0;
    buf[pos++] = 0x10;
    pos += mqtt_encode_remaining(buf + pos, remaining);
    memcpy(buf + pos, vh, sizeof(vh)); pos += sizeof(vh);
    buf[pos++] = (cid >> 8) & 0xFF; buf[pos++] = cid & 0xFF;
    memcpy(buf + pos, client_id, cid); pos += cid;
    return pos;
}

/* QoS 0 publish — no packet ID needed */
static int mqtt_build_publish_pkt(uint8_t *buf, size_t buf_len,
                                   const char *topic, const char *payload)
{
    uint16_t tlen = (uint16_t)strlen(topic);
    int plen = (int)strlen(payload);
    int remaining = 2 + tlen + plen;
    int pos = 0;

    if (buf_len < 5 || remaining > 268435455) {
        return -1;
    }
    if ((size_t)(1 + 4 + remaining) > buf_len) {
        return -1;
    }

    buf[pos++] = 0x30;  /* PUBLISH QoS 0 */
    pos += mqtt_encode_remaining(buf + pos, remaining);
    buf[pos++] = (tlen >> 8) & 0xFF; buf[pos++] = tlen & 0xFF;
    memcpy(buf + pos, topic, tlen); pos += tlen;
    memcpy(buf + pos, payload, plen); pos += plen;
    return pos;
}

/* ── High-level SSL/MQTT connection management ─────────────────────────── */

static bool mqtt_configure_tls(void)
{
    char cmd[256], rx[512];

    /* Load cert + key from NVS */
    if (!s_tls_cert) {
        s_tls_cert = (char *)malloc(POTHOLE_CERT_MAX_LEN);
        s_tls_key  = (char *)malloc(POTHOLE_KEY_MAX_LEN);
        if (!s_tls_cert || !s_tls_key) {
            ESP_LOGE(TAG_MODEM, "OOM allocating cert/key buffers");
            return false;
        }
    }
    if (cert_load_from_nvs(s_tls_cert, POTHOLE_CERT_MAX_LEN) != ESP_OK) return false;
    if (key_load_from_nvs(s_tls_key,  POTHOLE_KEY_MAX_LEN)  != ESP_OK) return false;

    size_t ca_len   = strlen(ROOT_CA_PEM);
    size_t cert_len = strlen(s_tls_cert);
    size_t key_len  = strlen(s_tls_key);

    /* Upload certs to modem UFS */
    at_send("AT+QFDEL=\"cacert.pem\"", "OK", 3000, rx, sizeof(rx));
    vTaskDelay(pdMS_TO_TICKS(300));
    snprintf(cmd, sizeof(cmd), "AT+QFUPL=\"cacert.pem\",%u,30", (unsigned)ca_len);
    if (!at_send(cmd, "CONNECT", 10000, rx, sizeof(rx))) {
        ESP_LOGE(TAG_MODEM, "QFUPL cacert failed"); return false;
    }
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(MODEM_UART_NUM, ROOT_CA_PEM, ca_len);
    xSemaphoreGive(s_uart_mutex);
    vTaskDelay(pdMS_TO_TICKS(3000));

    at_send("AT+QFDEL=\"client.pem\"", "OK", 3000, rx, sizeof(rx));
    vTaskDelay(pdMS_TO_TICKS(300));
    snprintf(cmd, sizeof(cmd), "AT+QFUPL=\"client.pem\",%u,30", (unsigned)cert_len);
    if (!at_send(cmd, "CONNECT", 10000, rx, sizeof(rx))) {
        ESP_LOGE(TAG_MODEM, "QFUPL client.pem failed"); return false;
    }
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(MODEM_UART_NUM, s_tls_cert, cert_len);
    xSemaphoreGive(s_uart_mutex);
    vTaskDelay(pdMS_TO_TICKS(3000));

    at_send("AT+QFDEL=\"client.key\"", "OK", 3000, rx, sizeof(rx));
    at_send("AT+QFDEL=\"client_key.pem\"", "OK", 3000, rx, sizeof(rx));
    vTaskDelay(pdMS_TO_TICKS(300));
    snprintf(cmd, sizeof(cmd), "AT+QFUPL=\"client_key.pem\",%u,30", (unsigned)key_len);
    if (!at_send(cmd, "CONNECT", 10000, rx, sizeof(rx))) {
        ESP_LOGE(TAG_MODEM, "QFUPL client_key.pem failed"); return false;
    }
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(MODEM_UART_NUM, s_tls_key, key_len);
    xSemaphoreGive(s_uart_mutex);
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Configure SSL context 2 for AWS IoT mutual TLS. */
#define SSL_CFG_OK(command) do { \
        if (!at_send((command), "OK", AT_TIMEOUT_MS, rx, sizeof(rx))) { \
            ESP_LOGE(TAG_MODEM, "SSL config failed: %s -> %.120s", (command), rx); \
            return false; \
        } \
    } while (0)

    SSL_CFG_OK("AT+QSSLCFG=\"sslversion\",2,4");
    SSL_CFG_OK("AT+QSSLCFG=\"ciphersuite\",2,0xFFFF");
    SSL_CFG_OK("AT+QSSLCFG=\"cacert\",2,\"UFS:cacert.pem\"");
    SSL_CFG_OK("AT+QSSLCFG=\"clientcert\",2,\"UFS:client.pem\"");
    SSL_CFG_OK("AT+QSSLCFG=\"clientkey\",2,\"UFS:client_key.pem\"");
    SSL_CFG_OK("AT+QSSLCFG=\"seclevel\",2,2");
    SSL_CFG_OK("AT+QSSLCFG=\"ignorelocaltime\",2,1");
    SSL_CFG_OK("AT+QSSLCFG=\"sni\",2,1");
#undef SSL_CFG_OK

    /*
     * AWS IoT MQTT on port 443 requires ALPN "x-amzn-mqtt-ca".
     * Some EC200U firmware builds reject QSSLCFG="alpn"; in that case,
     * fall back to AWS IoT's standard mutual-TLS MQTT port 8883.
     */
    s_mqtt_port = AWS_IOT_PORT;
    if (!at_send("AT+QSSLCFG=\"alpn\",2,1,\"x-amzn-mqtt-ca\"",
                 "OK", AT_TIMEOUT_MS, rx, sizeof(rx))) {
        ESP_LOGW(TAG_MODEM,
                 "ALPN config rejected by modem, trying alternate syntax: %.80s", rx);
        if (!at_send("AT+QSSLCFG=\"alpn\",2,\"x-amzn-mqtt-ca\"",
                     "OK", AT_TIMEOUT_MS, rx, sizeof(rx))) {
            ESP_LOGW(TAG_MODEM,
                     "ALPN unsupported, falling back to AWS IoT MQTT port 8883");
            s_mqtt_port = 8883;
        }
    }

    memset(s_tls_key, 0, POTHOLE_KEY_MAX_LEN);

    /* Verify cert files are present on modem UFS */
    char flst_rx[256];
    at_send("AT+QFLST", "OK", 5000, flst_rx, sizeof(flst_rx));
    ESP_LOGI(TAG_MODEM, "Modem UFS files: %.200s", flst_rx);

    ESP_LOGI(TAG_MODEM, "TLS configured");
    return true;
}

static bool mqtt_open(void)
{
    char cmd[256], rx[256];

    /* Close any stale SSL connection (ignore errors) */
    snprintf(cmd, sizeof(cmd), "AT+QSSLCLOSE=%d", SSL_CONN_ID);
    at_send(cmd, "OK", 5000, rx, sizeof(rx));
    vTaskDelay(pdMS_TO_TICKS(500));

    /* QSSLOPEN explicitly uses pdpctxID=1 → correct DNS path, bypasses QMTOPEN bug */
    snprintf(cmd, sizeof(cmd), "AT+QSSLOPEN=1,2,%d,\"%s\",%d,0",
             SSL_CONN_ID, AWS_IOT_ENDPOINT, s_mqtt_port);
    ESP_LOGI(TAG_MODEM, "QSSLOPEN → %s:%d (via PDP ctx 1)",
             AWS_IOT_ENDPOINT, s_mqtt_port);

    if (!at_send(cmd, "+QSSLOPEN:", 60000, rx, sizeof(rx))) {
        ESP_LOGE(TAG_MODEM, "QSSLOPEN timeout. Got: %.100s", rx);
        return false;
    }
    ESP_LOGI(TAG_MODEM, "QSSLOPEN raw: %.80s", rx);

    int conn_id = -1, result = -1;
    char *p = strstr(rx, "+QSSLOPEN:");
    if (p) sscanf(p + 10, "%d,%d", &conn_id, &result);
    if (result != 0) {
        ESP_LOGE(TAG_MODEM, "QSSLOPEN result=%d", result);
        return false;
    }
    ESP_LOGI(TAG_MODEM, "SSL socket opened");
    return true;
}

static bool mqtt_connect(void)
{
    uint8_t pkt[128];
    int pkt_len = mqtt_build_connect_pkt(pkt, DEVICE_ID);
    ESP_LOGI(TAG_MODEM, "Sending MQTT CONNECT (%d bytes)...", pkt_len);
    if (!ssl_send_bytes(pkt, pkt_len)) {
        ESP_LOGE(TAG_MODEM, "MQTT CONNECT send failed");
        return false;
    }

    /* Wait for CONNACK: 0x20 0x02 <session_present> <return_code> */
    uint8_t connack[8] = {0};
    int n = ssl_recv_bytes(connack, sizeof(connack), 15000);
    ESP_LOGI(TAG_MODEM, "CONNACK recv %d bytes: %02x %02x %02x %02x",
             n, connack[0], connack[1], connack[2], connack[3]);
    if (n < 4 || connack[0] != 0x20) {
        ESP_LOGE(TAG_MODEM, "Invalid CONNACK (expected 0x20, got 0x%02x)", connack[0]);
        return false;
    }
    if (connack[3] != 0) {
        ESP_LOGE(TAG_MODEM, "MQTT broker refused: code=%d", connack[3]);
        return false;
    }
    xEventGroupSetBits(s_modem_eg, BIT_MQTT_CONNECTED);
    ESP_LOGI(TAG_MODEM, "MQTT connected to AWS IoT Core");
    return true;
}

/**
 * Publish a JSON payload to MQTT_TOPIC (QoS 0 — fire and forget).
 */
static bool mqtt_publish(const char *payload)
{
    uint8_t pkt[600];
    int pkt_len = mqtt_build_publish_pkt(pkt, sizeof(pkt), MQTT_TOPIC, payload);
    if (pkt_len <= 0) {
        ESP_LOGW(TAG_PUB, "MQTT PUBLISH packet too large");
        return false;
    }
    if (!ssl_send_bytes(pkt, pkt_len)) {
        ESP_LOGW(TAG_PUB, "MQTT PUBLISH send failed");
        return false;
    }
    return true;
}

/* Send MQTT PINGREQ to keep the 120-second keepalive alive */
static void mqtt_ping(void) __attribute__((unused));
static void mqtt_ping(void)
{
    uint8_t ping[2] = {0xC0, 0x00};
    ssl_send_bytes(ping, 2);
    /* Drain the PINGRESP so the receive buffer doesn't fill up */
    uint8_t discard[8];
    ssl_recv_bytes(discard, sizeof(discard), 3000);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  OFFLINE BUFFER
 * ════════════════════════════════════════════════════════════════════════════ */
static void offline_buf_init(void)
{
    /* Try PSRAM first, fall back to internal heap */
    s_offline_buf = (pothole_event_t *)heap_caps_malloc(
                        OFFLINE_BUF_MAX * sizeof(pothole_event_t),
                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_offline_buf) {
        s_offline_buf = (pothole_event_t *)malloc(
                            OFFLINE_BUF_MAX * sizeof(pothole_event_t));
    }
    if (!s_offline_buf) {
        ESP_LOGE(TAG_MAIN, "Cannot allocate offline buffer!");
    } else {
        ESP_LOGI(TAG_MAIN, "Offline buffer: %d slots", OFFLINE_BUF_MAX);
    }
    s_offline_mutex = xSemaphoreCreateMutex();
}

static bool offline_enqueue(const pothole_event_t *ev)
{
    if (!s_offline_buf) return false;
    xSemaphoreTake(s_offline_mutex, portMAX_DELAY);
    if (s_offline_cnt >= OFFLINE_BUF_MAX) {
        /* Drop oldest */
        s_offline_tail = (s_offline_tail + 1) % OFFLINE_BUF_MAX;
        s_offline_cnt--;
    }
    s_offline_buf[s_offline_head] = *ev;
    s_offline_head = (s_offline_head + 1) % OFFLINE_BUF_MAX;
    s_offline_cnt++;
    xSemaphoreGive(s_offline_mutex);
    ESP_LOGW(TAG_PUB, "Event queued offline (%d buffered)", s_offline_cnt);
    return true;
}

static bool offline_dequeue(pothole_event_t *ev)
{
    if (!s_offline_buf) return false;
    xSemaphoreTake(s_offline_mutex, portMAX_DELAY);
    if (s_offline_cnt == 0) {
        xSemaphoreGive(s_offline_mutex);
        return false;
    }
    *ev = s_offline_buf[s_offline_tail];
    s_offline_tail = (s_offline_tail + 1) % OFFLINE_BUF_MAX;
    s_offline_cnt--;
    xSemaphoreGive(s_offline_mutex);
    return true;
}

/* ════════════════════════════════════════════════════════════════════════════
 *  BUILD JSON PAYLOAD
 * ════════════════════════════════════════════════════════════════════════════ */
static void build_payload(const pothole_event_t *ev, char *buf, size_t buf_len)
{
    if (ev->has_gps) {
        snprintf(buf, buf_len,
            "{"
            "\"event_id\":\"%s\","
            "\"device_id\":\"%s\","
            "\"org_id\":\"%s\","
            "\"timestamp_ms\":%" PRId64 ","
            "\"severity\":\"%s\","
            "\"peak_g\":%.3f,"
            "\"x_g\":%.3f,"
            "\"y_g\":%.3f,"
            "\"z_g\":%.3f,"
            "\"magnitude_g\":%.3f,"
            "\"lat\":%.6f,"
            "\"lng\":%.6f,"
            "\"speed_kmh\":%.1f,"
            "\"has_gps\":true"
            "}",
            ev->event_id,
            DEVICE_ID, ORG_ID,
            ev->timestamp_ms,
            ev->severity,
            ev->peak_g,
            ev->x_g, ev->y_g, ev->z_g,
            ev->magnitude_g,
            ev->lat, ev->lng,
            ev->speed_kmh);
    } else {
        snprintf(buf, buf_len,
            "{"
            "\"event_id\":\"%s\","
            "\"device_id\":\"%s\","
            "\"org_id\":\"%s\","
            "\"timestamp_ms\":%" PRId64 ","
            "\"severity\":\"%s\","
            "\"peak_g\":%.3f,"
            "\"x_g\":%.3f,"
            "\"y_g\":%.3f,"
            "\"z_g\":%.3f,"
            "\"magnitude_g\":%.3f,"
            "\"has_gps\":false"
            "}",
            ev->event_id,
            DEVICE_ID, ORG_ID,
            ev->timestamp_ms,
            ev->severity,
            ev->peak_g,
            ev->x_g, ev->y_g, ev->z_g,
            ev->magnitude_g);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  ACCELEROMETER TASK
 *  Samples GY-61 (ADXL335) at SAMPLING_RATE_HZ, computes magnitude,
 *  detects pothole events, pushes pothole_event_t to s_event_queue.
 * ════════════════════════════════════════════════════════════════════════════ */
static void save_pothole_event_to_nvs(pothole_event_t *ev)
{
    char payload[OFFLINE_MSG_SIZE];
    char key[16];
    uint32_t next = 0;
    uint32_t count = 0;
    size_t len;

    nvs_handle_t h;
    esp_err_t err = nvs_open(POTHOLE_LOG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_ACCEL, "Failed to open pothole log NVS: %s", esp_err_to_name(err));
        return;
    }

    len = sizeof(next);
    if (nvs_get_blob(h, "next", &next, &len) != ESP_OK || next >= POTHOLE_LOG_MAX) {
        next = 0;
    }

    len = sizeof(count);
    if (nvs_get_blob(h, "count", &count, &len) != ESP_OK || count > POTHOLE_LOG_MAX) {
        count = 0;
    }

    snprintf(key, sizeof(key), "event_%02lu", (unsigned long)next);
    snprintf(ev->event_id, sizeof(ev->event_id), "%s", key);
    build_payload(ev, payload, sizeof(payload));
    err = nvs_set_str(h, key, payload);
    if (err == ESP_OK) {
        nvs_set_str(h, "latest", payload);
        next = (next + 1) % POTHOLE_LOG_MAX;
        if (count < POTHOLE_LOG_MAX) {
            count++;
        }
        nvs_set_blob(h, "next", &next, sizeof(next));
        nvs_set_blob(h, "count", &count, sizeof(count));
        err = nvs_commit(h);
    }

    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG_ACCEL, "Saved pothole GPS event to NVS (%s)", key);
    } else {
        ESP_LOGE(TAG_ACCEL, "Failed to save pothole event: %s", esp_err_to_name(err));
    }
}

static void accel_task(void *arg)
{
    esp_task_wdt_add(NULL);

    /* Configure ADC */
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ACCEL_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t ch_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten    = ADC_ATTEN_DB_12,   /* 0–3.3V range */
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ACCEL_CH_X, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ACCEL_CH_Y, &ch_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ACCEL_CH_Z, &ch_cfg));

    ESP_LOGI(TAG_ACCEL, "Accelerometer ADC ready — sampling @ %d Hz",
             SAMPLING_RATE_HZ);

    const TickType_t sample_period = pdMS_TO_TICKS(1000 / SAMPLING_RATE_HZ);
    int64_t last_detect_us = 0;

    /* Running baseline (moving average over 32 samples for zero-G correction) */
    float base_x = 0.0f, base_y = 0.0f, base_z = 0.0f;
    int   base_cnt = 0;

    while (1) {
        esp_task_wdt_reset();

        int raw_x, raw_y, raw_z;
        adc_oneshot_read(adc_handle, ACCEL_CH_X, &raw_x);
        adc_oneshot_read(adc_handle, ACCEL_CH_Y, &raw_y);
        adc_oneshot_read(adc_handle, ACCEL_CH_Z, &raw_z);

        /* Convert raw → mV → g */
        float mv_x = (float)raw_x * ADC_VREF_MV / ADC_MAX_RAW;
        float mv_y = (float)raw_y * ADC_VREF_MV / ADC_MAX_RAW;
        float mv_z = (float)raw_z * ADC_VREF_MV / ADC_MAX_RAW;

        float gx = (mv_x - ADXL_ZERO_G_MV) / (float)ADXL_SENS_MV_PER_G;
        float gy = (mv_y - ADXL_ZERO_G_MV) / (float)ADXL_SENS_MV_PER_G;
        float gz = (mv_z - ADXL_ZERO_G_MV) / (float)ADXL_SENS_MV_PER_G;

        /* Warm up baseline for first 200 samples (2 s at 100 Hz) */
        if (base_cnt < 200) {
            base_x = (base_x * base_cnt + gx) / (base_cnt + 1);
            base_y = (base_y * base_cnt + gy) / (base_cnt + 1);
            base_z = (base_z * base_cnt + gz) / (base_cnt + 1);
            base_cnt++;
            vTaskDelay(sample_period);
            continue;
        }

        /* High-pass filter: subtract baseline (removes gravity + slow drift) */
        float ax = gx - base_x;
        float ay = gy - base_y;
        float az = gz - base_z;

        float magnitude = sqrtf(ax*ax + ay*ay + az*az);

        /* Detect pothole */
        if (magnitude >= THRESHOLD_LOW_G) {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_detect_us >= (int64_t)DETECTION_COOLDOWN_MS * 1000) {
                last_detect_us = now_us;

                pothole_event_t ev = {0};
                ev.timestamp_ms = get_timestamp_ms();
                ev.x_g          = ax;
                ev.y_g          = ay;
                ev.z_g          = az;
                ev.magnitude_g  = magnitude;
                ev.peak_g       = magnitude;

                if (magnitude >= THRESHOLD_HIGH_G) {
                    strcpy(ev.severity, "HIGH");
                } else if (magnitude >= THRESHOLD_MEDIUM_G) {
                    strcpy(ev.severity, "MEDIUM");
                } else {
                    strcpy(ev.severity, "LOW");
                }

                /* Snap GPS state */
                xSemaphoreTake(s_gps_mutex, pdMS_TO_TICKS(10));
                ev.lat       = s_lat;
                ev.lng       = s_lng;
                ev.speed_kmh = s_speed_kmh;
                ev.has_gps   = s_has_gps;
                xSemaphoreGive(s_gps_mutex);

                ESP_LOGI(TAG_ACCEL,
                         "Pothole! severity=%s peak=%.2fg (x=%.2f y=%.2f z=%.2f) "
                         "gps=%s lat=%.6f lng=%.6f",
                         ev.severity, ev.peak_g, ax, ay, az,
                         ev.has_gps ? "YES" : "NO", ev.lat, ev.lng);

                save_pothole_event_to_nvs(&ev);
                xQueueSend(s_event_queue, &ev, 0);  /* non-blocking */
            }
        }

        /* Slow update of baseline to track temperature drift */
        if (base_cnt % 10 == 0) {
            base_x = base_x * 0.995f + gx * 0.005f;
            base_y = base_y * 0.995f + gy * 0.005f;
            base_z = base_z * 0.995f + gz * 0.005f;
        }
        base_cnt++;

        vTaskDelay(sample_period);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  MODEM TASK
 *  Manages: LTE attach → TLS → MQTT connect → GPS polling → NTP sync
 *  Runs reconnection loop; signals BIT_MQTT_CONNECTED when up.
 * ════════════════════════════════════════════════════════════════════════════ */
static void modem_task(void *arg)
{
    esp_task_wdt_add(NULL);
    modem_uart_init();

    if (!modem_init()) {
        ESP_LOGE(TAG_MODEM, "Modem init failed — rebooting");
        esp_restart();
    }

    gnss_start();

    /* Main connectivity loop */
    uint32_t retry_ms = MQTT_RETRY_BASE_MS;

    while (1) {
        esp_task_wdt_reset();

        ESP_LOGI(TAG_MODEM, "Attaching to LTE...");
        if (!lte_attach()) {
            ESP_LOGW(TAG_MODEM, "LTE attach failed, retrying in %lu ms", retry_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > MQTT_RETRY_MAX_MS) ? MQTT_RETRY_MAX_MS : retry_ms * 2;
            continue;
        }
        xEventGroupSetBits(s_modem_eg, BIT_LTE_CONNECTED);
        retry_ms = MQTT_RETRY_BASE_MS;

        /* Sync time — AT+QLTS=2 reads time from cell tower, no data context needed */
        ntp_sync();

        /* Configure TLS certificates */
        if (!mqtt_configure_tls()) {
            ESP_LOGE(TAG_MODEM, "TLS config failed");
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            continue;
        }

        /* Open MQTT TCP connection */
        if (!mqtt_open()) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            continue;
        }

        /* MQTT CONNECT */
        if (!mqtt_connect()) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            continue;
        }

        xSemaphoreGive(s_modem_ready);
        ESP_LOGI(TAG_MODEM, "MQTT connected — signalling publish task");

        /* Health-check loop — GPS is handled by gps_task independently */
        int64_t last_ntp_us = 0;
        const int64_t NTP_INTERVAL_US = 300LL * 1000000;  /* re-sync every 5 min */

        while (1) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(5000));

            int64_t now_us = esp_timer_get_time();

            /* Periodic NTP re-sync */
            if (now_us - last_ntp_us >= NTP_INTERVAL_US) {
                last_ntp_us = now_us;
                ntp_sync();
            }

            /* Modem health check */
            char rx[64];
            if (!at_send("AT", "OK", 3000, rx, sizeof(rx))) {
                ESP_LOGE(TAG_MODEM, "Modem health check failed — reconnecting");
                xEventGroupClearBits(s_modem_eg,
                                     BIT_LTE_CONNECTED | BIT_MQTT_CONNECTED);
                break;
            }
        }

        /* Brief delay before reconnect attempt */
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = (retry_ms * 2 > MQTT_RETRY_MAX_MS) ? MQTT_RETRY_MAX_MS : retry_ms * 2;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  PUBLISH TASK
 *  Drains s_event_queue.  If MQTT connected → publish live.
 *  Else → buffer offline.  Drains offline buffer whenever MQTT reconnects.
 * ════════════════════════════════════════════════════════════════════════════ */
static void publish_task(void *arg)
{
    esp_task_wdt_add(NULL);

    /* Wait for modem to be ready — reset WDT every 5 s while waiting */
    while (xSemaphoreTake(s_modem_ready, pdMS_TO_TICKS(5000)) != pdTRUE) {
        esp_task_wdt_reset();
    }
    ESP_LOGI(TAG_PUB, "Publish task active");

    static char payload[OFFLINE_MSG_SIZE];

    while (1) {
        esp_task_wdt_reset();

        pothole_event_t ev;
        bool got_event = (xQueueReceive(s_event_queue, &ev,
                                        pdMS_TO_TICKS(500)) == pdTRUE);

        EventBits_t bits = xEventGroupGetBits(s_modem_eg);
        bool mqtt_up = (bits & BIT_MQTT_CONNECTED) != 0;

        if (got_event) {
            build_payload(&ev, payload, sizeof(payload));

            if (mqtt_up) {
                if (mqtt_publish(payload)) {
                    ESP_LOGI(TAG_PUB, "Published: %s", payload);
                } else {
                    ESP_LOGW(TAG_PUB, "Publish failed — buffering offline");
                    xEventGroupClearBits(s_modem_eg, BIT_MQTT_CONNECTED);
                    offline_enqueue(&ev);
                }
            } else {
                offline_enqueue(&ev);
            }
        }

        /* Drain offline buffer if MQTT is now up */
        if (mqtt_up) {
            pothole_event_t old_ev;
            while (offline_dequeue(&old_ev)) {
                esp_task_wdt_reset();
                build_payload(&old_ev, payload, sizeof(payload));
                if (!mqtt_publish(payload)) {
                    ESP_LOGW(TAG_PUB, "Offline drain failed — re-buffering");
                    offline_enqueue(&old_ev);
                    xEventGroupClearBits(s_modem_eg, BIT_MQTT_CONNECTED);
                    break;
                }
                ESP_LOGI(TAG_PUB, "Offline event published");
                vTaskDelay(pdMS_TO_TICKS(100));  /* pacing */
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  GPS TASK
 *  Runs independently of MQTT — polls GNSS every 5 s and updates the shared
 *  GPS state used by accel_task when stamping pothole events.
 *  Waits for BIT_LTE_CONNECTED (which means modem UART is initialised and
 *  gnss_start() has already been called by modem_task) before polling.
 * ════════════════════════════════════════════════════════════════════════════ */
static void gps_task(void *arg)
{
    esp_task_wdt_add(NULL);

    ESP_LOGI(TAG_MODEM, "GPS task: waiting for modem init to complete...");

    /* Wait for LTE connected, then an extra 60 s so modem_task finishes
     * cert upload + QMTOPEN before we start competing for the UART mutex */
    while (!(xEventGroupGetBits(s_modem_eg) & BIT_LTE_CONNECTED)) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* Give modem_task time to finish TLS config + QMTOPEN (~45 s worst case) */
    for (int i = 0; i < 60; i++) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG_MODEM, "GPS task: polling started");

    while (1) {
        esp_task_wdt_reset();

        double lat, lng;
        float  spd;

        if (gnss_read(&lat, &lng, &spd)) {
            xSemaphoreTake(s_gps_mutex, portMAX_DELAY);
            s_lat       = lat;
            s_lng       = lng;
            s_speed_kmh = spd;
            s_has_gps   = true;
            xSemaphoreGive(s_gps_mutex);
            ESP_LOGI(TAG_MODEM, "GPS fix: lat=%.6f  lng=%.6f  speed=%.1f km/h",
                     lat, lng, spd);
        } else {
            xSemaphoreTake(s_gps_mutex, portMAX_DELAY);
            s_has_gps = false;
            xSemaphoreGive(s_gps_mutex);
            ESP_LOGW(TAG_MODEM, "GPS: no fix yet (waiting for satellite lock)");
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 *  app_main
 * ════════════════════════════════════════════════════════════════════════════ */
void app_main(void)
{
    ESP_LOGI(TAG_MAIN, "Pothole LTE Firmware v%s starting", FW_VERSION);

    /* ── NVS init ─────────────────────────────────────────────────────────── */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* ── Load identity from NVS ───────────────────────────────────────────── */
    if (config_load_from_nvs() != ESP_OK) {
        ESP_LOGE(TAG_MAIN,
                 "Device not provisioned! Run tools/provision_device.py first.");
        /* Halt — blink LED or just spin */
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* ── Watchdog ─────────────────────────────────────────────────────────── */
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
        .idle_core_mask = 0,
        .trigger_panic  = true,
    };
    esp_task_wdt_reconfigure(&wdt_cfg);

    /* ── Offline buffer ───────────────────────────────────────────────────── */
    offline_buf_init();

    /* ── Primitives ───────────────────────────────────────────────────────── */
    s_event_queue  = xQueueCreate(32, sizeof(pothole_event_t));
    s_uart_mutex   = xSemaphoreCreateMutex();
    s_modem_ready  = xSemaphoreCreateBinary();
    s_gps_mutex    = xSemaphoreCreateMutex();
    s_modem_eg     = xEventGroupCreate();
    configASSERT(s_event_queue);
    configASSERT(s_uart_mutex);
    configASSERT(s_modem_ready);
    configASSERT(s_gps_mutex);
    configASSERT(s_modem_eg);

    /* ── Launch tasks ─────────────────────────────────────────────────────── */
    /*
     * Stack sizes:
     *   modem_task   : 6 KB  — AT parsing + TLS cert upload
     *   accel_task   : 4 KB  — ADC + math
     *   publish_task : 4 KB  — JSON formatting + MQTT write
     */
    xTaskCreate(modem_task,   "modem",   6144, NULL, 5, NULL);
    xTaskCreate(gps_task,     "gps",     4096, NULL, 4, NULL);
    xTaskCreate(accel_task,   "accel",   4096, NULL, 4, NULL);
    xTaskCreate(publish_task, "publish", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG_MAIN, "All tasks launched");
}
