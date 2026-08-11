/*
 * wifi_manager.h
 *
 * WiFi manager for ThinkINK EPD project.
 * Handles AP provisioning mode (ThinkINK-XXXX) and STA connection.
 * Stores credentials in NVS.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- NVS keys ---- */
#define WIFI_NVS_NAMESPACE  "wifi_cfg"
#define WIFI_NVS_KEY_SSID   "wifi_ssid"
#define WIFI_NVS_KEY_PASS   "wifi_password"

/* ---- AP provisioning defaults ---- */
#define WIFI_AP_SSID_PREFIX "ThinkINK-"
#define WIFI_AP_MAX_STA_CONN 4

/* ---- STA connection ---- */
#define WIFI_STA_CONNECT_TIMEOUT_MS  15000   /* 15 seconds to connect */
#define WIFI_STA_MAX_RETRY            3

/* ---- Public API ---- */

/**
 * Initialize WiFi stack (netif, event loop, default station/AP).
 * Call once at startup, before any other wifi_manager_* function.
 */
esp_err_t wifi_manager_init(void);

/**
 * Return true if valid SSID/password pair is stored in NVS.
 */
bool wifi_manager_has_config(void);

/**
 * Load SSID/password from NVS.
 * Caller provides buffers (ssid_out size >= 33, pass_out size >= 65).
 */
esp_err_t wifi_manager_load_config(char *ssid_out, size_t ssid_len,
                                   char *pass_out, size_t pass_len);

/**
 * Save SSID/password to NVS.
 */
esp_err_t wifi_manager_save_config(const char *ssid, const char *password);

/**
 * Connect to saved WiFi as station.
 * Returns ESP_OK when connected, or error on timeout.
 */
esp_err_t wifi_manager_connect(void);

/**
 * Start AP provisioning mode:
 *  - Creates SoftAP "ThinkINK-XXXX"
 *  - Starts DHCP server on 192.168.4.1
 *  - Waits for user to connect and POST config via web server
 */
esp_err_t wifi_manager_start_provisioning(void);

/**
 * Stop AP provisioning mode.
 */
esp_err_t wifi_manager_stop_provisioning(void);

/**
 * Return true if station is currently connected with an IP.
 */
bool wifi_manager_is_connected(void);

/**
 * Get the AP SSID used during provisioning (for display).
 */
const char *wifi_manager_get_ap_ssid(void);

#ifdef __cplusplus
}
#endif
