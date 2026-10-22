#ifndef BOOT_UPDATE_STORAGE_H
#define BOOT_UPDATE_STORAGE_H

#include "boot_protocol.h"

/* W25Q16 storage protocol v1. These are SPI byte offsets, not MCU addresses.
 * Contract: APP/docs/w25q16-update-storage.md; APP project is not modified. */
#define BOOT_STORAGE_SIZE       UINT32_C(0x00200000)
#define BOOT_STORAGE_REQUEST    UINT32_C(0x00000000)
#define BOOT_STORAGE_IMAGE      UINT32_C(0x00010000)
#define BOOT_STORAGE_CAPACITY   UINT32_C(0x00060000)
#define BOOT_STORAGE_MAGIC      UINT32_C(0x31505557)
#define BOOT_STORAGE_VERSION    UINT32_C(1)

typedef struct
{
    BootImageManifest image;
    uint32_t storage_magic;
    uint32_t layout_version;
    uint32_t image_offset;
    uint32_t image_capacity;
    uint32_t apply_mode;
    uint32_t reserved;
    uint32_t metadata_crc;
    uint32_t reserved_tail;
} BootUpdateRecord;

_Static_assert(sizeof(BootUpdateRecord) == 64U, "external record size");
_Static_assert(offsetof(BootUpdateRecord, metadata_crc) == 56U, "metadata CRC offset");
_Static_assert(BOOT_STORAGE_CAPACITY == BOOT_IMAGE_CAPACITY, "APP capacity contract");

#endif
