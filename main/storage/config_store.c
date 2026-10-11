/*
 * config_store.c
 *
 * Device configuration persistence + /sdcard/config.json override.
 */
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

#include "config_store.h"
#include "sd_card.h"
#include "ai_config.h"

static const char *TAG = "config";

#define NVS_NAMESPACE   "thinkink"
#define NVS_KEY_MODE    "mode"
#define NVS_KEY_SD_IDX  "sd_idx"
#define NVS_KEY_GEN_DAY "gen_day"

#define DEFAULT_IMAGE_DIR  "/sdcard/images"
#define DEFAULT_FONT       "/sdcard/fonts/NotoSansCJKsc-Subset.otf"
#define DEFAULT_REFRESH_HOUR  3   /* 03:00 local */

/* Ensure NVS flash is initialised (idempotent). */
static void nvs_init_once(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS truncated, erasing...");
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static thinkink_mode_t nvs_read_mode(void) {
    nvs_init_once();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return THINKINK_MODE_AI;
    }
    uint8_t mode = 0;
    esp_err_t err = nvs_get_u8(h, NVS_KEY_MODE, &mode);
    nvs_close(h);
    if (err != ESP_OK || mode > THINKINK_MODE_SD) {
        return THINKINK_MODE_AI;
    }
    return (thinkink_mode_t)mode;
}

esp_err_t config_store_save_mode(thinkink_mode_t mode) {
    nvs_init_once();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t config_store_save_mode_to_file(thinkink_mode_t mode) {
    char path[64];
    snprintf(path, sizeof(path), "%s/config.json", SD_MOUNT_POINT);

    cJSON *root = NULL;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len > 0 && len <= 4096) {
            char *buf = (char *)malloc((size_t)len + 1);
            if (buf) {
                size_t r = fread(buf, 1, (size_t)len, f);
                buf[r] = '\0';
                root = cJSON_Parse(buf);
                free(buf);
            }
        }
        fclose(f);
    }
    if (!root) root = cJSON_CreateObject();

    cJSON *m = cJSON_GetObjectItem(root, "mode");
    if (m) cJSON_DeleteItemFromObject(root, "mode");
    cJSON_AddStringToObject(root, "mode", mode == THINKINK_MODE_AI ? "ai" : "sd");

    char *json = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    f = fopen(path, "wb");
    if (!f) { free(json); return ESP_FAIL; }
    fputs(json, f);
    fclose(f);
    free(json);

    ESP_LOGI(TAG, "mode written to config.json: %s", mode == THINKINK_MODE_AI ? "ai" : "sd");
    return ESP_OK;
}

/* Parse config.json; only fills fields that are present & valid. */
static void config_json_apply(thinkink_config_t *cfg) {
    char path[64];
    snprintf(path, sizeof(path), "%s/config.json", SD_MOUNT_POINT);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGI(TAG, "No config.json at %s — using defaults", path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4096) {
        fclose(f);
        ESP_LOGW(TAG, "config.json invalid size (%ld)", len);
        return;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t r = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[r] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGE(TAG, "config.json parse failed");
        free(buf);
        return;
    }

    cJSON *mode = cJSON_GetObjectItem(root, "mode");
    if (cJSON_IsString(mode) && mode->valuestring) {
        if (strcmp(mode->valuestring, "sd") == 0) cfg->mode = THINKINK_MODE_SD;
        else if (strcmp(mode->valuestring, "ai") == 0) cfg->mode = THINKINK_MODE_AI;
    }

    cJSON *img_dir = cJSON_GetObjectItem(root, "image_dir");
    if (cJSON_IsString(img_dir) && img_dir->valuestring) {
        strncpy(cfg->image_dir, img_dir->valuestring, CONFIG_IMAGE_DIR_MAX_LEN - 1);
        cfg->image_dir[CONFIG_IMAGE_DIR_MAX_LEN - 1] = '\0';
    }

    cJSON *font = cJSON_GetObjectItem(root, "font");
    if (cJSON_IsString(font) && font->valuestring) {
        strncpy(cfg->font, font->valuestring, CONFIG_FONT_MAX_LEN - 1);
        cfg->font[CONFIG_FONT_MAX_LEN - 1] = '\0';
    }

    cJSON *rh = cJSON_GetObjectItem(root, "refresh_hour");
    if (cJSON_IsNumber(rh)) {
        int v = rh->valueint;
        if (v >= 0 && v <= 23) cfg->refresh_hour = v;
    }

    cJSON *wssid = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(wssid) && wssid->valuestring) {
        strncpy(cfg->wifi_ssid, wssid->valuestring, CONFIG_WIFI_SSID_MAX_LEN - 1);
        cfg->wifi_ssid[CONFIG_WIFI_SSID_MAX_LEN - 1] = '\0';
    }

    cJSON *wpass = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(wpass) && wpass->valuestring) {
        strncpy(cfg->wifi_pass, wpass->valuestring, CONFIG_WIFI_PASS_MAX_LEN - 1);
        cfg->wifi_pass[CONFIG_WIFI_PASS_MAX_LEN - 1] = '\0';
    }

    /* AI API config (overrides the built-in defaults) */
    cJSON *aik = cJSON_GetObjectItem(root, "ai_api_key");
    if (cJSON_IsString(aik) && aik->valuestring) {
        strncpy(ai_api_key, aik->valuestring, sizeof(ai_api_key) - 1);
        ai_api_key[sizeof(ai_api_key) - 1] = '\0';
    }
    cJSON *acu = cJSON_GetObjectItem(root, "ai_chat_url");
    if (cJSON_IsString(acu) && acu->valuestring) {
        strncpy(ai_chat_url, acu->valuestring, sizeof(ai_chat_url) - 1);
        ai_chat_url[sizeof(ai_chat_url) - 1] = '\0';
    }
    cJSON *aiu = cJSON_GetObjectItem(root, "ai_image_url");
    if (cJSON_IsString(aiu) && aiu->valuestring) {
        strncpy(ai_image_url, aiu->valuestring, sizeof(ai_image_url) - 1);
        ai_image_url[sizeof(ai_image_url) - 1] = '\0';
    }
    cJSON *acm = cJSON_GetObjectItem(root, "ai_chat_model");
    if (cJSON_IsString(acm) && acm->valuestring) {
        strncpy(ai_chat_model, acm->valuestring, sizeof(ai_chat_model) - 1);
        ai_chat_model[sizeof(ai_chat_model) - 1] = '\0';
    }
    cJSON *aim = cJSON_GetObjectItem(root, "ai_image_model");
    if (cJSON_IsString(aim) && aim->valuestring) {
        strncpy(ai_image_model, aim->valuestring, sizeof(ai_image_model) - 1);
        ai_image_model[sizeof(ai_image_model) - 1] = '\0';
    }

    cJSON_Delete(root);
    free(buf);
}

esp_err_t config_store_load(thinkink_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Defaults */
    cfg->mode = nvs_read_mode();
    strncpy(cfg->image_dir, DEFAULT_IMAGE_DIR, CONFIG_IMAGE_DIR_MAX_LEN - 1);
    strncpy(cfg->font, DEFAULT_FONT, CONFIG_FONT_MAX_LEN - 1);
    cfg->image_dir[CONFIG_IMAGE_DIR_MAX_LEN - 1] = '\0';
    cfg->font[CONFIG_FONT_MAX_LEN - 1] = '\0';
    cfg->refresh_hour = DEFAULT_REFRESH_HOUR;
    cfg->wifi_ssid[0] = '\0';
    cfg->wifi_pass[0] = '\0';

    /* Override from config.json if SD is mounted */
    if (sd_card_is_mounted()) {
        config_json_apply(cfg);
    }

    ESP_LOGI(TAG, "Config: mode=%s image_dir=%s font=%s refresh_hour=%d",
             cfg->mode == THINKINK_MODE_AI ? "ai" : "sd",
             cfg->image_dir, cfg->font, cfg->refresh_hour);

    return ESP_OK;
}

/* ================================================================
 * NVS helpers for SD index + last generation day
 * ================================================================ */

esp_err_t config_store_save_sd_index(int index) {
    nvs_init_once();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, NVS_KEY_SD_IDX, index);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

int config_store_load_sd_index(void) {
    nvs_init_once();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t idx = 0;
    nvs_get_i32(h, NVS_KEY_SD_IDX, &idx);
    nvs_close(h);
    return idx;
}

esp_err_t config_store_save_last_gen_day(uint32_t day) {
    nvs_init_once();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, NVS_KEY_GEN_DAY, day);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    return err;
}

uint32_t config_store_load_last_gen_day(void) {
    nvs_init_once();
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t day = 0;
    nvs_get_u32(h, NVS_KEY_GEN_DAY, &day);
    nvs_close(h);
    return day;
}
