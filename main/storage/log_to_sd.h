/*
 * log_to_sd.h
 *
 * Mirror esp_log output to a file on the SD card (/sdcard/log.txt) while
 * keeping the normal serial output.  Lets the user pull the card and read
 * the log on a computer to diagnose behaviour when running on battery
 * (no USB serial attached).
 *
 * The file is capped (rotated by truncation) so it cannot fill the card.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_TO_SD_PATH    "/sdcard/log.txt"
#define LOG_TO_SD_MAX      (512 * 1024)   /* cap log file at 512 KB */

/**
 * Open /sdcard/log.txt and install the vprintf hook so all esp_log output is
 * also written to the file.  Call AFTER sd_card_init() (SD must be mounted).
 * Serial output is preserved.
 */
esp_err_t log_to_sd_init(void);

/**
 * Read up to `buf_len-1` bytes from the END of the log file (the most recent
 * log), NUL-terminated.  Returns the number of bytes copied (>= 0), or -1 on
 * error.  Reads at most the last `max_bytes` of the file.
 */
int log_to_sd_tail(char *buf, size_t buf_len, size_t max_bytes);

#ifdef __cplusplus
}
#endif
