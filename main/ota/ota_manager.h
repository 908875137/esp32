/*
 * ota_manager.h
 *
 * Firmware OTA upgrade via the native ESP-IDF esp_ota_ops API.
 *
 * The Web upload handler streams the .bin straight into the non-running OTA
 * partition (factory is never overwritten).  The image header is checked
 * (project name must match) before the first write, and the image is
 * validated by esp_ota_end before the boot partition is switched.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_OK = 0,
    OTA_ERR_BUSY,          /* an OTA is already in progress */
    OTA_ERR_NO_PARTITION,  /* no OTA partition found (bad partition table) */
    OTA_ERR_BEGIN,
    OTA_ERR_IMAGE,         /* image is not for this project (header mismatch) */
    OTA_ERR_WRITE,
    OTA_ERR_END,
} ota_result_t;

/**
 * Start an OTA: pick the non-running OTA partition and begin writing.
 */
ota_result_t ota_begin(void);

/**
 * Write one chunk.  The first call(s) also verify the image header (project
 * name must match "epd-uc8179").
 */
ota_result_t ota_write(const uint8_t *data, size_t len);

/**
 * Finish the OTA: validate the written image and switch the boot partition.
 * On success a restart is scheduled (after ~1s, so the HTTP response flushes).
 */
ota_result_t ota_finish(void);

/**
 * Abort a failed OTA (discard the partial write).  Safe to call any time.
 */
void ota_abort(void);

/**
 * Running firmware version (from the app image description), e.g. "1.0.0".
 * Points to static storage.
 */
const char *ota_get_version(void);

/**
 * Running firmware project name (e.g. "epd-uc8179").  Static storage.
 */
const char *ota_get_project_name(void);

/**
 * Mark the running firmware as valid (cancel bootloader rollback).  Call once
 * after a successful boot self-test (SD/config/WiFi/display init all passed).
 * Safe to call on every boot — it is a no-op when not in the "pending verify"
 * state (e.g. running from factory).
 */
void ota_mark_app_valid(void);

#ifdef __cplusplus
}
#endif
