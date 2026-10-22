/* Native host test of the production core, using a NOR Flash model.
 * gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc tests/test_bootloader.c
 *     Core/Src/bootloader.c -o build/test_bootloader.exe
 */
#include "bootloader.h"
#include "boot_port.h"
#include "boot_update_storage.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static uint8_t flash[BOOT_FLASH_SIZE];
static uint8_t saved[BOOT_FLASH_SIZE];
static uint8_t new_image[67]; /* exercises padding of the last word */
static jmp_buf reset_point;
static unsigned operation;
static unsigned cut_at;
static unsigned erases;
static bool unlocked;
static bool fail_unlock;
static bool fail_program;
static bool fail_erase;
static uint32_t crc_register;
static uint32_t crc_words;
static bool crc_active;

/* Independent model of the F405's fixed MSB-first CRC peripheral. */
static uint32_t ReverseBits(uint32_t word)
{
    uint32_t reversed = 0U;
    for (unsigned bit = 0U; bit < 32U; ++bit)
    {
        reversed = (reversed << 1U) | (word & 1U);
        word >>= 1U;
    }
    return reversed;
}

void BootPort_CrcBegin(void)
{
    assert(!crc_active);
    crc_active = true;
    crc_register = UINT32_C(0xFFFFFFFF);
    crc_words = 0U;
}

void BootPort_CrcFeedWord(uint32_t word)
{
    assert(crc_active);
    ++crc_words;
    crc_register ^= ReverseBits(word);
    for (unsigned bit = 0U; bit < 32U; ++bit)
    {
        const bool high_bit = (crc_register & UINT32_C(0x80000000)) != 0U;
        crc_register <<= 1U;
        if (high_bit) { crc_register ^= UINT32_C(0x04C11DB7); }
    }
}

uint32_t BootPort_CrcFinish(void)
{
    assert(crc_active);
    crc_active = false;
    return ReverseBits(crc_register);
}

static uint32_t ReferenceCrc32(const uint8_t *data, uint32_t size)
{
    uint32_t crc = UINT32_C(0xFFFFFFFF);
    for (uint32_t i = 0U; i < size; ++i)
    {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit)
        {
            const bool low_bit = (crc & 1U) != 0U;
            crc >>= 1U;
            if (low_bit) { crc ^= UINT32_C(0xEDB88320); }
        }
    }
    return ~crc;
}

static void TestCrcHardwareCompatibility(void)
{
    uint8_t data[520];
    uint32_t random = UINT32_C(0x13579BDF);
    for (unsigned i = 0U; i < sizeof(data); ++i)
    {
        random ^= random << 13U;
        random ^= random >> 17U;
        random ^= random << 5U;
        data[i] = (uint8_t)random;
    }
    assert(Boot_Crc32(NULL, 0U) == 0U);
    for (unsigned offset = 0U; offset < 4U; ++offset)
    {
        for (uint32_t size = 0U; size <= 516U; ++size)
        {
            assert(Boot_Crc32(data + offset, size) == ReferenceCrc32(data + offset, size));
            assert(crc_words == size / 4U && !crc_active);
        }
    }
    puts("PASS: hardware CRC model matches ISO-HDLC for 2068 length/alignment cases");
}

static uint8_t external[BOOT_STORAGE_SIZE];
static uint8_t external_saved[BOOT_STORAGE_SIZE];
static bool storage_open;
static bool storage_present;
static bool fail_consume;
static bool fail_install_read;
static unsigned read_count;
static unsigned fail_read_at;

static uint8_t *At(uint32_t address)
{
    assert(address >= BOOT_FLASH_BASE && address < BOOT_FLASH_BASE + BOOT_FLASH_SIZE);
    return flash + (address - BOOT_FLASH_BASE);
}

const void *BootPort_Map(uint32_t address) { return At(address); }
bool BootPort_Unlock(void) { unlocked = !fail_unlock; return unlocked; }
void BootPort_Lock(void) { unlocked = false; }
bool BootPort_StorageInit(void) { storage_open = storage_present; return storage_open; }
void BootPort_StorageDeInit(void) { storage_open = false; }

bool BootPort_StorageRead(uint32_t offset, void *data, uint32_t size)
{
    assert(storage_open && offset <= BOOT_STORAGE_SIZE && size <= BOOT_STORAGE_SIZE - offset);
    ++read_count;
    if (read_count == fail_read_at || (fail_install_read && erases != 0U)) { return false; }
    memcpy(data, external + offset, size);
    return true;
}

static void Cut(void)
{
    if (++operation == cut_at)
    {
        unlocked = storage_open = false;
        longjmp(reset_point, 1);
    }
}

bool BootPort_StorageConsume(void)
{
    BootImageManifest installed;
    memcpy(&installed, At(BOOT_INSTALLED_ADDRESS), sizeof(installed));
    assert(storage_open && !unlocked && Boot_ImageValid(BOOT_APP_ADDRESS, &installed));
    if (fail_consume) { return false; }
    memset(external + 28U, 0, 2U);
    Cut();
    memset(external + 28U, 0, 4U);
    return true;
}

static bool Erase(uint32_t address, uint32_t size)
{
    assert(unlocked);
    ++erases;
    memset(At(address), 0xFF, size / 2U);
    Cut();
    if (fail_erase) { return false; }
    memset(At(address + size / 2U), 0xFF, size / 2U);
    return true;
}

bool BootPort_EraseInstalled(void) { return Erase(BOOT_INSTALLED_ADDRESS, 0x4000U); }
bool BootPort_EraseApplication(void) { return Erase(BOOT_APP_ADDRESS, BOOT_IMAGE_CAPACITY); }

bool BootPort_ProgramWord(uint32_t address, uint32_t value)
{
    const bool allowed = (address >= BOOT_APP_ADDRESS &&
                          address < BOOT_APP_ADDRESS + BOOT_IMAGE_CAPACITY) ||
                         (address >= BOOT_INSTALLED_ADDRESS && address < BOOT_INSTALLED_ADDRESS + 28U);
    assert(unlocked && allowed && (address & 3U) == 0U);
    uint32_t old;
    memcpy(&old, At(address), 4U);
    assert((old & value) == value);
    memcpy(At(address), &value, 2U);
    Cut();
    if (fail_program) { return false; }
    memcpy(At(address), &value, 4U);
    return true;
}

static BootImageManifest Manifest(const uint8_t *image, uint32_t size, uint32_t version)
{
    BootImageManifest m = {BOOT_IMAGE_MAGIC, BOOT_IMAGE_FORMAT, size,
                           ReferenceCrc32(image, size), version, 0U,
                           BOOT_IMAGE_COMMITTED, BOOT_ERASED_WORD};
    m.header_crc = ReferenceCrc32((const uint8_t *)&m, 20U);
    return m;
}

static void MetadataCrc(BootUpdateRecord *r)
{
    uint8_t bytes[52];
    memcpy(bytes, r, 24U);
    memcpy(bytes + 24U, (uint8_t *)r + 28U, 28U);
    r->metadata_crc = ReferenceCrc32(bytes, sizeof(bytes));
}

static void StageRecord(uint32_t size, uint32_t mode)
{
    BootUpdateRecord r = {0};
    r.image = Manifest(external + BOOT_STORAGE_IMAGE, size, 2U);
    r.storage_magic = BOOT_STORAGE_MAGIC;
    r.layout_version = BOOT_STORAGE_VERSION;
    r.image_offset = BOOT_STORAGE_IMAGE;
    r.image_capacity = BOOT_STORAGE_CAPACITY;
    r.apply_mode = mode;
    r.reserved = r.reserved_tail = BOOT_ERASED_WORD;
    MetadataCrc(&r);
    memcpy(external, &r, sizeof(r));
}

static void Setup(bool installed)
{
    memset(flash, 0x77, sizeof(flash));
    memset(At(BOOT_INSTALLED_ADDRESS), 0xFF, 0x4000U);
    memset(At(BOOT_APP_ADDRESS), 0xFF, BOOT_IMAGE_CAPACITY);
    memset(external, 0xA5, sizeof(external));
    memset(external, 0xFF, 0x1000U);
    operation = cut_at = erases = read_count = fail_read_at = 0U;
    unlocked = storage_open = fail_unlock = fail_program = fail_erase = false;
    fail_consume = fail_install_read = false;
    storage_present = true;
    for (unsigned i = 0U; i < sizeof(new_image); ++i) { new_image[i] = (uint8_t)i; }
    const uint32_t vectors[] = {0x20020000U, BOOT_APP_ADDRESS + 9U};
    memcpy(new_image, vectors, sizeof(vectors));
    memcpy(external + BOOT_STORAGE_IMAGE, new_image, sizeof(new_image));
    StageRecord(sizeof(new_image), 0U);
    if (installed)
    {
        uint8_t old_image[sizeof(new_image)];
        memcpy(old_image, new_image, sizeof(old_image));
        old_image[20] ^= 0xFFU;
        memcpy(At(BOOT_APP_ADDRESS), old_image, sizeof(old_image));
        BootImageManifest m = Manifest(old_image, sizeof(old_image), 1U);
        memcpy(At(BOOT_INSTALLED_ADDRESS), &m, sizeof(m));
    }
}

static void Snapshot(void)
{
    memcpy(saved, flash, sizeof(flash));
    memcpy(external_saved, external, sizeof(external));
}

static void CheckProtected(void)
{
    assert(memcmp(flash, saved, BOOT_INSTALLED_ADDRESS - BOOT_FLASH_BASE) == 0);
    assert(memcmp(At(BOOT_INSTALLED_ADDRESS + 0x4000U), saved + 0xC000U, 0x14000U) == 0);
    assert(memcmp(At(BOOT_APP_ADDRESS + BOOT_IMAGE_CAPACITY), saved + 0x80000U, 0x80000U) == 0);
    assert(memcmp(external, external_saved, 28U) == 0);
    assert(memcmp(external + 32U, external_saved + 32U, sizeof(external) - 32U) == 0);
}

static void CheckInstalled(void)
{
    BootImageManifest m;
    memcpy(&m, At(BOOT_INSTALLED_ADDRESS), sizeof(m));
    assert(Boot_ImageValid(BOOT_APP_ADDRESS, &m) && m.version == 2U);
    assert(memcmp(At(BOOT_APP_ADDRESS), new_image, sizeof(new_image)) == 0);
    assert(*At(BOOT_APP_ADDRESS + sizeof(new_image)) == 0xFFU);
    assert(!unlocked && !storage_open && !crc_active);
}

static void TestValidation(void)
{
    assert(Boot_Crc32("123456789", 9U) == 0xCBF43926U);
    const uint32_t bad[][2] = {{0x20020008U, BOOT_APP_ADDRESS + 9U},
        {0x20000000U, BOOT_APP_ADDRESS + 9U}, {0x20000004U, BOOT_APP_ADDRESS + 9U},
        {0x20020000U, BOOT_STAGE_ADDRESS + 9U}, {0x20020000U, BOOT_APP_ADDRESS + 8U},
        {0x20020000U, BOOT_APP_ADDRESS + 0x100U}};
    for (unsigned i = 0U; i < sizeof(bad) / sizeof(bad[0]); ++i)
    {
        Setup(true);
        memcpy(external + BOOT_STORAGE_IMAGE, bad[i], 8U);
        StageRecord(sizeof(new_image), 0U);
        assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U && !storage_open);
    }
    const struct { uint32_t offset; uint32_t value; } invalid[] = {
        {0, 0}, {4, 2}, {8, 7}, {8, BOOT_IMAGE_CAPACITY + 1U}, {12, 0},
        {20, 0}, {24, BOOT_IMAGE_COMMITTED ^ 1U}, {28, 0}, {28, 0xFFFF0000U},
        {32, 0}, {36, 2}, {40, 0}, {44, BOOT_IMAGE_CAPACITY + 4U},
        {48, 2}, {52, 0}, {56, 0}, {60, 0}};
    for (unsigned i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
    {
        Setup(true);
        BootUpdateRecord r;
        memcpy(&r, external, sizeof(r));
        memcpy((uint8_t *)&r + invalid[i].offset, &invalid[i].value, 4U);
        if (invalid[i].offset != 20U) { r.image.header_crc = ReferenceCrc32((uint8_t *)&r, 20U); }
        if (invalid[i].offset != 56U) { MetadataCrc(&r); }
        memcpy(external, &r, sizeof(r));
        Snapshot();
        assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U && !storage_open);
        assert(memcmp(flash, saved, sizeof(flash)) == 0);
        assert(memcmp(external, external_saved, sizeof(external)) == 0);
    }
    Setup(false);
    memset(external, 0xFF, 64U);
    /* A legacy internal staged request must not be used anymore. */
    BootImageManifest m = Manifest(new_image, sizeof(new_image), 2U);
    memcpy(At(BOOT_REQUEST_ADDRESS), &m, sizeof(m));
    memcpy(At(BOOT_STAGE_ADDRESS), new_image, sizeof(new_image));
    assert(Boot_PrepareApplication() == BOOT_NO_VALID_APP && erases == 0U);
    puts("PASS: external v1 record validation, vector checks and legacy storage ignored");
}

static void TestFailures(void)
{
    Setup(true);
    storage_present = false;
    Snapshot();
    assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U && !storage_open);
    assert(memcmp(flash, saved, sizeof(flash)) == 0);
    Setup(false);
    storage_present = false;
    assert(Boot_PrepareApplication() == BOOT_UPDATE_FAILED && !storage_open);
    for (unsigned read = 1U; read <= 3U; ++read)
    {
        Setup(true);
        fail_read_at = read;
        assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U && !storage_open && !crc_active);
    }
    Setup(true);
    external[BOOT_STORAGE_IMAGE + 20U] ^= 1U;
    assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U);
    Setup(true);
    fail_unlock = true;
    assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U);
    for (unsigned kind = 0U; kind < 3U; ++kind)
    {
        Setup(true);
        fail_program = kind == 0U;
        fail_erase = kind == 1U;
        fail_install_read = kind == 2U;
        Snapshot();
        assert(Boot_PrepareApplication() == BOOT_UPDATE_FAILED && !storage_open && !unlocked);
        fail_program = fail_erase = fail_install_read = false;
        assert(Boot_PrepareApplication() == BOOT_READY);
        CheckInstalled();
        CheckProtected();
    }
    Setup(true);
    fail_consume = true;
    assert(Boot_PrepareApplication() == BOOT_READY);
    CheckInstalled();
    const unsigned before = erases;
    fail_consume = false;
    assert(Boot_PrepareApplication() == BOOT_READY && erases == before);
    assert(external[28] == 0U);
    puts("PASS: missing device, SPI failures, Flash failures and acknowledgement retry");
}

static void TestPowerCuts(void)
{
    Setup(true);
    Snapshot();
    assert(Boot_PrepareApplication() == BOOT_READY);
    const unsigned count = operation;
    for (unsigned cut = 1U; cut <= count; ++cut)
    {
        memcpy(flash, saved, sizeof(flash));
        memcpy(external, external_saved, sizeof(external));
        operation = erases = read_count = 0U;
        cut_at = cut;
        if (setjmp(reset_point) == 0)
        {
            (void)Boot_PrepareApplication();
            assert(!"expected power cut");
        }
        cut_at = 0U;
        assert(Boot_PrepareApplication() == BOOT_READY);
        CheckInstalled();
        CheckProtected();
        const unsigned before = erases;
        assert(Boot_PrepareApplication() == BOOT_READY && erases == before);
    }
    printf("PASS: power loss at all %u internal erase/program + external consume operations\n", count);
}

static void TestSizes(void)
{
    const uint32_t sizes[] = {9U, 255U, 256U, 257U, 258U, 259U, 1025U, BOOT_IMAGE_CAPACITY};
    for (unsigned n = 0U; n < sizeof(sizes) / sizeof(sizes[0]); ++n)
    {
        Setup(true);
        for (uint32_t i = sizeof(new_image); i < sizes[n]; ++i)
        {
            external[BOOT_STORAGE_IMAGE + i] = (uint8_t)i;
        }
        StageRecord(sizes[n], n & 1U);
        Snapshot();
        assert(Boot_PrepareApplication() == BOOT_READY && !storage_open && !crc_active);
        assert(memcmp(At(BOOT_APP_ADDRESS), external + BOOT_STORAGE_IMAGE, sizes[n]) == 0);
        BootImageManifest installed;
        memcpy(&installed, At(BOOT_INSTALLED_ADDRESS), sizeof(installed));
        assert(installed.image_size == sizes[n] && Boot_ImageValid(BOOT_APP_ADDRESS, &installed));
        CheckProtected();
        const unsigned before = erases;
        assert(Boot_PrepareApplication() == BOOT_READY && erases == before);
    }
    /* Fail in a later CRC chunk before any internal erase. */
    Setup(true);
    StageRecord(1024U, 0U);
    fail_read_at = 5U;
    assert(Boot_PrepareApplication() == BOOT_READY && erases == 0U && !crc_active);
    puts("PASS: odd tails, 256-byte boundaries, both apply modes and maximum 384 KiB");
}

int main(void)
{
    TestCrcHardwareCompatibility();
    TestValidation();
    TestFailures();
    TestPowerCuts();
    TestSizes();
    return 0;
}
