/*
 * web_server.h
 *
 * Thin HTTP server for WiFi provisioning.
 * Handles GET / (provisioning page) and POST /save (save credentials).
 */
#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the provisioning web server on port 80.
 */
esp_err_t web_server_start(void);

/**
 * Stop the web server.
 */
esp_err_t web_server_stop(void);

/**
 * After POST /save, this returns true. Read credentials with _provision_get(),
 * then reset with _provision_reset().
 */
bool web_server_provision_done(void);

void web_server_provision_get(char *ssid_out, size_t ssid_len,
                               char *pass_out, size_t pass_len);

void web_server_provision_reset(void);

#ifdef __cplusplus
}
#endif
