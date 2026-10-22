#include "boot_port.h"
#include "boot_protocol.h"
#include "stm32f4xx_hal.h"
#include "w25q16.h"

bool BootPort_StorageInit(void) { return W25Q16_Init(); }
void BootPort_StorageDeInit(void) { W25Q16_DeInit(); }
bool BootPort_StorageRead(uint32_t offset, void *data, uint32_t size)
{
    return W25Q16_Read(offset, data, size);
}
bool BootPort_StorageConsume(void) { return W25Q16_ConsumeRequest(); }

const void *BootPort_Map(uint32_t address)
{
    return (const void *)(uintptr_t)address;
}

void BootPort_CrcBegin(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
    (void)RCC->AHB1ENR; /* Clock-enable propagation delay before register access. */
    CRC->CR = CRC_CR_RESET; /* Fixed initial value 0xFFFFFFFF. */
}

void BootPort_CrcFeedWord(uint32_t word)
{
    /* Fixed polynomial 0x04C11DB7, MSB first. RBIT gives the byte stream's
     * reflected input ordering required by CRC-32/ISO-HDLC. */
    CRC->DR = __RBIT(word);
}

uint32_t BootPort_CrcFinish(void)
{
    const uint32_t crc = __RBIT(CRC->DR);
    RCC->AHB1ENR &= ~RCC_AHB1ENR_CRCEN;
    return crc;
}

bool BootPort_Unlock(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK)
    {
        return false;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                          FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);
    return true;
}

void BootPort_Lock(void)
{
    (void)HAL_FLASH_Lock();
}

static bool EraseSectors(uint32_t first, uint32_t count)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error = BOOT_ERASED_WORD;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = first;
    erase.NbSectors = count;
    /* This board must supply 2.7-3.6 V for 32-bit Flash programming. */
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    return HAL_FLASHEx_Erase(&erase, &error) == HAL_OK && error == BOOT_ERASED_WORD;
}

bool BootPort_EraseInstalled(void)
{
    return EraseSectors(FLASH_SECTOR_2, 1U);
}

bool BootPort_EraseApplication(void)
{
    return EraseSectors(FLASH_SECTOR_5, 3U);
}

bool BootPort_ProgramWord(uint32_t address, uint32_t value)
{
    const bool allowed = (address >= BOOT_APP_ADDRESS &&
                          address <= BOOT_APP_ADDRESS + BOOT_IMAGE_CAPACITY - 4U) ||
                         (address >= BOOT_INSTALLED_ADDRESS &&
                          address < BOOT_INSTALLED_ADDRESS + offsetof(BootImageManifest, consumed));
    if (!allowed || (address & 3U) != 0U)
    {
        return false;
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address, value) != HAL_OK)
    {
        return false;
    }
    FLASH_FlushCaches();
    return *(const volatile uint32_t *)(uintptr_t)address == value;
}

/* r0 = initial MSP, r1 = Reset_Handler. No C code may touch the old stack
 * after MSP changes. All final state changes and the branch stay in assembly. */
__attribute__((naked, noreturn))
static void EnterApplication(uint32_t stack __attribute__((unused)),
                             uint32_t reset __attribute__((unused)))
{
    __asm volatile (
        "movs r2, #0\n"
        "msr control, r2\n"
        "isb\n"
        "msr basepri, r2\n"
        "msr faultmask, r2\n"
        "msr psp, r2\n"
        "msr msp, r0\n"
        "dsb\n"
        "isb\n"
        "cpsie i\n"
        "bx r1\n"
    );
}

bool BootPort_JumpApplication(void)
{
    const uint32_t stack = *(const uint32_t *)BOOT_APP_ADDRESS;
    const uint32_t reset = *(const uint32_t *)(BOOT_APP_ADDRESS + 4U);
    if (__get_IPSR() != 0U || HAL_RCC_DeInit() != HAL_OK)
    {
        return false;
    }
    __disable_irq();
    (void)HAL_DeInit();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    for (uint32_t i = 0U; i < 8U; ++i)
    {
        NVIC->ICER[i] = UINT32_C(0xFFFFFFFF);
        NVIC->ICPR[i] = UINT32_C(0xFFFFFFFF);
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR = BOOT_APP_ADDRESS;
    __DSB();
    __ISB();
    EnterApplication(stack, reset);
}
