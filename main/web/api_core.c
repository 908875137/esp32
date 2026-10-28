/*
 * api_core.c
 *
 * Status / control REST handlers: /api/status, /api/current, /api/stats,
 * /api/daily, /api/display/…, /api/mode.
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "cJSON.h"

#include "admin_server.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "image_library.h"
#include "quote_library.h"
#include "daily.h"
#include "current.h"
#include "ota_manager.h"

static const char *TAG = "api_core";

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
    cJSON_AddStringToObject(root, "mode", admin_get_mode() == THINKINK_MODE_AI ? "ai" : "sd");
    cJSON_AddNumberToObject(root, "image_count", sd_card_count_images());
    cJSON_AddNumberToObject(root, "sd_total", (double)sd_total);
    cJSON_AddNumberToObject(root, "sd_free", (double)sd_free);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- GET /api/current ---- */

static esp_err_t api_current_handler(httpd_req_t *req) {
    current_content_t cur;
    if (current_load(&cur) != ESP_OK) {
        admin_send_json(req, "{\"success\":true,\"has_content\":false}");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddBoolToObject(root, "has_content", true);
    cJSON_AddStringToObject(root, "mode", cur.mode == THINKINK_MODE_AI ? "ai" : "sd");
    cJSON_AddStringToObject(root, "quote_text", cur.quote_text);
    cJSON_AddStringToObject(root, "quote_author", cur.quote_author);
    cJSON_AddStringToObject(root, "quote_source", cur.quote_source);

    if (cur.mode == THINKINK_MODE_SD && cur.image_id != 0) {
        char name[16];
        snprintf(name, sizeof(name), "%08lu.png", (unsigned long)cur.image_id);
        uint32_t used = 0;
        image_library_get_used_count(cur.image_id, &used);
        cJSON_AddNumberToObject(root, "image_id", (double)cur.image_id);
        cJSON_AddStringToObject(root, "image_name", name);
        cJSON_AddNumberToObject(root, "image_used", (double)used);

        if (cur.quote_id != 0) {
            cJSON_AddNumberToObject(root, "quote_id", (double)cur.quote_id);
            quote_t q;
            if (quote_library_get_by_id(cur.quote_id, &q) == ESP_OK) {
                cJSON_AddNumberToObject(root, "quote_used", (double)q.used_count);
            }
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    admin_send_json(req, json ? json : "{}");
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
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
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
    admin_send_json(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

/* ---- Display / mode requests (flag only) ---- */

static esp_err_t api_display_image_handler(httpd_req_t *req) {
    bool ok = false;
    char body[256] = {0};
    int r = httpd_req_recv(req, body, sizeof(body) - 1);
    if (r > 0) {
        body[r] = '\0';
        cJSON *j = cJSON_Parse(body);
        if (j) {
            cJSON *fn = cJSON_GetObjectItem(j, "filename");
            if (cJSON_IsString(fn) && fn->valuestring &&
                image_library_id_from_name(fn->valuestring) != 0) {
                admin_set_request_filename(fn->valuestring);
                admin_set_request(ADMIN_REQ_DISPLAY_FILE);
                ok = true;
            }
            cJSON_Delete(j);
        }
    }
    if (!ok) {
        admin_send_json(req, "{\"success\":false,\"message\":\"bad request\"}");
        return ESP_FAIL;
    }
    admin_send_json(req, "{\"success\":true,\"message\":\"display task started\"}");
    return ESP_OK;
}

static esp_err_t api_display_nav_handler(httpd_req_t *req) {
    if (strcmp(req->uri, "/api/display/next") == 0) admin_set_request(ADMIN_REQ_NEXT);
    else if (strcmp(req->uri, "/api/display/previous") == 0) admin_set_request(ADMIN_REQ_PREV);
    else if (strcmp(req->uri, "/api/display/refresh") == 0) admin_set_request(ADMIN_REQ_REFRESH);
    else if (strcmp(req->uri, "/api/display/random") == 0) admin_set_request(ADMIN_REQ_NEXT); /* "next" is random */
    else if (strcmp(req->uri, "/api/display/image_only") == 0) admin_set_request(ADMIN_REQ_IMAGE_ONLY);
    else if (strcmp(req->uri, "/api/display/delete_image") == 0) admin_set_request(ADMIN_REQ_DELETE_IMAGE);
    admin_send_json(req, "{\"success\":true,\"message\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t api_mode_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode",
        admin_get_mode() == THINKINK_MODE_AI ? "ai" : "sd");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    admin_send_json(req, json ? json : "{}");
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
                if (strcmp(m->valuestring, "ai") == 0) admin_set_request(ADMIN_REQ_MODE_AI);
                else if (strcmp(m->valuestring, "sd") == 0) admin_set_request(ADMIN_REQ_MODE_SD);
            }
            cJSON_Delete(j);
        }
    }
    admin_send_json(req, "{\"success\":true,\"message\":\"ok\"}");
    return ESP_OK;
}

/* ---- registration ---- */

void api_core_register(httpd_handle_t server) {
    httpd_uri_t u;

    u = (httpd_uri_t){ .uri="/api/status", .method=HTTP_GET, .handler=api_status_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/current", .method=HTTP_GET, .handler=api_current_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/stats", .method=HTTP_GET, .handler=api_stats_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/daily", .method=HTTP_GET, .handler=api_daily_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/image", .method=HTTP_POST, .handler=api_display_image_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/next", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/previous", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/refresh", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/random", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/image_only", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/display/delete_image", .method=HTTP_POST, .handler=api_display_nav_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/mode", .method=HTTP_GET, .handler=api_mode_get_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    u = (httpd_uri_t){ .uri="/api/mode", .method=HTTP_POST, .handler=api_mode_post_handler, .user_ctx=NULL };
    httpd_register_uri_handler(server, &u);

    ESP_LOGI(TAG, "core routes registered");
}
