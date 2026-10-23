/*
 * Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
 * Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
 *
 * The installer carries its files as a payload appended to the executable.
 * Layout, all little-endian:
 *   [stub exe][entries...][trailer, 80 bytes]
 *   entry   : u32 path_len, path (UTF-8, forward slashes), u64 size, u64 packed,
 *             u32 crc32, packed bytes (zlib)
 *   trailer : "RVAUDITOR-PKG-01", u64 payload_offset, u64 payload_size,
 *             u32 file_count, u32 reserved, u64 total_size, 32 bytes SHA-256 of the entries
 * The stub is the first payload_offset bytes; the uninstaller is a copy of it.
 */
#ifndef RVA_PAYLOAD_H
#define RVA_PAYLOAD_H

#include <windows.h>
#include <stdint.h>

#define PAYLOAD_MAGIC "RVAUDITOR-PKG-01"
#define PAYLOAD_TRAILER_SIZE 80

typedef struct {
    HANDLE file;
    uint64_t offset;      /* where the entries start, which is also the stub size */
    uint64_t size;        /* bytes of entries */
    uint32_t count;
    uint64_t total_size;  /* uncompressed bytes of all files */
    BYTE sha256[32];
} Payload;

/* Opens the running executable's payload. When verify is TRUE the SHA-256 is checked. */
BOOL payload_open(Payload *p, BOOL verify, wchar_t *err, DWORD err_len);
void payload_close(Payload *p);

/* Calls visit for each entry in order. data is NULL when want_data is FALSE.
 * path is UTF-8 with forward slashes. Return FALSE from visit to stop. */
typedef BOOL (*payload_visit)(const char *path, const BYTE *data, uint64_t size, void *ctx);
BOOL payload_walk(Payload *p, BOOL want_data, payload_visit visit, void *ctx, wchar_t *err, DWORD err_len);

/* Copies the stub (the executable without its payload) to dest. */
BOOL payload_write_stub(const Payload *p, const wchar_t *dest, wchar_t *err, DWORD err_len);

#endif
