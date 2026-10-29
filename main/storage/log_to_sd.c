/*
 * log_to_sd.c — mirror esp_log output to /sdcard/log.txt.
 *
 * Uses POSIX open(O_APPEND)/write rather than stdio fopen("a"), because
 * FATFS's stdio append mode did not actually flush writes (see debugging:
 * fopen("a") fwrite+fflush left the file size unchanged, while fopen("w")
 * worked).  O_APPEND is the reliable append path on this stack.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "esp_log.h"

#include "log_to_sd.h"

static const char *TAG = "log_sd";

static int s_log_fd = -1;
static vprintf_like_t s_orig_vprintf = NULL;

static int log_vprintf_hook(const char *fmt, va_list args) {
    /* Copy args FIRST — the serial vprintf below consumes the original. */
    va_list args_copy;
    va_copy(args_copy, args);

    /* 1. keep the normal serial output */
    int ret = 0;
    if (s_orig_vprintf) {
        ret = s_orig_vprintf(fmt, args);
    }

    /* 2. mirror the same line to the SD file, prefixed with a wall-clock
       timestamp (once the RTC has been SNTP-synced). */
    if (s_log_fd >= 0) {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) == 0 && tv.tv_sec > 946684800L) {
            struct tm tmv;
            localtime_r(&tv.tv_sec, &tmv);
            char ts[40];
            int tsl = snprintf(ts, sizeof(ts), "[%04d-%02d-%02d %02d:%02d:%02d] ",
                               tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                               tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            if (tsl > 0) write(s_log_fd, ts, (size_t)tsl);
        }

        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, args_copy);
        if (n > 0) {
            size_t to_write = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
            write(s_log_fd, buf, to_write);
            fsync(s_log_fd);   /* flush to card — survives esp_restart() */
        }
    }
    va_end(args_copy);
    return ret;
}

esp_err_t log_to_sd_init(void) {
    /* Cap the file: if it grew past the limit, start fresh. */
    bool truncate = false;
    struct stat st;
    if (stat(LOG_TO_SD_PATH, &st) == 0 && st.st_size > LOG_TO_SD_MAX) {
        truncate = true;
    }

    s_log_fd = open(LOG_TO_SD_PATH,
                    O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0666);
    if (s_log_fd < 0) {
        ESP_LOGW(TAG, "cannot open %s", LOG_TO_SD_PATH);
        return ESP_FAIL;
    }

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf_hook);
    ESP_LOGI(TAG, "logging to %s", LOG_TO_SD_PATH);
    return ESP_OK;
}

int log_to_sd_tail(char *buf, size_t buf_len, size_t max_bytes) {
    if (!buf || buf_len == 0) return -1;
    buf[0] = '\0';

    FILE *f = fopen(LOG_TO_SD_PATH, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return 0;
    }

    size_t want = (size_t)size > max_bytes ? max_bytes : (size_t)size;
    if (want > buf_len - 1) want = buf_len - 1;

    fseek(f, size - (long)want, SEEK_SET);
    size_t n = fread(buf, 1, want, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

esp_err_t log_to_sd_clear(void) {
    /* Close the append fd and reopen with O_TRUNC so the file starts empty.
       (unlink() would leave the open fd writing to a deleted inode on FATFS.) */
    if (s_log_fd >= 0) {
        close(s_log_fd);
        s_log_fd = -1;
    }

    s_log_fd = open(LOG_TO_SD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (s_log_fd < 0) {
        ESP_LOGW(TAG, "cannot reopen %s for clear", LOG_TO_SD_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "log cleared");
    return ESP_OK;
}
