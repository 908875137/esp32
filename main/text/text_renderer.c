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
#include "esp_spiffs.h"

#include "ft2build.h"
#include FT_FREETYPE_H

#include "text_renderer.h"

static const char *TAG = "text";

static FT_Library  s_ft_library = NULL;
static FT_Face     s_ft_face    = NULL;
static bool        s_initialised = false;

/* ---- UTF-8 decode helper ---- */

/*
 * Read one UTF-8 codepoint from `*s` and advance `*s`.
 * Returns the Unicode codepoint, or 0 on error/end.
 */
static unsigned utf8_codepoint(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
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

/* ---- Average luminance of a region ---- */

static float rect_avg_luminance(const uint8_t *rgba, int img_w, int img_h,
                                int x0, int y0, int rw, int rh) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 + rw > img_w) rw = img_w - x0;
    if (y0 + rh > img_h) rh = img_h - y0;
    if (rw <= 0 || rh <= 0) return 128.0f;

    float sum = 0.0f;
    int count = rw * rh;
    for (int y = y0; y < y0 + rh; y++) {
        const uint8_t *row = rgba + (y * img_w + x0) * 4;
        for (int x = 0; x < rw; x++) {
            float r = (float)row[x * 4 + 0];
            float g = (float)row[x * 4 + 1];
            float b = (float)row[x * 4 + 2];
            sum += 0.299f * r + 0.587f * g + 0.114f * b;
        }
    }
    return sum / (float)count;
}

/* ================================================================
 * Public API
 * ================================================================ */

esp_err_t text_renderer_init(void) {
    if (s_initialised) return ESP_OK;

    ESP_LOGI(TAG, "Initialising SPIFFS...");
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = "fonts",
        .max_files              = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("fonts", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u / %u KB used", (unsigned)(used / 1024), (unsigned)(total / 1024));

    /* ---- Verify font file exists in SPIFFS ---- */
    struct stat st;
    if (stat(TEXT_FONT_PATH, &st) == 0) {
        ESP_LOGI(TAG, "Font file found: %s  size=%ld bytes", TEXT_FONT_PATH, (long)st.st_size);
    } else {
        ESP_LOGE(TAG, "Font file NOT FOUND: %s", TEXT_FONT_PATH);
        /* Quick sanity check — try fopen just in case stat is busted */
        FILE *fp = fopen(TEXT_FONT_PATH, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            ESP_LOGI(TAG, "fopen() found file! size=%ld bytes (stat was wrong)", ftell(fp));
            fclose(fp);
        } else {
            ESP_LOGE(TAG, "fopen() also failed — font is absent from SPIFFS image");
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Initialising FreeType...");
    int ft_err = FT_Init_FreeType(&s_ft_library);
    if (ft_err) {
        ESP_LOGE(TAG, "FT_Init_FreeType failed: %d", ft_err);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Loading font: %s (this opens from flash, not RAM)", TEXT_FONT_PATH);
    ft_err = FT_New_Face(s_ft_library, TEXT_FONT_PATH, 0, &s_ft_face);
    if (ft_err) {
        ESP_LOGE(TAG, "FT_New_Face failed: %d — is font file present in SPIFFS?", ft_err);
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
    esp_vfs_spiffs_unregister("fonts");
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

    /* ---- 1. Detect background luminance in bottom area ---- */
    float bottom_lum = rect_avg_luminance(rgba, W, H,
                                          TEXT_MARGIN_X, TEXT_AREA_Y_START,
                                          W - TEXT_MARGIN_X * 2, TEXT_AREA_Y_END - TEXT_AREA_Y_START);
    ESP_LOGI(TAG, "Bottom-area luminance: %.1f", bottom_lum);

    bool use_black = (bottom_lum >= 128.0f);   /* light bg → black text */
    uint8_t text_r = use_black ? 0 : 255;
    uint8_t text_g = use_black ? 0 : 255;
    uint8_t text_b = use_black ? 0 : 255;

    /* source line gets red tint if background is medium-light */
    bool source_red = (bottom_lum >= 110.0f && bottom_lum <= 220.0f);

    ESP_LOGI(TAG, "Text colour: %s", use_black ? "BLACK" : "WHITE");
    if (source_red) ESP_LOGI(TAG, "Source colour: RED");

    /* ---- 2. Memory log before rendering ---- */
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

    /* ---- 5. Draw each line of the quote ---- */
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
                rgba[idx + 0] = (uint8_t)((float)rgba[idx + 0] * (1.0f - a) + (float)text_r * a + 0.5f);
                rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * (1.0f - a) + (float)text_g * a + 0.5f);
                rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * (1.0f - a) + (float)text_b * a + 0.5f);
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

            uint8_t sr = source_red ? 200 : text_r;
            uint8_t sg = source_red ? 30  : text_g;
            uint8_t sb = source_red ? 30  : text_b;

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
                    rgba[idx + 1] = (uint8_t)((float)rgba[idx + 1] * (1.0f - a) + (float)sg * a + 0.5f);
                    rgba[idx + 2] = (uint8_t)((float)rgba[idx + 2] * (1.0f - a) + (float)sb * a + 0.5f);
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
