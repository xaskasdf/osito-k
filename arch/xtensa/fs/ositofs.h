/*
 * OsitoFS - Flat filesystem on SPI flash
 *
 * Inspired by BBC Micro's DFS: flat namespace, contiguous allocation.
 * Up to 128 files on ~3.75MB of flash.
 *
 * Flash layout (starting at FS_FLASH_BASE = 0x40000):
 *   Sector 0: Superblock (magic, version, stats)
 *   Sector 1: File table (128 entries x 32 bytes = 4096 bytes)
 *   Sector 2+: Data area (958 sectors = ~3.83MB)
 *
 * Usage:
 *   fs_init();                            // mount
 *   fs_format();                          // create fresh FS
 *   fs_create("hello.txt", data, len);    // write file
 *   fs_read("hello.txt", buf, sizeof buf); // read file
 *   fs_delete("hello.txt");               // delete file
 */
#ifndef OSITO_FS_H
#define OSITO_FS_H

#include "osito.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Derived constants */
#define FS_SUPER_ADDR   FS_FLASH_BASE
#define FS_TABLE_ADDR   (FS_FLASH_BASE + FS_SECTOR_SIZE)
#define FS_DATA_ADDR    (FS_FLASH_BASE + 2 * FS_SECTOR_SIZE)
#define FS_DATA_SECTORS ((FS_FLASH_END - FS_DATA_ADDR) / FS_SECTOR_SIZE)

#define FS_MAGIC        0x4F534654  /* "OSFT" */
#define FS_VERSION      1

/* File table entry (32 bytes) */
typedef struct {
    char     name[FS_NAME_LEN];  /* 24: null-terminated filename */
    uint32_t size;               /*  4: file size in bytes */
    uint16_t start_sector;       /*  2: first data sector */
    uint16_t sector_count;       /*  2: sectors used */
} __attribute__((packed)) fs_entry_t;

/* Superblock (4096 bytes, one sector) */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_sectors;     /* data sectors available */
    uint32_t file_count;        /* active files */
} fs_super_t;

/* Initialize / mount the filesystem. Returns 0 if valid FS found. */
int fs_init(void);

/* Format: erase and create a fresh filesystem. */
int fs_format(void);

/* Create a file. Returns 0 on success, -1 on error. */
int fs_create(const char *name, const void *data, uint32_t size);

/* Read a file into buf. Returns bytes read, or -1 if not found. */
int fs_read(const char *name, void *buf, uint32_t max_size);

/* Delete a file. Returns 0 on success, -1 if not found. */
int fs_delete(const char *name);

/* Get file size. Returns size in bytes, or -1 if not found. */
int fs_stat(const char *name);

/* List all files (prints to UART). */
void fs_list(void);

/* Free space in bytes. */
uint32_t fs_free(void);

/* Overwrite an existing file (or create if not found). Returns 0 on success. */
int fs_overwrite(const char *name, const void *data, uint32_t size);

/* Append data to an existing file. Returns 0 on success, -1 if not found or won't fit. */
int fs_append(const char *name, const void *data, uint32_t size);

/* Rename a file. Returns 0 on success, -1 if not found or new name exists. */
int fs_rename(const char *old_name, const char *new_name);

/* Upload a file via UART (binary protocol with sector-level ACK).
 * Handles sector allocation, UART reading, flash writes internally.
 * Returns CRC16 of received data on success, -1 on error. */
int fs_upload(const char *name, uint32_t total_size);

/* Is the filesystem mounted? */
int fs_mounted(void);

/* CRC16-CCITT (for upload verification) */
uint16_t fs_crc16(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* OSITO_FS_H */
