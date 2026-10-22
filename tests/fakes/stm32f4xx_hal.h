#ifndef TEST_STM32_HAL_H
#define TEST_STM32_HAL_H
#include <stddef.h>
#include <stdint.h>
#define __IO volatile
typedef struct { volatile uint32_t CR1, CR2, SR, DR; } SPI_TypeDef;
typedef struct { volatile uint32_t MODER, OTYPER, OSPEEDR, PUPDR, BSRR, AFR[2]; } GPIO_TypeDef;
typedef struct { volatile uint32_t AHB1ENR, APB2ENR, APB2RSTR; } RCC_TypeDef;
extern SPI_TypeDef test_spi;
extern GPIO_TypeDef test_gpio;
extern RCC_TypeDef test_rcc;
#define SPI1 (&test_spi)
#define GPIOA (&test_gpio)
#define RCC (&test_rcc)
#define GPIO_BSRR_BR2 (1U << 18U)
#define GPIO_BSRR_BS2 (1U << 2U)
#define RCC_AHB1ENR_GPIOAEN 1U
#define RCC_APB2ENR_SPI1EN (1U << 12U)
#define RCC_APB2RSTR_SPI1RST (1U << 12U)
#define SPI_CR1_MSTR (1U << 2U)
#define SPI_CR1_SSM (1U << 9U)
#define SPI_CR1_SSI (1U << 8U)
#define SPI_CR1_SPE (1U << 6U)
#define SPI_SR_TXE (1U << 1U)
#define SPI_SR_RXNE 1U
#define SPI_SR_BSY (1U << 7U)
#define SPI_SR_MODF (1U << 5U)
#define SPI_SR_OVR (1U << 6U)
uint32_t HAL_GetTick(void);
#endif
