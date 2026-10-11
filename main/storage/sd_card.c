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
#include "image_library.h"

static const char *TAG = "sd";

#define PIN_NUM_MISO         19
#define PIN_NUM_MOSI         21
#define PIN_NUM_CLK          20
#define PIN_NUM_CS           47

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

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

/* ---- Image enumeration (bridged to the image library) ---- */

int sd_card_count_images(void) {
    if (!s_mounted) return -1;
    uint32_t count = 0;
    if (image_library_count(&count) != ESP_OK) return -1;
    return (int)count;
}

esp_err_t sd_card_get_image_path(int index, char *path_out, size_t path_len) {
    if (!s_mounted || !path_out || path_len == 0) return ESP_ERR_INVALID_ARG;

    uint32_t id = 0;
    if (image_library_get_id_by_index((uint32_t)index, &id) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    return image_library_get_path(id, path_out, path_len);
}

esp_err_t sd_card_get_image_id(int index, uint32_t *id_out) {
    if (!s_mounted || !id_out) return ESP_ERR_INVALID_ARG;
    return image_library_get_id_by_index((uint32_t)index, id_out);
}

/* ---- Capacity ---- */

esp_err_t sd_card_get_capacity(uint64_t *total_out, uint64_t *free_out) {
    if (!s_mounted || !s_card) return ESP_ERR_INVALID_STATE;

    /* Use the official VFS API — it takes the base path and returns both
       total and free bytes.  Much safer than hand-rolling f_getfree. */
    return esp_vfs_fat_info(SD_MOUNT_POINT, total_out, free_out);
}
