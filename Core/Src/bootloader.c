#include "bootloader.h"
#include "boot_port.h"
#include <string.h>

uint32_t Boot_Crc32(const void *data, uint32_t size)
{
    const uint8_t *bytes = data;
    BootPort_CrcBegin();
    while (size >= 4U)
    {
        uint32_t word;
        /* memcpy also supports callers whose buffer is not word aligned. */
        memcpy(&word, bytes, sizeof(word));
        BootPort_CrcFeedWord(word);
        bytes += 4U;
        size -= 4U;
    }
    uint32_t crc = BootPort_CrcFinish();

    /* F405 CRC accepts 32-bit words. Continue its running state in software
     * for the final 0-3 bytes; padding would change the existing wire CRC. */
    for (uint32_t i = 0; i < size; ++i)
    {
        crc ^= bytes[i];
        for (uint32_t bit = 0; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((0U - (crc & 1U)) & UINT32_C(0xEDB88320));
        }
    }
    return ~crc;
}

bool Boot_ManifestValid(const BootImageManifest *manifest)
{
    return manifest->magic == BOOT_IMAGE_MAGIC &&
           manifest->format == BOOT_IMAGE_FORMAT &&
           manifest->commit == BOOT_IMAGE_COMMITTED &&
           manifest->image_size >= 8U &&
           manifest->image_size <= BOOT_IMAGE_CAPACITY &&
           manifest->header_crc == Boot_Crc32(manifest, 20U);
}

bool Boot_ImageValid(uint32_t storage_address, const BootImageManifest *manifest)
{
    uint32_t vectors[2];
    if ((storage_address != BOOT_APP_ADDRESS && storage_address != BOOT_STAGE_ADDRESS) ||
        !Boot_ManifestValid(manifest))
    {
        return false;
    }

    memcpy(vectors, BootPort_Map(storage_address), sizeof(vectors));
    const uint32_t sp = vectors[0];
    const uint32_t reset = vectors[1] & ~UINT32_C(1);
    /* SRAM1+SRAM2 (128 KiB), or CCM (64 KiB). An initial SP may equal RAM end. */
    const bool valid_stack = ((sp > UINT32_C(0x20000000) && sp <= UINT32_C(0x20020000)) ||
                             (sp > UINT32_C(0x10000000) && sp <= UINT32_C(0x10010000))) &&
                            (sp & 7U) == 0U;
    /* Staged firmware is linked for the execution address, NOT staging Flash. */
    return valid_stack && (vectors[1] & 1U) != 0U &&
           reset >= BOOT_APP_ADDRESS + 8U &&
           reset < BOOT_APP_ADDRESS + manifest->image_size &&
           Boot_Crc32(BootPort_Map(storage_address), manifest->image_size) == manifest->image_crc;
}

static bool InstalledValid(BootImageManifest *installed)
{
    memcpy(installed, BootPort_Map(BOOT_INSTALLED_ADDRESS), sizeof(*installed));
    return Boot_ImageValid(BOOT_APP_ADDRESS, installed);
}

static bool SameImage(const BootImageManifest *a, const BootImageManifest *b)
{
    return a->image_size == b->image_size && a->image_crc == b->image_crc &&
           a->version == b->version;
}

static bool WriteInstalledManifest(const BootImageManifest *request)
{
    /* Sector has already been erased. Never publish an image until copy CRC passes. */
    for (uint32_t offset = 0U; offset < offsetof(BootImageManifest, commit); offset += 4U)
    {
        uint32_t word;
        memcpy(&word, (const uint8_t *)request + offset, sizeof(word));
        if (!BootPort_ProgramWord(BOOT_INSTALLED_ADDRESS + offset, word))
        {
            return false;
        }
    }
    return BootPort_ProgramWord(BOOT_INSTALLED_ADDRESS + offsetof(BootImageManifest, commit),
                                BOOT_IMAGE_COMMITTED);
}

static bool Install(const BootImageManifest *request)
{
    if (!BootPort_EraseInstalled() || !BootPort_EraseApplication())
    {
        return false;
    }

    for (uint32_t offset = 0U; offset < request->image_size; offset += 4U)
    {
        uint32_t word = BOOT_ERASED_WORD;
        const uint32_t remaining = request->image_size - offset;
        const uint32_t count = remaining < 4U ? remaining : 4U;
        memcpy(&word, BootPort_Map(BOOT_STAGE_ADDRESS + offset), count);
        if (!BootPort_ProgramWord(BOOT_APP_ADDRESS + offset, word))
        {
            return false;
        }
    }

    return Boot_ImageValid(BOOT_APP_ADDRESS, request) && WriteInstalledManifest(request);
}

BootStatus Boot_PrepareApplication(void)
{
    BootImageManifest request;
    BootImageManifest installed;
    memcpy(&request, BootPort_Map(BOOT_REQUEST_ADDRESS), sizeof(request));

    if (Boot_ManifestValid(&request) && request.consumed != BOOT_REQUEST_CONSUMED)
    {
        /* If power failed after installed metadata commit, only finish the marker. */
        const bool already_installed = InstalledValid(&installed) && SameImage(&installed, &request);
        if (!already_installed && !Boot_ImageValid(BOOT_STAGE_ADDRESS, &request))
        {
            /* Reject incomplete/corrupt downloads without erasing the old APP. */
            return InstalledValid(&installed) ? BOOT_READY : BOOT_NO_VALID_APP;
        }

        if (!BootPort_Unlock())
        {
            return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
        }
        const bool installed_ok = already_installed || Install(&request);
        bool consumed_ok = false;
        if (installed_ok)
        {
            consumed_ok = BootPort_ProgramWord(BOOT_REQUEST_ADDRESS +
                                               offsetof(BootImageManifest, consumed),
                                               BOOT_REQUEST_CONSUMED);
        }
        BootPort_Lock();

        /* Keep the downloaded image and request intact on failure, so reset retries.
         * A failed consumed write is harmless: a valid installed image may boot. */
        if (!installed_ok || !consumed_ok)
        {
            return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
        }
    }

    return InstalledValid(&installed) ? BOOT_READY : BOOT_NO_VALID_APP;
}
