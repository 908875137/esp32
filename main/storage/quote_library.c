/*
 * quote_library.c
 *
 * Offline SD-card quote library (phase 2).  See quote_library.h.
 *
 * quotes.csv is read one line at a time and never loaded whole into RAM.
 * The CSV parser understands quoted fields (text may contain commas, quotes
 * and even newlines are left for a future pass — short quotes are expected
 * on a single line).  UTF-8 is passed through untouched.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_random.h"

#include "quote_library.h"

static const char *TAG = "q_lib";

#define QUOTE_LIB_TMP_PATH  "/sdcard/quotes.tmp"

/* ---- helpers ---- */

static bool is_all_digits(const char *s) {
    if (!s || !*s) return false;
    for (; *s; s++) if (*s < '0' || *s > '9') return false;
    return true;
}

/* Strip a leading UTF-8 BOM (EF BB BF) if present. */
static void strip_utf8_bom(char *line) {
    if ((unsigned char)line[0] == 0xEF &&
        (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) {
        memmove(line, line + 3, strlen(line + 3) + 1);
    }
}

/*
 * Read one CSV field starting at `p` into `out`.  Handles quoted fields
 * ("" is an escaped quote).  Returns a pointer to the next field, or NULL at
 * end of line.  Always NUL-terminates `out`.
 */
static const char *csv_read_field(const char *p, char *out, size_t out_len) {
    out[0] = '\0';
    if (!p || !out || out_len == 0) return NULL;
    size_t o = 0;

    if (*p == '"') {
        p++;
        while (*p && o < out_len - 1) {
            if (*p == '"') {
                if (p[1] == '"') {          /* escaped quote -> literal " */
                    out[o++] = '"';
                    p += 2;
                } else {                     /* closing quote */
                    p++;
                    break;
                }
            } else {
                out[o++] = *p++;
            }
        }
        out[o] = '\0';
        if (*p == ',') p++;
        return p;
    }

    /* unquoted field: up to the next comma */
    while (*p && *p != ',' && *p != '\n' && *p != '\r' && o < out_len - 1) {
        out[o++] = *p++;
    }
    out[o] = '\0';
    if (*p == ',') p++;
    return p;
}

/*
 * Parse one quotes.csv row into `out`.  Returns false for the header row or
 * any malformed/blank line (which are skipped by callers).
 */
static bool parse_quote_line(char *line, quote_t *out) {
    strip_utf8_bom(line);
    char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
    nl = strchr(line, '\r');       if (nl) *nl = '\0';

    char id_str[16];
    char used_str[16];
    char last_used[32];

    const char *p = line;
    p = csv_read_field(p, id_str,       sizeof(id_str));
    p = csv_read_field(p, out->text,    QUOTE_TEXT_MAX);
    p = csv_read_field(p, out->author,  QUOTE_AUTHOR_MAX);
    p = csv_read_field(p, out->source,  QUOTE_SOURCE_MAX);
    p = csv_read_field(p, used_str,     sizeof(used_str));
    p = csv_read_field(p, last_used,    sizeof(last_used));

    if (!is_all_digits(id_str)) return false;   /* header row ("id") */
    out->id = (uint32_t)strtoul(id_str, NULL, 10);
    if (out->id == 0) return false;
    out->used_count = is_all_digits(used_str) ? (uint32_t)strtoul(used_str, NULL, 10) : 0;
    return true;
}

/* ---- public: count / get / choose ---- */

esp_err_t quote_library_count(uint32_t *count_out) {
    if (!count_out) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) { *count_out = 0; return ESP_OK; }

    uint32_t count = 0;
    char line[1024];
    quote_t tmp;
    while (fgets(line, sizeof(line), f)) {
        if (parse_quote_line(line, &tmp)) count++;
    }
    fclose(f);
    *count_out = count;
    return ESP_OK;
}

esp_err_t quote_library_get_by_id(uint32_t id, quote_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (parse_quote_line(line, out) && out->id == id) {
            fclose(f);
            return ESP_OK;
        }
    }
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t quote_library_get_by_index(uint32_t index, quote_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint32_t i = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (parse_quote_line(line, out)) {
            if (i == index) { fclose(f); return ESP_OK; }
            i++;
        }
    }
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t quote_library_get_page(uint32_t offset, uint32_t limit,
                                 quote_t *out, uint32_t *out_count) {
    if (!out || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;
    if (limit == 0) return ESP_OK;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint32_t i = 0;      /* valid rows seen */
    uint32_t n = 0;      /* rows copied */
    char line[1024];
    quote_t cur;
    while (fgets(line, sizeof(line), f) && n < limit) {
        if (!parse_quote_line(line, &cur)) continue;
        if (i < offset) { i++; continue; }
        out[n++] = cur;
        i++;
    }
    fclose(f);
    *out_count = n;
    return ESP_OK;
}

esp_err_t quote_library_choose(quote_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    bool found = false;
    uint32_t best_used = 0;
    char line[1024];
    quote_t cur;
    while (fgets(line, sizeof(line), f)) {
        if (!parse_quote_line(line, &cur)) continue;
        if (!found || cur.used_count < best_used) {
            *out = cur;
            best_used = cur.used_count;
            found = true;
        }
    }
    fclose(f);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t quote_library_get_random(quote_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;

    uint32_t count = 0;
    quote_library_count(&count);
    if (count == 0) return ESP_ERR_NOT_FOUND;

    uint32_t r = esp_random() % count;
    return quote_library_get_by_index(r, out);
}

/* ---- public: mark_used ---- */

esp_err_t quote_library_mark_used(uint32_t id) {
    if (id == 0) return ESP_ERR_INVALID_ARG;

    time_t now = time(NULL);
    bool have_time = (now > 946684800L);   /* 2000-01-01 */

    FILE *in = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *tmp = fopen(QUOTE_LIB_TMP_PATH, "w");
    if (!tmp) { fclose(in); return ESP_FAIL; }

    bool updated = false;
    char line[1024];
    while (fgets(line, sizeof(line), in)) {
        char copy[1024];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        quote_t q;
        if (parse_quote_line(copy, &q) && q.id == id) {
            q.used_count++;
            if (have_time) {
                struct tm tmv;
                localtime_r(&now, &tmv);
                fprintf(tmp, "%08lu,\"%s\",\"%s\",\"%s\",%lu,%04d-%02d-%02d %02d:%02d:%02d\n",
                        (unsigned long)q.id, q.text, q.author, q.source,
                        (unsigned long)q.used_count,
                        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            } else {
                fprintf(tmp, "%08lu,\"%s\",\"%s\",\"%s\",%lu,\n",
                        (unsigned long)q.id, q.text, q.author, q.source,
                        (unsigned long)q.used_count);
            }
            updated = true;
        } else {
            fputs(line, tmp);   /* header or non-target row */
        }
    }
    fclose(in);
    if (fclose(tmp) != 0) { unlink(QUOTE_LIB_TMP_PATH); return ESP_FAIL; }
    if (!updated) {
        ESP_LOGW(TAG, "mark_used: quote %lu not found in CSV", (unsigned long)id);
        unlink(QUOTE_LIB_TMP_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    unlink(QUOTE_LIB_CSV_PATH);   /* FATFS f_rename won't overwrite an existing target */
    if (rename(QUOTE_LIB_TMP_PATH, QUOTE_LIB_CSV_PATH) != 0) {
        ESP_LOGE(TAG, "mark_used: rename failed");
        unlink(QUOTE_LIB_TMP_PATH);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mark_used: quote %lu used_count updated", (unsigned long)id);
    return ESP_OK;
}

/* forward declaration — quote_library_update() uses it before its definition */
static void normalize_text(char *dst, size_t dst_len, const char *src);

esp_err_t quote_library_remove(uint32_t id) {
    if (id == 0) return ESP_ERR_INVALID_ARG;

    FILE *in = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *tmp = fopen(QUOTE_LIB_TMP_PATH, "w");
    if (!tmp) { fclose(in); return ESP_FAIL; }

    bool removed = false;
    char line[1024];
    while (fgets(line, sizeof(line), in)) {
        char copy[1024];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        quote_t q;
        if (parse_quote_line(copy, &q) && q.id == id) {
            removed = true;   /* skip this row */
        } else {
            fputs(line, tmp);
        }
    }
    fclose(in);
    if (fclose(tmp) != 0) { unlink(QUOTE_LIB_TMP_PATH); return ESP_FAIL; }
    if (!removed) { unlink(QUOTE_LIB_TMP_PATH); return ESP_ERR_NOT_FOUND; }

    unlink(QUOTE_LIB_CSV_PATH);   /* FATFS f_rename won't overwrite an existing target */
    if (rename(QUOTE_LIB_TMP_PATH, QUOTE_LIB_CSV_PATH) != 0) {
        unlink(QUOTE_LIB_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t quote_library_update(uint32_t id, const char *text, const char *author,
                               const char *source) {
    if (id == 0 || !text) return ESP_ERR_INVALID_ARG;

    char norm[QUOTE_TEXT_MAX];
    normalize_text(norm, sizeof(norm), text);
    if (norm[0] == '\0') return ESP_ERR_INVALID_ARG;

    FILE *in = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *tmp = fopen(QUOTE_LIB_TMP_PATH, "w");
    if (!tmp) { fclose(in); return ESP_FAIL; }

    bool updated = false;
    char line[1024];
    while (fgets(line, sizeof(line), in)) {
        char copy[1024];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        quote_t q;
        if (parse_quote_line(copy, &q) && q.id == id) {
            fprintf(tmp, "%08lu,\"%s\",\"%s\",\"%s\",%lu,\n",
                    (unsigned long)q.id, norm,
                    author ? author : "", source ? source : "",
                    (unsigned long)q.used_count);
            updated = true;
        } else {
            fputs(line, tmp);
        }
    }
    fclose(in);
    if (fclose(tmp) != 0) { unlink(QUOTE_LIB_TMP_PATH); return ESP_FAIL; }
    if (!updated) { unlink(QUOTE_LIB_TMP_PATH); return ESP_ERR_NOT_FOUND; }

    unlink(QUOTE_LIB_CSV_PATH);   /* FATFS f_rename won't overwrite an existing target */
    if (rename(QUOTE_LIB_TMP_PATH, QUOTE_LIB_CSV_PATH) != 0) {
        unlink(QUOTE_LIB_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t quote_library_get_stats(quote_library_stats_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->min_used = 0xFFFFFFFF;

    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[1024];
    quote_t q;
    while (fgets(line, sizeof(line), f)) {
        if (!parse_quote_line(line, &q)) continue;
        out->count++;
        out->total_used += q.used_count;
        if (q.used_count > out->max_used) {
            out->max_used = q.used_count;
            out->max_used_id = q.id;
        }
        if (q.used_count < out->min_used) {
            out->min_used = q.used_count;
            out->min_used_id = q.id;
        }
    }
    fclose(f);

    if (out->count == 0) out->min_used = 0;
    return ESP_OK;
}

/* ---- normalisation + add (future Web import) ---- */

/*
 * Light normalisation for dedupe only: strip BOM, trim surrounding
 * whitespace, collapse runs of whitespace, and turn \r\n into \n.  Content
 * is otherwise preserved (no punctuation/simplification/rewriting).
 */
static void normalize_text(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) return;
    dst[0] = '\0';
    if (!src) return;

    const char *s = src;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r' || s[len-1]=='\n')) len--;

    size_t o = 0;
    bool prev_space = false;
    for (size_t i = 0; i < len && o < dst_len - 1; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == ' ' || c == '\t' || c == '\n') {
            if (prev_space) continue;
            c = (c == '\n') ? '\n' : ' ';
            prev_space = true;
        } else {
            prev_space = false;
        }
        dst[o++] = c;
    }
    dst[o] = '\0';
}

esp_err_t quote_library_add(const char *text, const char *author, const char *source,
                            uint32_t *id_out) {
    if (!text || !id_out) return ESP_ERR_INVALID_ARG;
    *id_out = 0;

    char norm[QUOTE_TEXT_MAX];
    normalize_text(norm, sizeof(norm), text);
    if (norm[0] == '\0') return ESP_ERR_INVALID_ARG;

    /* Find max id and detect an exact-text duplicate in one pass. */
    FILE *f = fopen(QUOTE_LIB_CSV_PATH, "r");
    uint32_t max_id = 0;
    uint32_t dup_id = 0;
    if (f) {
        char line[1024];
        quote_t q;
        while (fgets(line, sizeof(line), f)) {
            if (!parse_quote_line(line, &q)) continue;
            if (q.id > max_id) max_id = q.id;
            if (dup_id == 0 && strcmp(q.text, norm) == 0) dup_id = q.id;
        }
        fclose(f);
    }
    if (dup_id != 0) {
        ESP_LOGI(TAG, "add: exact duplicate of %08lu", (unsigned long)dup_id);
        *id_out = dup_id;
        return ESP_OK;
    }

    uint32_t id = max_id + 1;
    f = fopen(QUOTE_LIB_CSV_PATH, "a");
    if (!f) return ESP_FAIL;
    int r = fprintf(f, "%08lu,\"%s\",\"%s\",\"%s\",0,\n",
                    (unsigned long)id, norm, author ? author : "", source ? source : "");
    if (fclose(f) != 0) return ESP_FAIL;
    if (r <= 0) return ESP_FAIL;

    *id_out = id;
    ESP_LOGI(TAG, "add: quote id=%08lu", (unsigned long)id);
    return ESP_OK;
}
