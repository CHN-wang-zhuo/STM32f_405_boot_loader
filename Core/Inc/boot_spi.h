#ifndef BOOT_SPI_H
#define BOOT_SPI_H

#include <stdbool.h>
#include <stdint.h>

/* Blocking SPI1, Mode 0, 8-bit, HSI / 2 = 8 MHz. No interrupts or DMA.
 * PA5 SCK, PA6 MISO, PA7 MOSI; PA2 active-low CS starts high. */
void BootSpi_Init(void);
void BootSpi_DeInit(void);
void BootSpi_Select(bool selected);
bool BootSpi_Transfer(const uint8_t *tx, uint8_t *rx, uint32_t size);
uint32_t BootSpi_Tick(void);

#endif
