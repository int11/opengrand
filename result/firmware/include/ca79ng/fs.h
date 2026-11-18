/* OpenGrand — removable file system (USB memory) interface.
 *
 * The original firmware carries its own FAT12/16/32 stack and a USB-host
 * mass-storage driver (FileIO_Task,
 * RemovableTask, "No media error."). OpenGrand keeps the file system behind
 * this interface: the target port wraps FatFs (or similar), the simulator a
 * host directory (ports/sim --usb <dir>). Names are flat, relative to the
 * media root, '/' separated, at most FS_NAME_MAX-1 bytes.
 *
 * Return values are result codes from data/app/tables/result_codes.json
 * (KP_RES_* in kproto.h): 0 = success, 0x21 no media, 0x31 access error,
 * 0x32 disk full, 0x55 already exists, 0x56 not found. */
#ifndef CA79NG_FS_H
#define CA79NG_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FS_NAME_MAX 128u

typedef struct {
    void *ctx;
    bool (*mounted)(void *ctx);
    /* entry `index` (0-based, stable order: sorted by name) -> name and size;
     * returns false past the last entry */
    bool (*list)(void *ctx, uint32_t index, char *name, size_t cap, uint32_t *size);
    /* whole-file read: bytes read (> cap: truncated -> KP_RES_SIZE_OVER by
     * the caller), or a negative result code */
    int32_t (*read)(void *ctx, const char *name, uint8_t *buf, uint32_t cap);
    uint16_t (*write)(void *ctx, const char *name, const uint8_t *data, uint32_t len, bool overwrite);
    uint16_t (*remove)(void *ctx, const char *name);
    uint16_t (*rename)(void *ctx, const char *from, const char *to);
    uint16_t (*format)(void *ctx);
} hal_fs_t;

#endif
