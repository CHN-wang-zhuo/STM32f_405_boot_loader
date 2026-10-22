#include "w25q16.h"
#include "boot_spi.h"
#include "boot_update_storage.h"

#define W25_BUSY_TIMEOUT_MS 1000U

volatile uint32_t g_w25q16_jedec_id;
static bool available;

static bool Command(uint8_t command)
{
    BootSpi_Select(true);
    const bool ok = BootSpi_Transfer(&command, NULL, 1U);
    BootSpi_Select(false);
    return ok;
}

static bool ReadStatus(uint8_t *status)
{
    const uint8_t command = 0x05U;
    BootSpi_Select(true);
    const bool ok = BootSpi_Transfer(&command, NULL, 1U) &&
                    BootSpi_Transfer(NULL, status, 1U);
    BootSpi_Select(false);
    return ok;
}

static bool WaitReady(void)
{
    const uint32_t start = BootSpi_Tick();
    uint8_t status;
    do
    {
        if (!ReadStatus(&status)) { return false; }
        if ((status & 1U) == 0U) { return true; }
    } while ((uint32_t)(BootSpi_Tick() - start) < W25_BUSY_TIMEOUT_MS);
    return false;
}

bool W25Q16_Init(void)
{
    const uint8_t command = 0x9FU;
    uint8_t id[3] = {0U};
    available = false;
    g_w25q16_jedec_id = 0U;
    BootSpi_Init();
    if (!WaitReady()) { return false; }
    BootSpi_Select(true);
    const bool ok = BootSpi_Transfer(&command, NULL, 1U) &&
                    BootSpi_Transfer(NULL, id, sizeof(id));
    BootSpi_Select(false);
    g_w25q16_jedec_id = ((uint32_t)id[0] << 16U) | ((uint32_t)id[1] << 8U) | id[2];
    available = ok && id[0] == 0xEFU && id[2] == 0x15U;
    return available;
}

void W25Q16_DeInit(void)
{
    available = false;
    BootSpi_DeInit();
}

bool W25Q16_Read(uint32_t offset, void *data, uint32_t size)
{
    if (!available || offset > BOOT_STORAGE_SIZE || size > BOOT_STORAGE_SIZE - offset ||
        (size != 0U && data == NULL))
    {
        return false;
    }
    if (size == 0U) { return true; }
    const uint8_t command[] = {0x03U, (uint8_t)(offset >> 16U),
                              (uint8_t)(offset >> 8U), (uint8_t)offset};
    BootSpi_Select(true);
    const bool ok = BootSpi_Transfer(command, NULL, sizeof(command)) &&
                    BootSpi_Transfer(NULL, data, size);
    BootSpi_Select(false);
    return ok;
}

bool W25Q16_ConsumeRequest(void)
{
    const uint8_t command[] = {0x02U, 0U, 0U, 28U};
    const uint8_t zero[4] = {0U};
    uint32_t readback = BOOT_ERASED_WORD;
    uint8_t status;
    if (!available || !WaitReady() || !Command(0x06U) ||
        !ReadStatus(&status) || (status & 2U) == 0U)
    {
        return false;
    }
    BootSpi_Select(true);
    const bool ok = BootSpi_Transfer(command, NULL, sizeof(command)) &&
                    BootSpi_Transfer(zero, NULL, sizeof(zero));
    BootSpi_Select(false);
    if (!ok)
    {
        (void)Command(0x04U); /* Clear WEL if a failed command never started a write. */
        return false;
    }
    return WaitReady() && W25Q16_Read(28U, &readback, sizeof(readback)) && readback == 0U;
}
