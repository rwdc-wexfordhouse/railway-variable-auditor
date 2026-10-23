/*
 * Copyright (c) 2026, Ross WD Cameron <ross@wexfordhouse.co.za>
 * Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
 */
#include "payload.h"

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <zlib.h>

#define MAX_ENTRY_SIZE (1024ull * 1024 * 1024)

static void set_err(wchar_t *err, DWORD len, const wchar_t *fmt, ...)
{
    va_list ap;
    if (!err || !len)
        return;
    va_start(ap, fmt);
    _vsnwprintf(err, len - 1, fmt, ap);
    va_end(ap);
    err[len - 1] = 0;
}

static uint32_t rd32(const BYTE *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const BYTE *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static BOOL read_at(HANDLE h, uint64_t pos, void *buf, DWORD len)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)pos;
    return SetFilePointerEx(h, li, NULL, FILE_BEGIN) && ReadFile(h, buf, len, &got, NULL) && got == len;
}

static BOOL sha256_range(HANDLE h, uint64_t pos, uint64_t size, BYTE out[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE *buf = malloc(1 << 20);
    BOOL ok = FALSE;
    if (!buf)
        return FALSE;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0
        && BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0) == 0) {
        ok = TRUE;
        while (ok && size > 0) {
            DWORD n = size > (1 << 20) ? (1 << 20) : (DWORD)size;
            ok = read_at(h, pos, buf, n) && BCryptHashData(hash, buf, n, 0) == 0;
            pos += n;
            size -= n;
        }
        ok = ok && BCryptFinishHash(hash, out, 32, 0) == 0;
    }
    if (hash)
        BCryptDestroyHash(hash);
    if (alg)
        BCryptCloseAlgorithmProvider(alg, 0);
    free(buf);
    return ok;
}

BOOL payload_open(Payload *p, BOOL verify, wchar_t *err, DWORD err_len)
{
    wchar_t exe[MAX_PATH * 2];
    BYTE t[PAYLOAD_TRAILER_SIZE], digest[32];
    LARGE_INTEGER end;

    memset(p, 0, sizeof *p);
    GetModuleFileNameW(NULL, exe, MAX_PATH * 2);
    p->file = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (p->file == INVALID_HANDLE_VALUE) {
        set_err(err, err_len, L"Cannot read the installer file (error %lu).", GetLastError());
        return FALSE;
    }
    if (!GetFileSizeEx(p->file, &end) || end.QuadPart < PAYLOAD_TRAILER_SIZE
        || !read_at(p->file, (uint64_t)end.QuadPart - PAYLOAD_TRAILER_SIZE, t, PAYLOAD_TRAILER_SIZE)
        || memcmp(t, PAYLOAD_MAGIC, 16) != 0) {
        set_err(err, err_len, L"This installer has no payload. It may be damaged or incomplete.");
        payload_close(p);
        return FALSE;
    }
    p->offset = rd64(t + 16);
    p->size = rd64(t + 24);
    p->count = rd32(t + 32);
    p->total_size = rd64(t + 40);
    memcpy(p->sha256, t + 48, 32);
    if (p->offset + p->size + PAYLOAD_TRAILER_SIZE != (uint64_t)end.QuadPart) {
        set_err(err, err_len, L"The installer file is the wrong size. Download it again.");
        payload_close(p);
        return FALSE;
    }
    if (verify && (!sha256_range(p->file, p->offset, p->size, digest) || memcmp(digest, p->sha256, 32) != 0)) {
        set_err(err, err_len, L"The installer failed its integrity check. Download it again.");
        payload_close(p);
        return FALSE;
    }
    return TRUE;
}

void payload_close(Payload *p)
{
    if (p->file && p->file != INVALID_HANDLE_VALUE)
        CloseHandle(p->file);
    p->file = NULL;
}

BOOL payload_walk(Payload *p, BOOL want_data, payload_visit visit, void *ctx, wchar_t *err, DWORD err_len)
{
    uint64_t pos = p->offset, end = p->offset + p->size;
    for (uint32_t i = 0; i < p->count; i++) {
        BYTE h[4], f[20];
        uint32_t plen, crc;
        uint64_t usize, psize;
        char *path;
        BYTE *packed = NULL, *data = NULL;
        BOOL ok;

        if (pos + 4 > end || !read_at(p->file, pos, h, 4))
            goto bad;
        plen = rd32(h);
        if (plen == 0 || plen > 4096 || pos + 4 + plen + 20 > end)
            goto bad;
        path = malloc(plen + 1);
        if (!path)
            goto bad;
        if (!read_at(p->file, pos + 4, path, plen) || !read_at(p->file, pos + 4 + plen, f, 20)) {
            free(path);
            goto bad;
        }
        path[plen] = 0;
        usize = rd64(f);
        psize = rd64(f + 8);
        crc = rd32(f + 16);
        pos += 4 + plen + 20;
        if (usize > MAX_ENTRY_SIZE || psize > MAX_ENTRY_SIZE || pos + psize > end || strstr(path, "..")) {
            free(path);
            goto bad;
        }
        if (want_data) {
            uLongf out = (uLongf)usize;
            packed = malloc(psize ? (size_t)psize : 1);
            data = malloc(usize ? (size_t)usize : 1);
            if (!packed || !data || !read_at(p->file, pos, packed, (DWORD)psize)
                || uncompress(data, &out, packed, (uLong)psize) != Z_OK || out != usize
                || (uint32_t)crc32(0, data, (uInt)usize) != crc) {
                free(packed);
                free(data);
                free(path);
                set_err(err, err_len, L"A file in the installer is damaged.");
                return FALSE;
            }
            free(packed);
        }
        ok = visit(path, data, usize, ctx);
        free(data);
        free(path);
        pos += psize;
        if (!ok)
            return FALSE;
    }
    return TRUE;
bad:
    set_err(err, err_len, L"The installer payload is damaged.");
    return FALSE;
}

BOOL payload_write_stub(const Payload *p, const wchar_t *dest, wchar_t *err, DWORD err_len)
{
    HANDLE out = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BYTE *buf = malloc(1 << 20);
    uint64_t pos = 0;
    BOOL ok = out != INVALID_HANDLE_VALUE && buf;
    while (ok && pos < p->offset) {
        DWORD n = (p->offset - pos) > (1 << 20) ? (1 << 20) : (DWORD)(p->offset - pos), w = 0;
        ok = read_at(p->file, pos, buf, n) && WriteFile(out, buf, n, &w, NULL) && w == n;
        pos += n;
    }
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    free(buf);
    if (!ok)
        set_err(err, err_len, L"Could not write '%ls' (error %lu).", dest, GetLastError());
    return ok;
}
