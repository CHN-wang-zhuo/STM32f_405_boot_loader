/* Links the production W25Q16 command driver against a byte-level SPI NOR model. */
#include "w25q16.h"
#include "boot_spi.h"
#include "boot_update_storage.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t memory[BOOT_STORAGE_SIZE];
static uint8_t snapshot[BOOT_STORAGE_SIZE];
static uint8_t id[3];
static uint8_t command;
static unsigned position;
static uint32_t address;
static uint8_t page[256];
static unsigned page_count;
static bool selected;
static bool enabled;
static bool wel;
static bool stuck_busy;
static bool write_protected;
static unsigned busy_reads;
static uint32_t tick;
static int fail_after;

void BootSpi_Init(void) { enabled = true; selected = false; }
void BootSpi_DeInit(void) { enabled = selected = false; }
uint32_t BootSpi_Tick(void) { return tick++; }

void BootSpi_Select(bool active)
{
    assert(enabled);
    if (active)
    {
        assert(!selected);
        selected = true;
        command = 0U;
        position = page_count = 0U;
        address = 0U;
        return;
    }
    assert(selected);
    if (command == 0x06U) { wel = true; }
    if (command == 0x04U) { wel = false; }
    if (command == 0x02U && position >= 4U && page_count != 0U && wel)
    {
        assert(address == 28U && page_count <= 4U); /* Only request consumption. */
        if (!write_protected)
        {
            for (unsigned i = 0U; i < page_count; ++i) { memory[address + i] &= page[i]; }
        }
        wel = false;
        busy_reads = 2U;
    }
    selected = false;
}

bool BootSpi_Transfer(const uint8_t *tx, uint8_t *rx, uint32_t size)
{
    assert(enabled && selected);
    for (uint32_t i = 0U; i < size; ++i)
    {
        if (fail_after == 0) { return false; }
        if (fail_after > 0) { --fail_after; }
        const uint8_t out = tx != NULL ? tx[i] : 0xFFU;
        uint8_t in = 0xFFU;
        if (position == 0U)
        {
            command = out;
            assert(command == 0x03U || command == 0x05U || command == 0x9FU ||
                   command == 0x06U || command == 0x04U || command == 0x02U);
        }
        else if (command == 0x05U)
        {
            in = (uint8_t)((stuck_busy || busy_reads != 0U ? 1U : 0U) | (wel ? 2U : 0U));
            if (busy_reads != 0U) { --busy_reads; }
        }
        else if (command == 0x9FU) { in = id[(position - 1U) % 3U]; }
        else if (command == 0x03U || command == 0x02U)
        {
            if (position <= 3U) { address = (address << 8U) | out; }
            else if (command == 0x03U)
            {
                assert(address < sizeof(memory));
                in = memory[address++];
            }
            else
            {
                assert(page_count < sizeof(page));
                page[page_count++] = out;
            }
        }
        if (rx != NULL) { rx[i] = in; }
        ++position;
    }
    return true;
}

static void Setup(void)
{
    memset(memory, 0xA5, sizeof(memory));
    memset(memory + 28U, 0xFF, 4U);
    memcpy(snapshot, memory, sizeof(memory));
    id[0] = 0xEFU; id[1] = 0x40U; id[2] = 0x15U;
    stuck_busy = write_protected = wel = false;
    busy_reads = tick = 0U;
    fail_after = -1;
    enabled = selected = false;
}

static void CheckProtected(void)
{
    assert(!selected);
    assert(memcmp(memory, snapshot, 28U) == 0);
    assert(memcmp(memory + 32U, snapshot + 32U, sizeof(memory) - 32U) == 0);
}

int main(void)
{
    Setup();
    assert(W25Q16_Init() && g_w25q16_jedec_id == 0xEF4015U);
    uint8_t bytes[259];
    assert(W25Q16_Read(0x100FDU, bytes, sizeof(bytes)));
    assert(memcmp(bytes, memory + 0x100FDU, sizeof(bytes)) == 0);
    assert(W25Q16_Read(BOOT_STORAGE_SIZE - 1U, bytes, 1U));
    assert(W25Q16_Read(BOOT_STORAGE_SIZE, NULL, 0U));
    assert(!W25Q16_Read(BOOT_STORAGE_SIZE, bytes, 1U));
    assert(!W25Q16_Read(UINT32_MAX, bytes, 1U));
    assert(!W25Q16_Read(0U, NULL, 1U));
    assert(W25Q16_ConsumeRequest());
    assert(memcmp(memory + 28U, "\0\0\0\0", 4U) == 0);
    CheckProtected();
    W25Q16_DeInit();
    assert(!enabled && !W25Q16_Read(0U, bytes, 1U));

    Setup(); id[0] = 0U;
    assert(!W25Q16_Init() && !selected);
    W25Q16_DeInit();
    Setup(); id[2] = 0x16U;
    assert(!W25Q16_Init());
    W25Q16_DeInit();
    Setup(); stuck_busy = true;
    assert(!W25Q16_Init() && tick >= 1000U && !selected);
    W25Q16_DeInit();
    Setup(); tick = UINT32_MAX - 10U; stuck_busy = true;
    assert(!W25Q16_Init() && !selected); /* wrapping timeout */
    W25Q16_DeInit();
    Setup(); assert(W25Q16_Init()); write_protected = true;
    assert(!W25Q16_ConsumeRequest());
    assert(memcmp(memory, snapshot, sizeof(memory)) == 0);
    W25Q16_DeInit();

    for (int failure = 0; failure < 65; ++failure)
    {
        Setup();
        assert(W25Q16_Init());
        fail_after = failure;
        (void)W25Q16_ConsumeRequest();
        CheckProtected();
        W25Q16_DeInit();
        assert(!enabled && !selected);
    }
    Setup(); assert(W25Q16_Init()); fail_after = 5;
    assert(!W25Q16_Read(0x10000U, bytes, sizeof(bytes)) && !selected);
    W25Q16_DeInit();
    puts("PASS: JEDEC, SPI reads/bounds, BUSY timeout, readback, 65 transfer failures and CS release");
    return 0;
}
