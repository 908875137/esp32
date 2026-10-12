/*
 * api_content.c
 *
 * Content REST handlers: images (list/upload/delete), quotes
 * (list/add/edit/delete/import), and CSV export.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "cJSON.h"

#include "admin_server.h"
#include "image_library.h"
#include "quote_library.h"

static const char *TAG = "api_content";

#define MAX_UPLOAD_SIZE         (500 * 1024)
#define MAX_QUOTES_IMPORT_SIZE  (1024 * 1024)

/* ---- GET /api/images?offset=N&limit=M ---- */

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
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- POST /api/images/upload (raw PNG body) ---- */

static esp_err_t api_upload_handler(httpd_req_t *req) {
    const char *tmp_path = "/sdcard/.upload.tmp";

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "open failed");
        return ESP_FAIL;
    }

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
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- DELETE /api/images/<NNNNNNNN.png> ---- */

static esp_err_t api_delete_handler(httpd_req_t *req) {
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

    admin_send_json(req, "{\"success\":true}");
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
    admin_send_json(req, json ? json : "{}");
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
    admin_send_json(req, json ? json : "{}");
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
    admin_send_json(req, "{\"success\":true}");
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
    admin_send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- POST /api/quotes/import  (raw CSV body, overwrites quotes.csv) ---- */

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

    unlink(QUOTE_LIB_CSV_PATH);
    if (rename(tmp_path, QUOTE_LIB_CSV_PATH) != 0) {
        ESP_LOGE(TAG, "quotes import: rename failed");
        unlink(tmp_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "import failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "quotes imported: %d bytes", total);
    admin_send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- CSV export ---- */

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

/* ---- registration ---- */

void api_content_register(httpd_handle_t server) {
    httpd_uri_t u;

    u = (httpd_uri_t){ .uri="/api/images", .method=HTTP_GET, .handler=api_images_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/images/upload", .method=HTTP_POST, .handler=api_upload_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/images/*", .method=HTTP_DELETE, .handler=api_delete_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/quotes", .method=HTTP_GET, .handler=api_quotes_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/quotes", .method=HTTP_POST, .handler=api_quotes_post_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/quotes/edit", .method=HTTP_POST, .handler=api_quote_edit_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/quotes/import", .method=HTTP_POST, .handler=api_quotes_import_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/quotes/*", .method=HTTP_DELETE, .handler=api_quote_delete_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/export/images.csv", .method=HTTP_GET, .handler=api_export_images_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/export/quotes.csv", .method=HTTP_GET, .handler=api_export_quotes_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    ESP_LOGI(TAG, "content routes registered");
}
