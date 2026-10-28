/*
 * admin_server.c
 *
 * Web management server core: mDNS, static file serving, the shared JSON /
 * content-type / url-decode helpers, request-flag plumbing, and HTTP server
 * start/stop.  The REST handlers live in api_content.c / api_files.c /
 * api_core.c and register their own routes.
 *
 * Static files: served from /sdcard/www, path-normalised and restricted to
 * that directory (no ../ or absolute paths).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"

#include "admin_server.h"
#include "wifi_manager.h"
#include "image_library.h"

static const char *TAG = "admin";

static httpd_handle_t s_server = NULL;

/* ---- Pending requests (consumed by main loop) ---- */

static volatile int s_request = ADMIN_REQ_NONE;
static char s_req_filename[64] = {0};

/* Current mode — set by main via admin_server_set_mode() */
static volatile thinkink_mode_t s_cur_mode = THINKINK_MODE_SD;

void admin_server_set_mode(thinkink_mode_t mode) {
    s_cur_mode = mode;
}

thinkink_mode_t admin_get_mode(void) {
    return (thinkink_mode_t)s_cur_mode;
}

void admin_set_request(int req) {
    s_request = req;
}

void admin_set_request_filename(const char *name) {
    strncpy(s_req_filename, name, sizeof(s_req_filename) - 1);
    s_req_filename[sizeof(s_req_filename) - 1] = '\0';
}

/* ---- mDNS ---- */

static void mdns_init_if_needed(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set("thinkink");
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
    err = mdns_instance_name_set("ThinkINK e-paper art frame");
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_instance_name_set failed: %s", esp_err_to_name(err));
    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_service_add failed: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "mDNS ready: hostname=thinkink  url=http://thinkink.local");
}

/* ---- shared helpers ---- */

const char *admin_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".jpg")  == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".webmanifest") == 0) return "application/manifest+json";
    if (strcmp(ext, ".woff") == 0) return "font/woff";
    if (strcmp(ext, ".woff2")== 0) return "font/woff2";
    if (strcmp(ext, ".ttf")  == 0) return "font/ttf";
    if (strcmp(ext, ".otf")  == 0) return "font/otf";
    return "application/octet-stream";
}

void admin_send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

/* Decode %XX URL-encoding (httpd_query_key_value does NOT decode). */
void admin_url_decode(const char *src, char *dst, size_t dst_len) {
    size_t i = 0, o = 0;
    while (src[i] && o < dst_len - 1) {
        if (src[i] == '%' &&
            ((src[i+1] >= '0' && src[i+1] <= '9') || (src[i+1] >= 'a' && src[i+1] <= 'f') || (src[i+1] >= 'A' && src[i+1] <= 'F')) &&
            ((src[i+2] >= '0' && src[i+2] <= '9') || (src[i+2] >= 'a' && src[i+2] <= 'f') || (src[i+2] >= 'A' && src[i+2] <= 'F'))) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
}

/* ---- Static file handler (restricted to /sdcard/www) ---- */

static esp_err_t static_handler(httpd_req_t *req) {
    const char *uri = req->uri;

    if (strstr(uri, "..") || strchr(uri, '\\')) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char path[640];
    if (strcmp(uri, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.html", WWW_ROOT);
    } else {
        if (strlen(uri) > 512) {
            httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
            return ESP_FAIL;
        }
        snprintf(path, sizeof(path), "%s%s", WWW_ROOT, uri);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (strcmp(uri, "/") == 0) {
            ESP_LOGW(TAG, "%s/index.html does not exist — Web UI not found", WWW_ROOT);
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            httpd_resp_sendstr(req,
                "<html><body style='font-family:sans-serif;padding:2rem;'>"
                "<h1>ThinkINK Web UI not found</h1>"
                "<p>Place your web UI in <code>/sdcard/www/</code> on the SD card.</p>"
                "</body></html>");
            return ESP_OK;
        }
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, admin_content_type(path));
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- Image file handler (/images/NNNNNNNN.png -> images/NNN/NNNNNNNN.png) ---- */

static esp_err_t image_file_handler(httpd_req_t *req) {
    const char *name = req->uri + strlen("/images/");
    if (strstr(name, "..") || strchr(name, '\\') || *name == '\0') {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    uint32_t id = image_library_id_from_name(name);
    if (id == 0) { httpd_resp_send_404(req); return ESP_FAIL; }

    char path[96];
    image_library_get_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, admin_content_type(path));
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ---- Start / stop ---- */

esp_err_t admin_server_start(void) {
    mdns_init_if_needed();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 48;
    cfg.max_open_sockets = 8;
    cfg.stack_size = 16384;           /* upload handler has a 4KB body buffer */
    cfg.uri_match_fn = httpd_uri_match_wildcard;   /* enable wildcard routes */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Domain handlers register their own routes. */
    api_core_register(s_server);
    api_content_register(s_server);
    api_files_register(s_server);

    /* /images/… and the catch-all static route are core; register them last
       so the more-specific /api/ and /images/ routes win. */
    httpd_uri_t uri_images = { .uri="/images/*", .method=HTTP_GET, .handler=image_file_handler, .user_ctx=NULL };
    httpd_uri_t uri_static = { .uri="/*", .method=HTTP_GET, .handler=static_handler, .user_ctx=NULL };
    httpd_register_uri_handler(s_server, &uri_images);
    httpd_register_uri_handler(s_server, &uri_static);

    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));
    if (ip[0] != '\0') {
        ESP_LOGI(TAG, "Admin server started: http://thinkink.local  (direct: http://%s)", ip);
    } else {
        ESP_LOGW(TAG, "Admin server started: http://thinkink.local  (no STA IP yet)");
    }
    return ESP_OK;
}

void admin_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    mdns_free();
    ESP_LOGI(TAG, "Admin server stopped");
}

/* ---- Request accessors (used by main loop) ---- */

int admin_server_take_request(void) {
    int r = s_request;
    s_request = ADMIN_REQ_NONE;
    return r;
}

const char *admin_server_get_request_filename(void) {
    return s_req_filename;
}
