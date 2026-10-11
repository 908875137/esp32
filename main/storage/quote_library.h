/*
 * quote_library.h
 *
 * Offline SD-card quote/text library (phase 2).
 *
 * Quotes live in /sdcard/quotes.csv:
 *
 *   id,text,author,source,used_count,last_used_at
 *   00000001,"慢一点，也没有关系。","","原创",0,
 *
 * The user edits quotes.csv on a computer and re-inserts the card — no
 * reflash is needed to change quotes.  The device only reads, chooses and
 * marks quotes as used; it does not need to keep the whole file in RAM
 * (processed one line at a time).
 *
 * A small add_quote() is also provided (normalise + exact-text dedupe +
 * id allocation) for the future Web import stage; it is not wired to any UI
 * yet.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QUOTE_LIB_CSV_PATH   "/sdcard/quotes.csv"
#define QUOTE_TEXT_MAX       512
#define QUOTE_AUTHOR_MAX     128
#define QUOTE_SOURCE_MAX     128

typedef struct {
    uint32_t id;
    char     text[QUOTE_TEXT_MAX];
    char     author[QUOTE_AUTHOR_MAX];
    char     source[QUOTE_SOURCE_MAX];
    uint32_t used_count;
} quote_t;

/**
 * Count quotes in quotes.csv (streamed, never loaded whole into RAM).
 * Returns 0 for a missing/empty library.
 */
esp_err_t quote_library_count(uint32_t *count_out);

/**
 * Fetch the quote with the given id.  ESP_ERR_NOT_FOUND if absent.
 */
esp_err_t quote_library_get_by_id(uint32_t id, quote_t *out);

/**
 * Fetch the index-th quote (0-based, file order).  ESP_ERR_NOT_FOUND if
 * out of range.
 */
esp_err_t quote_library_get_by_index(uint32_t index, quote_t *out);

/**
 * Fetch one page of quotes: skip `offset` rows, copy up to `limit` rows into
 * `out` (caller allocates limit * sizeof(quote_t)).  *out_count receives the
 * number actually copied.  Reads the CSV exactly once (never whole into RAM).
 */
esp_err_t quote_library_get_page(uint32_t offset, uint32_t limit,
                                 quote_t *out, uint32_t *out_count);

/**
 * Choose a quote for display: the one with the lowest used_count (ties go
 * to the first found).  ESP_ERR_NOT_FOUND if the library is empty.
 */
esp_err_t quote_library_choose(quote_t *out);

/**
 * Choose a quote uniformly at random.  ESP_ERR_NOT_FOUND if the library is
 * empty.
 */
esp_err_t quote_library_get_random(quote_t *out);

/**
 * Record that a quote was actually shown: used_count+1 and last_used_at
 * updated (left empty without a trustworthy clock).  Rewrites quotes.csv
 * via a tmp file + rename to survive power loss.
 */
esp_err_t quote_library_mark_used(uint32_t id);

/**
 * Remove a quote: drop its quotes.csv row.
 */
esp_err_t quote_library_remove(uint32_t id);

/**
 * Update a quote's text/author/source (text is normalised).  Rewrites
 * quotes.csv via tmp + rename.
 */
esp_err_t quote_library_update(uint32_t id, const char *text, const char *author,
                               const char *source);

/**
 * Library statistics, read in one streaming pass over quotes.csv.
 */
typedef struct {
    uint32_t count;
    uint32_t total_used;
    uint32_t max_used_id;
    uint32_t max_used;
    uint32_t min_used_id;
    uint32_t min_used;
} quote_library_stats_t;

esp_err_t quote_library_get_stats(quote_library_stats_t *out);

/**
 * Add a quote (normalise whitespace/BOM, skip if the exact text already
 * exists, allocate the next id, append to quotes.csv).  Returns the new or
 * existing id.  Not wired to any UI in phase 2.
 */
esp_err_t quote_library_add(const char *text, const char *author, const char *source,
                            uint32_t *id_out);

#ifdef __cplusplus
}
#endif
