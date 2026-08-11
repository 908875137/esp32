/*
 * image_pipeline.c
 *
 * Step 3-5: Download PNG → Decode → Center-crop → RGB565
 *
 *   GET data[0].url (signed COS URL)
 *   → accumulate PNG bytes
 *   → lodepng_decode32 → RGBA
 *   → center-crop to 800:480 aspect
 *   → bilinear scale to 800×480
 *   → convert to RGB565
 *
 * Dependencies:
 *   - lodepng          (single-file PNG decoder)
 *   - esp_http_client  (ESP-IDF)
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#define LODEPNG_NO_COMPILE_DISK        /* no file I/O — memory only */
#define LODEPNG_NO_COMPILE_ENCODER     /* decode only */
#define LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
#include "lodepng.h"

#include "image_decoder.h"
#include "cos_trusted_certs.h"

static const char *TAG = "image";

/* ================================================================
 * Accumulator — grows by doubling to minimise realloc churn.
 * All allocations are from PSRAM (MALLOC_CAP_SPIRAM).
 *
 * When Content-Length is known ahead of time (chunked: false), the download
 * buffer is pre-allocated in one shot, avoiding any growth during the transfer.
 * ================================================================ */

#define ACCUM_CHUNK      8192
#define ACCUM_INIT_CAP  262144   /* start at 256KB — most PNGs are >128KB */

typedef struct {
    uint8_t *buf;
    int      len;
    int      cap;
} accum_t;

static void accum_init(accum_t *a) {
    a->buf = (uint8_t *)heap_caps_malloc(ACCUM_INIT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    a->cap = ACCUM_INIT_CAP;
    a->len = 0;
}

static void accum_free(accum_t *a) {
    heap_caps_free(a->buf);
    a->buf = NULL;
    a->len = 0;
    a->cap = 0;
}

static esp_err_t accum_prealloc(accum_t *a, int total_bytes) {
    if (total_bytes <= 0) return ESP_OK;
    if (total_bytes <= a->cap) return ESP_OK;   /* already large enough */
    heap_caps_free(a->buf);
    a->buf = (uint8_t *)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!a->buf) {
        ESP_LOGE(TAG, "OOM: accum prealloc %d bytes (PSRAM free: %lu largest: %lu)",
                 total_bytes,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        a->len = 0;
        a->cap = 0;
        return ESP_ERR_NO_MEM;
    }
    a->cap = total_bytes;
    a->len = 0;
    return ESP_OK;
}

static esp_err_t accum_append(accum_t *a, const uint8_t *src, int srclen) {
    if (srclen <= 0) return ESP_OK;
    int needed = a->len + srclen;
    if (needed > a->cap) {
        /* Exponential growth: double until we fit */
        int new_cap = a->cap ? a->cap : ACCUM_INIT_CAP;
        while (new_cap < needed) {
            int overflow = new_cap > (INT_MAX / 2);
            if (overflow) { new_cap = needed; break; }
            new_cap *= 2;
        }
        ESP_LOGI(TAG, "accum grow: %d → %d (PSRAM free: %lu largest: %lu)",
                 a->cap, new_cap,
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        uint8_t *p = (uint8_t *)heap_caps_realloc(a->buf, new_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) {
            ESP_LOGE(TAG, "OOM: accum realloc %d", new_cap);
            return ESP_ERR_NO_MEM;
        }
        a->buf = p;
        a->cap = new_cap;
    }
    memcpy(a->buf + a->len, src, srclen);
    a->len += srclen;
    return ESP_OK;
}

/* ---- HTTP event handler ---- */

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    accum_t *acc = (accum_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        return accum_append(acc, (const uint8_t *)evt->data, evt->data_len);
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP event error");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t image_download_and_decode(const char *url,
                                    uint8_t **rgb565_out) {
    if (!url || !rgb565_out) return ESP_ERR_INVALID_ARG;

    *rgb565_out = NULL;

    /* ---- 1. Download PNG ---- */
    ESP_LOGI(TAG, "Downloading PNG from URL (first 80 chars): %.80s...", url);

    ESP_LOGI(TAG, "DRAM free=%lu largest=%lu, PSRAM free=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* ---- 1a. HEAD request to peek Content-Length ---- */
    esp_http_client_config_t head_cfg = {
        .url               = url,
        .method            = HTTP_METHOD_HEAD,
        .timeout_ms        = 10000,
        .user_agent        = "ThinkINK-ESP32/1.0",
        .cert_pem          = COS_CA_CERT_PEM,
        .buffer_size       = 4096,
    };
    esp_http_client_handle_t head_client = esp_http_client_init(&head_cfg);
    int content_length = -1;
    if (head_client) {
        esp_err_t head_err = esp_http_client_perform(head_client);
        int head_status = esp_http_client_get_status_code(head_client);
        if (head_err == ESP_OK && head_status == 200) {
            content_length = esp_http_client_get_content_length(head_client);
            ESP_LOGI(TAG, "HEAD request: status=%d, Content-Length=%d", head_status, content_length);
        } else {
            ESP_LOGW(TAG, "HEAD request failed: err=%s status=%d", esp_err_to_name(head_err), head_status);
        }
        esp_http_client_cleanup(head_client);
    }

    accum_t acc;
    accum_init(&acc);
    if (!acc.buf) return ESP_ERR_NO_MEM;

    /* ---- 1b. Pre-allocate if Content-Length is known ---- */
    if (content_length > 0 && content_length < (8 * 1024 * 1024)) {
        esp_err_t pre_err = accum_prealloc(&acc, content_length);
        if (pre_err != ESP_OK) {
            /* Fall back to streaming growth — accum still has its initial chunk */
            ESP_LOGW(TAG, "Pre-alloc failed, will grow on-the-fly");
        }
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 300000,
        .user_agent        = "ThinkINK-ESP32/1.0",
        .cert_pem          = COS_CA_CERT_PEM,
        .event_handler     = http_event_handler,
        .user_data         = &acc,
        .buffer_size       = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        accum_free(&acc);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Download: HTTP status=%d, chunked=%d, downloaded=%d bytes",
             status,
             esp_http_client_is_chunked_response(client),
             acc.len);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        accum_free(&acc);
        return err;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Download returned HTTP %d", status);
        esp_http_client_cleanup(client);
        accum_free(&acc);
        return ESP_FAIL;
    }

    if (acc.len <= 0) {
        ESP_LOGE(TAG, "Downloaded 0 bytes");
        esp_http_client_cleanup(client);
        accum_free(&acc);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "PNG downloaded: %d bytes", acc.len);

    /* Shrink accum to actual size so lodepng has maximum contiguous PSRAM */
    {
        uint8_t *trimmed = (uint8_t *)heap_caps_realloc(acc.buf, acc.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (trimmed) { acc.buf = trimmed; acc.cap = acc.len; }
        /* If trim fails, we keep the larger buffer — still usable */
    }

    ESP_LOGI(TAG, "After PNG accum: DRAM free=%lu largest=%lu  PSRAM free=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* ---- 2. Decode PNG with lodepng ---- */

    ESP_LOGI(TAG, "Free heap before decode: DRAM=%lu largest=%lu  PSRAM=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    unsigned png_w = 0, png_h = 0;
    unsigned char *rgba = NULL;

    unsigned dec_err = lodepng_decode32(&rgba, &png_w, &png_h,
                                         acc.buf, acc.len);
    accum_free(&acc);   /* PNG bytes no longer needed */

    if (dec_err != 0) {
        ESP_LOGE(TAG, "PNG decode error %u: %s", dec_err, lodepng_error_text(dec_err));
        free(rgba);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PNG decoded: %u×%u RGBA", png_w, png_h);

    if (png_w == 0 || png_h == 0) {
        ESP_LOGE(TAG, "PNG has zero dimension");
        free(rgba);
        return ESP_FAIL;
    }

    /* ---- 3. Center-crop + bilinear scale to 800×480 RGB565 ---- */

    #define TARGET_W  800
    #define TARGET_H  480

    const float target_aspect = (float)TARGET_W / (float)TARGET_H;   /* 1.6667 */
    const float src_aspect    = (float)png_w / (float)png_h;

    int crop_w, crop_h, crop_ox, crop_oy;

    if (src_aspect > target_aspect) {
        /* Source is wider → crop left/right, keep full height */
        crop_h = (int)png_h;
        crop_w = (int)((float)png_h * target_aspect + 0.5f);
        crop_oy = 0;
        crop_ox = ((int)png_w - crop_w) / 2;
    } else {
        /* Source is taller (or equal) → crop top/bottom, keep full width */
        crop_w = (int)png_w;
        crop_h = (int)((float)png_w / target_aspect + 0.5f);
        crop_ox = 0;
        crop_oy = ((int)png_h - crop_h) / 2;
    }

    /* Clamp */
    if (crop_ox < 0) crop_ox = 0;
    if (crop_oy < 0) crop_oy = 0;
    if (crop_ox + crop_w > (int)png_w) crop_w = (int)png_w - crop_ox;
    if (crop_oy + crop_h > (int)png_h) crop_h = (int)png_h - crop_oy;

    ESP_LOGI(TAG, "Center crop: src=%u×%u → crop=(%d,%d) %d×%d → scale → %d×%d",
             png_w, png_h, crop_ox, crop_oy, crop_w, crop_h, TARGET_W, TARGET_H);

    /* ---- 4. Bilinear scale from crop region to 800×480 RGB565 ---- */

    const int out_size = TARGET_W * TARGET_H * 2;   /* 768000 bytes */
    uint8_t *out = (uint8_t *)malloc(out_size);
    if (!out) {
        ESP_LOGE(TAG, "OOM: RGB565 output buffer (%d bytes)", out_size);
        free(rgba);
        return ESP_ERR_NO_MEM;
    }

    /* Scale factors: source crop region → target dimensions */
    float sx = (float)(crop_w - 1) / (float)(TARGET_W - 1);
    float sy = (float)(crop_h - 1) / (float)(TARGET_H - 1);

    for (int dy = 0; dy < TARGET_H; dy++) {
        float fy = (float)dy * sy;
        int   iy = (int)fy;
        float fy_frac = fy - (float)iy;
        if (iy >= crop_h - 1) { iy = crop_h - 2; fy_frac = 1.0f; }
        if (iy < 0) { iy = 0; fy_frac = 0.0f; }

        for (int dx = 0; dx < TARGET_W; dx++) {
            float fx = (float)dx * sx;
            int   ix = (int)fx;
            float fx_frac = fx - (float)ix;
            if (ix >= crop_w - 1) { ix = crop_w - 2; fx_frac = 1.0f; }
            if (ix < 0) { ix = 0; fx_frac = 0.0f; }

            /* Source pixel addresses in RGBA buffer */
            int s0 = ((crop_oy + iy)     * (int)png_w + (crop_ox + ix))     * 4;   /* top-left */
            int s1 = ((crop_oy + iy)     * (int)png_w + (crop_ox + ix + 1)) * 4;   /* top-right */
            int s2 = ((crop_oy + iy + 1) * (int)png_w + (crop_ox + ix))     * 4;   /* bottom-left */
            int s3 = ((crop_oy + iy + 1) * (int)png_w + (crop_ox + ix + 1)) * 4;   /* bottom-right */

            /* Bilinear interpolation for each channel */
            float r = (1.0f - fy_frac) * ((1.0f - fx_frac) * (float)rgba[s0 + 0] + fx_frac * (float)rgba[s1 + 0])
                   +           fy_frac  * ((1.0f - fx_frac) * (float)rgba[s2 + 0] + fx_frac * (float)rgba[s3 + 0]);

            float g = (1.0f - fy_frac) * ((1.0f - fx_frac) * (float)rgba[s0 + 1] + fx_frac * (float)rgba[s1 + 1])
                   +           fy_frac  * ((1.0f - fx_frac) * (float)rgba[s2 + 1] + fx_frac * (float)rgba[s3 + 1]);

            float b = (1.0f - fy_frac) * ((1.0f - fx_frac) * (float)rgba[s0 + 2] + fx_frac * (float)rgba[s2 + 2])
                   +           fy_frac  * ((1.0f - fx_frac) * (float)rgba[s2 + 2] + fx_frac * (float)rgba[s3 + 2]);

            /* RGB → RGB565 */
            int r5 = (int)(r + 0.5f);
            int g6 = (int)(g + 0.5f);
            int b5 = (int)(b + 0.5f);
            if (r5 > 255) r5 = 255;
            if (r5 < 0) r5 = 0;
            if (g6 > 255) g6 = 255;
            if (g6 < 0) g6 = 0;
            if (b5 > 255) b5 = 255;
            if (b5 < 0) b5 = 0;

            uint16_t rgb565 = (uint16_t)(((r5 >> 3) << 11) | ((g6 >> 2) << 5) | (b5 >> 3));

            int out_idx = (dy * TARGET_W + dx) * 2;
            out[out_idx]     = (uint8_t)(rgb565 & 0xFF);
            out[out_idx + 1] = (uint8_t)(rgb565 >> 8);
        }

        /* Feed watchdog every 8 rows (bilinear float math is slow) */
        if ((dy & 0x07) == 0) {
            vTaskDelay(1);
        }
    }

    free(rgba);

    ESP_LOGI(TAG, "Scaled to 800×480 RGB565 (%d bytes)", out_size);

    *rgb565_out = out;
    return ESP_OK;
}
