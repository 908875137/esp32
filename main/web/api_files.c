/*
 * api_files.c
 *
 * File / system REST handlers: directory browser (/api/files*), www upload,
 * logs, and OTA firmware upgrade.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "cJSON.h"

#include "admin_server.h"
#include "sd_card.h"
#include "log_to_sd.h"
#include "ota_manager.h"

static const char *TAG = "api_files";

#define OTA_TOKEN "thinkink"

/* ---- directory browser ---- */

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
    admin_url_decode(path_raw, path, sizeof(path));
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
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t api_file_delete_handler(httpd_req_t *req) {
    char name[320];
    admin_url_decode(req->uri + strlen("/api/files/"), name, sizeof(name));
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
    admin_send_json(req, "{\"success\":true}");
    return ESP_OK;
}

static esp_err_t api_file_download_handler(httpd_req_t *req) {
    char name_raw[64] = {0};
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "name", name_raw, sizeof(name_raw));
    }
    char name[64] = {0};
    admin_url_decode(name_raw, name, sizeof(name));
    if (name[0] == '\0' || strstr(name, "..") || name[0] == '/' || strchr(name, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_FAIL;
    }

    char path[320];
    snprintf(path, sizeof(path), "%s/%s", SD_MOUNT_POINT, name);
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_404(req); return ESP_FAIL; }

    httpd_resp_set_type(req, admin_content_type(path));
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
    admin_url_decode(filename_raw, filename, sizeof(filename));
    if (filename[0] == '\0' || strstr(filename, "..") || strchr(filename, '/') || strchr(filename, '\\')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid filename");
        return ESP_FAIL;
    }

    char dir[128] = {0};
    admin_url_decode(path_raw, dir, sizeof(dir));
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
    admin_send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- POST /api/www/upload?filename=xxx  (overwrite a file in /sdcard/www) ---- */

static esp_err_t api_www_upload_handler(httpd_req_t *req) {
    char filename_raw[64] = {0};
    char qbuf[128];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        httpd_query_key_value(qbuf, "filename", filename_raw, sizeof(filename_raw));
    }
    char filename[64] = {0};
    admin_url_decode(filename_raw, filename, sizeof(filename));
    if (filename[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "filename required");
        return ESP_FAIL;
    }
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
    admin_send_json(req, "{\"success\":true}");
    return ESP_OK;
}

/* ---- GET /api/logs ---- */

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

/* ---- OTA firmware upgrade ---- */

static esp_err_t api_ota_status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", ota_get_version());
    cJSON_AddStringToObject(root, "project_name", ota_get_project_name());
    cJSON_AddBoolToObject(root, "success", true);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

static esp_err_t api_ota_upload_handler(httpd_req_t *req) {
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

    static char buf[4096];
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

    admin_send_json(req, "{\"success\":true,\"message\":\"firmware updated, rebooting\"}");
    return ESP_OK;
}

/* ---- registration ---- */

void api_files_register(httpd_handle_t server) {
    httpd_uri_t u;

    u = (httpd_uri_t){ .uri="/api/www/upload", .method=HTTP_POST, .handler=api_www_upload_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/logs", .method=HTTP_GET, .handler=api_logs_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/ota/status", .method=HTTP_GET, .handler=api_ota_status_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/ota", .method=HTTP_POST, .handler=api_ota_upload_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/files", .method=HTTP_GET, .handler=api_files_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/files/download", .method=HTTP_GET, .handler=api_file_download_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/files/*", .method=HTTP_DELETE, .handler=api_file_delete_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/files/upload", .method=HTTP_POST, .handler=api_file_upload_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    ESP_LOGI(TAG, "file/system routes registered");
}
