#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdbool.h>
#include "boot_protocol.h"

typedef enum
{
    BOOT_READY = 0,
    BOOT_NO_VALID_APP,
    BOOT_UPDATE_FAILED
} BootStatus;

/* CRC-32/ISO-HDLC, compatible with zlib.crc32. Hardware words + software tail.
 * Uses the CRC peripheral exclusively; call from main, never concurrently. */
uint32_t Boot_Crc32(const void *data, uint32_t size);
bool Boot_ManifestValid(const BootImageManifest *manifest);
bool Boot_ImageValid(uint32_t storage_address, const BootImageManifest *manifest);
/* Installs a committed request if present, then verifies the installed APP.
 * Does not download data. Call from main with HAL time base running. */
BootStatus Boot_PrepareApplication(void);

#endif
