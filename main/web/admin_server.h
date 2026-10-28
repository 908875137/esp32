/*
 * admin_server.h
 *
 * Web management server (REST API + static files from SD card) + mDNS.
 *
 * Runs AFTER WiFi STA connects, on port 80 with mDNS "thinkink.local".
 * The provisioning web_server (web/ web_server.c) is separate and runs
 * only during SoftAP provisioning.
 *
 * REST handlers are split by domain into api_content.c / api_files.c /
 * api_core.c; this header exposes the shared helpers and the per-domain
 * route-registration entry points.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Static web UI root on the SD card */
#define WWW_ROOT "/sdcard/www"

/* Request IDs returned by admin_server_take_request() */
typedef enum {
    ADMIN_REQ_NONE = 0,
    ADMIN_REQ_DISPLAY_FILE,
    ADMIN_REQ_NEXT,
    ADMIN_REQ_PREV,
    ADMIN_REQ_REFRESH,
    ADMIN_REQ_MODE_AI,
    ADMIN_REQ_MODE_SD,
    ADMIN_REQ_IMAGE_ONLY,      /* swap image, keep the current quote (AI + SD) */
    ADMIN_REQ_DELETE_IMAGE,    /* delete the current image (SD only) */
} admin_request_t;

/**
 * Start mDNS ("thinkink.local") + HTTP server.
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

/* ---- shared helpers (used by the api_*.c handler files) ---- */

void admin_send_json(httpd_req_t *req, const char *json);
const char *admin_content_type(const char *path);
void admin_url_decode(const char *src, char *dst, size_t dst_len);

void admin_set_request(int req);
void admin_set_request_filename(const char *name);
thinkink_mode_t admin_get_mode(void);

/* ---- per-domain route registration (called from admin_server_start) ---- */

void api_content_register(httpd_handle_t server);
void api_files_register(httpd_handle_t server);
void api_core_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
