#include "boot_spi.h"
#include "stm32f4xx_hal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

SPI_TypeDef test_spi;
GPIO_TypeDef test_gpio;
RCC_TypeDef test_rcc;
static uint32_t tick;
static uint32_t tick_calls;
uint32_t HAL_GetTick(void) { ++tick_calls; return tick++; }

int main(void)
{
    memset(&test_gpio, 0xA5, sizeof(test_gpio));
    const uint32_t old_moder = test_gpio.MODER;
    const uint32_t pins = (3U << 4U) | (3U << 10U) | (3U << 12U) | (3U << 14U);
    BootSpi_Init();
    assert(test_gpio.BSRR == GPIO_BSRR_BS2);
    assert((test_gpio.MODER & ~pins) == (old_moder & ~pins)); /* including SWD */
    assert((test_gpio.MODER & pins) == ((1U << 4U) | (2U << 10U) | (2U << 12U) | (2U << 14U)));
    assert((test_gpio.AFR[0] & 0xFFF00000U) == 0x55500000U);
    assert(test_spi.CR1 == (SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE));
    assert(test_spi.CR2 == 0U); /* No DMA or interrupt requests. */
    assert((test_rcc.APB2ENR & RCC_APB2ENR_SPI1EN) != 0U);
    BootSpi_Select(true);
    assert(test_gpio.BSRR == GPIO_BSRR_BR2);

    test_spi.SR = SPI_SR_TXE | SPI_SR_RXNE;
    const uint8_t tx[] = {0x03U, 0x01U, 0x23U, 0x45U};
    uint8_t rx[sizeof(tx)];
    /* Fake DR echoes writes; this checks the production 8-bit polling path. */
    assert(BootSpi_Transfer(tx, rx, sizeof(tx)) && memcmp(tx, rx, sizeof(tx)) == 0);
    assert(BootSpi_Transfer(NULL, rx, sizeof(rx)) && rx[0] == 0xFFU);
    assert(BootSpi_Transfer(tx, NULL, sizeof(tx)));
    const uint32_t failed_status[] = {0U, SPI_SR_TXE, SPI_SR_TXE | SPI_SR_RXNE | SPI_SR_BSY,
                                     SPI_SR_TXE | SPI_SR_RXNE | SPI_SR_MODF,
                                     SPI_SR_TXE | SPI_SR_RXNE | SPI_SR_OVR};
    for (unsigned i = 0U; i < sizeof(failed_status) / sizeof(failed_status[0]); ++i)
    {
        test_spi.SR = failed_status[i];
        tick = UINT32_MAX - 10U;
        tick_calls = 0U;
        assert(!BootSpi_Transfer(tx, rx, 1U));
        assert(tick_calls <= 102U); /* includes timer wrap */
    }
    BootSpi_DeInit();
    assert(test_gpio.BSRR == GPIO_BSRR_BS2 && test_spi.CR1 == 0U);
    assert((test_rcc.APB2ENR & RCC_APB2ENR_SPI1EN) == 0U);
    puts("PASS: production SPI polling, Mode 0/no DMA, pin configuration, error flags and timeouts");
    return 0;
}
