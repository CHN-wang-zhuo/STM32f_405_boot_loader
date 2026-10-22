/* Native host test of the production core, using a NOR Flash model.
 * gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc tests/test_bootloader.c
 *     Core/Src/bootloader.c -o build/test_bootloader.exe
 */
#include "bootloader.h"
#include "boot_port.h"
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

static uint8_t *At(uint32_t address)
{
    assert(address >= BOOT_FLASH_BASE && address < BOOT_FLASH_BASE + BOOT_FLASH_SIZE);
    return flash + (address - BOOT_FLASH_BASE);
}

const void *BootPort_Map(uint32_t address) { return At(address); }
bool BootPort_Unlock(void) { unlocked = !fail_unlock; return unlocked; }
void BootPort_Lock(void) { unlocked = false; }

static void Cut(void)
{
    if (++operation == cut_at)
    {
        unlocked = false;
        longjmp(reset_point, 1);
    }
}

static bool Erase(uint32_t address, uint32_t size)
{
    assert(unlocked);
    ++erases;
    memset(At(address), 0xFF, size / 2U);
    Cut(); /* reset partway through erasing a sector/range */
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
                         (address >= BOOT_INSTALLED_ADDRESS && address < BOOT_INSTALLED_ADDRESS + 28U) ||
                         (address == BOOT_REQUEST_ADDRESS + 28U && value == 0U);
    assert(unlocked && allowed && (address & 3U) == 0U);
    uint32_t old;
    memcpy(&old, At(address), 4U);
    assert((old & value) == value); /* NOR cannot change 0 back to 1 */
    memcpy(At(address), &value, 2U);
    Cut(); /* includes a torn commit / consumed marker */
    if (fail_program) { return false; }
    memcpy(At(address), &value, 4U);
    return true;
}

static BootImageManifest Manifest(const uint8_t *image, uint32_t size, uint32_t version)
{
    BootImageManifest m = {BOOT_IMAGE_MAGIC, BOOT_IMAGE_FORMAT, size,
                           Boot_Crc32(image, size), version, 0U,
                           BOOT_IMAGE_COMMITTED, BOOT_ERASED_WORD};
    m.header_crc = Boot_Crc32(&m, 20U);
    return m;
}

static void Setup(bool installed)
{
    memset(flash, 0xFF, sizeof(flash));
    /* Sentinels for regions that must never be changed. */
    memset(At(BOOT_FLASH_BASE), 0x5A, BOOT_LOADER_SIZE);
    memset(At(0x08004000), 0x3C, 0x4000U); /* sector 1 is now reserved */
    memset(At(0x08010000), 0xA5, 0x10000U);
    memset(At(0x080E0000), 0xA5, 0x20000U);
    operation = cut_at = erases = 0U;
    unlocked = fail_unlock = fail_program = fail_erase = false;
    for (unsigned i = 0U; i < sizeof(new_image); ++i) { new_image[i] = (uint8_t)i; }
    const uint32_t vectors[] = {0x20020000U, BOOT_APP_ADDRESS + 9U};
    memcpy(new_image, vectors, sizeof(vectors));
    memcpy(At(BOOT_STAGE_ADDRESS), new_image, sizeof(new_image));
    BootImageManifest m = Manifest(new_image, sizeof(new_image), 2U);
    memcpy(At(BOOT_REQUEST_ADDRESS), &m, sizeof(m));
    if (installed)
    {
        uint8_t old_image[sizeof(new_image)];
        memcpy(old_image, new_image, sizeof(old_image));
        old_image[20] ^= 0xFFU;
        memcpy(At(BOOT_APP_ADDRESS), old_image, sizeof(old_image));
        m = Manifest(old_image, sizeof(old_image), 1U);
        memcpy(At(BOOT_INSTALLED_ADDRESS), &m, sizeof(m));
    }
}

static void CheckInstalled(void)
{
    BootImageManifest m;
    memcpy(&m, At(BOOT_INSTALLED_ADDRESS), sizeof(m));
    assert(Boot_ImageValid(BOOT_APP_ADDRESS, &m));
    assert(m.version == 2U);
    assert(memcmp(At(BOOT_APP_ADDRESS), new_image, sizeof(new_image)) == 0);
    assert(*At(BOOT_APP_ADDRESS + sizeof(new_image)) == 0xFFU);
    assert(memcmp(At(BOOT_STAGE_ADDRESS), new_image, sizeof(new_image)) == 0);
    uint32_t consumed;
    memcpy(&consumed, At(BOOT_REQUEST_ADDRESS + 28U), 4U);
    assert(consumed == 0U);
    assert(!unlocked);
}

static void TestValidation(void)
{
    assert(Boot_Crc32("123456789", 9U) == 0xCBF43926U);
    Setup(false);
    BootImageManifest m = Manifest(new_image, sizeof(new_image), 2U);
    assert(Boot_ImageValid(BOOT_STAGE_ADDRESS, &m));
    m.commit ^= 1U;
    assert(!Boot_ManifestValid(&m));
    m.commit ^= 1U;
    m.image_size = BOOT_IMAGE_CAPACITY + 1U;
    m.header_crc = Boot_Crc32(&m, 20U);
    assert(!Boot_ImageValid(BOOT_STAGE_ADDRESS, &m));
    m.image_size = 7U;
    m.header_crc = Boot_Crc32(&m, 20U);
    assert(!Boot_ManifestValid(&m));
    const uint32_t bad_vectors[][2] = {
        {0x20020008U, BOOT_APP_ADDRESS + 9U},
        {0x20000000U, BOOT_APP_ADDRESS + 9U},
        {0x20000004U, BOOT_APP_ADDRESS + 9U},
        {0x20020000U, BOOT_STAGE_ADDRESS + 9U},
        {0x20020000U, BOOT_APP_ADDRESS + 8U},
        {0x20020000U, BOOT_APP_ADDRESS + 0x100U}
    };
    for (unsigned i = 0U; i < sizeof(bad_vectors) / sizeof(bad_vectors[0]); ++i)
    {
        memcpy(At(BOOT_STAGE_ADDRESS), bad_vectors[i], 8U);
        m = Manifest(At(BOOT_STAGE_ADDRESS), sizeof(new_image), 2U);
        assert(!Boot_ImageValid(BOOT_STAGE_ADDRESS, &m));
    }
    const uint32_t ccm_vectors[] = {0x10010000U, BOOT_APP_ADDRESS + 9U};
    memcpy(At(BOOT_STAGE_ADDRESS), ccm_vectors, 8U);
    m = Manifest(At(BOOT_STAGE_ADDRESS), sizeof(new_image), 2U);
    assert(Boot_ImageValid(BOOT_STAGE_ADDRESS, &m));
}

static void TestFailures(void)
{
    Setup(false);
    memset(At(BOOT_REQUEST_ADDRESS), 0xFF, 0x4000U);
    memcpy(At(BOOT_APP_ADDRESS), new_image, sizeof(new_image));
    assert(Boot_PrepareApplication() == BOOT_NO_VALID_APP); /* no vector-only bypass */
    assert(erases == 0U);

    Setup(true);
    At(BOOT_STAGE_ADDRESS)[20] ^= 1U;
    assert(Boot_PrepareApplication() == BOOT_READY); /* preserve old APP */
    assert(erases == 0U);
    Setup(true);
    At(BOOT_REQUEST_ADDRESS)[24] ^= 1U;
    assert(Boot_PrepareApplication() == BOOT_READY);
    assert(erases == 0U);
    Setup(false);
    fail_unlock = true;
    assert(Boot_PrepareApplication() == BOOT_UPDATE_FAILED);
    fail_unlock = false;
    assert(Boot_PrepareApplication() == BOOT_READY);
    CheckInstalled();

    Setup(true);
    fail_program = true;
    assert(Boot_PrepareApplication() == BOOT_UPDATE_FAILED);
    assert(!unlocked);
    fail_program = false;
    assert(Boot_PrepareApplication() == BOOT_READY);
    CheckInstalled();

    Setup(true);
    fail_erase = true;
    assert(Boot_PrepareApplication() == BOOT_UPDATE_FAILED);
    assert(!unlocked);
    fail_erase = false;
    assert(Boot_PrepareApplication() == BOOT_READY);
    CheckInstalled();
}

static void TestPowerCuts(void)
{
    Setup(true);
    memcpy(saved, flash, sizeof(flash));
    assert(Boot_PrepareApplication() == BOOT_READY);
    const unsigned count = operation;
    CheckInstalled();
    for (unsigned cut = 1U; cut <= count; ++cut)
    {
        memcpy(flash, saved, sizeof(flash));
        operation = erases = 0U;
        cut_at = cut;
        if (setjmp(reset_point) == 0)
        {
            (void)Boot_PrepareApplication();
            assert(!"expected simulated power cut");
        }
        cut_at = 0U;
        assert(Boot_PrepareApplication() == BOOT_READY);
        CheckInstalled();
        assert(memcmp(At(BOOT_FLASH_BASE), saved, BOOT_LOADER_SIZE) == 0);
        assert(memcmp(At(0x08004000), saved + 0x4000U, 0x4000U) == 0);
        assert(memcmp(At(0x08010000), saved + 0x10000U, 0x10000U) == 0);
        assert(memcmp(At(BOOT_STAGE_ADDRESS), saved + 0x80000U, 0x60000U) == 0);
        assert(memcmp(At(0x080E0000), saved + 0xE0000U, 0x20000U) == 0);
        const unsigned before = erases;
        assert(Boot_PrepareApplication() == BOOT_READY);
        assert(erases == before); /* consumed requests do not reinstall */
    }
    printf("PASS: reset recovery at all %u erase/program operations\n", count);
}

static void TestMaximumImage(void)
{
    Setup(false);
    for (uint32_t i = 0U; i < BOOT_IMAGE_CAPACITY; ++i)
    {
        At(BOOT_STAGE_ADDRESS)[i] = (uint8_t)i;
    }
    const uint32_t vectors[] = {0x20020000U, BOOT_APP_ADDRESS + BOOT_IMAGE_CAPACITY - 1U};
    memcpy(At(BOOT_STAGE_ADDRESS), vectors, sizeof(vectors));
    BootImageManifest m = Manifest(At(BOOT_STAGE_ADDRESS), BOOT_IMAGE_CAPACITY, 3U);
    memcpy(At(BOOT_REQUEST_ADDRESS), &m, sizeof(m));
    memcpy(saved, flash, sizeof(flash));
    assert(Boot_PrepareApplication() == BOOT_READY);
    assert(Boot_ImageValid(BOOT_APP_ADDRESS, &m));
    assert(memcmp(At(BOOT_APP_ADDRESS), At(BOOT_STAGE_ADDRESS), BOOT_IMAGE_CAPACITY) == 0);
    assert(memcmp(At(BOOT_FLASH_BASE), saved, BOOT_LOADER_SIZE) == 0);
    assert(memcmp(At(0x08004000), saved + 0x4000U, 0x4000U) == 0);
    assert(memcmp(At(BOOT_STAGE_ADDRESS), saved + 0x80000U, 0x60000U) == 0);
    assert(memcmp(At(0x080E0000), saved + 0xE0000U, 0x20000U) == 0);
    puts("PASS: full 384 KiB image and partition boundaries");
}

int main(void)
{
    TestCrcHardwareCompatibility();
    TestValidation();
    TestFailures();
    TestPowerCuts();
    TestMaximumImage();
    puts("PASS: CRC, vectors, metadata, corrupt image, Flash failures and protected regions");
    return 0;
}
