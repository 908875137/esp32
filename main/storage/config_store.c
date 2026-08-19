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

static const char *TAG = "config";

#define NVS_NAMESPACE   "thinkink"
#define NVS_KEY_MODE    "mode"

#define DEFAULT_IMAGE_DIR  "/sdcard/images"
#define DEFAULT_FONT       "/sdcard/fonts/NotoSansCJKsc-Subset.otf"

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

    /* Override from config.json if SD is mounted */
    if (sd_card_is_mounted()) {
        config_json_apply(cfg);
    }

    ESP_LOGI(TAG, "Config: mode=%s image_dir=%s font=%s",
             cfg->mode == THINKINK_MODE_AI ? "ai" : "sd",
             cfg->image_dir, cfg->font);

    return ESP_OK;
}
