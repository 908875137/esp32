/*
 * ai_config.c
 *
 * Runtime AI config values — initialised to the built-in defaults, overridable
 * at startup by /sdcard/config.json (see config_store.c).
 */
#include "ai_config.h"

char ai_api_key[256]       = AI_API_KEY_DEFAULT;
char ai_chat_url[256]      = AI_CHAT_URL_DEFAULT;
char ai_image_url[256]     = AI_IMAGE_URL_DEFAULT;
char ai_chat_model[64]     = AI_CHAT_MODEL_DEFAULT;
char ai_image_model[64]    = AI_IMAGE_MODEL_DEFAULT;
