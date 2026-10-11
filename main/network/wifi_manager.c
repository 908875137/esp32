/*
 * wifi_manager.c
 *
 * WiFi manager implementation for ThinkINK EPD.
 *
 * Flow:
 *   Has saved config? → connect STA
 *     success → done
 *     fail → fall through to provisioning
 *   No config → provisioning AP
 *
 * Uses ESP-IDF WiFi event group to synchronise connection attempts.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "wifi_manager.h"

static const char *TAG = "wifi_mgr";

/* ---- Event group bits ---- */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_evt = NULL;
static char s_ap_ssid[32] = {0};
static char s_sta_ip[16] = {0};
static int  s_retry_count = 0;
static bool s_provisioning = false;

/* ================================================================
 * NVS helpers
 * ================================================================ */

static esp_err_t nvs_init_once(void) {
    static bool inited = false;
    if (inited) return ESP_OK;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err == ESP_OK) inited = true;
    return err;
}

/* ================================================================
 * Public API — Config
 * ================================================================ */

bool wifi_manager_has_config(void) {
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t err = wifi_manager_load_config(ssid, sizeof(ssid), pass, sizeof(pass));
    return (err == ESP_OK && strlen(ssid) > 0);
}

esp_err_t wifi_manager_load_config(char *ssid_out, size_t ssid_len,
                                   char *pass_out, size_t pass_len) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = ssid_len;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid_out, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    len = pass_len;
    err = nvs_get_str(handle, WIFI_NVS_KEY_PASS, pass_out, &len);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_manager_save_config(const char *ssid, const char *password) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, password ? password : "");
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

/* ================================================================
 * Event handler
 * ================================================================ */

/* WiFi disconnect reason names */
static const char *disc_reason_str(int reason) {
    switch (reason) {
    case 1:  return "UNSPECIFIED";
    case 2:  return "AUTH_EXPIRE";
    case 3:  return "AUTH_LEAVE";
    case 4:  return "ASSOC_EXPIRE";
    case 5:  return "ASSOC_TOOMANY";
    case 6:  return "NOT_AUTHED";
    case 7:  return "NOT_ASSOCED";
    case 8:  return "ASSOC_LEAVE";
    case 9:  return "ASSOC_NOT_AUTHED";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    default:  return "UNKNOWN";
    }
}

/* Event handler */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
            const char *rs = disc_reason_str(ev->reason);
            if (s_provisioning) {
                ESP_LOGI(TAG, "STA disconnected (prov): reason=%d(%s)", ev->reason, rs);
                break;
            }
            if (s_retry_count < WIFI_STA_MAX_RETRY) {
                esp_wifi_connect();
                s_retry_count++;
                ESP_LOGW(TAG, "STA retry %d/%d reason=%d(%s)",
                         s_retry_count, WIFI_STA_MAX_RETRY, ev->reason, rs);
            } else {
                xEventGroupSetBits(s_wifi_evt, WIFI_FAIL_BIT);
                ESP_LOGE(TAG, "STA failed after %d retries, reason=%d(%s)",
                         WIFI_STA_MAX_RETRY, ev->reason, rs);
            }
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "AP: station " MACSTR " joined, AID=%d",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "AP: station " MACSTR " left, AID=%d",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

/* ================================================================
 * Public API — Init
 * ================================================================ */

esp_err_t wifi_manager_init(void) {
    /* One-time NVS init */
    esp_err_t err = nvs_init_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Create event group before registering handlers */
    if (s_wifi_evt == NULL) {
        s_wifi_evt = xEventGroupCreate();
        if (s_wifi_evt == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Initialise TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create default WiFi station */
    esp_netif_create_default_wifi_sta();

    /* Create default WiFi AP (only used when provisioning) */
    esp_netif_create_default_wifi_ap();

    /* WiFi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Reduce TX power to shrink the WiFi current spike (battery brownout
       mitigation).  Unit is 0.25 dBm; 32 = 8 dBm.  Lower = less current. */
    esp_wifi_set_max_tx_power(32);

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Station + AP mode by default */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    ESP_LOGI(TAG, "WiFi manager initialised");
    return ESP_OK;
}

/* ================================================================
 * Public API — Connect STA
 * ================================================================ */

esp_err_t wifi_manager_connect(void) {
    char ssid[33] = {0};
    char pass[65] = {0};

    esp_err_t err = wifi_manager_load_config(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No WiFi config in NVS");
        return err;
    }

    ESP_LOGI(TAG, "Connecting to WiFi SSID=\"%s\" pass=\"%s\"...", ssid, pass);

    s_retry_count = 0;
    s_provisioning = false;

    /* Configure as STA */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);

    /* Try auth modes in turn — WPA2 first (most common), others as fallback
       for WPA3-only or WPA/WPA2-mixed routers. */
    static const wifi_auth_mode_t authmodes[] = {
        WIFI_AUTH_WPA2_PSK,
        WIFI_AUTH_WPA2_WPA3_PSK,
        WIFI_AUTH_WPA3_PSK,
        WIFI_AUTH_WPA_WPA2_PSK,
    };

    for (size_t i = 0; i < sizeof(authmodes) / sizeof(authmodes[0]); i++) {
        cfg.sta.threshold.authmode = authmodes[i];
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_count = 0;
        ESP_ERROR_CHECK(esp_wifi_start());

        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_evt,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,        /* don't clear on exit */
            pdFALSE,        /* wait for any bit   */
            pdMS_TO_TICKS(WIFI_STA_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi connected (authmode %d)", (int)authmodes[i]);
            return ESP_OK;
        }

        esp_wifi_stop();
        ESP_LOGW(TAG, "authmode %d failed — trying next", (int)authmodes[i]);
    }

    ESP_LOGE(TAG, "WiFi connection failed with all auth modes");
    return ESP_ERR_TIMEOUT;
}

/* ================================================================
 * Public API — Provisioning AP
 * ================================================================ */

static void generate_ap_ssid(char *buf, size_t len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, len, "%s%02X%02X",
             WIFI_AP_SSID_PREFIX, mac[4], mac[5]);
}

esp_err_t wifi_manager_start_provisioning(void) {
    s_provisioning = true;

    generate_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
    ESP_LOGI(TAG, "Starting provisioning AP: %s", s_ap_ssid);

    /* Switch to AP + STA mode — STA can still try, AP is for provisioning */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Configure AP */
    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    strncpy((char *)ap_cfg.ap.password, "", sizeof(ap_cfg.ap.password) - 1); /* open */
    ap_cfg.ap.max_connection = WIFI_AP_MAX_STA_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Start WiFi if not already started */
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Provisioning AP active — connect to %s, open 192.168.4.1", s_ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_stop_provisioning(void) {
    ESP_LOGI(TAG, "Stopping provisioning AP");
    s_provisioning = false;

    /* Switch to pure STA mode (keep STA connection if any) */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    return ESP_OK;
}

/* ================================================================
 * Public API — Status
 * ================================================================ */

bool wifi_manager_is_connected(void) {
    if (s_wifi_evt == NULL) return false;
    EventBits_t bits = xEventGroupGetBits(s_wifi_evt);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_disconnect(void) {
    ESP_LOGI(TAG, "Disconnecting and powering down WiFi");
    esp_err_t err = esp_wifi_stop();
    if (s_wifi_evt) {
        xEventGroupClearBits(s_wifi_evt, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    s_sta_ip[0] = '\0';
    return err;
}

const char *wifi_manager_get_ap_ssid(void) {
    return s_ap_ssid;
}

esp_err_t wifi_manager_get_ip(char *ip_out, size_t ip_len) {
    if (!ip_out || ip_len == 0) return ESP_ERR_INVALID_ARG;
    if (s_sta_ip[0] == '\0') {
        ip_out[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    strncpy(ip_out, s_sta_ip, ip_len - 1);
    ip_out[ip_len - 1] = '\0';
    return ESP_OK;
}
