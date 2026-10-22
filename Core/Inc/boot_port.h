#ifndef BOOT_PORT_H
#define BOOT_PORT_H

#include <stdbool.h>
#include <stdint.h>

/* Map internal Flash only. External offsets must use StorageRead instead.
 * Internal writes are restricted to APP/installed metadata; the external
 * driver can only clear the request consumed word and cannot erase anything. */
const void *BootPort_Map(uint32_t address);
bool BootPort_StorageInit(void);
void BootPort_StorageDeInit(void);
bool BootPort_StorageRead(uint32_t offset, void *data, uint32_t size);
bool BootPort_StorageConsume(void);
/* CRC-32 peripheral session, called only from Boot_Crc32 in main context.
 * FeedWord accepts four bytes packed little endian. The port reflects input
 * bits and returns a reflected running CRC, without the final XOR.
 * Finish releases the CRC peripheral clock. Calls must not be nested. */
void BootPort_CrcBegin(void);
void BootPort_CrcFeedWord(uint32_t word);
uint32_t BootPort_CrcFinish(void);
bool BootPort_Unlock(void);
void BootPort_Lock(void);
bool BootPort_EraseInstalled(void);
bool BootPort_EraseApplication(void);
bool BootPort_ProgramWord(uint32_t address, uint32_t value);
/* Returns false on deinitialization failure; a successful jump never returns. */
bool BootPort_JumpApplication(void);

#endif
