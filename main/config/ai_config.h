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
#define AI_IMAGE_URL_MAX_LEN    2048
#define AI_REVISED_PROMPT_MAX_LEN  4096
#define AI_CHAT_RESPONSE_MAX_LEN   8192

/*
 * System prompt for DeepSeek — it selects the quote, NOT Hunyuan.
 */
#define AI_QUOTE_SYSTEM_PROMPT \
    "你是一个"每日一句"文学内容策划助手。\n" \
    "每天选择一句真正值得反复阅读的话。\n" \
    "来源不限：经典电影台词、电视剧台词、小说、剧本、诗歌、哲学、心理学、文学作品、\n" \
    "历史人物、思想家、作家、艺术家等具有明确出处的人物言论皆可。\n\n" \
    "不要选择："沉默是金""坚持就是胜利""时间会证明一切"等鸡汤、成功学、企业文化式名言。\n" \
    "优先选择有文学性、思想深度、故事感、画面感、人生意味、复杂情绪、余味的句子。\n" \
    "情绪可以多样化：温暖、浪漫、幽默、荒诞、可爱、治愈、明亮、青春、怀旧、哲思、孤独、遗憾。\n" \
    "尽量选择真实存在、出处明确的经典句子，不要编造。\n\n" \
    "只输出JSON，不要输出Markdown，不要输出解释。格式：\n" \
    "{\"quote\":\"原文\",\"author\":\"作者\",\"source\":\"作品或出处\",\"type\":\"类别\",\"mood\":\"情绪\"}"

/*
 * User prompt for DeepSeek — asks for one fresh quote each call.
 */
#define AI_QUOTE_USER_PROMPT  "请为我生成今天的一句话。"

/* ---- Image URL buffer ---- */
#define AI_IMAGE_URL_MAX_LEN  2048
#define AI_REVISED_PROMPT_MAX_LEN  4096

/*
 * EPD-optimized daily-art prompt for 7.5" 800×480 tri-color (B/W/R) display.
 *
 * API sends size=854x512 which is a valid preset, close to 5:3,
 * and within the 1024×1024 pixel-area limit (854×512 = 437 248 px).
 */
#define AI_IMAGE_PROMPT \
    "为 7.5 英寸黑白红三色电子墨水屏创作一幅每日艺术作品，画面尺寸 854×512。\n\n" \
    "【内容】\n" \
    "自行选择一句真正有文学价值、思想深度或情绪张力的文字。\n" \
    "来源可以是经典电影台词、优秀电视剧台词、小说、剧本、诗歌、哲学著作、心理学著作或有影响力的人物言论。\n" \
    "不要选择\"沉默是金\"\"坚持就是胜利\"\"时间会证明一切\"这类俗套、鸡汤化、企业文化式的名言。\n" \
    "优先选择有画面感、故事感、人生意味或复杂情绪的句子。\n" \
    "可以是浪漫的、孤独的、荒诞的、温柔的、遗憾的、哲思的，也可以带有一点黑色幽默。\n" \
    "如果找不到足够优秀的文字，宁可选择电影、小说或剧本中的一句有具体场景感的台词，也不要选择普通励志名言。\n\n" \
    "【画面】\n" \
    "不要制作成名言海报、励志海报或文字卡片。\n" \
    "不要使用中央文字+大面积空白的模板构图。\n" \
    "不要使用书法、毛笔、砚台、古典书房等刻板的\"中国名言\"视觉套路。\n" \
    "把这句话当成一部电影或一幅艺术作品的核心灵感，先构建一幅完整、有故事、有氛围的画面。\n" \
    "视觉内容应占据画面绝大部分区域，主体尽量覆盖 70%～90% 的画面。\n\n" \
    "【文字】\n" \
    "将选中的原文自然地融入画面。\n" \
    "文字不要固定在中央，可以根据构图放在左侧、右侧、顶部、底部或角落，也可以横排或竖排。\n" \
    "寻找画面中自然、简洁的区域放置文字，使其清晰但不过度抢夺画面主体。\n" \
    "文字应该像电影中的场景设计、书中的排版或艺术作品的一部分，而不是贴上去的字幕。\n" \
    "同时标注作者和作品出处，但保持低调，不要让出处成为视觉主体。\n" \
    "不要添加其他文字、Logo或水印。\n\n" \
    "【视觉风格】\n" \
    "电影感、文学感、艺术感、克制、高级。\n" \
    "避免大面积空白、随机几何图形、装饰性色块、企业宣传海报风格。\n" \
    "每次根据文字的时代、出处、情绪和内容选择不同的视觉风格，不要所有作品使用相同模板。\n" \
    "最终效果应该像一张值得每天停下来观看的电影剧照或艺术摄影作品，而不是一张名言海报。"
