/*
 * daily.h
 *
 * Per-day "what is shown today" record, kept in /sdcard/daily.csv:
 *
 *   date,image_id,quote_id
 *   2026-09-18,00000125,00000432
 *
 * On startup the app looks up today's date; if present it reuses that
 * image+quote pair so a reboot on the same day shows the same content.
 * Otherwise it picks a new pair and appends a row.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DAILY_CSV_PATH   "/sdcard/daily.csv"

/**
 * Look up `date` (format "YYYY-MM-DD").  On success fills *image_id and
 * *quote_id.  Returns ESP_ERR_NOT_FOUND if today has no entry.
 */
esp_err_t daily_get(const char *date, uint32_t *image_id, uint32_t *quote_id);

/**
 * Read the last (most recent) row of daily.csv into date/image_id/quote_id.
 * ESP_ERR_NOT_FOUND if the file is missing or has no data rows.
 */
esp_err_t daily_get_last(char *date_out, size_t date_len, uint32_t *image_id, uint32_t *quote_id);

/**
 * Record today's pair.  No-op if the date already has an entry.  Appends a
 * row otherwise.
 */
esp_err_t daily_set(const char *date, uint32_t image_id, uint32_t quote_id);

#ifdef __cplusplus
}
#endif
