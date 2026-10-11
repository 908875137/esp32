/*
 * text_renderer.c
 *
 * FreeType-based Chinese text overlay on an RGBA8 framebuffer.
 *
 * Font handling — FT_New_Face(FILE_PATH) tells FreeType to open the font
 * from SPIFFS.  FreeType then reads glyph tables on demand via fseek/fread
 * through the SPIFFS VFS layer — the 8MB OTF is never fully loaded into RAM.
 *
 * Renders the quote with auto word-wrap, then the source line below it.
 * Text colour is chosen automatically based on background luminance.
 *
 * Layout parameters are in text_renderer.h — change them there, not here.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "ft2build.h"
#include FT_FREETYPE_H

#include "text_renderer.h"
#include "color_space.h"

static const char *TAG = "text";

static FT_Library  s_ft_library = NULL;
static FT_Face     s_ft_face    = NULL;
static bool        s_initialised = false;

/* ---- Text colour decision ---- */
typedef enum { TEXT_BLACK = 0, TEXT_WHITE = 1 } text_color_t;

/* ------------------------------------------------------------------
 * choose_text_color
 *
 * Sample the background region that ONE GLYPH will occupy (BEFORE it is
 * drawn, so the region holds background only), convert every pixel to
 * OKLab, take the MEDIAN of L via a 256-bin histogram (median, not mean —
 * a bright sky or dark tree in the region won't skew the decision), then
 * pick BLACK or WHITE:
 *
 *   median L > 0.50          → BLACK   (light background)
 *   median L < 0.35          → WHITE   (dark background)
 *   0.35 ≤ median L ≤ 0.50   → pick the colour whose OKLab distance from
 *                               the background is LARGER (max contrast)
 *
 * OKLab L is 0..1; 0.5 and 0.35 map to CIELAB L* of 50 and 35.
 *
 * Histogram median, not malloc+qsort — this runs PER GLYPH, so it must be
 * allocation-free and cheap (a glyph region is only ~40×40 px).
 * ------------------------------------------------------------------ */
static text_color_t choose_text_color(const uint8_t *rgba, int W, int H,
                                      int x0, int y0, int x1, int y1,
                                      float *out_med_l,
                                      float *out_dist_black,
                                      float *out_dist_white) {
    /* Clamp region to framebuffer */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    int rw = x1 - x0;
    int rh = y1 - y0;
    if (rw <= 0 || rh <= 0) {
        *out_med_l = 0.5f;
        *out_dist_black = 0.0f;
        *out_dist_white = 0.0f;
        return TEXT_BLACK;
    }

    color_space_build_lut();

    /* 256-bin histogram of OKLab L.  a/b are averaged (they cancel in the
       black-vs-white distance, so a mean is fine; only L needs the median). */
    int hist[256] = {0};
    float sum_a = 0.0f, sum_b = 0.0f;
    int n = 0;

    for (int y = y0; y < y1; y++) {
        const uint8_t *row = rgba + (y * W + x0) * 4;
        for (int x = 0; x < rw; x++) {
            float lr = color_space_lut_value(row[x * 4 + 0]);
            float lg = color_space_lut_value(row[x * 4 + 1]);
            float lb = color_space_lut_value(row[x * 4 + 2]);
            oklab_t lab = rgb_linear_to_oklab(lr, lg, lb);
            int bin = (int)(lab.l * 256.0f);
            if (bin < 0) bin = 0;
            if (bin > 255) bin = 255;
            hist[bin]++;
            sum_a += lab.a;
            sum_b += lab.b;
            n++;
        }
    }

    /* Find median L from the histogram */
    int half = n / 2;
    int cum = 0, med_bin = 0;
    for (int i = 0; i < 256; i++) {
        cum += hist[i];
        if (cum >= half) { med_bin = i; break; }
    }
    float med_l = (float)med_bin / 255.0f;
    float med_a = sum_a / (float)n;
    float med_b = sum_b / (float)n;

    /* Text colours are pure black (0,0,0) → OKLab (0,0,0) and pure white
       (255,255,255) → OKLab (1,0,0).  Distance is plain OKLab Euclidean.
       (The a/b terms cancel for black-vs-white, so this is governed by L,
       but we compute both for the log.) */
    float dblack = sqrtf(med_l * med_l + med_a * med_a + med_b * med_b);
    float dwhite = sqrtf((med_l - 1.0f) * (med_l - 1.0f) + med_a * med_a + med_b * med_b);

    *out_med_l = med_l;
    *out_dist_black = dblack;
    *out_dist_white = dwhite;

    if (med_l > 0.50f) return TEXT_BLACK;
    if (med_l < 0.35f) return TEXT_WHITE;
    /* Mid-tone: pick the colour with the LARGER distance = more contrast */
    return (dblack > dwhite) ? TEXT_BLACK : TEXT_WHITE;
}

/* ---- UTF-8 decode helper ---- */

/*
 * Read one UTF-8 codepoint from `*s` and advance `*s`.
 * Returns the Unicode codepoint, or 0 on error/end.
 */
static unsigned utf8_codepoint(const char **s) {    const unsigned char *p = (const unsigned char *)*s;
    unsigned cp;

    if (*p == 0) return 0;

    if (*p < 0x80) {
        cp = *p;
        *s += 1;
    } else if ((*p & 0xE0) == 0xC0) {
        cp  = (*p & 0x1F) << 6;
        cp |= (*(p + 1) & 0x3F);
        *s += 2;
    } else if ((*p & 0xF0) == 0xE0) {
        cp  = (*p & 0x0F) << 12;
        cp |= (*(p + 1) & 0x3F) << 6;
        cp |= (*(p + 2) & 0x3F);
        *s += 3;
    } else {
        cp  = (*p & 0x07) << 18;
        cp |= (*(p + 1) & 0x3F) << 12;
        cp |= (*(p + 2) & 0x3F) << 6;
        cp |= (*(p + 3) & 0x3F);
        *s += 4;
    }
    return cp;
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t text_renderer_init(const char *font_path) {
    if (s_initialised) return ESP_OK;

    const char *path = font_path ? font_path : TEXT_FONT_PATH_DEFAULT;

    /* ---- Verify font file exists; fall back to the default name if the
       configured path is missing (e.g. stale config.json) ---- */
    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGW(TAG, "Font file NOT FOUND: %s — trying default", path);
        path = TEXT_FONT_PATH_DEFAULT;
        if (stat(path, &st) != 0) {
            ESP_LOGE(TAG, "Default font also NOT FOUND: %s", path);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "Font file found: %s  size=%ld bytes", path, (long)st.st_size);

    ESP_LOGI(TAG, "Initialising FreeType...");
    int ft_err = FT_Init_FreeType(&s_ft_library);
    if (ft_err) {
        ESP_LOGE(TAG, "FT_Init_FreeType failed: %d", ft_err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loading font: %s (opens from filesystem, not RAM)", path);
    ft_err = FT_New_Face(s_ft_library, path, 0, &s_ft_face);
    if (ft_err) {
        ESP_LOGE(TAG, "FT_New_Face failed: %d — is font file present?", ft_err);
        FT_Done_FreeType(s_ft_library);
        s_ft_library = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Font family: %s  style: %s",
             s_ft_face->family_name ? s_ft_face->family_name : "(null)",
             s_ft_face->style_name  ? s_ft_face->style_name  : "(null)");
    ESP_LOGI(TAG, "Text renderer ready");

    s_initialised = true;
    return ESP_OK;
}

void text_renderer_deinit(void) {
    if (s_ft_face) { FT_Done_Face(s_ft_face); s_ft_face = NULL; }
    if (s_ft_library) { FT_Done_FreeType(s_ft_library); s_ft_library = NULL; }
    s_initialised = false;
}

esp_err_t text_render_overlay(uint8_t *rgba, int W, int H,
                              const char *quote,
                              const char *source) {
    if (!s_initialised || !rgba || !quote) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "--- Text overlay ---");
    ESP_LOGI(TAG, "Quote: \"%s\"", quote);
    if (source && source[0])
        ESP_LOGI(TAG, "Source: %s", source);

    /* ---- 1. Memory log before rendering ---- */
    ESP_LOGI(TAG, "DRAM free=%lu largest=%lu  PSRAM free=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    /* ---- 3. Layout pass: measure every glyph, wrap lines ---- */
    FT_Set_Pixel_Sizes(s_ft_face, TEXT_QUOTE_SIZE, TEXT_QUOTE_SIZE);

    int line_widths[16] = {0};
    int line_count = 0;
    int cur_x = 0;
    const char *p = quote;
    int layout_glyph_count = 0;  /* for watchdog feeding */

    while (*p && line_count < 16) {
        unsigned cp = utf8_codepoint(&p);
        if (cp == 0) break;

        int glyph_idx = FT_Get_Char_Index(s_ft_face, cp);
        if (glyph_idx == 0) continue;  /* missing glyph — skip */

        FT_Load_Glyph(s_ft_face, glyph_idx, FT_LOAD_NO_BITMAP);
        int adv = s_ft_face->glyph->advance.x >> 6;

        /* Feed watchdog every 4 glyphs (FT_Load_Glyph hits SPIFFS → flash I/O) */
        if ((layout_glyph_count & 0x03) == 0) vTaskDelay(1);
        layout_glyph_count++;

        if (cur_x + adv > W - TEXT_MARGIN_X * 2 && cur_x > 0) {
            /* wrap */
            line_widths[line_count] = cur_x;
            line_count++;
            cur_x = 0;
        }
        cur_x += adv;
    }
    if (cur_x > 0) {
        line_widths[line_count] = cur_x;
        line_count++;
    }

    ESP_LOGI(TAG, "Quote layout: %d line(s), widths:", line_count);
    for (int i = 0; i < line_count; i++) {
        ESP_LOGI(TAG, "  line %d: %d px wide", i, line_widths[i]);
    }

    /* ---- 4. Calculate start Y (bottom-aligned) ---- */
    int quote_block_h = line_count * (TEXT_QUOTE_SIZE + TEXT_LINE_SPACING) - TEXT_LINE_SPACING;
    int source_line_h = (source && source[0]) ? (TEXT_SOURCE_SIZE + TEXT_SOURCE_GAP) : 0;
    int total_text_h = quote_block_h + source_line_h;
    int start_y = H - TEXT_BOTTOM_MARGIN - total_text_h;
    if (start_y < 0) start_y = 0;  /* too much text — don't draw off-screen */

    ESP_LOGI(TAG, "Text block: %d×%d px at Y=%d", (int)(W - TEXT_MARGIN_X * 2), total_text_h, start_y);

    /* ---- 5. Draw each glyph, sampling ITS OWN background just before it is
       drawn.  A glyph that lands on a light patch gets BLACK; a neighbour on
       a dark patch gets WHITE. ---- */
    FT_Set_Pixel_Sizes(s_ft_face, TEXT_QUOTE_SIZE, TEXT_QUOTE_SIZE);

    int draw_y = start_y;
    int line_idx = 0;
    int draw_glyph_count = 0;   /* for watchdog feeding */
    p = quote;
    cur_x = TEXT_MARGIN_X;

    while (*p && line_idx < line_count) {
        unsigned cp = utf8_codepoint(&p);
        if (cp == 0) break;

        int glyph_idx = FT_Get_Char_Index(s_ft_face, cp);
        if (glyph_idx == 0) continue;

        FT_Load_Glyph(s_ft_face, glyph_idx, FT_LOAD_RENDER);

        /* Feed watchdog every 4 glyphs (FT_Load_Glyph does flash I/O) */
        if ((draw_glyph_count & 0x03) == 0) vTaskDelay(1);
        draw_glyph_count++;

        FT_GlyphSlot slot = s_ft_face->glyph;
        int adv = slot->advance.x >> 6;
        int left = cur_x + slot->bitmap_left;
        int baseline = draw_y + (int)(TEXT_QUOTE_SIZE * 0.85f);  /* ~85% up for visual centering */
        int top  = baseline - slot->bitmap_top;
        if (top < 0) top = 0;  /* glyph bitmap starts above screen — clamp */

        /* wrap check */
        if (cur_x + adv > W - TEXT_MARGIN_X && cur_x > TEXT_MARGIN_X) {
            line_idx++;
            draw_y += TEXT_QUOTE_SIZE + TEXT_LINE_SPACING;
            cur_x = TEXT_MARGIN_X;
            /* re-load glyph for new line (need bitmap this time) */
            FT_Load_Glyph(s_ft_face, glyph_idx, FT_LOAD_RENDER);
            left = cur_x + slot->bitmap_left;
            baseline = draw_y + (int)(TEXT_QUOTE_SIZE * 0.85f);
            top  = baseline - slot->bitmap_top;
            if (top < 0) top = 0;
        }

        /* Sample THIS glyph's footprint and pick its colour */
        float med_l = 0.5f, dblack = 0.0f, dwhite = 0.0f;
        text_color_t tcol = choose_text_color(
            rgba, W, H,
            left, top,
            left + (int)slot->bitmap.width, top + (int)slot->bitmap.rows,
            &med_l, &dblack, &dwhite);
        uint8_t tr = (tcol == TEXT_BLACK) ? 0 : 255;

        /* Alpha-blend glyph onto RGBA */
        for (int gy = 0; gy < (int)slot->bitmap.rows; gy++) {
            int screen_y = top + gy;
            if (screen_y < 0 || screen_y >= H) continue;
            for (int gx = 0; gx < (int)slot->bitmap.width; gx++) {
                int screen_x = left + gx;
                if (screen_x < 0 || screen_x >= W) continue;
                unsigned char alpha = slot->bitmap.buffer[gy * slot->bitmap.pitch + gx];
                if (alpha == 0) continue;
                int idx = (screen_y * W + screen_x) * 4;
                float a = (float)alpha / 255.0f;
                rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * (1.0f - a) + (float)tr * a + 0.5f);
                rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * (1.0f - a) + (float)tr * a + 0.5f);
                rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * (1.0f - a) + (float)tr * a + 0.5f);
                rgba[idx + 3] = 255;
            }
        }
        cur_x += adv;
    }

    /* ---- 6. Draw source line ---- */
    if (source && source[0]) {
        FT_Set_Pixel_Sizes(s_ft_face, TEXT_SOURCE_SIZE, TEXT_SOURCE_SIZE);

        line_idx++;
        draw_y += TEXT_QUOTE_SIZE + TEXT_SOURCE_GAP;

        const char *sp = source;
        int src_x = W - TEXT_MARGIN_X;  /* right-aligned */

        /* measure source width first */
        int src_w = 0;
        const char *mp = source;
        while (*mp) {
            unsigned cp = utf8_codepoint(&mp);
            int gi = FT_Get_Char_Index(s_ft_face, cp);
            if (gi) {
                FT_Load_Glyph(s_ft_face, gi, FT_LOAD_DEFAULT);
                src_w += s_ft_face->glyph->advance.x >> 6;
            }
        }
        src_x = W - TEXT_MARGIN_X - src_w;
        if (src_x < TEXT_MARGIN_X) src_x = TEXT_MARGIN_X;

        while (*sp) {
            unsigned cp = utf8_codepoint(&sp);
            if (cp == 0) break;
            int glyph_idx = FT_Get_Char_Index(s_ft_face, cp);
            if (glyph_idx == 0) continue;

            FT_Load_Glyph(s_ft_face, glyph_idx, FT_LOAD_RENDER);
            FT_GlyphSlot slot = s_ft_face->glyph;
            int left = src_x + slot->bitmap_left;
            int baseline = draw_y + (int)(TEXT_SOURCE_SIZE * 0.85f);
            int top  = baseline - slot->bitmap_top;
            if (top < 0) top = 0;

            /* Per-glyph colour for the source line too */
            float med_l = 0.5f, dblack = 0.0f, dwhite = 0.0f;
            text_color_t stcol = choose_text_color(
                rgba, W, H,
                left, top,
                left + (int)slot->bitmap.width, top + (int)slot->bitmap.rows,
                &med_l, &dblack, &dwhite);
            uint8_t sr = (stcol == TEXT_BLACK) ? 0 : 255;

            for (int gy = 0; gy < (int)slot->bitmap.rows; gy++) {
                int screen_y = top + gy;
                if (screen_y < 0 || screen_y >= H) continue;
                for (int gx = 0; gx < (int)slot->bitmap.width; gx++) {
                    int screen_x = left + gx;
                    if (screen_x < 0 || screen_x >= W) continue;
                    unsigned char alpha = slot->bitmap.buffer[gy * slot->bitmap.pitch + gx];
                    if (alpha == 0) continue;
                    int idx = (screen_y * W + screen_x) * 4;
                    float a = (float)alpha / 255.0f;
                    rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * (1.0f - a) + (float)sr * a + 0.5f);
                    rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * (1.0f - a) + (float)sr * a + 0.5f);
                    rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * (1.0f - a) + (float)sr * a + 0.5f);
                    rgba[idx + 3] = 255;
                }
            }
            src_x += slot->advance.x >> 6;
        }
    }

    /* ---- 7. Memory log after rendering ---- */
    ESP_LOGI(TAG, "DRAM free=%lu largest=%lu  PSRAM free=%lu largest=%lu",
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    ESP_LOGI(TAG, "Text overlay complete");
    return ESP_OK;
}

/* ================================================================
 * Side overlay (portrait images) — vertical text along the right edge.
 *
 * A portrait image is rotated 90° to fill the landscape panel, so its
 * "bottom" edge lands on a screen side.  We render the quote horizontally
 * into a small alpha buffer, then rotate it 90° onto the right edge:
 * traditional vertical reading (each line runs top-to-bottom, and lines run
 * right-to-left).  The text colour is picked once from the side-panel
 * background luminance (not per glyph — the panel is narrow).
 * ================================================================ */
esp_err_t text_render_side_overlay(uint8_t *rgba, int W, int H,
                                   const char *quote, const char *source) {
    if (!s_initialised || !rgba || !quote) return ESP_ERR_INVALID_STATE;

    ESP_LOGI(TAG, "--- Side overlay (portrait) ---");

    const int wrap_w = H - TEXT_SIDE_MARGIN * 2;   /* text runs along the tall side */

    /* ---- 1. measure + wrap (quote) ---- */
    FT_Set_Pixel_Sizes(s_ft_face, TEXT_SIDE_QUOTE_SIZE, TEXT_SIDE_QUOTE_SIZE);

    int line_widths[16] = {0};
    int line_count = 0;
    int cur_x = 0;
    const char *p = quote;
    int mg = 0;
    while (*p && line_count < 16) {
        unsigned cp = utf8_codepoint(&p);
        if (cp == 0) break;
        int gi = FT_Get_Char_Index(s_ft_face, cp);
        if (gi == 0) continue;
        FT_Load_Glyph(s_ft_face, gi, FT_LOAD_NO_BITMAP);
        int adv = s_ft_face->glyph->advance.x >> 6;
        if ((mg & 0x03) == 0) vTaskDelay(1);
        mg++;
        if (cur_x + adv > wrap_w && cur_x > 0) {
            line_widths[line_count] = cur_x;
            line_count++;
            cur_x = 0;
        }
        cur_x += adv;
    }
    if (cur_x > 0) { line_widths[line_count] = cur_x; line_count++; }
    if (line_count == 0) return ESP_OK;

    int tw = 0;
    for (int i = 0; i < line_count; i++) if (line_widths[i] > tw) tw = line_widths[i];

    int quote_h = line_count * (TEXT_SIDE_QUOTE_SIZE + TEXT_SIDE_LINE_SPACING) - TEXT_SIDE_LINE_SPACING;
    int source_h = (source && source[0]) ? (TEXT_SIDE_SOURCE_SIZE + TEXT_SIDE_SOURCE_GAP) : 0;
    int th = quote_h + source_h;

    /* ---- 2. render glyphs into alpha + per-glyph colour buffers ---- */
    uint8_t *alpha = (uint8_t *)calloc(1, (size_t)tw * th);
    uint8_t *color = (uint8_t *)calloc(1, (size_t)tw * th);  /* 0=black, 1=white */
    if (!alpha || !color) {
        free(alpha);
        free(color);
        return ESP_ERR_NO_MEM;
    }

    int draw_y = 0;
    int line_idx = 0;
    p = quote;
    cur_x = 0;
    FT_Set_Pixel_Sizes(s_ft_face, TEXT_SIDE_QUOTE_SIZE, TEXT_SIDE_QUOTE_SIZE);
    int dg = 0;
    while (*p && line_idx < line_count) {
        unsigned cp = utf8_codepoint(&p);
        if (cp == 0) break;
        int gi = FT_Get_Char_Index(s_ft_face, cp);
        if (gi == 0) continue;
        FT_Load_Glyph(s_ft_face, gi, FT_LOAD_RENDER);
        if ((dg & 0x03) == 0) vTaskDelay(1);
        dg++;
        FT_GlyphSlot slot = s_ft_face->glyph;
        int adv = slot->advance.x >> 6;
        int left = cur_x + slot->bitmap_left;
        int baseline = draw_y + (int)(TEXT_SIDE_QUOTE_SIZE * 0.85f);
        int top = baseline - slot->bitmap_top;

        if (cur_x + adv > wrap_w && cur_x > 0) {
            line_idx++;
            draw_y += TEXT_SIDE_QUOTE_SIZE + TEXT_SIDE_LINE_SPACING;
            cur_x = 0;
            FT_Load_Glyph(s_ft_face, gi, FT_LOAD_RENDER);
            left = cur_x + slot->bitmap_left;
            baseline = draw_y + (int)(TEXT_SIDE_QUOTE_SIZE * 0.85f);
            top = baseline - slot->bitmap_top;
        }

        /* Per-glyph colour: sample this glyph's own background in the rotated
           frame, so a dark glyph on dark clothing flips to white. */
        int sx0 = W - th + top;
        int sy0 = tw - left - (int)slot->bitmap.width;
        float gl, gdb, gdw;
        text_color_t gcol = choose_text_color(rgba, W, H,
                                              sx0, sy0,
                                              sx0 + (int)slot->bitmap.rows,
                                              sy0 + (int)slot->bitmap.width,
                                              &gl, &gdb, &gdw);
        uint8_t gc = (gcol == TEXT_BLACK) ? 0 : 1;

        for (int gy = 0; gy < (int)slot->bitmap.rows; gy++) {
            int ty = top + gy;
            if (ty < 0 || ty >= th) continue;
            for (int gx = 0; gx < (int)slot->bitmap.width; gx++) {
                int tx = left + gx;
                if (tx < 0 || tx >= tw) continue;
                unsigned char a = slot->bitmap.buffer[gy * slot->bitmap.pitch + gx];
                if (a > alpha[ty * tw + tx]) {
                    alpha[ty * tw + tx] = a;
                    color[ty * tw + tx] = gc;
                }
            }
        }
        cur_x += adv;
    }

    /* ---- 3. source line (below the quote, left-aligned) ---- */
    if (source && source[0]) {
        FT_Set_Pixel_Sizes(s_ft_face, TEXT_SIDE_SOURCE_SIZE, TEXT_SIDE_SOURCE_SIZE);
        int sy_base = quote_h + TEXT_SIDE_SOURCE_GAP;
        const char *sp = source;
        int sx_cur = 0;
        while (*sp) {
            unsigned cp = utf8_codepoint(&sp);
            if (cp == 0) break;
            int gi = FT_Get_Char_Index(s_ft_face, cp);
            if (gi == 0) continue;
            FT_Load_Glyph(s_ft_face, gi, FT_LOAD_RENDER);
            FT_GlyphSlot slot = s_ft_face->glyph;
            int adv = slot->advance.x >> 6;
            int left = sx_cur + slot->bitmap_left;
            int baseline = sy_base + (int)(TEXT_SIDE_SOURCE_SIZE * 0.85f);
            int top = baseline - slot->bitmap_top;
            /* Per-glyph colour for the source line too. */
            int ssx0 = W - th + top;
            int ssy0 = tw - left - (int)slot->bitmap.width;
            float sgl, sgdb, sgdw;
            text_color_t sgcol = choose_text_color(rgba, W, H,
                                                   ssx0, ssy0,
                                                   ssx0 + (int)slot->bitmap.rows,
                                                   ssy0 + (int)slot->bitmap.width,
                                                   &sgl, &sgdb, &sgdw);
            uint8_t sgc = (sgcol == TEXT_BLACK) ? 0 : 1;

            for (int gy = 0; gy < (int)slot->bitmap.rows; gy++) {
                int ty = top + gy;
                if (ty < 0 || ty >= th) continue;
                for (int gx = 0; gx < (int)slot->bitmap.width; gx++) {
                    int tx = left + gx;
                    if (tx < 0 || tx >= tw) continue;
                    unsigned char a = slot->bitmap.buffer[gy * slot->bitmap.pitch + gx];
                    if (a > alpha[ty * tw + tx]) {
                        alpha[ty * tw + tx] = a;
                        color[ty * tw + tx] = sgc;
                    }
                }
            }
            sx_cur += adv;
        }
    }

    /* ---- 4. rotate 90° onto the right edge, using per-glyph colour ---- */
    for (int ty = 0; ty < th; ty++) {
        for (int tx = 0; tx < tw; tx++) {
            unsigned char a = alpha[ty * tw + tx];
            if (a == 0) continue;
            int sx = W - th + ty;
            int sy = tw - 1 - tx;
            if (sx < 0 || sx >= W || sy < 0 || sy >= H) continue;
            uint8_t tr = color[ty * tw + tx] ? 255 : 0;
            int idx = (sy * W + sx) * 4;
            float af = (float)a / 255.0f;
            rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * (1.0f - af) + (float)tr * af + 0.5f);
            rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * (1.0f - af) + (float)tr * af + 0.5f);
            rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * (1.0f - af) + (float)tr * af + 0.5f);
            rgba[idx + 3] = 255;
        }
    }

    free(alpha);
    free(color);
    ESP_LOGI(TAG, "Side overlay complete (%d×%d text -> vertical)", tw, th);
    return ESP_OK;
}
