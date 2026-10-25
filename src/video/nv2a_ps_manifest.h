/* Optional, bounded specialised-PS warm-up metadata. Bytecode keeps its own
 * independently validated cache; any metadata failure leaves lazy compilation. */
#ifndef NV2A_PS_MANIFEST_H
#define NV2A_PS_MANIFEST_H

#include "nv2a_shader_cache.h"

#define NV_PS_MANIFEST_SCHEMA 1u
#define NV_PS_MANIFEST_WORDS 54u
#define NV_PS_MANIFEST_LIMIT 512u
#define NV_PS_MANIFEST_HEADER 88u

static uint32_t nv_ps_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void nv_ps_put32(uint8_t *p, uint32_t v)
{
    unsigned i;
    for (i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static int nv_ps_manifest_path(const uint8_t family[32], wchar_t path[NV_SHADER_CACHE_PATH])
{
    if (!nv_shader_cache_path(family, path)) return 0;
    wcscpy_s(path + wcslen(path) - 4, 5, L".psm");
    return 1;
}

static int nv_ps_manifest_read(const wchar_t *path, const uint8_t family[32],
                              uint32_t records[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS],
                              unsigned *count)
{
    uint8_t header[NV_PS_MANIFEST_HEADER], checksum[32];
    uint8_t payload[NV_PS_MANIFEST_LIMIT * NV_PS_MANIFEST_WORDS * 4];
    NvShaderHash hash;
    LARGE_INTEGER size;
    DWORD read, bytes;
    unsigned n, i, j;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int ok = 0;
    *count = 0;
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &size) ||
        !ReadFile(file, header, sizeof header, &read, NULL) || read != sizeof header ||
        memcmp(header, "NVPSMAN1", 8) || nv_ps_get32(header + 8) != NV_PS_MANIFEST_SCHEMA ||
        nv_ps_get32(header + 12) != NV_PS_MANIFEST_WORDS * 4 ||
        nv_ps_get32(header + 20) != 0 || memcmp(header + 24, family, 32)) goto done;
    n = nv_ps_get32(header + 16);
    if (n > NV_PS_MANIFEST_LIMIT) goto done;
    bytes = n * NV_PS_MANIFEST_WORDS * 4;
    if (size.QuadPart != (LONGLONG)sizeof header + bytes ||
        !ReadFile(file, payload, bytes, &read, NULL) || read != bytes) goto done;
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, payload, bytes);
    nv_shader_hash_finish(&hash, checksum);
    if (memcmp(header + 56, checksum, 32)) goto done;
    /* Decode only after validating the complete file; never partially import. */
    for (i = 0; i < n; i++) for (j = 0; j < NV_PS_MANIFEST_WORDS; j++)
        records[i][j] = nv_ps_get32(payload + (i * NV_PS_MANIFEST_WORDS + j) * 4);
    *count = n;
    ok = 1;
done:
    CloseHandle(file);
    return ok;
}

static int nv_ps_manifest_write(const wchar_t *path, const uint8_t family[32],
                               const uint32_t records[][NV_PS_MANIFEST_WORDS], unsigned count)
{
    static volatile LONG serial;
    uint8_t header[NV_PS_MANIFEST_HEADER] = {0};
    uint8_t payload[NV_PS_MANIFEST_LIMIT * NV_PS_MANIFEST_WORDS * 4];
    wchar_t temporary[NV_SHADER_CACHE_PATH], dir[NV_SHADER_CACHE_PATH];
    NvShaderHash hash;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD wrote, bytes;
    unsigned i, j;
    int ok;
    if (count > NV_PS_MANIFEST_LIMIT) return 0;
    bytes = count * NV_PS_MANIFEST_WORDS * 4;
    for (i = 0; i < count; i++) for (j = 0; j < NV_PS_MANIFEST_WORDS; j++)
        nv_ps_put32(payload + (i * NV_PS_MANIFEST_WORDS + j) * 4, records[i][j]);
    memcpy(header, "NVPSMAN1", 8);
    nv_ps_put32(header + 8, NV_PS_MANIFEST_SCHEMA);
    nv_ps_put32(header + 12, NV_PS_MANIFEST_WORDS * 4);
    nv_ps_put32(header + 16, count);
    memcpy(header + 24, family, 32);
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, payload, bytes);
    nv_shader_hash_finish(&hash, header + 56);
    wcscpy_s(dir, NV_SHADER_CACHE_PATH, path);
    *wcsrchr(dir, L'\\') = 0;
    for (i = 3; dir[i]; i++) if (dir[i] == L'\\' || dir[i] == L'/') {
        wchar_t slash = dir[i]; dir[i] = 0;
        CreateDirectoryW(dir, NULL); dir[i] = slash;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    for (i = 0; i < 4 && file == INVALID_HANDLE_VALUE; i++) {
        if (swprintf_s(temporary, NV_SHADER_CACHE_PATH, L"%s.%lu.%lu.%lu.tmp", path,
                       GetCurrentProcessId(), GetCurrentThreadId(),
                       (unsigned long)InterlockedIncrement(&serial)) < 0) return 0;
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return 0;
    }
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, header, sizeof header, &wrote, NULL) && wrote == sizeof header;
    if (ok) ok = WriteFile(file, payload, bytes, &wrote, NULL) && wrote == bytes;
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(temporary);
    return ok;
}

#endif
