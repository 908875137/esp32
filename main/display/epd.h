/*
 * epd.h — UC8179 tri-color EPD driver.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_WIDTH  800
#define PANEL_HEIGHT 480
#define BUFFER_SIZE  (PANEL_WIDTH / 8 * PANEL_HEIGHT)   /* 48000 bytes */

void epdInit(void);
void epdWriteKW(uint8_t *buffer, unsigned size);
void epdWriteRED(uint8_t *buffer, unsigned size);
void epdUpdate(bool fast);
void epdDown(void);

#ifdef __cplusplus
}
#endif
