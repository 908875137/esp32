/*
 * io.h — SPI & GPIO interface for UC8179 EPD.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

int  ioInit(void);
esp_err_t epdCommand(uint8_t cmd);
esp_err_t epdData(const uint8_t *data, size_t len);
void setDC(void);
void unsetDC(void);
void setReset(void);
void unsetReset(void);
int  isBusy(void);
void waitMs(unsigned ms);

#ifdef __cplusplus
}
#endif
