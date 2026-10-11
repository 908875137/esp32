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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
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

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", AI_QUOTE_USER_PROMPT);
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
 * Prompt builder: daily_quote_t → Hunyuan image prompt
 * ================================================================ */

void ai_build_image_prompt(const daily_quote_t *quote,
                           char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';

    const char *q = quote && quote->quote[0]   ? quote->quote   : "";
    const char *a = quote && quote->author[0]  ? quote->author  : "未知";
    const char *s = quote && quote->source[0]  ? quote->source  : "未知";
    const char *m = quote && quote->mood[0]    ? quote->mood    : "";

    snprintf(buf, buf_size,
        "为 7.5 英寸屏创作一幅每日艺术作品。画面尺寸 854×512，横向构图。\n\n"

        "【核心灵感】\n"
        "这是一句来自作者\"%s\"、作品/出处\"%s\"的文字：\"%s\"。\n"
        "这句话的情绪：%s。\n\n"
        
        "请根据这句话的含义、情绪、故事和时代背景，构建一个完整、有故事感的纯视觉场景。\n"
        "不要直接表现文字本身，而是用画面表达这句话的精神和情绪。\n"
        "让画面本身成为主要内容。\n\n"

        "【视觉主体选择】\n"
        "优先选择建筑、城市街景、自然风景、室内空间、生活物件、动物、交通工具或具有故事感的环境作为视觉主体。\n"
        "可以使用人物，但人物不是默认主体，只作为完整场景中的叙事元素。\n"
        "优先通过环境、物体、光影、空间和动作表达情绪，而不是依靠人物脸部表达情绪。\n\n"

        "【生物与昆虫限制】\n"
        "绝对不要出现大量虫子、昆虫、飞虫、爬虫或成群结队的虫子。\n"
        "绝对不要出现巨大的虫子或昆虫特写，不要画出放大的虫类作为视觉主体。\n"
        "不要因为文字中提到了虫子、跳蚤、虱子等意象，就在画面中直接绘制大量真实的虫子。\n"
        "最多允许偶尔出现一只很小的苍蝇、蜜蜂或蝴蝶作为自然环境的点缀，且必须很小、不显眼、远离视觉焦点。\n"
        "优先用隐喻、抽象或象征的方式表达这类意象，而不是直接画出真实的虫子。\n\n"

        "【人物限制】\n"
        "不要把人物肖像作为画面主体。\n"
        "禁止人物正脸特写、侧脸特写、半身肖像、证件照式构图、美颜人像、人物写真。\n"
        "不要让人物的脸占据画面主要视觉区域。\n"
        "如果需要人物，优先使用远景、背影、剪影、小比例人物或多人场景。\n"
        "人物通常不应超过画面面积的 15%%。\n"
        "不要让观众的第一视觉焦点落在人脸上。\n\n"

        "【严格禁止】\n"
        "画面上绝对不能出现任何文字、字幕、标题、Logo、字母、数字、水印、签名、书法、符号。\n"
        "不要绘制作者名字、作品名称、原文台词。\n"
        "不要绘制名言海报、励志海报或文字卡片。\n"
        "所有文字由外部系统在图片生成后单独叠加。\n\n"

        "【构图】\n"
        "主体视觉内容应该占据画面绝大部分区域，画面内容覆盖约 80%%～95%%。\n"
        "不要出现中央一个小主体加四周大面积空白的构图。\n"
        "不要出现大面积纯白或纯黑的空白区域。\n"
        "不要把画面设计成上下两个独立区域。\n"
        "使用完整场景、前中后景、三分法、对角线、框景或自然视觉引导线构建丰富的空间层次。\n\n"

        "【文字区域】\n"
        "不要为文字专门划出独立区域。\n"
        "不要产生明显的底栏、字幕栏、信息栏、矩形文字区域或分割线。\n"
        "画面首先必须作为一幅完整的纯视觉艺术作品成立。\n"
        "可以在自然存在的低复杂度区域保留少量视觉空间，例如天空、墙面、雾气、地面或简单背景。\n"
        "这些区域必须自然融入场景，不能看起来像专门为文字预留的空白。\n\n"

        "【色彩】\n"
        "避免大面积纯黑，避免整体阴暗压抑。\n"
        "保持明亮、通透、有层次的画面。\n"
        "允许温暖、轻松、可爱、浪漫、快乐、明亮的视觉作品。\n"

        "【整体感觉】\n"
        "电影感、文学感、艺术感、高级、自然、克制、有故事、有画面感。\n"
        "根据每句话的内容自动选择不同的视觉风格，不要固定模板。\n"
        "可以是电影感艺术摄影、城市生活、自然风景、温暖日常、动物、旅行、复古电影、现代插画、日系动画、欧美动画、童话或轻松可爱的卡通等。\n"
        "不要所有作品都使用人物、雨夜、孤独背影、窗边人物等套路。\n"
        "不要像企业宣传海报、励志海报、名言卡片或手机壁纸模板。\n"
        "不要使用随机几何图形、装饰性色块、书法、毛笔、砚台、古典书房等模板化元素。\n\n"

        "【最终要求】\n"
        "只生成完整的纯视觉艺术画面。\n"
        "不要生成任何文字、字母、数字、Logo、水印或符号。\n"
        "文字全部由外部系统在图片生成后单独叠加。",
        a, s, q, m);
}
