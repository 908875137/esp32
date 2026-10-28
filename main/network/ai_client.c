/*
 * ai_client.c
 *
 * Two-stage AI pipeline:
 *   Stage 1 — DeepSeek Chat Completions → daily quote JSON
 *   Stage 2 — Hunyuan Image Generation → data[0].url
 *
 * Both use esp_http_client + cJSON + esp_crt_bundle.
 * Accumulator is shared across both stages (same pattern).
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "ai_config.h"
#include "ai_client.h"

static const char *TAG = "ai";

/* ---- Accumulator for streaming HTTP response (shared) ---- */

#define ACCUM_CHUNK  4096

typedef struct {
    char  *buf;
    int    len;          /* used bytes */
    int    cap;          /* allocated bytes */
} accum_t;

static void accum_init(accum_t *a) {
    a->buf = (char *)malloc(ACCUM_CHUNK);
    a->cap = ACCUM_CHUNK;
    a->len = 0;
    if (a->buf) a->buf[0] = '\0';
}

static void accum_free(accum_t *a) {
    free(a->buf);
    a->buf = NULL;
    a->len = 0;
    a->cap = 0;
}

static esp_err_t accum_append(accum_t *a, const char *src, int srclen) {
    if (srclen <= 0) return ESP_OK;
    int needed = a->len + srclen + 1;
    if (needed > a->cap) {
        int new_cap = a->cap;
        while (new_cap < needed) new_cap += ACCUM_CHUNK;
        char *p = (char *)realloc(a->buf, new_cap);
        if (!p) {
            ESP_LOGE(TAG, "OOM: accum realloc %d", new_cap);
            return ESP_ERR_NO_MEM;
        }
        a->buf = p;
        a->cap = new_cap;
    }
    memcpy(a->buf + a->len, src, srclen);
    a->len += srclen;
    a->buf[a->len] = '\0';
    return ESP_OK;
}

/* ---- HTTP event handler ---- */

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    accum_t *acc = (accum_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        return accum_append(acc, (const char *)evt->data, evt->data_len);
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP event error");
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ---- Shared helper: do a POST with JSON body, return response string ---- */

static char *http_post_json(const char *url, const char *json_body,
                            int timeout_ms, int *out_status) {
    accum_t acc;
    accum_init(&acc);
    if (!acc.buf) return NULL;

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = timeout_ms,
        .user_agent        = AI_HTTP_USER_AGENT,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event_handler,
        .user_data         = &acc,
        .buffer_size       = 4096,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { accum_free(&acc); return NULL; }

    char auth_hdr[320];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", ai_api_key);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, (int)strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    *out_status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || *out_status != 200) {
        if (acc.len > 0) {
            ESP_LOGE(TAG, "Response (status=%d): %.500s", *out_status, acc.buf);
        }
        accum_free(&acc);
        return NULL;
    }

    if (acc.len <= 0) { accum_free(&acc); return NULL; }
    return acc.buf;  /* caller must free() */
}

/* ================================================================
 * Quote history — remember the most recent generated quotes so we can feed
 * them back to DeepSeek and avoid repetition.
 *
 * Format mirrors quotes.csv's core columns (one row per line, quoted CSV):
 *   "text","author","source"
 * ================================================================ */

#define AI_HISTORY_PATH      "/sdcard/ai_history.txt"
#define AI_HISTORY_BUF       2048           /* dedup prompt buffer */
#define AI_HISTORY_MAX_FILE  (128 * 1024)   /* cap on the history file read */

/* Parse one CSV field starting at *pp into out.  Handles quoted fields ("" is
   an escaped quote).  Advances *pp past the field and any following comma. */
static bool csv_field(const char **pp, char *out, size_t out_len) {
    const char *p = *pp;
    out[0] = '\0';
    if (!p || !*p) return false;
    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p && o < out_len - 1) {
            if (*p == '"') {
                if (p[1] == '"') { out[o++] = '"'; p += 2; }
                else { p++; break; }
            } else {
                out[o++] = *p++;
            }
        }
        out[o] = '\0';
        if (*p == ',') p++;
        *pp = p;
        return true;
    }
    while (*p && *p != ',' && *p != '\r' && *p != '\n' && o < out_len - 1) out[o++] = *p++;
    out[o] = '\0';
    if (*p == ',') p++;
    *pp = p;
    return true;
}

/* Append one generated quote as a CSV row (text/author/source).  POSIX
   O_APPEND because stdio fopen("a") does not flush on FATFS. */
static void append_ai_history(const daily_quote_t *q) {
    if (!q || !q->quote[0]) return;
    int fd = open(AI_HISTORY_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd < 0) return;
    char line[1024];
    int n = snprintf(line, sizeof(line), "\"%s\",\"%s\",\"%s\"\n",
                     q->quote, q->author, q->source);
    if (n > 0) write(fd, line, (size_t)n);
    close(fd);
}

/* Read the AI history into out (newest first).  Returns the number of entries. */
int ai_history_read(ai_history_entry_t *out, int max) {
    if (!out || max <= 0) return 0;

    FILE *f = fopen(AI_HISTORY_PATH, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) size = 0;
    if (size > AI_HISTORY_MAX_FILE) size = AI_HISTORY_MAX_FILE;

    char *raw = (char *)malloc((size_t)size + 1);
    if (!raw) { fclose(f); return 0; }
    size_t n = fread(raw, 1, (size_t)size, f);
    fclose(f);
    raw[n] = '\0';

    int count = 0;
    char *line = raw;
    while (line < raw + n && count < max) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        const char *p = line;
        char text[512], author[128], source[256];
        csv_field(&p, text, sizeof(text));
        csv_field(&p, author, sizeof(author));
        csv_field(&p, source, sizeof(source));

        if (text[0] != '\0') {
            strncpy(out[count].text, text, sizeof(out[count].text) - 1);
            out[count].text[sizeof(out[count].text) - 1] = '\0';
            strncpy(out[count].author, author, sizeof(out[count].author) - 1);
            out[count].author[sizeof(out[count].author) - 1] = '\0';
            strncpy(out[count].source, source, sizeof(out[count].source) - 1);
            out[count].source[sizeof(out[count].source) - 1] = '\0';
            count++;
        }
        if (!end) break;
        line = end + 1;
    }
    free(raw);

    for (int i = 0, j = count - 1; i < j; i++, j--) {
        ai_history_entry_t tmp = out[i];
        out[i] = out[j];
        out[j] = tmp;
    }
    return count;
}

/* Format the recent history as plain "quote —— author" lines for the DeepSeek
   prompt.  Returns bytes written (0 if none). */
static size_t read_ai_history(char *buf, size_t buf_size) {
    buf[0] = '\0';
    FILE *f = fopen(AI_HISTORY_PATH, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }
    size_t want = (size_t)size > buf_size - 1 ? buf_size - 1 : (size_t)size;
    fseek(f, size - (long)want, SEEK_SET);

    char *raw = (char *)malloc(want + 1);
    if (!raw) { fclose(f); return 0; }
    size_t n = fread(raw, 1, want, f);
    fclose(f);
    raw[n] = '\0';

    size_t o = 0;
    char *line = raw;
    while (line < raw + n && o < buf_size - 1) {
        char *end = strchr(line, '\n');
        if (end) *end = '\0';
        const char *p = line;
        char text[512], author[128];
        csv_field(&p, text, sizeof(text));
        csv_field(&p, author, sizeof(author));
        if (text[0] != '\0') {
            int w = snprintf(buf + o, buf_size - o, "%s%s%s\n",
                             text, author[0] ? " —— " : "", author);
            if (w < 0) break;
            if ((size_t)w >= buf_size - o) { o = buf_size - 1; break; }
            o += (size_t)w;
        }
        if (!end) break;
        line = end + 1;
    }
    free(raw);
    return o;
}

/* ================================================================
 * Stage 1: DeepSeek → daily quote
 * ================================================================ */

esp_err_t ai_generate_daily_quote(daily_quote_t *quote) {
    if (!quote) return ESP_ERR_INVALID_ARG;
    memset(quote, 0, sizeof(*quote));

    ESP_LOGI(TAG, "=== Stage 1: DeepSeek daily quote ===");

    /* Build Chat Completions JSON body */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", ai_chat_model);
    cJSON_AddBoolToObject(root, "stream", false);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");

    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", AI_QUOTE_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys_msg);

    /* Build the user prompt — include recent history so the model avoids
       repeating or near-repeating previous quotes. */
    char history[AI_HISTORY_BUF] = {0};
    read_ai_history(history, sizeof(history));

    char user_prompt[AI_HISTORY_BUF + 256];
    if (history[0] != '\0') {
        snprintf(user_prompt, sizeof(user_prompt),
            "%s\n\n以下是最近已经生成过的句子，请务必避免重复，也不要过于相似：\n%s",
            AI_QUOTE_USER_PROMPT, history);
    } else {
        snprintf(user_prompt, sizeof(user_prompt), "%s", AI_QUOTE_USER_PROMPT);
    }

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", user_prompt);
    cJSON_AddItemToArray(messages, user_msg);

    char *req_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_str) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "DeepSeek request: %d bytes", (int)strlen(req_str));
    ESP_LOGI(TAG, "DeepSeek body: %s", req_str);

    int status = 0;
    char *resp = http_post_json(ai_chat_url, req_str, AI_CHAT_TIMEOUT_MS, &status);
    free(req_str);

    if (!resp) {
        ESP_LOGE(TAG, "DeepSeek API failed (status=%d)", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DeepSeek response (%d bytes): %s", (int)strlen(resp), resp);

    /* Parse Chat Completions JSON → choices[0].message.content */
    cJSON *resp_root = cJSON_Parse(resp);
    if (!resp_root) {
        ESP_LOGE(TAG, "DeepSeek JSON parse failed. Raw: %.200s", resp);
        free(resp);
        return ESP_FAIL;
    }

    /* Prefer reasoning_content if present, otherwise content */
    const char *raw_content = NULL;
    cJSON *choices = cJSON_GetObjectItem(resp_root, "choices");
    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
        cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
        cJSON *msg = cJSON_GetObjectItem(choice0, "message");
        if (msg) {
            cJSON *content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(content) && content->valuestring) {
                raw_content = content->valuestring;
            }
        }
    }

    if (!raw_content || raw_content[0] == '\0') {
        ESP_LOGE(TAG, "DeepSeek: empty content field");
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Quote JSON content: %s", raw_content);

    /* Parse the inner JSON within content → {quote, author, source, type, mood} */
    cJSON *quote_json = cJSON_Parse(raw_content);
    if (!quote_json) {
        /* Sometimes the model wraps JSON in markdown code fences */
        const char *p = strstr(raw_content, "{");
        if (p) {
            quote_json = cJSON_Parse(p);
        }
    }
    if (!quote_json) {
        ESP_LOGE(TAG, "DeepSeek: content is not valid JSON. Raw: %.300s", raw_content);
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *q = cJSON_GetObjectItem(quote_json, "quote");
    cJSON *a = cJSON_GetObjectItem(quote_json, "author");
    cJSON *s = cJSON_GetObjectItem(quote_json, "source");
    cJSON *t = cJSON_GetObjectItem(quote_json, "type");
    cJSON *m = cJSON_GetObjectItem(quote_json, "mood");

    if (!cJSON_IsString(q) || !q->valuestring ||
        !cJSON_IsString(a) || !a->valuestring) {
        ESP_LOGE(TAG, "DeepSeek: quote or author missing in JSON");
        cJSON_Delete(quote_json);
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    strncpy(quote->quote,  q->valuestring, sizeof(quote->quote) - 1);
    strncpy(quote->author, a->valuestring, sizeof(quote->author) - 1);
    if (cJSON_IsString(s) && s->valuestring)
        strncpy(quote->source, s->valuestring, sizeof(quote->source) - 1);
    if (cJSON_IsString(t) && t->valuestring)
        strncpy(quote->type,   t->valuestring, sizeof(quote->type) - 1);
    if (cJSON_IsString(m) && m->valuestring)
        strncpy(quote->mood,   m->valuestring, sizeof(quote->mood) - 1);

    cJSON_Delete(quote_json);
    cJSON_Delete(resp_root);
    free(resp);

    ESP_LOGI(TAG, "Quote: \"%s\"", quote->quote);
    ESP_LOGI(TAG, "Author: %s", quote->author);
    ESP_LOGI(TAG, "Source: %s", quote->source);
    ESP_LOGI(TAG, "Type: %s", quote->type);
    ESP_LOGI(TAG, "Mood: %s", quote->mood);

    /* Remember this quote (text/author/source) so the next generation avoids it. */
    append_ai_history(quote);

    return ESP_OK;
}

/* ================================================================
 * Stage 2: Hunyuan Image → image URL
 * ================================================================ */

esp_err_t ai_generate_image(const char *prompt, const char *size,
                            ai_image_result_t *result) {
    if (!prompt || !size || !result) return ESP_ERR_INVALID_ARG;
    memset(result, 0, sizeof(*result));

    ESP_LOGI(TAG, "=== Stage 2: Hunyuan image generation ===");

    cJSON *req_root = cJSON_CreateObject();
    cJSON_AddStringToObject(req_root, "model", ai_image_model);
    cJSON_AddStringToObject(req_root, "prompt", prompt);
    cJSON_AddStringToObject(req_root, "size", size);

    char *req_str = cJSON_PrintUnformatted(req_root);
    cJSON_Delete(req_root);
    if (!req_str) return ESP_ERR_NO_MEM;

    int prompt_len = (int)strlen(prompt);
    ESP_LOGI(TAG, "Image request: %d bytes, prompt=%d chars", (int)strlen(req_str), prompt_len);
    ESP_LOGI(TAG, "Image body: %s", req_str);

    int status = 0;
    char *resp = http_post_json(ai_image_url, req_str, AI_IMAGE_TIMEOUT_MS, &status);
    free(req_str);

    if (!resp) {
        ESP_LOGE(TAG, "Image API failed (status=%d)", status);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Image response (%d bytes): %s", (int)strlen(resp), resp);

    cJSON *resp_root = cJSON_Parse(resp);
    if (!resp_root) {
        ESP_LOGE(TAG, "Image JSON parse failed. Raw: %.200s", resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(resp_root, "data");
    if (!cJSON_IsArray(data) || cJSON_GetArraySize(data) <= 0) {
        ESP_LOGE(TAG, "Image JSON: 'data' missing or empty");
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *item = cJSON_GetArrayItem(data, 0);
    if (!item) {
        ESP_LOGE(TAG, "Image JSON: data[0] is null");
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *url = cJSON_GetObjectItem(item, "url");
    if (!cJSON_IsString(url) || !url->valuestring) {
        ESP_LOGE(TAG, "Image JSON: data[0].url missing");
        cJSON_Delete(resp_root);
        free(resp);
        return ESP_FAIL;
    }

    strncpy(result->image_url, url->valuestring, sizeof(result->image_url) - 1);
    result->image_url[sizeof(result->image_url) - 1] = '\0';
    ESP_LOGI(TAG, "Image URL: length=%d", (int)strlen(result->image_url));

    cJSON *revised = cJSON_GetObjectItem(item, "revised_prompt");
    if (cJSON_IsString(revised) && revised->valuestring) {
        strncpy(result->revised_prompt, revised->valuestring,
                sizeof(result->revised_prompt) - 1);
        result->revised_prompt[sizeof(result->revised_prompt) - 1] = '\0';
        ESP_LOGI(TAG, "Revised prompt: %.200s...", result->revised_prompt);
    }

    cJSON_Delete(resp_root);
    free(resp);
    return ESP_OK;
}

/* ================================================================
 * Prompt builder: random pretty/cozy/fun theme → Hunyuan image prompt.
 * The image is now INDEPENDENT of the daily quote — pick a random theme and
 * style, so the artwork is always fresh and pleasant rather than a literal
 * illustration of the text.
 * ================================================================ */

void ai_build_image_prompt(const daily_quote_t *quote,
                           char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    /* Randomly pick one pleasant theme — this is what makes each image
       different, not the quote. */
    static const char *themes[] = {
        "一只可爱的小动物（小猫、小狗、兔子、狐狸、熊猫等）在温暖治愈的场景里",
        "温馨的日常生活一隅（阳光洒进窗边、书桌、咖啡杯、绿植、翻开的书）",
        "治愈的自然风景（海边、森林小径、山间云雾、草原花田）",
        "梦幻的星空、云海、极光或月夜",
        "有趣的童话插画场景，充满想象力",
        "精致的美食或甜点特写（蛋糕、咖啡、水果）",
        "复古的旅行风景（老式火车、海边小镇、石板老街）",
        "活泼的城市街角（花店、书店、咖啡馆、清晨的市场）",
    };
    int t = esp_random() % (sizeof(themes) / sizeof(themes[0]));

    snprintf(buf, buf_size,
        "创作一幅适合电子相框显示的横版（854×512）视觉艺术作品。\n"
        "本次主题：%s。\n\n"

        "【风格】\n"
        "任选一种：清新插画、日系治愈、水彩、电影感摄影、温暖童话、可爱卡通。\n"
        "整体要好看、可爱、有趣、治愈、温暖。\n\n"

        "【构图与色彩】\n"
        "画面明亮通透、色彩丰富有层次，主体占据画面绝大部分区域，构图饱满。\n"
        "要有故事感和氛围感，不要中央一个小主体加四周大面积空白。\n\n"

        "【严格禁止】\n"
        "不要出现任何文字、字母、数字、Logo、水印、签名。\n"
        "不要大量虫子昆虫。\n"
        "不要人物正脸特写、证件照式构图。\n"
        "不要阴暗压抑、大面积纯黑。",
        themes[t]);
}
