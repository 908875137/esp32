/*
 * image_library.c
 *
 * Offline SD-card image library (phase 1).  See image_library.h for the
 * layout and scope.  Design goals: simple, reliable, low RAM.
 *
 * - images.csv is always processed one line at a time (fgets); it is never
 *   read whole into RAM.
 * - SHA-256 reuses mbedtls (already a project dependency).
 * - Mutating CSV rewrites (mark_used / remove) go through a tmp file + rename
 *   so a power cut leaves the original intact; append (add) just appends.
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"

#include "image_library.h"

static const char *TAG = "img_lib";

#define IMAGE_LIB_TMP_PATH  "/sdcard/images.tmp"
#define IMAGE_LIB_EXT       ".png"
#define IMAGE_LIB_MAX_LIST  256      /* phase-1 enumeration cap (as before) */
#define HASH_HEX_LEN        64

/* id list used by count / by-index / random (phase-1 cap: 256).
   Cached so repeated enumeration (Web list, SD rotation) doesn't rescan the
   whole directory tree each time; invalidated on add/remove. */
static uint32_t s_id_list[IMAGE_LIB_MAX_LIST];
static int s_id_list_count = -1;   /* -1 = not collected */

static void invalidate_id_cache(void) {
    s_id_list_count = -1;
}

/* ---- small helpers ---- */

static bool is_digits(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return false;
    }
    return true;
}

static bool is_3_digit_dir(const char *name) {
    return strlen(name) == 3 && is_digits(name, 3);
}

/* Parse the leading 8 digits of a filename ("00000001.png") -> id, else 0. */
static uint32_t parse_id_from_filename(const char *name) {
    uint32_t v = 0;
    for (int i = 0; i < IMAGE_LIB_ID_DIGITS; i++) {
        char c = name[i];
        if (c == '\0' || c < '0' || c > '9') return 0;
        v = v * 10 + (uint32_t)(c - '0');
    }
    return v;
}

/* Verify a PNG magic number without decoding it. */
static bool is_png_file(const char *path) {
    static const uint8_t magic[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    uint8_t head[8];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(head, 1, 8, f);
    fclose(f);
    return n == 8 && memcmp(head, magic, 8) == 0;
}

/* ---- public: path / id ---- */

esp_err_t image_library_get_path(uint32_t id, char *path, size_t path_len) {
    if (!path || path_len == 0) return ESP_ERR_INVALID_ARG;
    if (id == 0) return ESP_ERR_INVALID_ARG;

    uint32_t folder = (id - 1) / IMAGE_LIB_PER_DIR;
    snprintf(path, path_len, "%s/%03lu/%08lu%s",
             IMAGE_LIB_ROOT,
             (unsigned long)folder,
             (unsigned long)id,
             IMAGE_LIB_EXT);
    return ESP_OK;
}

uint32_t image_library_id_from_name(const char *name) {
    if (!name) return 0;
    uint32_t v = 0;
    for (int i = 0; i < IMAGE_LIB_ID_DIGITS; i++) {
        char c = name[i];
        if (c == '\0' || c < '0' || c > '9') return 0;
        v = v * 10 + (uint32_t)(c - '0');
    }
    /* after the 8 digits we accept end-of-string or exactly ".png" */
    if (name[IMAGE_LIB_ID_DIGITS] != '\0' &&
        strcmp(name + IMAGE_LIB_ID_DIGITS, IMAGE_LIB_EXT) != 0) {
        return 0;
    }
    return v;
}

/* ---- directory-tree scanning ---- */

/*
 * Collect all image ids into s_id_list, sorted ascending.  Returns the count
 * (0 for an empty library), or -1 if the images/ directory does not exist.
 * Phase-1 cap: IMAGE_LIB_MAX_LIST (256).
 */
static int collect_ids_sorted(void) {
    if (s_id_list_count >= 0) return s_id_list_count;   /* cache hit */

    DIR *root = opendir(IMAGE_LIB_ROOT);
    if (!root) return -1;

    int count = 0;
    struct dirent *de;
    while ((de = readdir(root)) != NULL && count < IMAGE_LIB_MAX_LIST) {
        if (!is_3_digit_dir(de->d_name)) continue;

        char sub[320];
        snprintf(sub, sizeof(sub), "%s/%s", IMAGE_LIB_ROOT, de->d_name);
        DIR *d = opendir(sub);
        if (!d) continue;

        struct dirent *fe;
        while ((fe = readdir(d)) != NULL && count < IMAGE_LIB_MAX_LIST) {
            uint32_t id = parse_id_from_filename(fe->d_name);
            if (id != 0) s_id_list[count++] = id;
        }
        closedir(d);
    }
    closedir(root);

    /* Bubble sort by id (256 max, fine for phase 1). */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (s_id_list[i] > s_id_list[j]) {
                uint32_t t = s_id_list[i];
                s_id_list[i] = s_id_list[j];
                s_id_list[j] = t;
            }
        }
    }
    s_id_list_count = count;
    return count;
}

esp_err_t image_library_get_next_id(uint32_t *id_out) {
    if (!id_out) return ESP_ERR_INVALID_ARG;

    uint32_t max_id = 0;
    DIR *root = opendir(IMAGE_LIB_ROOT);
    if (root) {
        struct dirent *de;
        while ((de = readdir(root)) != NULL) {
            if (!is_3_digit_dir(de->d_name)) continue;

            char sub[320];
            snprintf(sub, sizeof(sub), "%s/%s", IMAGE_LIB_ROOT, de->d_name);
            DIR *d = opendir(sub);
            if (!d) continue;

            struct dirent *fe;
            while ((fe = readdir(d)) != NULL) {
                uint32_t id = parse_id_from_filename(fe->d_name);
                if (id > max_id) max_id = id;
            }
            closedir(d);
        }
        closedir(root);
    }

    *id_out = max_id + 1;
    return ESP_OK;
}

esp_err_t image_library_count(uint32_t *count_out) {
    if (!count_out) return ESP_ERR_INVALID_ARG;
    int c = collect_ids_sorted();
    *count_out = (c < 0) ? 0 : (uint32_t)c;
    return ESP_OK;
}

esp_err_t image_library_get_id_by_index(uint32_t index, uint32_t *id_out) {
    if (!id_out) return ESP_ERR_INVALID_ARG;
    int c = collect_ids_sorted();
    if (c < 0 || index >= (uint32_t)c) return ESP_ERR_NOT_FOUND;
    *id_out = s_id_list[index];
    return ESP_OK;
}

esp_err_t image_library_get_random_id(uint32_t *id_out) {
    if (!id_out) return ESP_ERR_INVALID_ARG;
    int c = collect_ids_sorted();
    if (c <= 0) return ESP_ERR_NOT_FOUND;
    *id_out = s_id_list[esp_random() % (uint32_t)c];
    return ESP_OK;
}

/* ---- SHA-256 (mbedtls, streamed) ---- */

esp_err_t image_library_hash_file(const char *path, char *hex_out, size_t hex_len) {
    if (!path || !hex_out || hex_len < HASH_HEX_LEN + 1) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md_setup(&ctx, info, 0) != 0) {
        mbedtls_md_free(&ctx);
        fclose(f);
        return ESP_FAIL;
    }
    mbedtls_md_starts(&ctx);

    static uint8_t buf[4096];  /* static: httpd task stack is tight */
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        mbedtls_md_update(&ctx, buf, n);
    }
    bool read_ok = !ferror(f);
    fclose(f);

    uint8_t digest[32];
    mbedtls_md_finish(&ctx, digest);
    mbedtls_md_free(&ctx);
    if (!read_ok) return ESP_FAIL;

    for (int i = 0; i < 32; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", digest[i]);
    }
    hex_out[HASH_HEX_LEN] = '\0';
    return ESP_OK;
}

/* ---- CSV parsing (line by line) ---- */

/*
 * Parse one CSV row "id,hash,used_count,last_used_at".  Returns false for the
 * header row or any malformed/blank line.  On success fills id/hash/used_count.
 */
static bool parse_csv_line(char *line, uint32_t *id, char *hash, size_t hash_len,
                           uint32_t *used_count) {
    char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
    nl = strchr(line, '\r');       if (nl) *nl = '\0';

    char *tok = strtok(line, ",");
    if (!tok || !is_digits(tok, strlen(tok))) return false;   /* skips "id" header */
    uint32_t idv = (uint32_t)strtoul(tok, NULL, 10);
    if (idv == 0) return false;

    char *hash_tok = strtok(NULL, ",");
    if (!hash_tok || strlen(hash_tok) != HASH_HEX_LEN) return false;

    char *used_tok = strtok(NULL, ",");
    uint32_t used = used_tok ? (uint32_t)strtoul(used_tok, NULL, 10) : 0;

    if (id) *id = idv;
    if (hash) { strncpy(hash, hash_tok, hash_len - 1); hash[hash_len - 1] = '\0'; }
    if (used_count) *used_count = used;
    return true;
}

static void ensure_csv_header(void) {
    struct stat st;
    if (stat(IMAGE_LIB_CSV_PATH, &st) == 0 && st.st_size > 0) return;

    FILE *f = fopen(IMAGE_LIB_CSV_PATH, "w");
    if (f) {
        fputs("id,hash,used_count,last_used_at\n", f);
        fclose(f);
    }
}

static bool append_csv_row(uint32_t id, const char *hash) {
    ensure_csv_header();
    FILE *f = fopen(IMAGE_LIB_CSV_PATH, "a");
    if (!f) return false;
    int r = fprintf(f, "%08lu,%s,0,\n", (unsigned long)id, hash);
    if (fclose(f) != 0) return false;
    return r > 0;
}

esp_err_t image_library_find_by_hash(const char *hash, uint32_t *id_out) {
    if (!hash || !id_out) return ESP_ERR_INVALID_ARG;
    *id_out = 0;

    FILE *f = fopen(IMAGE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        uint32_t id = 0;
        char h[HASH_HEX_LEN + 1] = {0};
        uint32_t used = 0;
        if (!parse_csv_line(line, &id, h, sizeof(h), &used)) continue;
        if (id == 0) continue;
        if (strcmp(h, hash) == 0) {
            *id_out = id;
            fclose(f);
            return ESP_OK;
        }
    }
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

/* ---- file copy (streamed) ---- */

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return false;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }

    static uint8_t buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) unlink(dst);
    return ok;
}

/* ---- public: add / mark_used / remove / init ---- */

esp_err_t image_library_add(const char *src_path, uint32_t *id_out) {
    if (!src_path || !id_out) return ESP_ERR_INVALID_ARG;
    *id_out = 0;

    struct stat st;
    if (stat(src_path, &st) != 0) {
        ESP_LOGW(TAG, "add: source not found: %s", src_path);
        return ESP_ERR_NOT_FOUND;
    }
    if (!is_png_file(src_path)) {
        ESP_LOGW(TAG, "add: %s is not a PNG", src_path);
        return ESP_ERR_INVALID_ARG;
    }

    char hash[HASH_HEX_LEN + 1] = {0};
    esp_err_t err = image_library_hash_file(src_path, hash, sizeof(hash));
    if (err != ESP_OK) return err;

    /* dedupe by hash */
    uint32_t existing = 0;
    if (image_library_find_by_hash(hash, &existing) == ESP_OK && existing != 0) {
        ESP_LOGI(TAG, "add: duplicate of %08lu (hash %s) — not copied",
                 (unsigned long)existing, hash);
        *id_out = existing;
        return ESP_OK;
    }

    uint32_t id = 0;
    err = image_library_get_next_id(&id);
    if (err != ESP_OK || id == 0) {
        ESP_LOGE(TAG, "add: cannot allocate id");
        return err != ESP_OK ? err : ESP_FAIL;
    }

    char dst[96];
    image_library_get_path(id, dst, sizeof(dst));

    char dir[64];
    snprintf(dir, sizeof(dir), "%s/%03lu", IMAGE_LIB_ROOT,
             (unsigned long)((id - 1) / IMAGE_LIB_PER_DIR));
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "add: mkdir %s: errno %d", dir, errno);
    }

    if (!copy_file(src_path, dst)) {
        ESP_LOGE(TAG, "add: copy %s -> %s failed", src_path, dst);
        return ESP_FAIL;
    }

    if (!append_csv_row(id, hash)) {
        ESP_LOGE(TAG, "add: csv append failed — removing %s", dst);
        unlink(dst);
        return ESP_FAIL;
    }

    *id_out = id;
    invalidate_id_cache();
    ESP_LOGI(TAG, "add: id=%08lu hash=%s", (unsigned long)id, hash);
    return ESP_OK;
}

esp_err_t image_library_mark_used(uint32_t id) {
    if (id == 0) return ESP_ERR_INVALID_ARG;

    time_t now = time(NULL);
    /* Only trust wall-clock after SNTP sync; otherwise leave last_used_at empty. */
    bool have_time = (now > 946684800L);   /* 2000-01-01 */

    FILE *in = fopen(IMAGE_LIB_CSV_PATH, "r");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *tmp = fopen(IMAGE_LIB_TMP_PATH, "w");
    if (!tmp) { fclose(in); return ESP_FAIL; }

    bool updated = false;
    char line[160];
    while (fgets(line, sizeof(line), in)) {
        char copy[160];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        uint32_t rid = 0;
        char h[HASH_HEX_LEN + 1] = {0};
        uint32_t used = 0;
        if (parse_csv_line(copy, &rid, h, sizeof(h), &used) && rid == id) {
            used++;
            if (have_time) {
                struct tm tmv;
                localtime_r(&now, &tmv);
                fprintf(tmp, "%08lu,%s,%lu,%04d-%02d-%02d %02d:%02d:%02d\n",
                        (unsigned long)rid, h, (unsigned long)used,
                        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            } else {
                fprintf(tmp, "%08lu,%s,%lu,\n", (unsigned long)rid, h, (unsigned long)used);
            }
            updated = true;
        } else {
            fputs(line, tmp);   /* header or non-target row: copy verbatim */
        }
    }
    fclose(in);
    if (fclose(tmp) != 0) { unlink(IMAGE_LIB_TMP_PATH); return ESP_FAIL; }

    if (!updated) {
        ESP_LOGW(TAG, "mark_used: id %lu not found in CSV", (unsigned long)id);
        unlink(IMAGE_LIB_TMP_PATH);
        return ESP_ERR_NOT_FOUND;
    }

    unlink(IMAGE_LIB_CSV_PATH);   /* FATFS f_rename won't overwrite an existing target */
    if (rename(IMAGE_LIB_TMP_PATH, IMAGE_LIB_CSV_PATH) != 0) {
        ESP_LOGE(TAG, "mark_used: rename failed");
        unlink(IMAGE_LIB_TMP_PATH);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "mark_used: id %lu used_count updated", (unsigned long)id);
    return ESP_OK;
}

esp_err_t image_library_get_used_count(uint32_t id, uint32_t *used_count_out) {
    if (!used_count_out) return ESP_ERR_INVALID_ARG;
    *used_count_out = 0;
    if (id == 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(IMAGE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char copy[160];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        uint32_t rid = 0;
        char h[HASH_HEX_LEN + 1] = {0};
        uint32_t used = 0;
        if (parse_csv_line(copy, &rid, h, sizeof(h), &used) && rid == id) {
            *used_count_out = used;
            fclose(f);
            return ESP_OK;
        }
    }
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t image_library_get_stats(image_library_stats_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->min_used = 0xFFFFFFFF;

    FILE *f = fopen(IMAGE_LIB_CSV_PATH, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[160];
    while (fgets(line, sizeof(line), f)) {
        char copy[160];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        uint32_t id = 0;
        char h[HASH_HEX_LEN + 1] = {0};
        uint32_t used = 0;
        if (!parse_csv_line(copy, &id, h, sizeof(h), &used)) continue;
        out->count++;
        out->total_used += used;
        if (used > out->max_used) { out->max_used = used; out->max_used_id = id; }
        if (used < out->min_used) { out->min_used = used; out->min_used_id = id; }
    }
    fclose(f);

    if (out->count == 0) out->min_used = 0;
    return ESP_OK;
}

esp_err_t image_library_remove(uint32_t id) {
    if (id == 0) return ESP_ERR_INVALID_ARG;

    char path[96];
    image_library_get_path(id, path, sizeof(path));
    /* deleting the file is the important part; ignore unlink error so we can
       still drop the CSV row below */
    unlink(path);

    FILE *in = fopen(IMAGE_LIB_CSV_PATH, "r");
    if (!in) return ESP_ERR_NOT_FOUND;
    FILE *tmp = fopen(IMAGE_LIB_TMP_PATH, "w");
    if (!tmp) { fclose(in); return ESP_FAIL; }

    bool removed = false;
    char line[160];
    while (fgets(line, sizeof(line), in)) {
        char copy[160];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        uint32_t rid = 0;
        char h[HASH_HEX_LEN + 1] = {0};
        uint32_t used = 0;
        if (parse_csv_line(copy, &rid, h, sizeof(h), &used) && rid == id) {
            removed = true;   /* skip this row */
        } else {
            fputs(line, tmp);
        }
    }
    fclose(in);
    if (fclose(tmp) != 0) { unlink(IMAGE_LIB_TMP_PATH); return ESP_FAIL; }

    if (!removed) { unlink(IMAGE_LIB_TMP_PATH); return ESP_ERR_NOT_FOUND; }

    unlink(IMAGE_LIB_CSV_PATH);   /* FATFS f_rename won't overwrite an existing target */
    if (rename(IMAGE_LIB_TMP_PATH, IMAGE_LIB_CSV_PATH) != 0) {
        unlink(IMAGE_LIB_TMP_PATH);
        return ESP_FAIL;
    }
    invalidate_id_cache();
    return ESP_OK;
}

esp_err_t image_library_init(void) {
    struct stat st;
    if (stat(IMAGE_LIB_ROOT, &st) != 0) {
        if (mkdir(IMAGE_LIB_ROOT, 0777) != 0 && errno != EEXIST) {
            ESP_LOGW(TAG, "init: mkdir %s: errno %d", IMAGE_LIB_ROOT, errno);
            return ESP_FAIL;
        }
    }
    ensure_csv_header();
    ESP_LOGI(TAG, "image library ready (root=%s)", IMAGE_LIB_ROOT);
    return ESP_OK;
}
