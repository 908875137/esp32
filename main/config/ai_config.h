/*
 * ai_config.h
 *
 * AI API configuration — ALL keys/URLs/tokens in ONE place.
 *
 * Tencent TokenHub provides two models under the same API Key:
 *   - deepseek-v4-pro-202606  → Chat Completions  → daily quote
 *   - hy-image-v3              → Image Generation  → artwork
 */
#pragma once

/* ---- Shared API auth ---- */
#define AI_API_KEY   "sk-e7PepnmYbu6iPfjNnZo2NpRgYVKcgvDVuKlUpy2eo8KxaVpR"
#define AI_HTTP_USER_AGENT  "ThinkINK-ESP32/1.0"

/* ---- Endpoints ---- */
#define AI_IMAGE_URL  "https://tokenhub.tencentmaas.com/v1/wand/hunyuan-image/v3-generation"
#define AI_CHAT_URL   "https://tokenhub.tencentmaas.com/v1/chat/completions"

/* ---- Model IDs ---- */
#define AI_IMAGE_MODEL  "hy-image-v3"
#define AI_CHAT_MODEL   "deepseek-v4-pro-202606"

/* ---- HTTP client settings ---- */
#define AI_IMAGE_TIMEOUT_MS  120000  /* image generation can be slow */
#define AI_CHAT_TIMEOUT_MS    30000  /* chat completion is fast */

/* ---- Buffer sizes ---- */
#define AI_IMAGE_URL_MAX_LEN       2048
#define AI_REVISED_PROMPT_MAX_LEN  4096
#define AI_CHAT_RESPONSE_MAX_LEN   8192

/*
 * System prompt for DeepSeek — it selects the quote, NOT Hunyuan.
 */
#define AI_QUOTE_SYSTEM_PROMPT \
    "你是一个\"每日一句\"文学内容策划助手。\n" \
    "每天选择一句真正值得反复阅读的话。\n" \
    "来源不限：经典电影台词、电视剧台词、小说、剧本、诗歌、哲学、心理学、文学作品、\n" \
    "历史人物、思想家、作家、艺术家等具有明确出处的人物言论皆可。\n\n" \
    "不要选择：\"沉默是金\"\"坚持就是胜利\"\"时间会证明一切\"等鸡汤、成功学、企业文化式名言。\n" \
    "优先选择有文学性、思想深度、故事感、画面感、人生意味、复杂情绪、余味的句子。\n" \
    "情绪可以多样化：温暖、浪漫、幽默、荒诞、可爱、治愈、明亮、青春、怀旧、哲思、孤独、遗憾。\n" \
    "尽量选择真实存在、出处明确的经典句子，不要编造。\n\n" \
    "只输出JSON，不要输出Markdown，不要输出解释。格式：\n" \
    "{\"quote\":\"原文\",\"author\":\"作者\",\"source\":\"作品或出处\",\"type\":\"类别\",\"mood\":\"情绪\"}"

/*
 * User prompt for DeepSeek — asks for one fresh quote each call.
 */
#define AI_QUOTE_USER_PROMPT  "请为我生成今天的一句话。"
