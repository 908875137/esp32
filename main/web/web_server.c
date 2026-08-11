/*
 * web_server.c
 *
 * Lightweight HTTP server for WiFi provisioning.
 *
 * GET  /      → returns embedded HTML page
 * POST /save  → accepts ssid + password form data, saves to NVS, triggers STA connect
 *
 * After successful save, the server returns "OK" and the caller should:
 *   1. Call wifi_manager_save_config()
 *   2. Call wifi_manager_stop_provisioning()
 *   3. Call wifi_manager_connect()
 *   4. Restart (optional)
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "web_page.h"
#include "web_server.h"

static const char *TAG = "web_srv";
static httpd_handle_t s_server = NULL;

/* ---- POST /save callback ---- */

/* URL percent-decode in-place */
static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = (r[1] >= 'A') ? ((r[1] & 0x5F) - 'A' + 10) : (r[1] - '0');
            int lo = (r[2] >= 'A') ? ((r[2] & 0x5F) - 'A' + 10) : (r[2] - '0');
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' '; r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

typedef struct {
    char ssid[65];
    char password[65];
    bool done;
} provision_result_t;

static provision_result_t s_prov_result = {0};

static esp_err_t save_post_handler(httpd_req_t *req) {
    char buf[512] = {0};
    char ssid[65] = {0};
    char pass[65] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "POST raw body (%d bytes)", received);

    /* Try httpd_query_key_value first (handles urlencoded) */
    if (httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        /* Fallback: manual parse for multipart */
        char *p = strstr(buf, "name=\"ssid\"");
        if (p) {
            p = strstr(p, "\r\n\r\n");
            if (p) {
                p += 4;
                char *end = strstr(p, "\r\n");
                size_t len = end ? (size_t)(end - p) : strlen(p);
                if (len >= sizeof(ssid)) len = sizeof(ssid) - 1;
                memcpy(ssid, p, len);
            }
        }
    }

    if (httpd_query_key_value(buf, "password", pass, sizeof(pass)) != ESP_OK) {
        char *p = strstr(buf, "name=\"password\"");
        if (p) {
            p = strstr(p, "\r\n\r\n");
            if (p) {
                p += 4;
                char *end = strstr(p, "\r\n");
                size_t len = end ? (size_t)(end - p) : strlen(p);
                if (len >= sizeof(pass)) len = sizeof(pass) - 1;
                memcpy(pass, p, len);
            }
        }
    }

    /* URL-decode */
    url_decode(ssid);
    url_decode(pass);

    /* Mask password for logging */
    char pass_masked[65] = {0};
    int pass_len = (int)strlen(pass);
    if (pass_len > 0) {
        pass_masked[0] = pass[0];
        for (int i = 1; i < pass_len; i++) pass_masked[i] = '*';
    }

    ESP_LOGI(TAG, "POST /save: ssid=\"%s\" pass=\"%s\" len=%d",
             ssid, pass_masked, pass_len);

    if (strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
        return ESP_FAIL;
    }

    /* Save to result struct — main loop will pick it up */
    strncpy(s_prov_result.ssid, ssid, sizeof(s_prov_result.ssid) - 1);
    strncpy(s_prov_result.password, pass, sizeof(s_prov_result.password) - 1);
    s_prov_result.done = true;

    httpd_resp_sendstr(req, "OK — WiFi 已保存，设备正在连接...");
    return ESP_OK;
}

/* ---- GET / handler ---- */

static esp_err_t index_get_handler(httpd_req_t *req) {
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, WEB_PROVISION_HTML);
    return ESP_OK;
}

/* ---- Public API ---- */

esp_err_t web_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;
    cfg.max_open_sockets = 4;  /* LWIP_MAX_SOCKETS=8, minus 3 for HTTPD internal = 5 max */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* GET / */
    httpd_uri_t uri_get = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_get_handler,
        .user_ctx  = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_get);

    /* POST /save */
    httpd_uri_t uri_save = {
        .uri       = "/save",
        .method    = HTTP_POST,
        .handler   = save_post_handler,
        .user_ctx  = NULL,
    };
    httpd_register_uri_handler(s_server, &uri_save);

    /* Reset provision result */
    memset(&s_prov_result, 0, sizeof(s_prov_result));

    ESP_LOGI(TAG, "Web server started on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t web_server_stop(void) {
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
    return ESP_OK;
}

/* ---- Provision result accessor (used by main loop) ---- */

bool web_server_provision_done(void) {
    return s_prov_result.done;
}

void web_server_provision_get(char *ssid_out, size_t ssid_len,
                               char *pass_out, size_t pass_len) {
    if (ssid_out) strncpy(ssid_out, s_prov_result.ssid, ssid_len);
    if (pass_out) strncpy(pass_out, s_prov_result.password, pass_len);
}

void web_server_provision_reset(void) {
    memset(&s_prov_result, 0, sizeof(s_prov_result));
}
