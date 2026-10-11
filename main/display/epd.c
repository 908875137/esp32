#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "epd.h"
#include "io.h"

static void epdBusyWait(void) {
    TickType_t start = xTaskGetTickCount();
    while (isBusy()) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(30000)) {  /* 30 s timeout */
            printf("EPD: Busy timeout!\n");
            break;
        }
        waitMs(10);
    }
}

/* 0x90 partial window: x_h x_l xe_h xe_l y_h y_l ye_h ye_l 00  (big-endian).
   xe = (x+w-1)|0x07, ye = y+h-1, x &= 0xFFF8 — as GxEPD2 does. */
static void epdSetPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    x &= 0xFFF8;
    uint16_t xe = (x + w - 1) | 0x07;
    uint16_t ye = y + h - 1;
    uint8_t data[9] = {
        (uint8_t)(x >> 8),  (uint8_t)(x & 0xFF),
        (uint8_t)(xe >> 8), (uint8_t)(xe & 0xFF),
        (uint8_t)(y >> 8),  (uint8_t)(y & 0xFF),
        (uint8_t)(ye >> 8), (uint8_t)(ye & 0xFF),
        0x00
    };
    ESP_ERROR_CHECK(epdCommand(0x90));
    ESP_ERROR_CHECK(epdData(data, sizeof(data)));
}

void epdInit() {
    printf("EPD: Initialize\n");

    ioInit();

    // RESET: HIGH 10ms -> LOW 10ms -> HIGH 10ms (GxEPD2)
    setReset();
    waitMs(10);
    unsetReset();
    waitMs(10);
    setReset();
    waitMs(10);

    // Panel Setting
    uint8_t panel[] = { 0b00001111 }; // LUT from OTP, KWR mode, Scan Up, Booster on, No soft reset
    ESP_ERROR_CHECK(epdCommand(0x00));
    ESP_ERROR_CHECK(epdData(panel, sizeof(panel)));

    // Power setting
    uint8_t power[] = { 0x07, 0x07, 0x3F, 0x3F };
    ESP_ERROR_CHECK(epdCommand(0x01));
    ESP_ERROR_CHECK(epdData(power, sizeof(power)));

    // Software Boost Start
    uint8_t btst[] = { 0x17, 0x17, 0x28, 0x17 };
    ESP_ERROR_CHECK(epdCommand(0x06));
    ESP_ERROR_CHECK(epdData(btst, sizeof(btst)));

    // Resolution
    uint8_t res[] = { PANEL_WIDTH>>8, PANEL_WIDTH&0xFF, PANEL_HEIGHT>>8, PANEL_HEIGHT&0xff }; // 800 x 480
    ESP_ERROR_CHECK(epdCommand(0x61));
    ESP_ERROR_CHECK(epdData(res, sizeof(res)));

    // Dual SPI
    uint8_t dual_spi[] = { 0x00 }; // Dual SPI disabled, Input pin disabled
    ESP_ERROR_CHECK(epdCommand(0x15));
    ESP_ERROR_CHECK(epdData(dual_spi, sizeof(dual_spi)));

    // VCOM and Data interval (DDX=10 inverts BW to match this dither's 0/1 polarity)
    uint8_t vcomdi[] = { 0x20, 0x07 };
    ESP_ERROR_CHECK(epdCommand(0x50));
    ESP_ERROR_CHECK(epdData(vcomdi, sizeof(vcomdi)));

    // TCON Setting (GxEPD2: 0x22)
    uint8_t tcon[] = { 0x22 };
    ESP_ERROR_CHECK(epdCommand(0x60));
    ESP_ERROR_CHECK(epdData(tcon, sizeof(tcon)));
}

void epdWriteKW(uint8_t *buffer, unsigned size) {
    ESP_ERROR_CHECK(epdCommand(0x91)); // partial in
    epdSetPartialRamArea(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    ESP_ERROR_CHECK(epdCommand(0x10)); // set black/white RAM
    ESP_ERROR_CHECK(epdData(buffer, size));
    ESP_ERROR_CHECK(epdCommand(0x92)); // partial out
}

void epdWriteRED(uint8_t *buffer, unsigned size) {
    ESP_ERROR_CHECK(epdCommand(0x91)); // partial in
    epdSetPartialRamArea(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    ESP_ERROR_CHECK(epdCommand(0x13)); // set red/white RAM
    ESP_ERROR_CHECK(epdData(buffer, size));
    ESP_ERROR_CHECK(epdCommand(0x92)); // partial out
}

void epdUpdate(bool fast) {
    /* GxEPD2 _Update_Full re-sets panel/VCOM/temperature right before refresh. */
    uint8_t panel[] = { 0x0f };
    ESP_ERROR_CHECK(epdCommand(0x00));
    ESP_ERROR_CHECK(epdData(panel, sizeof(panel)));

    uint8_t vcom[] = { 0x20, 0x07 };
    ESP_ERROR_CHECK(epdCommand(0x50));
    ESP_ERROR_CHECK(epdData(vcom, sizeof(vcom)));

    uint8_t ccset[] = { 0x02 };  /* CCSET = TSFIX (fixed temperature) */
    ESP_ERROR_CHECK(epdCommand(0xE0));
    ESP_ERROR_CHECK(epdData(ccset, sizeof(ccset)));

    uint8_t tsset[] = { 0x5A };  /* TSSET = 90°C */
    ESP_ERROR_CHECK(epdCommand(0xE5));
    ESP_ERROR_CHECK(epdData(tsset, sizeof(tsset)));

    printf("EPD: Update\n");
    printf("EPD: Power On\n");
    ESP_ERROR_CHECK(epdCommand(0x04)); // PON
    epdBusyWait();
    printf("EPD: Refresh\n");
    ESP_ERROR_CHECK(epdCommand(0x12)); // DRF
    epdBusyWait();
    printf("EPD: Power Off\n");
    ESP_ERROR_CHECK(epdCommand(0x02)); // POF
    epdBusyWait();
}

void epdDown() {
    uint8_t a5 = 0xA5;

    printf("EPD: Deep Sleep\n");
    ESP_ERROR_CHECK(epdCommand(0x07));
    ESP_ERROR_CHECK(epdData(&a5, 1));
}
