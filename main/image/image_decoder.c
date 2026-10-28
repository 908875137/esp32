/*
 * image_decoder.c
 *
 * Download PNG → Decode → (text overlay) → Center-crop → RGBA or RGB565.
 *
 * Split pipeline:
 *   1. image_download_and_decode_rgba  → RGBA8 framebuffer  (for text overlay)
 *   2. text_render_overlay()            → draw Chinese text on RGBA
 *   3. image_rgba_to_rgb565           → final dither-ready buffer
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
 * Shared decode core — PNG bytes → RGBA8 800×480
 *
 * Used by both the URL path (after download) and the SD path (after
 * fread).  Caller owns the PNG buffer and frees it after this returns.
 * ================================================================ */

#define TARGET_W  800
#define TARGET_H  480

static esp_err_t png_bytes_to_rgba(const uint8_t *png, int png_len,
                                   uint8_t **rgba_out, image_info_t *info) {
    if (!png || png_len <= 0 || !rgba_out) return ESP_ERR_INVALID_ARG;
    *rgba_out = NULL;
    if (info) { info->width = 0; info->height = 0; }

    /* ---- Read PNG width/height directly from the IHDR chunk (no allocation,
       no state object).  PNG layout is fixed: bytes 16..19 = width,
       20..23 = height, big-endian. ---- */
    if (png_len < 24) {
        ESP_LOGE(TAG, "PNG too short (%d bytes)", png_len);
        return ESP_FAIL;
    }
    unsigned png_w = ((unsigned)png[16] << 24) | ((unsigned)png[17] << 16) |
                     ((unsigned)png[18] << 8)  |  (unsigned)png[19];
    unsigned png_h = ((unsigned)png[20] << 24) | ((unsigned)png[21] << 16) |
                     ((unsigned)png[22] << 8)  |  (unsigned)png[23];

    /* Decoded RGB = w*h*3.  lodepng decodes the whole frame at once, so peak
       PSRAM = my PNG buffer + lodepng's idat copy + inflate scanlines + RGB
       output ≈ 7×pixels + PNG-size.  On the 8MB PSRAM (of which ~4MB is held
       by WiFi/mDNS/httpd/SD and ~2MB is the largest contiguous block), ~470k
       px decodes but ~850k px OOMs.  Cap at 500k px (~800×625) — already well
       above the 800×480 (384k px) screen, so extra pixels add nothing. */
    const unsigned long long pixels = (unsigned long long)png_w * png_h;
    if (pixels > 500000ULL) {
        ESP_LOGE(TAG, "Image too large: %u×%u (%llu px) — max ~800×625. "
                 "Resize to 5:3 (e.g. 1000×600 downsampled to ≤800×625).",
                 png_w, png_h, pixels);
        return ESP_ERR_NO_MEM;
    }

    unsigned char *raw = NULL;

    /* Decode to RGB24 (3 bytes/px) — alpha is not used downstream */
    unsigned dec_err = lodepng_decode24(&raw, &png_w, &png_h, png, png_len);
    if (dec_err != 0) {
        ESP_LOGE(TAG, "PNG decode error %u: %s", dec_err, lodepng_error_text(dec_err));
        free(raw);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "PNG decoded: %u×%u RGB", png_w, png_h);

    if (png_w == 0 || png_h == 0) {
        ESP_LOGE(TAG, "PNG has zero dimension");
        free(raw);
        return ESP_FAIL;
    }

    if (info) { info->width = png_w; info->height = png_h; }

    /* ---- Rotation classification ----
     *   portrait (h > w) → rotate 90° CCW to fill the landscape screen
     *   landscape/square → no rotation here (the whole frame, text included,
     *     is rotated 180° at display time to compensate for the upside-down
     *     panel mounting)
     * Rotation is a coordinate transform in the sampling loop (no extra buffer). */
    bool rotate90  = png_h > png_w;
    bool rotate180 = false;
    if (rotate90)  ESP_LOGI(TAG, "Portrait (%u×%u) — rotating 90° CCW", png_w, png_h);

    /* ---- Crop to 5:3 (800:480) aspect, FILL (no letterbox) ---- */
    const float target_aspect = (float)TARGET_W / (float)TARGET_H;
    /* Effective source dims after rotation */
    int src_w = rotate90 ? (int)png_h : (int)png_w;
    int src_h = rotate90 ? (int)png_w : (int)png_h;
    const float src_aspect = (float)src_w / (float)src_h;

    int crop_w, crop_h, crop_ox, crop_oy;

    if (src_aspect > target_aspect) {
        crop_h = src_h;
        crop_w = (int)((float)src_h * target_aspect + 0.5f);
        crop_oy = 0;
        crop_ox = (src_w - crop_w) / 2;
    } else {
        crop_w = src_w;
        crop_h = (int)((float)src_w / target_aspect + 0.5f);
        crop_ox = 0;
        crop_oy = (src_h - crop_h) / 2;
    }

    if (crop_ox < 0) crop_ox = 0;
    if (crop_oy < 0) crop_oy = 0;
    if (crop_ox + crop_w > src_w) crop_w = src_w - crop_ox;
    if (crop_oy + crop_h > src_h) crop_h = src_h - crop_oy;

    ESP_LOGI(TAG, "Crop: src=%ux%u%s → (%d,%d) %d×%d → %d×%d",
             png_w, png_h, rotate90 ? " (rot90ccw)" : rotate180 ? " (rot180)" : "",
             crop_ox, crop_oy, crop_w, crop_h, TARGET_W, TARGET_H);

    /* ---- Bilinear scale to 800×480 RGBA8 ---- */
    const int out_size = TARGET_W * TARGET_H * 4;   /* 1 536 000 bytes */
    uint8_t *out = (uint8_t *)malloc(out_size);
    if (!out) {
        ESP_LOGE(TAG, "OOM: RGBA8 output buffer (%d bytes)", out_size);
        free(raw);
        return ESP_ERR_NO_MEM;
    }

    float sx = (float)(crop_w - 1) / (float)(TARGET_W - 1);
    float sy = (float)(crop_h - 1) / (float)(TARGET_H - 1);

    for (int dy = 0; dy < TARGET_H; dy++) {
        float sy_val = (float)crop_oy + (float)dy * sy;

        for (int dx = 0; dx < TARGET_W; dx++) {
            float sx_val = (float)crop_ox + (float)dx * sx;

            /* Map crop-space (sx_val, sy_val) → source float coords (ox, oy) */
            float ox, oy;
            if (rotate90) {
                /* 90° CCW: (rx,ry) → (x,y) = (png_w-1-ry, rx) */
                ox = (float)png_w - 1.0f - sy_val;
                oy = sx_val;
            } else if (rotate180) {
                /* 180°: (rx,ry) → (x,y) = (png_w-1-rx, png_h-1-ry) */
                ox = (float)png_w - 1.0f - sx_val;
                oy = (float)png_h - 1.0f - sy_val;
            } else {
                ox = sx_val;
                oy = sy_val;
            }

            int ix = (int)ox;
            int iy = (int)oy;
            float fx = ox - (float)ix;
            float fy = oy - (float)iy;

            /* Clamp to valid bilinear neighbourhood */
            if (ix < 0)               { ix = 0; fx = 0.0f; }
            if (ix >= (int)png_w - 1) { ix = (int)png_w - 2; fx = 1.0f; }
            if (iy < 0)               { iy = 0; fy = 0.0f; }
            if (iy >= (int)png_h - 1) { iy = (int)png_h - 2; fy = 1.0f; }

            int s0 = (iy * (int)png_w + ix) * 3;
            int s1 = (iy * (int)png_w + ix + 1) * 3;
            int s2 = ((iy + 1) * (int)png_w + ix) * 3;
            int s3 = ((iy + 1) * (int)png_w + ix + 1) * 3;

            for (int ch = 0; ch < 3; ch++) {
                float v = (1.0f - fy) * ((1.0f - fx) * (float)raw[s0 + ch] + fx * (float)raw[s1 + ch])
                       +           fy  * ((1.0f - fx) * (float)raw[s2 + ch] + fx * (float)raw[s3 + ch]);
                int iv = (int)(v + 0.5f);
                if (iv > 255) iv = 255;
                if (iv < 0) iv = 0;
                out[(dy * TARGET_W + dx) * 4 + ch] = (uint8_t)iv;
            }
            /* Alpha always opaque */
            out[(dy * TARGET_W + dx) * 4 + 3] = 255;
        }
        if ((dy & 0x07) == 0) vTaskDelay(1);
    }

    free(raw);

    ESP_LOGI(TAG, "Scaled to %d×%d RGBA8 (%d bytes)", TARGET_W, TARGET_H, out_size);

    *rgba_out = out;
    return ESP_OK;
}

/* ================================================================
 * Public API — Part 1a: URL source (download → decode)
 * ================================================================ */

esp_err_t image_download_and_decode_rgba(const char *url,
                                          uint8_t **rgba_out,
                                          image_info_t *info) {
    if (!url || !rgba_out) return ESP_ERR_INVALID_ARG;

    *rgba_out = NULL;
    if (info) { info->width = 0; info->height = 0; }

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

    /* Save a copy as the "current AI image" so the Web UI can show a
       thumbnail without mixing AI images into the SD library. */
    {
        FILE *save = fopen("/sdcard/current_ai.png", "wb");
        if (save) {
            fwrite(acc.buf, 1, (size_t)acc.len, save);
            fclose(save);
        }
    }

    /* Shrink accum to actual size so lodepng has maximum contiguous PSRAM */
    {
        uint8_t *trimmed = (uint8_t *)heap_caps_realloc(acc.buf, acc.len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (trimmed) { acc.buf = trimmed; acc.cap = acc.len; }
        /* If trim fails, we keep the larger buffer — still usable */
    }

    /* ---- 2. Decode PNG with shared core ---- */
    esp_err_t dec_ret = png_bytes_to_rgba(acc.buf, acc.len, rgba_out, info);
    accum_free(&acc);
    return dec_ret;
}

/* ================================================================
 * Public API — Part 1b: SD/local file source (fread → decode)
 * ================================================================ */

esp_err_t image_file_decode_rgba(const char *path,
                                  uint8_t **rgba_out,
                                  image_info_t *info) {
    if (!path || !rgba_out) return ESP_ERR_INVALID_ARG;

    *rgba_out = NULL;
    if (info) { info->width = 0; info->height = 0; }

    ESP_LOGI(TAG, "Reading image file: %s", path);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    /* Get file size */
    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGE(TAG, "fseek failed: %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    long fsize = ftell(f);
    if (fsize <= 0) {
        ESP_LOGE(TAG, "File is empty: %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    fseek(f, 0, SEEK_SET);

    /* Read the whole file into PSRAM */
    uint8_t *png = (uint8_t *)heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!png) {
        ESP_LOGE(TAG, "OOM: file buffer (%ld bytes)", fsize);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(png, 1, (size_t)fsize, f);
    fclose(f);

    if (read != (size_t)fsize) {
        ESP_LOGE(TAG, "Short read: %u/%ld bytes", (unsigned)read, fsize);
        heap_caps_free(png);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Read %ld bytes from %s", fsize, path);

    /* Decode with shared core (this frees png's decoded copy internally,
       but we still own `png` and must free it) */
    esp_err_t ret = png_bytes_to_rgba(png, (int)fsize, rgba_out, info);
    heap_caps_free(png);
    return ret;
}

/* ================================================================
 * Public API — Part 2: RGBA → RGB565 conversion
 * ================================================================ */

void image_rgba_rotate_180(uint8_t *rgba, int W, int H) {
    if (!rgba || W <= 0 || H <= 0) return;

    const int total = W * H;
    /* Swap pixels symmetric about the centre (in-place).  Each pixel is 4 bytes. */
    for (int i = 0; i < total / 2; i++) {
        int j = total - 1 - i;
        uint8_t tmp[4];
        memcpy(tmp,          rgba + i * 4, 4);
        memcpy(rgba + i * 4, rgba + j * 4, 4);
        memcpy(rgba + j * 4, tmp,          4);
    }
    ESP_LOGI(TAG, "Rotated frame 180° (%d×%d)", W, H);
}

esp_err_t image_rgba_to_rgb565(const uint8_t *rgba,
                                int W, int H,
                                uint8_t **rgb565_out) {
    if (!rgba || !rgb565_out) return ESP_ERR_INVALID_ARG;
    if (W <= 0 || H <= 0) return ESP_ERR_INVALID_ARG;

    const int out_size = W * H * 2;
    uint8_t *out = (uint8_t *)malloc(out_size);
    if (!out) {
        ESP_LOGE(TAG, "OOM: RGB565 output buffer (%d bytes)", out_size);
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int in_idx = (y * W + x) * 4;
            int r = rgba[in_idx + 0];
            int g = rgba[in_idx + 1];
            int b = rgba[in_idx + 2];
            uint16_t rgb565 = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            int out_idx = (y * W + x) * 2;
            out[out_idx]     = (uint8_t)(rgb565 & 0xFF);
            out[out_idx + 1] = (uint8_t)(rgb565 >> 8);
        }
        if ((y & 0x1F) == 0) vTaskDelay(1);
    }

    ESP_LOGI(TAG, "RGBA→RGB565: %d×%d (%d bytes)", W, H, out_size);
    *rgb565_out = out;
    return ESP_OK;
}
