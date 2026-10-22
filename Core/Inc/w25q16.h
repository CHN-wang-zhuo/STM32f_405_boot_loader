#ifndef W25Q16_H
#define W25Q16_H

#include <stdbool.h>
#include <stdint.h>

extern volatile uint32_t g_w25q16_jedec_id;
bool W25Q16_Init(void);
void W25Q16_DeInit(void);
bool W25Q16_Read(uint32_t offset, void *data, uint32_t size);
/* The only external write allowed to the installer: clear bytes 28..31.
 * Never erases external Flash or changes firmware / immutable metadata. */
bool W25Q16_ConsumeRequest(void);

#endif
