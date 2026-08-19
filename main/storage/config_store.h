/*
 * config_store.h
 *
 * Device configuration: mode (AI/SD) persisted in NVS, with optional
 * overrides read from /sdcard/config.json.
 *
 * config.json fields (all optional):
 *   mode         "ai" | "sd"
 *   image_mode   "sequential" | "random"   (reserved)
 *   image_dir    "/sdcard/images"
 *   font         "/sdcard/fonts/NotoSansSC-Regular.ttf"
 *   quote_enabled true | false              (reserved)
 *   refresh_hour 8                          (reserved)
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    THINKINK_MODE_AI = 0,
    THINKINK_MODE_SD = 1,
} thinkink_mode_t;

/* Font path — filled from config.json, or a sensible default */
#define CONFIG_FONT_MAX_LEN     128
#define CONFIG_IMAGE_DIR_MAX_LEN 128

typedef struct {
    thinkink_mode_t mode;
    char image_dir[CONFIG_IMAGE_DIR_MAX_LEN];
    char font[CONFIG_FONT_MAX_LEN];
} thinkink_config_t;

/**
 * Load configuration:
 *   1. read saved mode from NVS (persists across reboots)
 *   2. if SD card is mounted, read /sdcard/config.json and override
 *      mode / image_dir / font with any fields present.
 *
 * `cfg` is filled with resolved values.  Call after sd_card_init().
 */
esp_err_t config_store_load(thinkink_config_t *cfg);

/**
 * Persist the current mode to NVS.
 */
esp_err_t config_store_save_mode(thinkink_mode_t mode);

#ifdef __cplusplus
}
#endif
