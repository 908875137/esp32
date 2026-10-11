/*
 * admin_server.h
 *
 * Web management server (REST API + static files from SD card) + mDNS.
 *
 * Runs AFTER WiFi STA connects, on port 80 with mDNS "thinkink.local".
 * The provisioning web_server (web/ web_server.c) is separate and runs
 * only during SoftAP provisioning.
 */
#pragma once

#include "esp_err.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Request IDs returned by admin_server_take_request() */
typedef enum {
    ADMIN_REQ_NONE = 0,
    ADMIN_REQ_DISPLAY_FILE,
    ADMIN_REQ_NEXT,
    ADMIN_REQ_PREV,
    ADMIN_REQ_REFRESH,
    ADMIN_REQ_MODE_AI,
    ADMIN_REQ_MODE_SD,
} admin_request_t;

/**
 * Start mDNS ("thinkink.local") + HTTP server.
 *
 * Static files are served from /sdcard/www (if present).  REST API lives
 * under /api/ (see admin_server.c).  Returns ESP_OK on success.
 */
esp_err_t admin_server_start(void);

/**
 * Stop the HTTP server and mDNS.
 */
void admin_server_stop(void);

/**
 * Tell the admin server the current mode (for /api/status and /api/mode GET).
 */
void admin_server_set_mode(thinkink_mode_t mode);

/**
 * Non-blocking: consume a pending web request (or ADMIN_REQ_NONE).
 * The main loop calls this and executes the actual work.
 */
int admin_server_take_request(void);

/**
 * For ADMIN_REQ_DISPLAY_FILE, returns the requested filename.
 */
const char *admin_server_get_request_filename(void);

#ifdef __cplusplus
}
#endif
