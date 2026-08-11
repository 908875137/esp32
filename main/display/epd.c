#include <stdio.h>
#include <stdint.h>
#include "epd.h"
#include "io.h"

void epdCommandData(uint8_t cmd, uint8_t *data, unsigned length) {
    // Command
    unsetDC();
    // Send command
    sendSPI(&cmd, 1);
    // Data
    setDC();
    // Send data
    sendSPI(data, length);
}

void epdInit() {
    printf("EPD: Initialize\n");

    // SPI and GPIO initialization
    ioInit();

    // Do a RESET
    unsetReset();
    waitMs(10); // Minimum is actually 50us
    setReset();
    waitMs(10); // Actually, at most after 1ms

    // Panel Setting
    uint8_t panel[] = { 0b00001111 }; // LUT from OTP, KWR mode, Scan Up, Booster on, No soft reset
    epdCommandData(0x00, panel, sizeof(panel));

    // Power setting
    uint8_t power[] = { 0x07, 0x07, 0x3F, 0x3F };
    epdCommandData(0x01, power, sizeof(power));

    // Sofware Boost Start
    uint8_t btst[] = { 0x17, 0x17, 0x28, 0x17 };
    epdCommandData(0x06, btst, sizeof(btst));

    // Resolution
    uint8_t res[] = { PANEL_WIDTH>>8, PANEL_WIDTH&0xFF, PANEL_HEIGHT>>8, PANEL_HEIGHT&0xff }; // 800 x 480
    epdCommandData(0x61, res, sizeof(res));

    // Dual SPI
    uint8_t dual_spi[] = { 0x00 }; // Dual SPI disabled, Input pin disabled
    epdCommandData(0x15, dual_spi, sizeof(dual_spi));

    // Borders, VCOM and Data interval
    uint8_t vcomdi[] = { 0x20, 0x07 }; // Invert BW polarity. BDZ=0, BDV=10, DDX=00, N2OCP=0
    epdCommandData(0x50, vcomdi, sizeof(vcomdi));
}

void epdWriteKW(uint8_t *buffer, unsigned size) {
    epdCommandData(0x10, buffer, size);
}

void epdWriteRED(uint8_t *buffer, unsigned size) {
    epdCommandData(0x13, buffer, size);
}

void epdBusyWait() {
    while(isBusy()) {
	waitMs(10);
    }
}

void epdUpdate(bool fast) {
    printf("EPD: Update\n");
    printf("EPD: Power On\n");
    epdCommandData(0x04, NULL, 0); // PON
    epdBusyWait();
    printf("EPD: Refresh\n");
    epdCommandData(0x12, NULL, 0); // DRF
    epdBusyWait();
    printf("EPD: Power Off\n");
    epdCommandData(0x02, NULL, 0); // POF
    epdBusyWait();
}

void epdDown() {
    uint8_t a5 = 0xA5;

    printf("EPD: Deep Sleep\n");
    epdCommandData(0x07, &a5, 1); // Deep Sleep
}
