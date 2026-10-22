/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"

/* USER CODE BEGIN Includes */
#include "bootloader.h"
#include "boot_port.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Inspect this value in the debugger if no APP can be started. */
volatile BootStatus boot_status = BOOT_NO_VALID_APP;
/* USER CODE END PV */

int main(void)
{
  /* Reset HSI is already 16 MHz. Keep the 1 ms tick for HAL Flash timeouts.
   * CRC clock/reset is handled directly by Boot_Crc32's register adapter. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  boot_status = Boot_PrepareApplication();
  if (boot_status == BOOT_READY)
  {
    /* A successful jump never returns. */
    (void)BootPort_JumpApplication();
  }

  /* No usable APP or jump failure: halt without starting any peripherals.
   * Stay inside USER CODE so regeneration cannot run later init calls. */
  Error_Handler();
  /* USER CODE END Init */
}

void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  SysTick->CTRL = 0U;
  while (1)
  {
    __WFI();
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  (void)file;
  (void)line;
  Error_Handler();
  /* USER CODE END 6 */
}
#endif
