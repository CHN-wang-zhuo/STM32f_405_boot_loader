#ifndef BOOT_PROTOCOL_H
#define BOOT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* STM32F405RG, 1 MiB Flash. Share this file with the APP project. */
#define BOOT_FLASH_BASE          UINT32_C(0x08000000)
#define BOOT_FLASH_SIZE          UINT32_C(0x00100000)
#define BOOT_LOADER_SIZE         UINT32_C(0x00004000) /* sector 0; sector 1 reserved */
#define BOOT_INSTALLED_ADDRESS   UINT32_C(0x08008000) /* sector 2 */
#define BOOT_REQUEST_ADDRESS     UINT32_C(0x0800C000) /* sector 3 */
#define BOOT_APP_ADDRESS         UINT32_C(0x08020000) /* sectors 5-7 */
#define BOOT_STAGE_ADDRESS       UINT32_C(0x08080000) /* sectors 8-10 */
#define BOOT_IMAGE_CAPACITY      UINT32_C(0x00060000) /* 384 KiB */

#define BOOT_IMAGE_MAGIC         UINT32_C(0x424C4657)
#define BOOT_IMAGE_FORMAT        UINT32_C(1)
#define BOOT_IMAGE_COMMITTED     UINT32_C(0xB007C0DE)
#define BOOT_REQUEST_CONSUMED    UINT32_C(0)
#define BOOT_ERASED_WORD         UINT32_C(0xFFFFFFFF)

/* Little endian, no packing required. CRC-32/ISO-HDLC (Python zlib.crc32).
 * Write fields 0..23 first and commit at offset 24 LAST. For a new request,
 * consumed must remain erased. Bootloader writes it to zero after installation.
 * Header CRC covers bytes 0..19. Image CRC covers exactly image_size bytes.
 */
typedef struct
{
    uint32_t magic;
    uint32_t format;
    uint32_t image_size;
    uint32_t image_crc;
    uint32_t version;
    uint32_t header_crc;
    uint32_t commit;
    uint32_t consumed;
} BootImageManifest;

_Static_assert(sizeof(BootImageManifest) == 32U, "manifest wire size");
_Static_assert(offsetof(BootImageManifest, commit) == 24U, "commit wire offset");
_Static_assert(offsetof(BootImageManifest, consumed) == 28U, "consumed wire offset");

#endif
