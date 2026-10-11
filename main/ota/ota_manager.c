/*
 * ota_manager.c
 *
 * Firmware OTA via esp_ota_ops.  See ota_manager.h.
 */
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include "ota_manager.h"

static const char *TAG = "ota";

#define OTA_PROJECT_NAME    "epd-uc8179"
#define OTA_HEADER_BUF_SIZE 4096

static esp_ota_handle_t s_ota_handle = 0;
static const esp_partition_t *s_update_partition = NULL;
static bool s_in_progress = false;

/* Header check: accumulate the first 4KB so we can find the app descriptor. */
static bool s_header_checked = false;
static uint8_t s_header_buf[OTA_HEADER_BUF_SIZE];
static size_t  s_header_len = 0;

/* Find the app descriptor in the buffer and verify the project name. */
static bool ota_verify_image_header(const uint8_t *buf, size_t len) {
    for (size_t i = 0; i + sizeof(esp_app_desc_t) <= len; i++) {
        const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + i);
        if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) continue;
        if (strncmp(desc->project_name, OTA_PROJECT_NAME,
                    sizeof(desc->project_name)) == 0) {
            return true;
        }
        /* Wrong project — reject explicitly rather than falling through. */
        return false;
    }
    return false;  /* no descriptor found yet */
}

static void ota_restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1000));   /* let the HTTP response flush */
    ESP_LOGW(TAG, "restarting into new firmware");
    esp_restart();
    vTaskDelete(NULL);
}

ota_result_t ota_begin(void) {
    if (s_in_progress) {
        ESP_LOGW(TAG, "OTA already in progress");
        return OTA_ERR_BUSY;
    }

    s_update_partition = esp_ota_get_next_update_partition(NULL);
    if (s_update_partition == NULL) {
        ESP_LOGE(TAG, "no OTA partition found — is the partition table OTA-enabled?");
        return OTA_ERR_NO_PARTITION;
    }

    esp_err_t err = esp_ota_begin(s_update_partition, OTA_SIZE_UNKNOWN, &s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return OTA_ERR_BEGIN;
    }

    s_in_progress = true;
    s_header_checked = false;
    s_header_len = 0;

    ESP_LOGI(TAG, "OTA begin -> partition %s", s_update_partition->label);
    return OTA_OK;
}

ota_result_t ota_write(const uint8_t *data, size_t len) {
    if (!s_in_progress) {
        return OTA_ERR_BUSY;
    }

    /* Accumulate the first bytes to verify the image header once. */
    if (!s_header_checked) {
        size_t to_copy = len;
        if (to_copy > sizeof(s_header_buf) - s_header_len) {
            to_copy = sizeof(s_header_buf) - s_header_len;
        }
        memcpy(s_header_buf + s_header_len, data, to_copy);
        s_header_len += to_copy;

        if (s_header_len >= sizeof(s_header_buf)) {
            s_header_checked = true;
            if (!ota_verify_image_header(s_header_buf, sizeof(s_header_buf))) {
                ESP_LOGE(TAG, "image header check failed (wrong project / not an app image)");
                return OTA_ERR_IMAGE;
            }
            ESP_LOGI(TAG, "image header OK (project matches)");
        }
    }

    esp_err_t err = esp_ota_write(s_ota_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
        return OTA_ERR_WRITE;
    }
    return OTA_OK;
}

ota_result_t ota_finish(void) {
    if (!s_in_progress) {
        return OTA_ERR_BUSY;
    }

    esp_err_t err = esp_ota_end(s_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image invalid)", esp_err_to_name(err));
        s_in_progress = false;
        return OTA_ERR_END;
    }

    err = esp_ota_set_boot_partition(s_update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        s_in_progress = false;
        return OTA_ERR_END;
    }

    s_in_progress = false;
    ESP_LOGI(TAG, "OTA complete — switching boot to %s", s_update_partition->label);

    /* Schedule the reboot so the HTTP response can be sent first. */
    xTaskCreate(ota_restart_task, "ota_rst", 2048, NULL, 5, NULL);
    return OTA_OK;
}

void ota_abort(void) {
    if (s_in_progress) {
        esp_ota_abort(s_ota_handle);
        s_in_progress = false;
        ESP_LOGW(TAG, "OTA aborted");
    }
}

const char *ota_get_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "?";
}

const char *ota_get_project_name(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->project_name : "?";
}

void ota_mark_app_valid(void) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mark_app_valid: %s (not in pending-verify state?)",
                 esp_err_to_name(err));
    }
}
