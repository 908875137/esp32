/*
 * current.h
 *
 * Persist the "currently displayed" content (image + quote) to
 * /sdcard/current.json, so the Web UI and the "image-only" / "delete image"
 * actions can know what is showing in BOTH AI and SD modes.
 *
 * AI mode stores the generated quote text/author/source (image_id stays 0 —
 * the AI image is a separate /sdcard/current_ai.png, not part of the library).
 * SD mode stores the library image_id + quote_id (and their text for display).
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "config_store.h"   /* thinkink_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

#define CURRENT_JSON_PATH   "/sdcard/current.json"

typedef struct {
    thinkink_mode_t mode;
    char quote_text[512];
    char quote_author[128];
    char quote_source[256];
    uint32_t quote_id;      /* SD mode only (AI = 0) */
    uint32_t image_id;      /* SD mode only (AI = 0) */
} current_content_t;

/**
 * Write the current content to /sdcard/current.json (truncate + fsync).
 */
esp_err_t current_save(const current_content_t *c);

/**
 * Load the current content.  ESP_ERR_NOT_FOUND if the file is absent.
 * Zeroes `c` first.
 */
esp_err_t current_load(current_content_t *c);

#ifdef __cplusplus
}
#endif
