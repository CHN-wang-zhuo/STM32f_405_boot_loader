#include "boot_spi.h"
#include "stm32f4xx_hal.h"

#define SPI_TIMEOUT_MS 100U

uint32_t BootSpi_Tick(void) { return HAL_GetTick(); }

void BootSpi_Select(bool selected)
{
    GPIOA->BSRR = selected ? GPIO_BSRR_BR2 : GPIO_BSRR_BS2;
}

void BootSpi_Init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    (void)RCC->AHB1ENR;
    BootSpi_Select(false); /* Set the output latch BEFORE enabling PA2 output. */
    GPIOA->MODER = (GPIOA->MODER & ~((3U << 4U) | (3U << 10U) |
                                    (3U << 12U) | (3U << 14U))) |
                   (1U << 4U) | (2U << 10U) | (2U << 12U) | (2U << 14U);
    GPIOA->OTYPER &= ~((1U << 2U) | (1U << 5U) | (1U << 6U) | (1U << 7U));
    GPIOA->PUPDR &= ~((3U << 4U) | (3U << 10U) | (3U << 12U) | (3U << 14U));
    GPIOA->OSPEEDR |= (3U << 10U) | (3U << 12U) | (3U << 14U);
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~((15U << 20U) | (15U << 24U) | (15U << 28U))) |
                    (5U << 20U) | (5U << 24U) | (5U << 28U);

    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    (void)RCC->APB2ENR;
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    SPI1->CR2 = 0U; /* All DMA and interrupt requests disabled. */
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_SPE;
}

static bool WaitFlag(uint32_t flag, bool set, uint32_t start)
{
    for (;;)
    {
        const uint32_t status = SPI1->SR;
        if ((status & (SPI_SR_MODF | SPI_SR_OVR)) != 0U)
        {
            return false;
        }
        if (((status & flag) != 0U) == set) { return true; }
        if ((uint32_t)(BootSpi_Tick() - start) >= SPI_TIMEOUT_MS) { return false; }
    }
}

bool BootSpi_Transfer(const uint8_t *tx, uint8_t *rx, uint32_t size)
{
    const uint32_t start = BootSpi_Tick();
    for (uint32_t i = 0U; i < size; ++i)
    {
        if (!WaitFlag(SPI_SR_TXE, true, start)) { return false; }
        *(__IO uint8_t *)&SPI1->DR = tx != NULL ? tx[i] : 0xFFU;
        if (!WaitFlag(SPI_SR_RXNE, true, start)) { return false; }
        const uint8_t byte = *(__IO uint8_t *)&SPI1->DR;
        if (rx != NULL) { rx[i] = byte; }
    }
    return WaitFlag(SPI_SR_BSY, false, start);
}

void BootSpi_DeInit(void)
{
    BootSpi_Select(false);
    SPI1->CR1 = 0U;
    RCC->APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    RCC->APB2ENR &= ~RCC_APB2ENR_SPI1EN;
}
