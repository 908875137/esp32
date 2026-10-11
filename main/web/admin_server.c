/*
 * admin_server.c
 *
 * Web management server (REST API + static files from SD) + mDNS.
 *
 * Static files: served from /sdcard/www, path-normalised and restricted to
 * that directory (no ../ or absolute paths).  If the directory / index is
 * missing, we return a clear 404 page rather than failing the device.
 *
 * REST API under /api/ (see handlers below).  Display/mode requests are
 * NOT executed in the HTTP handler — they just set a flag; the main loop does the real
 * decode/dither/EPD work (which is too heavy for the HTTP task).
 *
 * mDNS: hostname "thinkink" → http://thinkink.local
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"
#include "cJSON.h"

#include "admin_server.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "config_store.h"
#include "image_library.h"
#include "quote_library.h"
#include "log_to_sd.h"
#include "daily.h"
#include "ota_manager.h"

static const char *TAG = "admin";

static httpd_handle_t s_server = NULL;

#define WWW_ROOT        "/sdcard/www"
#define MAX_UPLOAD_SIZE (500 * 1024)

/* ---- Pending requests (consumed by main loop) ---- */

static volatile int s_request = ADMIN_REQ_NONE;
static char s_req_filename[64] = {0};

/* Current mode — set by main via admin_server_set_mode() */
static volatile thinkink_mode_t s_cur_mode = THINKINK_MODE_SD;

void admin_server_set_mode(thinkink_mode_t mode) {
    s_cur_mode = mode;
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

/* ---- Content-Type by extension ---- */

static const char *content_type_for(const char *path) {
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

/* ---- Static file handler (restricted to /sdcard/www) ---- */

static esp_err_t static_handler(httpd_req_t *req) {
    const char *uri = req->uri;

    /* Reject path traversal and backslashes */
    if (strstr(uri, "..") || strchr(uri, '\\')) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char path[640];
    if (strcmp(uri, "/") == 0) {
        snprintf(path, sizeof(path), "%s/index.html", WWW_ROOT);
    } else {
        /* uri is already vetted (no "..", no backslash); bound its length
           defensively so snprintf can never overflow path. */
        if (strlen(uri) > 512) {
            httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "uri too long");
            return ESP_FAIL;
        }
        snprintf(path, sizeof(path), "%s%s", WWW_ROOT, uri);
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Distinguish "no www dir at all" from "specific file missing" */
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

    /* Serve the file in chunks */
    httpd_resp_set_type(req, content_type_for(path));
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
    httpd_resp_send_chunk(req, NULL, 0);  /* end chunk */
    return ESP_OK;
}

/* ---- JSON helpers ---- */

static void send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

/* ---- GET /api/status ---- */

static esp_err_t api_status_handler(httpd_req_t *req) {
    char ip[16] = "";
    wifi_manager_get_ip(ip, sizeof(ip));

    uint64_t sd_total = 0, sd_free = 0;
    sd_card_get_capacity(&sd_total, &sd_free);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device", "ThinkINK");
    cJSON_AddStringToObject(root, "version", ota_get_version());
    cJSON_AddBoolToObject(root, "wifi", wifi_manager_is_connected());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "mdns", "thinkink.local");
    cJSON_AddStringToObject(root, "mode", s_cur_mode == THINKINK_MODE_AI ? "ai" : "sd");
    cJSON_AddNumberToObject(root, "image_count", sd_card_count_images());
    cJSON_AddNumberToObject(root, "sd_total", (double)sd_total);
    cJSON_AddNumberToObject(root, "sd_free", (double)sd_free);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- GET /api/images ---- */

static esp_err_t api_images_handler(httpd_req_t *req) {
    uint32_t offset = 0, limit = 24;
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(qbuf, "offset", v, sizeof(v)) == ESP_OK)
            offset = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(qbuf, "limit", v, sizeof(v)) == ESP_OK)
            limit = (uint32_t)strtoul(v, NULL, 10);
    }
    if (limit == 0) limit = 24;
    if (limit > 100) limit = 100;

    uint32_t total = 0;
    image_library_count(&total);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "images");
    for (uint32_t i = 0; i < limit && offset + i < total; i++) {
        uint32_t id = 0;
        if (image_library_get_id_by_index(offset + i, &id) != ESP_OK) break;

        char path[96];
        image_library_get_path(id, path, sizeof(path));

        char name[16];
        snprintf(name, sizeof(name), "%08lu.png", (unsigned long)id);

        struct stat st;
        long size = 0;
        if (stat(path, &st) == 0) size = (long)st.st_size;

        uint32_t used = 0;
        image_library_get_used_count(id, &used);

        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", name);
        cJSON_AddNumberToObject(item, "size", size);
        cJSON_AddNumberToObject(item, "used_count", (double)used);
        cJSON_AddItemToArray(arr, item);
    }

    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "total", (double)total);
    cJSON_AddNumberToObject(root, "offset", (double)offset);
    cJSON_AddNumberToObject(root, "limit", (double)limit);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- GET /api/quotes?offset=N&limit=M ---- */

static esp_err_t api_quotes_handler(httpd_req_t *req) {
    uint32_t offset = 0, limit = 10;
    char qbuf[64];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char v[16];
        if (httpd_query_key_value(qbuf, "offset", v, sizeof(v)) == ESP_OK)
            offset = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(qbuf, "limit", v, sizeof(v)) == ESP_OK)
            limit = (uint32_t)strtoul(v, NULL, 10);
    }
    if (limit == 0) limit = 10;
    if (limit > 50) limit = 50;

    uint32_t total = 0;
    quote_library_count(&total);

    quote_t *page = (quote_t *)calloc(limit, sizeof(quote_t));
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }
    uint32_t n = 0;
    quote_library_get_page(offset, limit, page, &n);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "total", (double)total);
    cJSON_AddNumberToObject(root, "offset", (double)offset);
    cJSON_AddNumberToObject(root, "limit", (double)limit);
    cJSON *arr = cJSON_AddArrayToObject(root, "quotes");
    for (uint32_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", (double)page[i].id);
        cJSON_AddStringToObject(item, "text", page[i].text);
        cJSON_AddStringToObject(item, "author", page[i].author);
        cJSON_AddStringToObject(item, "source", page[i].source);
        cJSON_AddNumberToObject(item, "used_count", (double)page[i].used_count);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddBoolToObject(root, "success", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(page);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- POST /api/quotes  { text, author, source } ---- */

static esp_err_t api_quotes_post_handler(httpd_req_t *req) {
    char body[1024] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[r] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    const char *text = NULL, *author = "", *source = "";
    cJSON *t = cJSON_GetObjectItem(j, "text");
    cJSON *a = cJSON_GetObjectItem(j, "author");
    cJSON *s = cJSON_GetObjectItem(j, "source");
    if (cJSON_IsString(t) && t->valuestring) text = t->valuestring;
    if (cJSON_IsString(a) && a->valuestring) author = a->valuestring;
    if (cJSON_IsString(s) && s->valuestring) source = s->valuestring;

    if (!text || text[0] == '\0') {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "text required");
        return ESP_FAIL;
    }

    uint32_t id = 0;
    esp_err_t err = quote_library_add(text, author, source, &id);
    cJSON_Delete(j);
    if (err != ESP_OK || id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "add quote failed");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "id", (double)id);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- GET /api/current ---- */

static esp_err_t api_current_handler(httpd_req_t *req) {
    char date[16] = {0};
    uint32_t image_id = 0, quote_id = 0;
    if (daily_get_last(date, sizeof(date), &image_id, &quote_id) != ESP_OK) {
        send_json(req, "{\"success\":true,\"has_content\":false}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "has_content", true);
    cJSON_AddStringToObject(root, "date", date);

    if (image_id != 0) {
        char name[16];
        snprintf(name, sizeof(name), "%08lu.png", (unsigned long)image_id);
        uint32_t used = 0;
        image_library_get_used_count(image_id, &used);
        cJSON_AddNumberToObject(root, "image_id", (double)image_id);
        cJSON_AddStringToObject(root, "image_name", name);
        cJSON_AddNumberToObject(root, "image_used", (double)used);
    }
    if (quote_id != 0) {
        quote_t q;
        if (quote_library_get_by_id(quote_id, &q) == ESP_OK) {
            cJSON_AddNumberToObject(root, "quote_id", (double)quote_id);
            cJSON_AddStringToObject(root, "quote_text", q.text);
            cJSON_AddStringToObject(root, "quote_author", q.author);
            cJSON_AddStringToObject(root, "quote_source", q.source);
            cJSON_AddNumberToObject(root, "quote_used", (double)q.used_count);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- GET /api/stats ---- */

static esp_err_t api_stats_handler(httpd_req_t *req) {
    image_library_stats_t is;
    quote_library_stats_t qs;
    image_library_get_stats(&is);
    quote_library_get_stats(&qs);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    cJSON *img = cJSON_CreateObject();
    cJSON_AddNumberToObject(img, "count", (double)is.count);
    cJSON_AddNumberToObject(img, "total_used", (double)is.total_used);
    cJSON_AddNumberToObject(img, "avg_used", is.count ? (double)is.total_used / (double)is.count : 0.0);
    cJSON_AddNumberToObject(img, "max_used_id", (double)is.max_used_id);
    cJSON_AddNumberToObject(img, "max_used", (double)is.max_used);
    cJSON_AddNumberToObject(img, "min_used_id", (double)is.min_used_id);
    cJSON_AddNumberToObject(img, "min_used", (double)is.min_used);
    cJSON_AddItemToObject(root, "images", img);

    cJSON *qo = cJSON_CreateObject();
    cJSON_AddNumberToObject(qo, "count", (double)qs.count);
    cJSON_AddNumberToObject(qo, "total_used", (double)qs.total_used);
    cJSON_AddNumberToObject(qo, "avg_used", qs.count ? (double)qs.total_used / (double)qs.count : 0.0);
    cJSON_AddNumberToObject(qo, "max_used_id", (double)qs.max_used_id);
    cJSON_AddNumberToObject(qo, "max_used", (double)qs.max_used);
    cJSON_AddNumberToObject(qo, "min_used_id", (double)qs.min_used_id);
    cJSON_AddNumberToObject(qo, "min_used", (double)qs.min_used);
    cJSON_AddItemToObject(root, "quotes", qo);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- DELETE /api/quotes/<id> ---- */

static esp_err_t api_quote_delete_handler(httpd_req_t *req) {
    const char *id_str = req->uri + strlen("/api/quotes/");
    uint32_t id = (uint32_t)strtoul(id_str, NULL, 10);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid id");
        return ESP_FAIL;
    }
    if (quote_library_remove(id) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- POST /api/quotes/edit  {id,text,author,source} ---- */

static esp_err_t api_quote_edit_handler(httpd_req_t *req) {
    char body[1024] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }
    body[r] = '\0';

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    uint32_t id = 0;
    const char *text = NULL, *author = "", *source = "";
    cJSON *i = cJSON_GetObjectItem(j, "id");
    cJSON *t = cJSON_GetObjectItem(j, "text");
    cJSON *a = cJSON_GetObjectItem(j, "author");
    cJSON *s = cJSON_GetObjectItem(j, "source");
    if (cJSON_IsNumber(i)) id = (uint32_t)i->valuedouble;
    if (cJSON_IsString(t) && t->valuestring) text = t->valuestring;
    if (cJSON_IsString(a) && a->valuestring) author = a->valuestring;
    if (cJSON_IsString(s) && s->valuestring) source = s->valuestring;

    if (id == 0 || !text) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id and text required");
        return ESP_FAIL;
    }
    esp_err_t err = quote_library_update(id, text, author, source);
    cJSON_Delete(j);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "update failed");
        return ESP_FAIL;
    }
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- POST /api/quotes/import  (raw CSV body, overwrites quotes.csv) ---- */

#define MAX_QUOTES_IMPORT_SIZE (1024 * 1024)

static esp_err_t api_quotes_import_handler(httpd_req_t *req) {
    const char *tmp_path = "/sdcard/quotes_import.tmp";

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    static char buf[1024];
    int total = 0;
    int received;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        total += received;
        if (total > MAX_QUOTES_IMPORT_SIZE) {
            fclose(f);
            unlink(tmp_path);
            httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "too large");
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, f);
    }
    fclose(f);

    /* overwrite quotes.csv (unlink first — FATFS rename won't overwrite) */
    unlink(QUOTE_LIB_CSV_PATH);
    if (rename(tmp_path, QUOTE_LIB_CSV_PATH) != 0) {
        ESP_LOGE(TAG, "quotes import: rename failed");
        unlink(tmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "import failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "quotes imported: %d bytes", total);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* forward decl — defined below, before the file-manager handlers */
static void url_decode(const char *src, char *dst, size_t dst_len);

/* ---- POST /api/www/upload?filename=xxx  (overwrite a file in /sdcard/www) ---- */

static esp_err_t api_www_upload_handler(httpd_req_t *req) {
    char filename_raw[64] = {0};
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "filename", filename_raw, sizeof(filename_raw));
    }
    char filename[64] = {0};
    url_decode(filename_raw, filename, sizeof(filename));
    if (filename[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename required");
        return ESP_FAIL;
    }
    /* relative path allowed (e.g. "js/app.js"); forbid traversal / absolute */
    if (strstr(filename, "..") || filename[0] == '/' || strchr(filename, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
        return ESP_FAIL;
    }

    /* auto-map bare front-end filenames into their sub-directories */
    char rel[96];
    if (!strchr(filename, '/')) {
        if (strcmp(filename, "app.js") == 0) snprintf(rel, sizeof(rel), "js/app.js");
        else if (strcmp(filename, "style.css") == 0) snprintf(rel, sizeof(rel), "css/style.css");
        else snprintf(rel, sizeof(rel), "%s", filename);
    } else {
        snprintf(rel, sizeof(rel), "%s", filename);
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", WWW_ROOT, rel);

    FILE *f = fopen(path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    static char buf[1024];
    int received;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, received, f);
    }
    fclose(f);

    ESP_LOGI(TAG, "www file uploaded: %s -> %s", filename, rel);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- GET /api/logs (tail of the SD log file, plain text) ---- */

static esp_err_t api_logs_handler(httpd_req_t *req) {
    const size_t cap = 65536;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int n = log_to_sd_tail(buf, cap, cap);
    if (n < 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no log file");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

/* ---- File manager: list / delete / download / upload (directory browser) ---- */

/* Decode %XX URL-encoding (httpd_query_key_value does NOT decode). */
static void url_decode(const char *src, char *dst, size_t dst_len) {
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

typedef struct {
    char name[64];
    bool is_dir;
    uint32_t size;
    uint32_t mtime;
} file_entry_t;

static int file_entry_cmp(const void *a, const void *b) {
    const file_entry_t *ea = (const file_entry_t *)a;
    const file_entry_t *eb = (const file_entry_t *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;  /* dirs first */
    return strcmp(ea->name, eb->name);
}

static esp_err_t api_files_handler(httpd_req_t *req) {
    char path_raw[128] = {0};
    char qbuf[256];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "path", path_raw, sizeof(path_raw));
    }
    char path[128] = {0};
    url_decode(path_raw, path, sizeof(path));
    if (strstr(path, "..") || path[0] == '/' || strchr(path, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    char full[320];
    if (path[0] == '\0') snprintf(full, sizeof(full), "%s", SD_MOUNT_POINT);
    else snprintf(full, sizeof(full), "%s/%s", SD_MOUNT_POINT, path);

    DIR *dir = opendir(full);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "dir not found");
        return ESP_FAIL;
    }

    const int max = 1024;
    file_entry_t *entries = (file_entry_t *)malloc(max * sizeof(file_entry_t));
    if (!entries) {
        closedir(dir);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_FAIL;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;   /* skip . .. and hidden */

        char p[640];
        snprintf(p, sizeof(p), "%s/%s", full, name);
        struct stat st;
        if (stat(p, &st) != 0) continue;

        strncpy(entries[count].name, name, sizeof(entries[count].name) - 1);
        entries[count].name[sizeof(entries[count].name) - 1] = '\0';
        entries[count].is_dir = S_ISDIR(st.st_mode);
        entries[count].size = (uint32_t)st.st_size;
        entries[count].mtime = (uint32_t)st.st_mtime;
        count++;
    }
    closedir(dir);

    qsort(entries, count, sizeof(file_entry_t), file_entry_cmp);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "path", path);
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", entries[i].name);
        cJSON_AddBoolToObject(item, "is_dir", entries[i].is_dir);
        cJSON_AddNumberToObject(item, "size", (double)entries[i].size);
        cJSON_AddNumberToObject(item, "mtime", (double)entries[i].mtime);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddNumberToObject(root, "count", (double)count);
    cJSON_AddBoolToObject(root, "success", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(entries);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t api_file_delete_handler(httpd_req_t *req) {
    char name[320];
    url_decode(req->uri + strlen("/api/files/"), name, sizeof(name));
    if (name[0] == '\0' || strstr(name, "..") || strchr(name, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_FAIL;
    }

    char path[640];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    if (unlink(path) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "file deleted: %s", name);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_file_download_handler(httpd_req_t *req) {
    char name_raw[64] = {0};
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "name", name_raw, sizeof(name_raw));
    }
    char name[64] = {0};
    url_decode(name_raw, name, sizeof(name));
    if (name[0] == '\0' || strstr(name, "..") || name[0] == '/' || strchr(name, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_FAIL;
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, content_type_for(path));
    char cd[128];
    snprintf(cd, sizeof(cd), "attachment; filename=\"%s\"", name);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_file_upload_handler(httpd_req_t *req) {
    char filename_raw[64] = {0};
    char path_raw[128] = {0};
    char qbuf[256];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "filename", filename_raw, sizeof(filename_raw));
        httpd_query_key_value(qbuf, "path", path_raw, sizeof(path_raw));
    }
    char filename[64] = {0};
    url_decode(filename_raw, filename, sizeof(filename));
    if (filename[0] == '\0' || strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
        return ESP_FAIL;
    }

    char dir[128] = {0};
    url_decode(path_raw, dir, sizeof(dir));
    if (strstr(dir, "..") || dir[0] == '/' || strchr(dir, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_FAIL;
    }

    char full[320];
    if (dir[0] == '\0') snprintf(full, sizeof(full), "%s/%s", SD_MOUNT_POINT, filename);
    else snprintf(full, sizeof(full), "%s/%s/%s", SD_MOUNT_POINT, dir, filename);

    FILE *f = fopen(full, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    static char buf[1024];
    int received;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, received, f);
    }
    fclose(f);

    ESP_LOGI(TAG, "file uploaded: %s/%s", dir[0] ? dir : ".", filename);
    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- OTA firmware upgrade ---- */

#define OTA_TOKEN "thinkink"

static esp_err_t api_ota_status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", ota_get_version());
    cJSON_AddStringToObject(root, "project_name", ota_get_project_name());
    cJSON_AddBoolToObject(root, "success", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t api_ota_upload_handler(httpd_req_t *req) {
    /* Basic access restriction: require a shared token. */
    char token[64] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token)) != ESP_OK ||
        strcmp(token, OTA_TOKEN) != 0) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid token");
        return ESP_FAIL;
    }

    ota_result_t r = ota_begin();
    if (r != OTA_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_FAIL;
    }

    static char buf[4096];  /* static — httpd task stack is tight */
    int received;
    bool failed = false;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        r = ota_write((const uint8_t *)buf, (size_t)received);
        if (r != OTA_OK) {
            ESP_LOGE(TAG, "ota_write failed (%d)", (int)r);
            failed = true;
            break;
        }
    }
    if (received < 0) {
        ESP_LOGE(TAG, "httpd_req_recv error");
        failed = true;
    }

    if (failed) {
        ota_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
        return ESP_FAIL;
    }

    r = ota_finish();
    if (r != OTA_OK) {
        ota_abort();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota finish failed");
        return ESP_FAIL;
    }

    send_json(req, "{\"success\":true,\"message\":\"firmware updated, rebooting\"}");
    return ESP_OK;
}

/* ---- CSV export helper ---- */

static esp_err_t export_csv(httpd_req_t *req, const char *path, const char *download_name) {
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "attachment; filename=\"%s\"", download_name);
    httpd_resp_set_hdr(req, "Content-Disposition", hdr);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t api_export_images_handler(httpd_req_t *req) {
    return export_csv(req, IMAGE_LIB_CSV_PATH, "images.csv");
}

static esp_err_t api_export_quotes_handler(httpd_req_t *req) {
    return export_csv(req, QUOTE_LIB_CSV_PATH, "quotes.csv");
}

/* ---- GET /api/daily ---- */

static esp_err_t api_daily_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");

    FILE *f = fopen(DAILY_CSV_PATH, "r");
    if (f) {
        char line[96];
        while (fgets(line, sizeof(line), f)) {
            char *d = strtok(line, ",\r\n");
            char *iid = strtok(NULL, ",\r\n");
            char *qid = strtok(NULL, ",\r\n");
            if (d && iid && qid && strcmp(d, "date") != 0) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "date", d);
                cJSON_AddNumberToObject(item, "image_id", (double)strtoul(iid, NULL, 10));
                cJSON_AddNumberToObject(item, "quote_id", (double)strtoul(qid, NULL, 10));
                cJSON_AddItemToArray(arr, item);
            }
        }
        fclose(f);
    }

    cJSON_AddBoolToObject(root, "success", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- Upload (stream raw PNG body to SD, then import into the library) ---- */

static esp_err_t api_upload_handler(httpd_req_t *req) {
    /* Raw PNG body streamed to a fixed temp file, then imported into the
       image library (which hashes, dedupes, assigns the id, and copies into
       images/NNN/NNNNNNNN.png).  We do NOT trust a user-supplied filename. */
    const char *tmp_path = "/sdcard/.upload.tmp";

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

    /* Stream body -> SD, enforce max size */
    static char buf[4096];  /* static — 4KB stack buffer would overflow httpd task */
    int total = 0;
    int received;
    while ((received = httpd_req_recv(req, buf, sizeof(buf))) > 0) {
        total += received;
        if (total > MAX_UPLOAD_SIZE) {
            fclose(f);
            unlink(tmp_path);
            httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "too large");
            return ESP_FAIL;
        }
        fwrite(buf, 1, received, f);
    }
    fclose(f);

    uint32_t id = 0;
    esp_err_t err = image_library_add(tmp_path, &id);
    unlink(tmp_path);
    if (err != ESP_OK || id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "import failed (PNG only)");
        return ESP_FAIL;
    }

    char name[16];
    snprintf(name, sizeof(name), "%08lu.png", (unsigned long)id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddNumberToObject(root, "id", (double)id);
    cJSON_AddStringToObject(root, "name", name);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- DELETE /api/images/<NNNNNNNN.png> ---- */

static esp_err_t api_delete_handler(httpd_req_t *req) {
    /* req->uri = /api/images/NNNNNNNN.png */
    const char *name = req->uri + strlen("/api/images/");
    uint32_t id = image_library_id_from_name(name);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
        return ESP_FAIL;
    }

    if (image_library_remove(id) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }

    send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- Display / mode requests (flag only) ---- */

static esp_err_t api_display_image_handler(httpd_req_t *req) {
    char body[256] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r > 0) {
        body[r] = '\0';
        cJSON *j = cJSON_Parse(body);
        if (j) {
            cJSON *fn = cJSON_GetObjectItem(j, "filename");
            if (cJSON_IsString(fn) && fn->valuestring &&
                image_library_id_from_name(fn->valuestring) != 0) {
                strncpy(s_req_filename, fn->valuestring, sizeof(s_req_filename) - 1);
                s_request = ADMIN_REQ_DISPLAY_FILE;
            }
            cJSON_Delete(j);
        }
    }
    if (s_request != ADMIN_REQ_DISPLAY_FILE) {
        send_json(req, "{\"success\":false,\"message\":\"bad request\"}");
        return ESP_FAIL;
    }
    send_json(req, "{\"success\":true,\"message\":\"display task started\"}");
    return ESP_OK;
}

static esp_err_t api_display_nav_handler(httpd_req_t *req) {
    if (strcmp(req->uri, "/api/display/next") == 0) s_request = ADMIN_REQ_NEXT;
    else if (strcmp(req->uri, "/api/display/previous") == 0) s_request = ADMIN_REQ_PREV;
    else if (strcmp(req->uri, "/api/display/refresh") == 0) s_request = ADMIN_REQ_REFRESH;
    else if (strcmp(req->uri, "/api/display/random") == 0) s_request = ADMIN_REQ_NEXT; /* "next" is random */
    send_json(req, "{\"success\":true,\"message\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t api_mode_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode",
        s_cur_mode == THINKINK_MODE_AI ? "ai" : "sd");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t api_mode_post_handler(httpd_req_t *req) {
    char body[128] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r > 0) {
        body[r] = '\0';
        cJSON *j = cJSON_Parse(body);
        if (j) {
            cJSON *m = cJSON_GetObjectItem(j, "mode");
            if (cJSON_IsString(m) && m->valuestring) {
                if (strcmp(m->valuestring, "ai") == 0) s_request = ADMIN_REQ_MODE_AI;
                else if (strcmp(m->valuestring, "sd") == 0) s_request = ADMIN_REQ_MODE_SD;
            }
            cJSON_Delete(j);
        }
    }
    send_json(req, "{\"success\":true,\"message\":\"ok\"}");
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

    httpd_resp_set_type(req, content_type_for(path));
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
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = 8;
    cfg.stack_size = 16384;           /* upload handler has a 4KB body buffer */
    cfg.uri_match_fn = httpd_uri_match_wildcard;   /* enable wildcard routes */

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Register specific routes first; the catch-all static handler is last
       so the more-specific /api/ and /images/ routes win. */
    httpd_uri_t uri_api_status = { .uri="/api/status", .method=HTTP_GET, .handler=api_status_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_images = { .uri="/api/images", .method=HTTP_GET, .handler=api_images_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_quotes = { .uri="/api/quotes", .method=HTTP_GET, .handler=api_quotes_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_quotes_post = { .uri="/api/quotes", .method=HTTP_POST, .handler=api_quotes_post_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_current = { .uri="/api/current", .method=HTTP_GET, .handler=api_current_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_stats = { .uri="/api/stats", .method=HTTP_GET, .handler=api_stats_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_quote_edit = { .uri="/api/quotes/edit", .method=HTTP_POST, .handler=api_quote_edit_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_quotes_import = { .uri="/api/quotes/import", .method=HTTP_POST, .handler=api_quotes_import_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_quote_delete = { .uri="/api/quotes/*", .method=HTTP_DELETE, .handler=api_quote_delete_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_export_images = { .uri="/api/export/images.csv", .method=HTTP_GET, .handler=api_export_images_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_export_quotes = { .uri="/api/export/quotes.csv", .method=HTTP_GET, .handler=api_export_quotes_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_daily = { .uri="/api/daily", .method=HTTP_GET, .handler=api_daily_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_random = { .uri="/api/display/random", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_www_upload = { .uri="/api/www/upload", .method=HTTP_POST, .handler=api_www_upload_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_logs = { .uri="/api/logs", .method=HTTP_GET, .handler=api_logs_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_ota_status = { .uri="/api/ota/status", .method=HTTP_GET, .handler=api_ota_status_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_ota_upload = { .uri="/api/ota", .method=HTTP_POST, .handler=api_ota_upload_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_files = { .uri="/api/files", .method=HTTP_GET, .handler=api_files_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_file_download = { .uri="/api/files/download", .method=HTTP_GET, .handler=api_file_download_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_file_delete = { .uri="/api/files/*", .method=HTTP_DELETE, .handler=api_file_delete_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_file_upload = { .uri="/api/files/upload", .method=HTTP_POST, .handler=api_file_upload_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_upload = { .uri="/api/images/upload", .method=HTTP_POST, .handler=api_upload_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_delete = { .uri="/api/images/*", .method=HTTP_DELETE, .handler=api_delete_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_display_image = { .uri="/api/display/image", .method=HTTP_POST, .handler=api_display_image_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_next = { .uri="/api/display/next", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_prev = { .uri="/api/display/previous", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_refresh = { .uri="/api/display/refresh", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_mode_get = { .uri="/api/mode", .method=HTTP_GET, .handler=api_mode_get_handler, .user_ctx=NULL };
    httpd_uri_t uri_api_mode_post = { .uri="/api/mode", .method=HTTP_POST, .handler=api_mode_post_handler, .user_ctx=NULL };
    httpd_uri_t uri_images = { .uri="/images/*", .method=HTTP_GET, .handler=image_file_handler, .user_ctx=NULL };
    httpd_uri_t uri_static = { .uri="/*", .method=HTTP_GET, .handler=static_handler, .user_ctx=NULL };

    httpd_register_uri_handler(s_server, &uri_api_status);
    httpd_register_uri_handler(s_server, &uri_api_images);
    httpd_register_uri_handler(s_server, &uri_api_quotes);
    httpd_register_uri_handler(s_server, &uri_api_quotes_post);
    httpd_register_uri_handler(s_server, &uri_api_current);
    httpd_register_uri_handler(s_server, &uri_api_stats);
    httpd_register_uri_handler(s_server, &uri_api_quote_edit);
    httpd_register_uri_handler(s_server, &uri_api_quotes_import);
    httpd_register_uri_handler(s_server, &uri_api_quote_delete);
    httpd_register_uri_handler(s_server, &uri_api_export_images);
    httpd_register_uri_handler(s_server, &uri_api_export_quotes);
    httpd_register_uri_handler(s_server, &uri_api_daily);
    httpd_register_uri_handler(s_server, &uri_api_random);
    httpd_register_uri_handler(s_server, &uri_api_www_upload);
    httpd_register_uri_handler(s_server, &uri_api_logs);
    httpd_register_uri_handler(s_server, &uri_api_ota_status);
    httpd_register_uri_handler(s_server, &uri_api_ota_upload);
    httpd_register_uri_handler(s_server, &uri_api_files);
    httpd_register_uri_handler(s_server, &uri_api_file_download);
    httpd_register_uri_handler(s_server, &uri_api_file_delete);
    httpd_register_uri_handler(s_server, &uri_api_file_upload);
    httpd_register_uri_handler(s_server, &uri_api_upload);
    httpd_register_uri_handler(s_server, &uri_api_delete);
    httpd_register_uri_handler(s_server, &uri_api_display_image);
    httpd_register_uri_handler(s_server, &uri_api_next);
    httpd_register_uri_handler(s_server, &uri_api_prev);
    httpd_register_uri_handler(s_server, &uri_api_refresh);
    httpd_register_uri_handler(s_server, &uri_api_mode_get);
    httpd_register_uri_handler(s_server, &uri_api_mode_post);
    httpd_register_uri_handler(s_server, &uri_images);
    httpd_register_uri_handler(s_server, &uri_static);

    /* Log the actual STA IP so the user can fall back if mDNS is blocked */
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
