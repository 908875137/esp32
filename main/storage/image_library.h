/*
 * image_library.h
 *
 * Offline SD-card image library (phase 1).
 *
 * Layout on the SD card:
 *
 *   /sdcard/images/
 *       000/00000001.png
 *       000/00000002.png
 *       ...
 *       000/00001000.png
 *       001/00001001.png
 *       ...
 *   /sdcard/images.csv          (id,hash,used_count,last_used_at)
 *
 * Every image gets a global, monotonic 8-digit id.  Directory = (id-1)/1000,
 * so each folder holds up to 1000 images.  The CSV holds the metadata and is
 * read/written one line at a time (never loaded whole into RAM).
 *
 * Phase 1 scope: image-library basics only.  No quotes, no SQLite, no
 * networking, no semantic matching, no complex history.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_LIB_ROOT       "/sdcard/images"
#define IMAGE_LIB_CSV_PATH   "/sdcard/images.csv"
#define IMAGE_LIB_PER_DIR    1000
#define IMAGE_LIB_ID_DIGITS  8

/**
 * Prepare the library: ensure /sdcard/images exists and images.csv has a
 * header row.  Call after sd_card_init().  Safe to call repeatedly.
 */
esp_err_t image_library_init(void);

/**
 * Compute the full filesystem path for an image id:
 *   id=1    -> /sdcard/images/000/00000001.png
 *   id=1001 -> /sdcard/images/001/00001001.png
 * Returns ESP_ERR_INVALID_ARG for id==0 or bad args.
 */
esp_err_t image_library_get_path(uint32_t id, char *path, size_t path_len);

/**
 * Recover the next free id by scanning the images/ directory tree and taking
 * max(id)+1 (1 for an empty library).  Does not depend on images.csv, so the
 * id sequence survives even if the CSV is lost.
 */
esp_err_t image_library_get_next_id(uint32_t *id_out);

/**
 * Number of images currently in the library (phase-1 enumeration is capped
 * at 256, matching the existing Web UI / SD-mode enumeration limit).
 */
esp_err_t image_library_count(uint32_t *count_out);

/**
 * Id of the index-th image (0-based, ordered by id).  ESP_ERR_NOT_FOUND if
 * out of range.
 */
esp_err_t image_library_get_id_by_index(uint32_t index, uint32_t *id_out);

/**
 * Randomly chosen image id.  ESP_ERR_NOT_FOUND if the library is empty.
 */
esp_err_t image_library_get_random_id(uint32_t *id_out);

/**
 * Stream SHA-256 of a file into a 64-char lowercase hex string (caller
 * supplies a buffer of >= 65 bytes).  Reads the file in 4KB chunks so the
 * whole file is never held in RAM.
 */
esp_err_t image_library_hash_file(const char *path, char *hex_out, size_t hex_len);

/**
 * Import an image into the library:
 *   1. verify the source exists and is a PNG
 *   2. compute its SHA-256
 *   3. if the hash already exists in images.csv -> return that id (no copy)
 *   4. otherwise allocate the next id, mkdir images/NNN, copy the file to
 *      images/NNN/NNNNNNNN.png, and append a row to images.csv
 * On success *id_out is the (new or existing) id.
 */
esp_err_t image_library_add(const char *src_path, uint32_t *id_out);

/**
 * Return the id whose hash matches, or ESP_ERR_NOT_FOUND.  Reads images.csv
 * line by line.
 */
esp_err_t image_library_find_by_hash(const char *hash, uint32_t *id_out);

/**
 * Record that an image was actually shown: used_count+1 and last_used_at
 * updated (left empty when no trustworthy wall-clock time is available).
 * Rewrites images.csv via a tmp file + rename to survive power loss.
 */
esp_err_t image_library_mark_used(uint32_t id);

/**
 * Read an image's used_count from images.csv (0 for a missing row).
 */
esp_err_t image_library_get_used_count(uint32_t id, uint32_t *used_count_out);

/**
 * Library statistics, read in one streaming pass over images.csv.
 */
typedef struct {
    uint32_t count;
    uint32_t total_used;
    uint32_t max_used_id;
    uint32_t max_used;
    uint32_t min_used_id;
    uint32_t min_used;
} image_library_stats_t;

esp_err_t image_library_get_stats(image_library_stats_t *out);

/**
 * Remove an image: delete the file and its images.csv row.
 */
esp_err_t image_library_remove(uint32_t id);

/**
 * Parse an image-library filename ("00000001.png", or bare "00000001")
 * into its id.  Returns 0 if the name is not a valid 8-digit id.
 */
uint32_t image_library_id_from_name(const char *name);

#ifdef __cplusplus
}
#endif
