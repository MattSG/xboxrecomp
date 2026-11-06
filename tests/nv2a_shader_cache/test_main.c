/* Compile actual HLSL to verify hits, input invalidation, and corrupt-file recovery. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <stdio.h>
#include "nv2a_shader_cache.h"

static unsigned failures;
static const char source[] =
    "float4 main() : SV_Target { return float4(VALUE,0,0,1); }\n"
    "float4 other() : SV_Target { return float4(VALUE,0,0,1); }\n";
static const D3D_SHADER_MACRO one[] = {{"VALUE", "1"}, {NULL, NULL}};
static const D3D_SHADER_MACRO zero[] = {{"VALUE", "0"}, {NULL, NULL}};

static void check(int condition, const char *label)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) failures++;
}

static void compile_expect(const char *text, const char *name, const D3D_SHADER_MACRO *macros,
                           const char *entry, const char *target, UINT flags, int expect_hit,
                           const char *label)
{
    ID3DBlob *code = NULL, *errors = NULL;
    int hit = -1;
    HRESULT hr = nv_shader_cache_compile(text, strlen(text), name, macros, entry, target,
                                         flags, 0, &code, &errors, &hit);
    check(SUCCEEDED(hr) && code && hit == expect_hit, label);
    if (FAILED(hr) && errors) printf("%s\n", (char *)ID3D10Blob_GetBufferPointer(errors));
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
}

static void basic(int hit, const char *label)
{
    compile_expect(source, "test", one, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, hit, label);
}

static DWORD WINAPI compile_worker(void *unused)
{
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr;
    (void)unused;
    hr = nv_shader_cache_compile(source, sizeof source - 1, "parallel", one, "main", "ps_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors, NULL);
    if (code) ID3D10Blob_Release(code);
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(hr) ? 0 : 1;
}

int main(void)
{
    wchar_t temp[MAX_PATH], dir[NV_SHADER_CACHE_PATH], path[NV_SHADER_CACHE_PATH], glob[NV_SHADER_CACHE_PATH];
    uint8_t key[32];
    HANDLE file, workers[4];
    DWORD wrote, exit_code;
    unsigned i;
    WIN32_FIND_DATAW found;
    char changed[sizeof source + 2];
    if (!GetTempPathW(MAX_PATH, temp)) return 2;
    swprintf_s(dir, NV_SHADER_CACHE_PATH, L"%snv2a-shader-cache-%lu-%llu", temp,
               GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", dir);
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE", NULL);

    basic(0, "first compile misses");
    basic(1, "second compile reads bytecode");
    compile_expect(source, "test", zero, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, "macro value invalidates");
    compile_expect(source, "test", one, "other", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, "entry point invalidates");
    compile_expect(source, "test", one, "main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, "target invalidates");
    compile_expect(source, "test", one, "main", "ps_5_0", D3DCOMPILE_DEBUG, 0, "compile flags invalidate");
    compile_expect(source, "renamed", one, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, "source name invalidates");
    snprintf(changed, sizeof changed, "%s ", source);
    compile_expect(changed, "test", one, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, "source bytes invalidate");
    basic(1, "original entry survives other inputs");

    nv_shader_cache_key(source, sizeof source - 1, "test", one, "main", "ps_5_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, key);
    check(nv_shader_cache_path(key, path), "cache path available");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    check(file != INVALID_HANDLE_VALUE, "cache file created");
    if (file != INVALID_HANDLE_VALUE) {
        uint8_t bad = 0;
        SetFilePointer(file, -1, NULL, FILE_END);
        WriteFile(file, &bad, 1, &wrote, NULL);
        CloseHandle(file);
    }
    /* Changing the checksum unconditionally avoids depending on the final DXBC byte. */
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        uint8_t bad[32] = {0};
        SetFilePointer(file, 48, NULL, FILE_BEGIN);
        WriteFile(file, bad, sizeof bad, &wrote, NULL);
        CloseHandle(file);
    }
    basic(0, "checksum corruption recompiles");
    basic(1, "recompiled corruption is cached");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    basic(0, "truncated file recompiles");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        uint32_t huge = UINT32_MAX;
        SetFilePointer(file, 12, NULL, FILE_BEGIN);
        WriteFile(file, &huge, sizeof huge, &wrote, NULL);
        CloseHandle(file);
    }
    basic(0, "unbounded blob size rejected");
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE", L"0");
    basic(0, "cache switch bypasses existing bytecode");
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE", NULL);
    basic(1, "bypass preserves existing bytecode");
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", path); /* An existing file cannot be a directory. */
    basic(0, "unavailable cache directory falls back to compile");
    SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", dir);

    for (i = 0; i < 4; i++) {
        workers[i] = CreateThread(NULL, 0, compile_worker, NULL, 0, NULL);
        if (!workers[i]) return 2;
    }
    check(WaitForMultipleObjects(4, workers, TRUE, 30000) == WAIT_OBJECT_0, "parallel compiles finish");
    for (i = 0; i < 4; i++) {
        check(GetExitCodeThread(workers[i], &exit_code) && exit_code == 0, "concurrent compile succeeds");
        CloseHandle(workers[i]);
    }
    compile_expect(source, "parallel", one, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 1, "atomic concurrent write remains readable");

    /* Only our newly created flat temporary cache is removed. */
    swprintf_s(glob, NV_SHADER_CACHE_PATH, L"%s\\*", dir);
    file = FindFirstFileW(glob, &found);
    if (file != INVALID_HANDLE_VALUE) {
        do {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf_s(path, NV_SHADER_CACHE_PATH, L"%s\\%s", dir, found.cFileName);
            DeleteFileW(path);
        } while (FindNextFileW(file, &found));
        FindClose(file);
    }
    RemoveDirectoryW(dir);
    printf("%u failed assertions\n", failures);
    return failures ? 1 : 0;
}
