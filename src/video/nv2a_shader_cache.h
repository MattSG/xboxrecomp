/* Persistent D3DCompile bytecode. Cache failures always fall back to compiling;
 * specialised shaders call this on their existing compiler worker threads. */
#ifndef NV2A_SHADER_CACHE_H
#define NV2A_SHADER_CACHE_H

#include <windows.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define NV_SHADER_CACHE_SCHEMA 1u
#define NV_SHADER_CACHE_MAX_CODE (16u * 1024u * 1024u)
#define NV_SHADER_CACHE_PATH 1040

/* SHA-256 keeps the filename and the bytecode checksum independent of runtime
 * addresses, compiler struct padding, and ambiguous macro concatenations. */
typedef struct {
    uint32_t h[8];
    uint64_t bytes;
    unsigned used;
    uint8_t block[64];
} NvShaderHash;

static uint32_t nv_shader_ror(uint32_t x, unsigned n)
{
    return (x >> n) | (x << (32 - n));
}

static void nv_shader_hash_block(NvShaderHash *s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64], a,b,c,d,e,f,g,h;
    unsigned i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (; i < 64; i++) {
        uint32_t x = w[i-15], y = w[i-2];
        w[i] = w[i-16] + (nv_shader_ror(x,7) ^ nv_shader_ror(x,18) ^ (x >> 3)) +
               w[i-7] + (nv_shader_ror(y,17) ^ nv_shader_ror(y,19) ^ (y >> 10));
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (nv_shader_ror(e,6) ^ nv_shader_ror(e,11) ^ nv_shader_ror(e,25)) +
                      ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (nv_shader_ror(a,2) ^ nv_shader_ror(a,13) ^ nv_shader_ror(a,22)) +
                      ((a & b) ^ (a & c) ^ (b & c));
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void nv_shader_hash_init(NvShaderHash *s)
{
    static const uint32_t initial[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    memset(s, 0, sizeof *s);
    memcpy(s->h, initial, sizeof initial);
}

static void nv_shader_hash_add(NvShaderHash *s, const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    s->bytes += size;
    while (size) {
        size_t n = 64 - s->used;
        if (n > size) n = size;
        memcpy(s->block + s->used, p, n);
        s->used += (unsigned)n; p += n; size -= n;
        if (s->used == 64) {
            nv_shader_hash_block(s, s->block);
            s->used = 0;
        }
    }
}

static void nv_shader_hash_finish(NvShaderHash *s, uint8_t out[32])
{
    uint64_t bits = s->bytes * 8;
    uint8_t tail[72] = {0x80};
    unsigned i, n = s->used < 56 ? 56 - s->used : 120 - s->used;
    for (i = 0; i < 8; i++) tail[n+i] = (uint8_t)(bits >> (56-i*8));
    nv_shader_hash_add(s, tail, n+8);
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(s->h[i/4] >> (24-(i%4)*8));
}

static void nv_shader_hash_number(NvShaderHash *s, uint64_t value)
{
    uint8_t bytes[8];
    unsigned i;
    for (i = 0; i < 8; i++) bytes[i] = (uint8_t)(value >> (i*8));
    nv_shader_hash_add(s, bytes, sizeof bytes);
}

static void nv_shader_hash_string(NvShaderHash *s, const char *text)
{
    size_t n = text ? strlen(text) : 0;
    nv_shader_hash_number(s, text ? n : UINT64_MAX);
    if (n) nv_shader_hash_add(s, text, n);
}

static void nv_shader_cache_key(const void *source, size_t size, const char *name,
                               const D3D_SHADER_MACRO *macros, const char *entry,
                               const char *target, UINT flags1, UINT flags2,
                               uint8_t key[32])
{
    NvShaderHash hash;
    size_t i, count = 0;
    if (macros) while (macros[count].Name) count++;
    nv_shader_hash_init(&hash);
    nv_shader_hash_number(&hash, NV_SHADER_CACHE_SCHEMA);
    nv_shader_hash_number(&hash, size);
    nv_shader_hash_add(&hash, source, size);
    nv_shader_hash_string(&hash, name);
    nv_shader_hash_string(&hash, entry);
    nv_shader_hash_string(&hash, target);
    nv_shader_hash_number(&hash, flags1);
    nv_shader_hash_number(&hash, flags2);
    nv_shader_hash_number(&hash, count);
    for (i = 0; i < count; i++) {
        nv_shader_hash_string(&hash, macros[i].Name);
        nv_shader_hash_string(&hash, macros[i].Definition);
    }
    nv_shader_hash_finish(&hash, key);
}

static int nv_shader_cache_path(const uint8_t key[32], wchar_t path[NV_SHADER_CACHE_PATH])
{
    wchar_t dir[NV_SHADER_CACHE_PATH], full[NV_SHADER_CACHE_PATH], enabled[8];
    DWORD n = GetEnvironmentVariableW(L"RECOMP_SHADER_CACHE", enabled, 8);
    size_t length;
    unsigned i;
    if (n == 1 && enabled[0] == L'0') return 0;
    n = GetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", dir, NV_SHADER_CACHE_PATH);
    if (n >= NV_SHADER_CACHE_PATH) return 0;
    if (!n) {
        n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, NV_SHADER_CACHE_PATH);
        if (!n || n >= NV_SHADER_CACHE_PATH - 40) return 0;
        wcscat_s(dir, NV_SHADER_CACHE_PATH, L"\\xboxrecomp\\ShaderCache");
    }
    n = GetFullPathNameW(dir, NV_SHADER_CACHE_PATH, full, NULL);
    if (!n || n >= NV_SHADER_CACHE_PATH - 80) return 0;
    length = wcslen(full);
    while (length && (full[length-1] == L'\\' || full[length-1] == L'/')) length--;
    memcpy(path, full, length * sizeof *path);
    path[length++] = L'\\';
    for (i = 0; i < 32; i++) {
        static const wchar_t hex[] = L"0123456789abcdef";
        path[length++] = hex[key[i] >> 4];
        path[length++] = hex[key[i] & 15];
    }
    wcscpy_s(path + length, NV_SHADER_CACHE_PATH - length, L".bin");
    return 1;
}

typedef struct {
    uint8_t magic[8];
    uint32_t schema, size;
    uint8_t key[32], checksum[32];
} NvShaderCacheHeader;

static ID3DBlob *nv_shader_cache_read(const wchar_t *path, const uint8_t key[32])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    NvShaderCacheHeader header;
    LARGE_INTEGER length;
    ID3DBlob *code = NULL;
    NvShaderHash hash;
    uint8_t checksum[32];
    DWORD read;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &length) ||
        !ReadFile(file, &header, sizeof header, &read, NULL) || read != sizeof header ||
        memcmp(header.magic, "NVSCACHE", 8) || header.schema != NV_SHADER_CACHE_SCHEMA ||
        !header.size || header.size > NV_SHADER_CACHE_MAX_CODE ||
        length.QuadPart != (LONGLONG)sizeof header + header.size ||
        memcmp(header.key, key, 32) || FAILED(D3DCreateBlob(header.size, &code))) goto done;
    if (!ReadFile(file, ID3D10Blob_GetBufferPointer(code), header.size, &read, NULL) ||
        read != header.size) goto invalid;
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, ID3D10Blob_GetBufferPointer(code), header.size);
    nv_shader_hash_finish(&hash, checksum);
    if (memcmp(checksum, header.checksum, 32)) goto invalid;
    goto done;
invalid:
    ID3D10Blob_Release(code);
    code = NULL;
done:
    CloseHandle(file);
    return code;
}

static void nv_shader_cache_write(const wchar_t *path, const uint8_t key[32], ID3DBlob *code)
{
    static volatile LONG serial;
    wchar_t temporary[NV_SHADER_CACHE_PATH], dir[NV_SHADER_CACHE_PATH];
    NvShaderCacheHeader header;
    NvShaderHash hash;
    HANDLE file = INVALID_HANDLE_VALUE;
    size_t size = ID3D10Blob_GetBufferSize(code), i;
    DWORD wrote;
    int ok;
    if (!size || size > NV_SHADER_CACHE_MAX_CODE) return;
    wcscpy_s(dir, NV_SHADER_CACHE_PATH, path);
    *wcsrchr(dir, L'\\') = 0;
    /* Parent creation is idempotent when several compiler workers arrive. */
    for (i = 3; dir[i]; i++) if (dir[i] == L'\\' || dir[i] == L'/') {
        wchar_t slash = dir[i]; dir[i] = 0;
        CreateDirectoryW(dir, NULL); dir[i] = slash;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return;
    for (i = 0; i < 4 && file == INVALID_HANDLE_VALUE; i++) {
        if (swprintf_s(temporary, NV_SHADER_CACHE_PATH, L"%s.%lu.%lu.%lu.tmp", path,
                       GetCurrentProcessId(), GetCurrentThreadId(),
                       (unsigned long)InterlockedIncrement(&serial)) < 0) return;
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return;
    }
    if (file == INVALID_HANDLE_VALUE) return;
    memcpy(header.magic, "NVSCACHE", 8);
    header.schema = NV_SHADER_CACHE_SCHEMA; header.size = (uint32_t)size;
    memcpy(header.key, key, 32);
    nv_shader_hash_init(&hash);
    nv_shader_hash_add(&hash, ID3D10Blob_GetBufferPointer(code), size);
    nv_shader_hash_finish(&hash, header.checksum);
    ok = WriteFile(file, &header, sizeof header, &wrote, NULL) && wrote == sizeof header;
    if (ok) ok = WriteFile(file, ID3D10Blob_GetBufferPointer(code), (DWORD)size, &wrote, NULL) && wrote == size;
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(temporary);
}

static HRESULT nv_shader_cache_compile(const void *source, size_t size, const char *name,
                                      const D3D_SHADER_MACRO *macros, const char *entry,
                                      const char *target, UINT flags1, UINT flags2,
                                      ID3DBlob **code, ID3DBlob **errors, int *cache_hit)
{
    uint8_t key[32];
    wchar_t path[NV_SHADER_CACHE_PATH];
    int cache;
    HRESULT hr;
    *code = NULL;
    if (errors) *errors = NULL;
    if (cache_hit) *cache_hit = 0;
    nv_shader_cache_key(source, size, name, macros, entry, target, flags1, flags2, key);
    cache = nv_shader_cache_path(key, path);
    if (cache && (*code = nv_shader_cache_read(path, key)) != NULL) {
        if (cache_hit) *cache_hit = 1;
        return S_OK;
    }
    hr = D3DCompile(source, size, name, macros, NULL, entry, target, flags1, flags2, code, errors);
    if (SUCCEEDED(hr) && cache) nv_shader_cache_write(path, key, *code);
    return hr;
}

#endif
