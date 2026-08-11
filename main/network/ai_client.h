/*
 * ai_client.h
 *
 * Two-stage AI pipeline:
 *   Stage 1 — DeepSeek V4 Pro Chat Completions → daily quote
 *   Stage 2 — Hunyuan Image V3              → artwork image URL
 *
 * All calls use the shared TokenHub endpoint with the same API Key.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* ---- Daily quote (Stage 1) ---- */

typedef struct {
    char quote[512];        /* the original text */
    char author[128];       /* who said/wrote it */
    char source[256];       /* work or origin */
    char type[64];          /* 电影台词/小说/诗歌/哲学… */
    char mood[128];         /* one-line emotional description */
} daily_quote_t;

/*
 * Call DeepSeek to generate a daily quote.
 * Returns ESP_OK and fills `quote` on success.
 * Caller MUST NOT proceed to image generation on failure.
 */
esp_err_t ai_generate_daily_quote(daily_quote_t *quote);

/* ---- Image generation (Stage 2) ---- */

typedef struct {
    char image_url[2048];           /* signed COS PNG URL */
    char revised_prompt[4096];      /* AI's revised prompt (for logging) */
} ai_image_result_t;

/*
 * Call Hunyuan Image API with a dynamically-built prompt.
 * `size` should be "854x512" — must match a valid preset.
 */
esp_err_t ai_generate_image(const char *prompt, const char *size,
                            ai_image_result_t *result);

/* ---- Prompt builder ---- */

/*
 * Build a complete Hunyuan image prompt from a daily quote.
 * Fills `buf` (max `buf_size` bytes) with a fully-formed Chinese prompt.
 * Safe to call even if quote fields are empty (produces a generic prompt).
 */
void ai_build_image_prompt(const daily_quote_t *quote,
                           char *buf, size_t buf_size);
