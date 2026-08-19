#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io.h"

#include "driver/spi_master.h"
#include "driver/gpio.h"

/*
 * ESP32-S3 with 7.5" EPD (UC8179)
 *
 * SPI2_HOST (FSPI) used
 *
 * GPIO7  for D/C
 * GPIO6  for Reset
 * GPIO16 for Busy
 * GPIO4  for SCK
 * GPIO5  for MOSI (SDA)
 * GPIO15 for CS
 * MISO not connected
 */

#define DC_PIN    (7)
#define RESET_PIN (6)
#define BUSY_PIN  (16)

static spi_device_handle_t spi;
static bool s_io_initialised = false;

int ioInit() {
    // FSPI Pin definitions for ESP32-S3
    #define PIN_NUM_MISO -1   // Not connected
    #define PIN_NUM_MOSI 5
    #define PIN_NUM_CLK  4
    #define PIN_NUM_CS   15

    esp_err_t ret;

    /* SPI bus + device are initialised once.  epdInit() → ioInit() runs on
     * every refresh, but re-initialising the bus aborts. */
    if (s_io_initialised) {
        return 0;
    }

    // SPI bus configuration
    spi_bus_config_t buscfg = {
        .miso_io_num = PIN_NUM_MISO,
        .mosi_io_num = PIN_NUM_MOSI,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32, // Max transfer size
    };

    // SPI peripheral configuration
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4000000,           // 4 MHz
        .mode = 0,                           // SPI Mode 0
        .spics_io_num = PIN_NUM_CS,          // CS pin
        .queue_size = 3,                     // Queue size
    };

    // FSPI initialization (SPI2_HOST for ESP32-S3)
    ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    // Add device to bus
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &spi);
    ESP_ERROR_CHECK(ret);

    // GPIOs
    gpio_config_t io_conf = {};

    // Outputs: DC and RESET
    io_conf.pin_bit_mask = ((1ULL << DC_PIN) | (1ULL << RESET_PIN) );
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Input: BUSY
    io_conf.pin_bit_mask = ((1ULL << BUSY_PIN) );
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    s_io_initialised = true;
    return 0;
}

void sendSPI(uint8_t *data, unsigned size) {
    esp_err_t ret;
    spi_transaction_t t;

    while(size--) {
        memset(&t, 0, sizeof(t));       // Zero out the transaction
        t.length = 8;                   // Len is in bytes, transaction length is in bits.
        t.tx_buffer = data++;           // Data
        ret = spi_device_polling_transmit(spi, &t); // Transmit!

        assert(ret == ESP_OK);          // Should have had no issues.
    }
}

void setDC() {
    gpio_set_level(DC_PIN, 1);
}

void unsetDC() {
    gpio_set_level(DC_PIN, 0);
}

void setReset() {
    gpio_set_level(RESET_PIN, 1);
}

void unsetReset() {
    gpio_set_level(RESET_PIN, 0);
}

int isBusy() {
    return gpio_get_level(BUSY_PIN) == 0 ? 1 : 0;
}

void waitMs(unsigned ms) {
    vTaskDelay(ms / portTICK_PERIOD_MS);
}
