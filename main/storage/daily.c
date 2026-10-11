/*
 * daily.c — per-day image+quote record.  See daily.h.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_log.h"

#include "daily.h"

static const char *TAG = "daily";

esp_err_t daily_get(const char *date, uint32_t *image_id, uint32_t *quote_id) {
    if (!date) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(DAILY_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    bool found = false;
    char line[96];
    while (fgets(line, sizeof(line), f)) {
        char *d = strtok(line, ",\r\n");
        char *iid = strtok(NULL, ",\r\n");
        char *qid = strtok(NULL, ",\r\n");
        if (d && iid && qid && strcmp(d, date) == 0) {
            if (image_id) *image_id = (uint32_t)strtoul(iid, NULL, 10);
            if (quote_id) *quote_id = (uint32_t)strtoul(qid, NULL, 10);
            found = true;
            break;
        }
    }
    fclose(f);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t daily_get_last(char *date_out, size_t date_len, uint32_t *image_id, uint32_t *quote_id) {
    FILE *f = fopen(DAILY_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[96];
    char last_d[16] = {0};
    uint32_t last_img = 0, last_q = 0;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char *d = strtok(line, ",\r\n");
        char *iid = strtok(NULL, ",\r\n");
        char *qid = strtok(NULL, ",\r\n");
        if (d && iid && qid && strcmp(d, "date") != 0) {  /* skip header */
            strncpy(last_d, d, sizeof(last_d) - 1);
            last_d[sizeof(last_d) - 1] = '\0';
            last_img = (uint32_t)strtoul(iid, NULL, 10);
            last_q = (uint32_t)strtoul(qid, NULL, 10);
            found = true;
        }
    }
    fclose(f);
    if (!found) return ESP_ERR_NOT_FOUND;

    if (date_out) { strncpy(date_out, last_d, date_len - 1); date_out[date_len - 1] = '\0'; }
    if (image_id) *image_id = last_img;
    if (quote_id) *quote_id = last_q;
    return ESP_OK;
}

esp_err_t daily_set(const char *date, uint32_t image_id, uint32_t quote_id) {
    if (!date || image_id == 0 || quote_id == 0) return ESP_ERR_INVALID_ARG;

    /* No-op if today already has a row. */
    uint32_t ei = 0, eq = 0;
    if (daily_get(date, &ei, &eq) == ESP_OK) {
        ESP_LOGI(TAG, "%s already recorded — keeping existing", date);
        return ESP_OK;
    }

    /* Ensure the header exists (also handles a fresh card). */
    struct stat st;
    if (stat(DAILY_CSV_PATH, &st) != 0 || st.st_size == 0) {
        FILE *h = fopen(DAILY_CSV_PATH, "w");
        if (h) { fputs("date,image_id,quote_id\n", h); fclose(h); }
    }

    FILE *f = fopen(DAILY_CSV_PATH, "a");
    if (!f) return ESP_FAIL;
    int r = fprintf(f, "%s,%08lu,%08lu\n",
                    date, (unsigned long)image_id, (unsigned long)quote_id);
    if (fclose(f) != 0) return ESP_FAIL;
    if (r <= 0) return ESP_FAIL;

    ESP_LOGI(TAG, "recorded %s -> image %lu + quote %lu",
             date, (unsigned long)image_id, (unsigned long)quote_id);
    return ESP_OK;
}
