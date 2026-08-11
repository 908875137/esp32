/*
 * io.h — SPI & GPIO interface for UC8179 EPD.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  ioInit(void);
void sendSPI(uint8_t *data, unsigned size);
void setDC(void);
void unsetDC(void);
void setReset(void);
void unsetReset(void);
int  isBusy(void);
void waitMs(unsigned ms);

#ifdef __cplusplus
}
#endif
