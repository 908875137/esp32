/*
 * sd_card.h
 *
 * Persistent SD card (SPI mode) module.
 *
 * Wiring (from HARDWARE.md):
 *   ESP32-S3  GPIO19 = MISO
 *   ESP32-S3  GPIO20 = CLK
 *   ESP32-S3  GPIO21 = MOSI
 *   ESP32-S3  GPIO47 = CS
 *
 * Uses SPI3_HOST (HSPI) — SPI2_HOST (FSPI) is already taken by the EPD.
 *
 * Once mounted, the card stays mounted for the whole run so that
 * FreeType (font) and the image loader can keep reading from it.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount point exposed to the rest of the app */
#define SD_MOUNT_POINT   "/sdcard"
#define SD_IMAGES_DIR    "/sdcard/images"

/**
 * Mount the SD card on /sdcard and keep it mounted.
 * Returns ESP_OK on success.  On failure the app may continue without SD.
 */
esp_err_t sd_card_init(void);

/**
 * Return true if the SD card is currently mounted.
 */
bool sd_card_is_mounted(void);

/**
 * Count PNG images (*.png) in the images directory.
 * Returns -1 on error (e.g. dir missing or SD unmounted).
 */
int sd_card_count_images(void);

/**
 * Copy the full path of the n-th PNG image (0-based, ordered by name)
 * into `path_out` (size `path_len`).  Returns ESP_OK on success,
 * ESP_ERR_NOT_FOUND if n is out of range.
 */
esp_err_t sd_card_get_image_path(int index, char *path_out, size_t path_len);

#ifdef __cplusplus
}
#endif
