# Only compile the startup, boot logic and HAL services used by this loader.
# Keep CubeMX-generated GPIO / USART files available for later experiments.
set(SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Startup/startup_stm32f405rgtx.s
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/system_stm32f4xx.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/stm32f4xx_it.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/bootloader.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/boot_port_stm32.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/boot_spi_stm32.c
    ${CMAKE_CURRENT_LIST_DIR}/../Core/Src/w25q16.c
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c
    ${CMAKE_CURRENT_LIST_DIR}/../Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c
)
