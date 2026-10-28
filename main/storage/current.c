/*
 * current.c — persist the currently displayed content.  See current.h.
 *
 * Uses POSIX open(O_TRUNC)+write+fsync for the write (FATFS stdio append is
 * known to not flush — see log_to_sd.c) and a plain cJSON round-trip.
 */
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"

#include "current.h"

esp_err_t current_save(const current_content_t *c) {
    if (!c) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mode", c->mode == THINKINK_MODE_AI ? "ai" : "sd");
    cJSON_AddStringToObject(root, "quote_text", c->quote_text);
    cJSON_AddStringToObject(root, "quote_author", c->quote_author);
    cJSON_AddStringToObject(root, "quote_source", c->quote_source);
    cJSON_AddNumberToObject(root, "quote_id", (double)c->quote_id);
    cJSON_AddNumberToObject(root, "image_id", (double)c->image_id);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    int fd = open(CURRENT_JSON_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { free(json); return ESP_FAIL; }
    size_t len = strlen(json);
    ssize_t w = write(fd, json, len);
    fsync(fd);
    close(fd);
    free(json);

    return (w == (ssize_t)len) ? ESP_OK : ESP_FAIL;
}

esp_err_t current_load(current_content_t *c) {
    if (!c) return ESP_ERR_INVALID_ARG;
    memset(c, 0, sizeof(*c));

    FILE *f = fopen(CURRENT_JSON_PATH, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) { fclose(f); return ESP_FAIL; }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ESP_FAIL;

    cJSON *m = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(m) && m->valuestring)
        c->mode = strcmp(m->valuestring, "ai") == 0 ? THINKINK_MODE_AI : THINKINK_MODE_SD;

    cJSON *t = cJSON_GetObjectItem(root, "quote_text");
    if (cJSON_IsString(t) && t->valuestring) strncpy(c->quote_text, t->valuestring, sizeof(c->quote_text) - 1);
    cJSON *a = cJSON_GetObjectItem(root, "quote_author");
    if (cJSON_IsString(a) && a->valuestring) strncpy(c->quote_author, a->valuestring, sizeof(c->quote_author) - 1);
    cJSON *s = cJSON_GetObjectItem(root, "quote_source");
    if (cJSON_IsString(s) && s->valuestring) strncpy(c->quote_source, s->valuestring, sizeof(c->quote_source) - 1);
    cJSON *qi = cJSON_GetObjectItem(root, "quote_id");
    if (cJSON_IsNumber(qi)) c->quote_id = (uint32_t)qi->valuedouble;
    cJSON *ii = cJSON_GetObjectItem(root, "image_id");
    if (cJSON_IsNumber(ii)) c->image_id = (uint32_t)ii->valuedouble;

    cJSON_Delete(root);
    return ESP_OK;
}
