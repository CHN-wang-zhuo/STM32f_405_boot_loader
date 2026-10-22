#include "bootloader.h"
#include "boot_port.h"
#include "boot_update_storage.h"
#include <string.h>

#define COPY_BUFFER_SIZE 256U

static void CrcFeedWords(const uint8_t *bytes, uint32_t size)
{
    for (uint32_t i = 0U; i < size / 4U; ++i)
    {
        uint32_t word;
        memcpy(&word, bytes + i * 4U, sizeof(word));
        BootPort_CrcFeedWord(word);
    }
}

static uint32_t CrcFinishTail(const uint8_t *tail, uint32_t size)
{
    uint32_t crc = BootPort_CrcFinish();
    for (uint32_t i = 0U; i < size; ++i)
    {
        crc ^= tail[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit)
        {
            crc = (crc >> 1U) ^ ((0U - (crc & 1U)) & UINT32_C(0xEDB88320));
        }
    }
    return ~crc;
}

uint32_t Boot_Crc32(const void *data, uint32_t size)
{
    const uint8_t *bytes = data;
    BootPort_CrcBegin();
    CrcFeedWords(bytes, size);
    return CrcFinishTail(size != 0U ? bytes + (size & ~3U) : bytes, size & 3U);
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

static bool VectorsValid(const uint32_t vectors[2], uint32_t size)
{
    const uint32_t sp = vectors[0];
    const uint32_t reset = vectors[1] & ~UINT32_C(1);
    const bool valid_stack = ((sp > UINT32_C(0x20000000) && sp <= UINT32_C(0x20020000)) ||
                             (sp > UINT32_C(0x10000000) && sp <= UINT32_C(0x10010000))) &&
                            (sp & 7U) == 0U;
    return valid_stack && (vectors[1] & 1U) != 0U &&
           reset >= BOOT_APP_ADDRESS + 8U && reset < BOOT_APP_ADDRESS + size;
}

bool Boot_ImageValid(uint32_t storage_address, const BootImageManifest *manifest)
{
    uint32_t vectors[2];
    if (storage_address != BOOT_APP_ADDRESS || !Boot_ManifestValid(manifest))
    {
        return false;
    }
    memcpy(vectors, BootPort_Map(storage_address), sizeof(vectors));
    return VectorsValid(vectors, manifest->image_size) &&
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

static bool RecordValid(const BootUpdateRecord *record)
{
    if (!Boot_ManifestValid(&record->image) || record->image.consumed != BOOT_ERASED_WORD ||
        record->storage_magic != BOOT_STORAGE_MAGIC || record->layout_version != BOOT_STORAGE_VERSION ||
        record->image_offset != BOOT_STORAGE_IMAGE || record->image_capacity != BOOT_STORAGE_CAPACITY ||
        record->apply_mode > 1U || record->reserved != BOOT_ERASED_WORD ||
        record->reserved_tail != BOOT_ERASED_WORD)
    {
        return false;
    }
    uint8_t metadata[52];
    memcpy(metadata, record, 24U);
    memcpy(metadata + 24U, (const uint8_t *)record + 28U, 28U);
    return Boot_Crc32(metadata, sizeof(metadata)) == record->metadata_crc;
}

static bool ExternalImageValid(const BootImageManifest *manifest)
{
    uint8_t buffer[COPY_BUFFER_SIZE];
    uint32_t vectors[2];
    if (!BootPort_StorageRead(BOOT_STORAGE_IMAGE, vectors, sizeof(vectors)) ||
        !VectorsValid(vectors, manifest->image_size))
    {
        return false;
    }
    BootPort_CrcBegin();
    for (uint32_t offset = 0U; offset < manifest->image_size; offset += COPY_BUFFER_SIZE)
    {
        const uint32_t remaining = manifest->image_size - offset;
        const uint32_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!BootPort_StorageRead(BOOT_STORAGE_IMAGE + offset, buffer, count))
        {
            (void)BootPort_CrcFinish();
            return false;
        }
        CrcFeedWords(buffer, count);
        if (remaining <= sizeof(buffer))
        {
            return CrcFinishTail(buffer + (count & ~3U), count & 3U) == manifest->image_crc;
        }
    }
    (void)BootPort_CrcFinish();
    return false;
}

static bool WriteInstalledManifest(const BootImageManifest *request)
{
    for (uint32_t offset = 0U; offset < offsetof(BootImageManifest, commit); offset += 4U)
    {
        uint32_t word;
        memcpy(&word, (const uint8_t *)request + offset, sizeof(word));
        if (!BootPort_ProgramWord(BOOT_INSTALLED_ADDRESS + offset, word)) { return false; }
    }
    return BootPort_ProgramWord(BOOT_INSTALLED_ADDRESS + offsetof(BootImageManifest, commit),
                                BOOT_IMAGE_COMMITTED);
}

static bool Install(const BootImageManifest *request)
{
    uint8_t buffer[COPY_BUFFER_SIZE];
    if (!BootPort_EraseInstalled() || !BootPort_EraseApplication()) { return false; }
    for (uint32_t offset = 0U; offset < request->image_size; offset += COPY_BUFFER_SIZE)
    {
        const uint32_t remaining = request->image_size - offset;
        const uint32_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!BootPort_StorageRead(BOOT_STORAGE_IMAGE + offset, buffer, count)) { return false; }
        for (uint32_t i = 0U; i < count; i += 4U)
        {
            uint32_t word = BOOT_ERASED_WORD;
            const uint32_t bytes = count - i < 4U ? count - i : 4U;
            memcpy(&word, buffer + i, bytes);
            if (!BootPort_ProgramWord(BOOT_APP_ADDRESS + offset + i, word)) { return false; }
        }
    }
    return Boot_ImageValid(BOOT_APP_ADDRESS, request) && WriteInstalledManifest(request);
}

static BootStatus PrepareExternalRequest(void)
{
    BootUpdateRecord request;
    BootImageManifest installed;
    if (!BootPort_StorageRead(BOOT_STORAGE_REQUEST, &request, sizeof(request)))
    {
        return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
    }
    if (!RecordValid(&request))
    {
        return InstalledValid(&installed) ? BOOT_READY : BOOT_NO_VALID_APP;
    }
    const bool already_installed = InstalledValid(&installed) && SameImage(&installed, &request.image);
    if (!already_installed && !ExternalImageValid(&request.image))
    {
        return InstalledValid(&installed) ? BOOT_READY : BOOT_NO_VALID_APP;
    }
    if (!already_installed)
    {
        if (!BootPort_Unlock())
        {
            return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
        }
        const bool installed_ok = Install(&request.image);
        BootPort_Lock();
        if (!installed_ok)
        {
            return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
        }
    }
    /* Only acknowledge after internal image + installed commit are durable.
     * Clearing consumed invalidates metadata_crc by design (v1 includes it).
     * A torn acknowledgement is also inactive, with the installed APP intact.
     * Never rewrite that CRC or erase the external request / candidate. */
    (void)BootPort_StorageConsume();
    return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
}

BootStatus Boot_PrepareApplication(void)
{
    BootImageManifest installed;
    if (!BootPort_StorageInit())
    {
        BootPort_StorageDeInit();
        return InstalledValid(&installed) ? BOOT_READY : BOOT_UPDATE_FAILED;
    }
    const BootStatus status = PrepareExternalRequest();
    BootPort_StorageDeInit();
    return status;
}
