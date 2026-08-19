/*
 * sd_card.c
 *
 * Persistent SD card (SPI mode) module.
 *
 *   ESP32-S3  GPIO19 = MISO
 *   ESP32-S3  GPIO20 = CLK
 *   ESP32-S3  GPIO21 = MOSI
 *   ESP32-S3  GPIO47 = CS
 *
 * Uses SPI3_HOST (HSPI) — SPI2_HOST (FSPI) is already taken by the EPD.
 *
 * The card stays mounted for the lifetime of the app so FreeType and the
 * image loader can keep reading files from it.
 */
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"

#include "sd_card.h"

static const char *TAG = "sd";

#define PIN_NUM_MISO         19
#define PIN_NUM_MOSI         21
#define PIN_NUM_CLK          20
#define PIN_NUM_CS           47

#define MAX_IMAGE_FILES      256

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

static bool has_png_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 4) return false;
    return strcasecmp(name + n - 4, ".png") == 0;
}

esp_err_t sd_card_init(void) {
    if (s_mounted) return ESP_OK;

    ESP_LOGI(TAG, "Mounting SD card via SPI3_HOST (HSPI)...");

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;          /* HSPI — FSPI is used by the EPD */
    host.max_freq_khz = 10000;      /* 10 MHz, conservative */

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = PIN_NUM_MOSI,
        .miso_io_num     = PIN_NUM_MISO,
        .sclk_io_num     = PIN_NUM_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs   = PIN_NUM_CS;
    slot_cfg.host_id   = host.slot;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check: wiring, pull-ups on CS/MISO, and a FAT32-formatted card.");
        spi_bus_free(host.slot);
        s_card = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);

    s_mounted = true;
    return ESP_OK;
}

bool sd_card_is_mounted(void) {
    return s_mounted;
}

int sd_card_count_images(void) {
    if (!s_mounted) return -1;

    DIR *dir = opendir(SD_IMAGES_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "No images directory: %s", SD_IMAGES_DIR);
        return -1;
    }

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (has_png_ext(entry->d_name)) count++;
    }
    closedir(dir);

    ESP_LOGI(TAG, "Found %d PNG image(s) in %s", count, SD_IMAGES_DIR);
    return count;
}

esp_err_t sd_card_get_image_path(int index, char *path_out, size_t path_len) {
    if (!s_mounted || !path_out || path_len == 0) return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(SD_IMAGES_DIR);
    if (!dir) return ESP_ERR_NOT_FOUND;

    struct dirent *entry;
    int cur = 0;
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    while ((entry = readdir(dir)) != NULL) {
        if (!has_png_ext(entry->d_name)) continue;
        if (cur == index) {
            snprintf(path_out, path_len, "%s/%s", SD_IMAGES_DIR, entry->d_name);
            ret = ESP_OK;
            break;
        }
        cur++;
    }
    closedir(dir);
    return ret;
}
