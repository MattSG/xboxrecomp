/* Exercise production manifest, scheduler, workers and GPU-specialised pixels. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_ps_manifest.h"
static ID3D11Resource *watched_ps_buffer;
static unsigned ps_map_calls;
static int fail_ps_map;
static HRESULT test_map(ID3D11DeviceContext *ctx, ID3D11Resource *resource,
                        UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped);
#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map(ctx,resource,sub,type,flags,mapped) test_map(ctx,resource,sub,type,flags,mapped)
static int hooked_manifest_write(const wchar_t *path, const uint8_t family[32],
                                 const uint32_t records[][NV_PS_MANIFEST_WORDS], unsigned count);
#define nv_ps_manifest_write hooked_manifest_write
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
#undef nv_ps_manifest_write

static HRESULT test_map(ID3D11DeviceContext *ctx, ID3D11Resource *resource,
                        UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *mapped)
{
    if (resource == watched_ps_buffer && type == D3D11_MAP_WRITE_DISCARD) {
        ps_map_calls++;
        if (fail_ps_map) { fail_ps_map = 0; return E_OUTOFMEMORY; }
    }
    return ctx->lpVtbl->Map(ctx, resource, sub, type, flags, mapped);
}

static HANDLE write_entered, write_release;
static volatile LONG write_calls;
static int hooked_manifest_write(const wchar_t *path, const uint8_t family[32],
                                 const uint32_t records[][NV_PS_MANIFEST_WORDS], unsigned count)
{
    InterlockedIncrement(&write_calls);
    if (write_entered) {
        SetEvent(write_entered);
        WaitForSingleObject(write_release, 10000);
    }
    return nv_ps_manifest_write(path, family, records, count);
}

ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static wchar_t dir[NV_SHADER_CACHE_PATH], path[NV_SHADER_CACHE_PATH];
static uint8_t family[32];
static uint32_t records[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS];

static int setup(void)
{
    wchar_t temp[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, temp));
    /* PIDs are reused, and a prior run may leave its cache directory behind.
     * Reserve a fresh path rather than reusing that run's cache or failing. */
    CHECK(GetTempFileNameW(temp, L"psp", 0, dir));
    CHECK(DeleteFileW(dir));
    CHECK(CreateDirectoryW(dir, NULL));
    CHECK(SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", dir));
    _putenv_s("RECOMP_SHADER_CACHE", "1");
    _putenv_s("RECOMP_PS_SPEC", "1");
    _putenv_s("RECOMP_PS_PREWARM", "1");
    _putenv_s("RECOMP_MSAA", "1");
    _putenv_s("RECOMP_RENDER_SCALE", "1");
    spec_family(family);
    CHECK(nv_ps_manifest_path(family, path));
    return 0;
}

static int overwrite(unsigned offset, const void *data, DWORD bytes, int truncate)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    DWORD wrote;
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(SetFilePointer(file, offset, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER);
    CHECK(WriteFile(file, data, bytes, &wrote, NULL) && wrote == bytes);
    if (truncate) CHECK(SetEndOfFile(file));
    CloseHandle(file);
    return 0;
}

static DWORD WINAPI manifest_writer(void *arg)
{
    unsigned i, count = 64 + (unsigned)(uintptr_t)arg;
    DWORD successes = 0;
    for (i = 0; i < 8; i++) if (nv_ps_manifest_write(path, family, records, count)) successes++;
    return successes;
}

static int test_manifest(void)
{
    uint32_t readback[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS];
    uint8_t alternate[32], bytes[4];
    unsigned i, j, n = 99;
    for (i = 0; i < NV_PS_MANIFEST_LIMIT; i++) for (j = 0; j < NV_PS_MANIFEST_WORDS; j++)
        records[i][j] = i * 12345u + j * 6789u;
    CHECK(nv_ps_manifest_write(path, family, records, NV_PS_MANIFEST_LIMIT));
    CHECK(nv_ps_manifest_read(path, family, readback, &n));
    CHECK(n == NV_PS_MANIFEST_LIMIT && !memcmp(records, readback, sizeof records));
    CHECK(!nv_ps_manifest_write(path, family, records, NV_PS_MANIFEST_LIMIT + 1));
    memcpy(alternate, family, 32); alternate[3] ^= 1;
    CHECK(!nv_ps_manifest_read(path, alternate, readback, &n) && n == 0);
    /* Each bad header/payload must reject all records. */
    for (i = 0; i < 7; i++) {
        CHECK(nv_ps_manifest_write(path, family, records, 2));
        nv_ps_put32(bytes, i == 2 ? NV_PS_MANIFEST_LIMIT + 1 : 0xA5A5A5A5u);
        CHECK(!overwrite((unsigned[]){0, 8, 16, 12, 20, 24, 56}[i], bytes, 4, 0));
        CHECK(!nv_ps_manifest_read(path, family, readback, &n) && n == 0);
    }
    CHECK(nv_ps_manifest_write(path, family, records, 2));
    CHECK(!overwrite(NV_PS_MANIFEST_HEADER + 17, bytes, 1, 0));
    CHECK(!nv_ps_manifest_read(path, family, readback, &n) && n == 0);
    CHECK(nv_ps_manifest_write(path, family, records, 2));
    CHECK(!overwrite(NV_PS_MANIFEST_HEADER + 2 * sizeof(PsKey) - 1, bytes, 0, 1));
    CHECK(!nv_ps_manifest_read(path, family, readback, &n) && n == 0);
    CHECK(nv_ps_manifest_write(path, family, records, 2));
    CHECK(!overwrite(NV_PS_MANIFEST_HEADER + 2 * sizeof(PsKey), bytes, 1, 0));
    CHECK(!nv_ps_manifest_read(path, family, readback, &n) && n == 0);
    CHECK(nv_ps_manifest_write(path, family, records, 0));
    CHECK(nv_ps_manifest_read(path, family, readback, &n) && n == 0);
    {
        HANDLE writers[4];
        DWORD code, successes = 0;
        for (i = 0; i < 4; i++) {
            writers[i] = CreateThread(NULL, 0, manifest_writer, (void *)(uintptr_t)i, 0, NULL);
            CHECK(writers[i]);
        }
        do {
            CHECK(nv_ps_manifest_read(path, family, readback, &n));
            CHECK((n == 0 || (n >= 64 && n <= 67)) && !memcmp(readback, records, n * sizeof(PsKey)));
        } while (WaitForMultipleObjects(4, writers, TRUE, 0) == WAIT_TIMEOUT);
        for (i = 0; i < 4; i++) {
            CHECK(GetExitCodeThread(writers[i], &code) && code <= 8);
            successes += code;
            CloseHandle(writers[i]);
        }
        CHECK(successes > 0); /* Competing atomic replacements may fail safely. */
    }
    _putenv_s("RECOMP_SHADER_CACHE", "0");
    CHECK(!nv_ps_manifest_path(family, path));
    _putenv_s("RECOMP_SHADER_CACHE", "1");
    CHECK(nv_ps_manifest_path(family, path));
    CHECK(!wcsncmp(path, dir, wcslen(dir)));
    puts("PASS: canonical bounded manifest, schema/family/checksum/length corruption, cache bypass and directory override");
    return 0;
}

static PsKey number_key(unsigned number)
{
    PsKey key = {0}; key.fin0 = number; return key;
}

static void fixture_init(void)
{
    s_spec = calloc(PS_SPEC_SLOTS, sizeof *s_spec);
    InitializeCriticalSection(&s_spec_cs);
    InitializeConditionVariable(&s_spec_cv);
    s_spec_head = s_spec_tail = s_spec_warm_head = s_spec_warm_tail = 0;
}

static volatile LONG counts[PS_SPEC_SLOTS], claimed;
static DWORD WINAPI claim_worker(void *unused)
{
    PsSpec *e;
    (void)unused;
    for (;;) {
        EnterCriticalSection(&s_spec_cs);
        e = spec_claim();
        LeaveCriticalSection(&s_spec_cs);
        if (!e) break;
        InterlockedIncrement(&counts[e - s_spec]);
        Sleep(1); /* Ensure live lookups overlap claimed entries/publication. */
        e->ps = (ID3D11PixelShader *)e;
        InterlockedExchange(&e->state, 2);
        InterlockedIncrement(&claimed);
    }
    return 0;
}

static int test_scheduler(void)
{
    PsKey key;
    PsSpec *e;
    HANDLE threads[2];
    unsigned i;
    fixture_init(); CHECK(s_spec);
    for (i = 0; i < 8; i++) { key = number_key(i); CHECK(!spec_request(&key, 1)); }
    key = number_key(7); CHECK(!spec_request(&key, 0));
    key = number_key(900); CHECK(!spec_request(&key, 0));
    EnterCriticalSection(&s_spec_cs);
    e = spec_claim(); CHECK(e && e->key.fin0 == 7 && e->state == 4);
    e = spec_claim(); CHECK(e && e->key.fin0 == 900);
    for (i = 0; i < 7; i++) { e = spec_claim(); CHECK(e && e->key.fin0 == i); }
    CHECK(!spec_claim()); /* Promoted low-priority duplicate did not compile. */
    LeaveCriticalSection(&s_spec_cs);
    DeleteCriticalSection(&s_spec_cs); free(s_spec);
    fixture_init(); CHECK(s_spec);
    for (i = 0; i < NV_PS_MANIFEST_LIMIT; i++) {
        key = number_key(i); spec_request(&key, 1); spec_request(&key, 1);
        CHECK(s_spec_warm_head == i + 1); spec_request(&key, 0);
    }
    for (i = 0; i < PS_SPEC_QUEUE - NV_PS_MANIFEST_LIMIT; i++) {
        key = number_key(1000 + i); spec_request(&key, 0);
    }
    CHECK(s_spec_head - s_spec_tail == PS_SPEC_QUEUE);
    key = number_key(3000); CHECK(!spec_request(&key, 0));
    CHECK(spec_find(&key)->state == 0); /* Saturation leaves a retryable empty slot. */
    for (i = 0; i < 2; i++) { threads[i] = CreateThread(NULL, 0, claim_worker, NULL, 0, NULL); CHECK(threads[i]); }
    for (;;) {
        unsigned queued;
        EnterCriticalSection(&s_spec_cs); queued = s_spec_head - s_spec_tail; LeaveCriticalSection(&s_spec_cs);
        if (queued < PS_SPEC_QUEUE) break;
        Sleep(0);
    }
    CHECK(!spec_request(&key, 0)); /* Retry inserts while both workers are active. */
    CHECK(spec_find(&key)->state != 0);
    for (i = 0; i < 20000; i++) {
        key = number_key(i % NV_PS_MANIFEST_LIMIT);
        e = spec_find(&key);
        CHECK(spec_request(&key, 0) == (InterlockedCompareExchange(&e->state, 0, 0) == 2 ? e->ps : NULL) || e->state == 2);
    }
    CHECK(WaitForMultipleObjects(2, threads, TRUE, 10000) == WAIT_OBJECT_0);
    CHECK(claimed == PS_SPEC_QUEUE + 1);
    for (i = 0; i < PS_SPEC_SLOTS; i++) CHECK(counts[i] <= 1);
    key = number_key(3001); CHECK(!spec_request(&key, 0));
    CHECK(spec_find(&key)->state == 1);
    for (i = 0; i < 2; i++) CloseHandle(threads[i]);
    puts("PASS: live priority, promotion, deduplication, bounded queue retry, two-worker claim/publication versus live lookup");
    return 0;
}

static DWORD WINAPI flush_worker(void *unused)
{
    (void)unused;
    spec_manifest_flush();
    return 0;
}

static int test_persistence(void)
{
    PsKey key = number_key(1);
    HANDLE worker;
    unsigned n;
    fixture_init(); CHECK(s_spec);
    s_spec_manifest = 1;
    memcpy(s_spec_family, family, sizeof family);
    wcscpy_s(s_spec_manifest_path, NV_SHADER_CACHE_PATH, path);
    spec_remember(&key);
    write_entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    write_release = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(write_entered && write_release);
    worker = CreateThread(NULL, 0, flush_worker, NULL, 0, NULL); CHECK(worker);
    CHECK(WaitForSingleObject(write_entered, 10000) == WAIT_OBJECT_0);
    /* These acquire the same CS while the first writer is blocked outside it. */
    key = number_key(2); spec_remember(&key);
    spec_manifest_flush(); CHECK(write_calls == 1);
    SetEvent(write_release);
    CHECK(WaitForSingleObject(worker, 10000) == WAIT_OBJECT_0);
    CHECK(s_spec_dirty != s_spec_saved && !s_spec_saving);
    CHECK(nv_ps_manifest_read(path, family, records, &n) && n == 1 && records[0][32] == 1);
    CloseHandle(worker); CloseHandle(write_entered); CloseHandle(write_release);
    write_entered = write_release = NULL;
    spec_manifest_flush(); CHECK(write_calls == 1); /* One-second throttle. */
    s_spec_last_save = 0; /* Make the next interval due without delaying CI. */
    spec_manifest_flush(); CHECK(write_calls == 2 && s_spec_dirty == s_spec_saved);
    CHECK(nv_ps_manifest_read(path, family, records, &n) && n == 2 && records[1][32] == 2);
    puts("PASS: blocked file I/O leaves queue lock free, concurrent snapshots serialize, new dirty generation survives, save throttle");
    return 0;
}

static void fixture_key(PSConsts *pc, PsKey *key, unsigned state, unsigned variant)
{
    memset(pc, 0, sizeof *pc); memset(key, 0, sizeof *key);
    pc->alpha_func = key->alpha_func = 0x204;
    if (state) {
        pc->ps_flags = key->ps_flags = 3;
        pc->rc_control = key->rc_control = 1;
        pc->c_icw[0] = key->c_icw[0] = 0x01040000;
        pc->a_icw[0] = key->a_icw[0] = 0x11140000;
        pc->c_ocw[0] = key->c_ocw[0] = 0xC00;
        pc->a_ocw[0] = key->a_ocw[0] = 0xC00;
        pc->fin0 = key->fin0 = 0x0C;
        pc->fin1 = key->fin1 = 0x1C00;
    }
    pc->zp1[0] = 1.0f / 16777216.0f; pc->zp1[1] = 16777215.0f;
    key->variant = variant;
}

static int write_fixture(unsigned duplicate, unsigned invalid)
{
    PSConsts pc; PsKey key;
    unsigned i;
    for (i = 0; i < 8; i++) {
        fixture_key(&pc, &key, i / 4, i % 4);
        memcpy(records[i], &key, sizeof key);
    }
    if (duplicate) memcpy(records[8], records[0], sizeof(PsKey));
    if (invalid) records[3][41] = 4;
    CHECK(nv_ps_manifest_write(path, family, records, 8 + duplicate));
    return 0;
}

static int test_gate(const char *which)
{
    CHECK(!write_fixture(1, !strcmp(which, "invalid_variant")));
    if (!strcmp(which, "disabled")) _putenv_s("RECOMP_PS_SPEC", "0");
    if (!strcmp(which, "cache_off")) _putenv_s("RECOMP_SHADER_CACHE", "0");
    if (!strcmp(which, "prewarm_off")) _putenv_s("RECOMP_PS_PREWARM", "0");
    CHECK(nv2a_d3d_init());
    if (!strcmp(which, "disabled")) CHECK(!s_spec_enabled && !s_spec);
    else {
        PSConsts pc; PsKey key;
        CHECK(s_spec_enabled && s_spec_warm_head == 0 && s_spec_known_count == 0);
        CHECK(!strcmp(which, "invalid_variant") || !s_spec_manifest);
        fixture_key(&pc, &key, 0, 0);
        CHECK(spec_ps(&pc, 0) == NULL); /* First lookup still uses generic PS. */
        CHECK(spec_find(&key)->state != 0); /* Lazy request survives all bypasses. */
        {
            ULONGLONG deadline = GetTickCount64() + 10000;
            while (!spec_ps(&pc, 0)) { CHECK(GetTickCount64() < deadline); Sleep(1); }
        }
    }
    printf("PASS: %s bypass/rejection keeps generic/lazy shader fallback\n", which);
    return 0;
}

static int render_pixels(ID3D11PixelShader *ps, ID3D11VertexShader *vs, const PSConsts *pc,
                         ID3D11RenderTargetView *rtv, ID3D11Texture2D *target,
                         ID3D11Texture2D *staging, uint32_t pixels[256])
{
    D3D11_VIEWPORT viewport = {0, 0, 16, 16, 0, 1};
    D3D11_RECT rect = {0, 0, 16, 16};
    D3D11_MAPPED_SUBRESOURCE mapped;
    float clear[4] = {0.125f, 0.25f, 0.5f, 1};
    unsigned y;
    upload_ps_constants(pc);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, rtv, clear);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, s_blend_opaque, NULL, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, s_ds_off, 0);
    ID3D11DeviceContext_RSSetState(s_ctx, s_rs_plain);
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &viewport);
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &rect);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_cb_ps);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)target);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
    for (y = 0; y < 16; y++) memcpy(pixels + y * 16, (const uint8_t *)mapped.pData + y * mapped.RowPitch, 64);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
    return 0;
}

static int test_gpu(int cache_failure)
{
    static const char vs_source[] =
        "#ifndef FLATQ\n#define FLATQ centroid\n#endif\n"
        "struct O{float4 p:SV_Position;FLATQ float4 d0:COLOR0;FLATQ float4 d1:COLOR1;"
        "centroid float4 f:FOG;centroid float4 t0:TEXCOORD0;centroid float4 t1:TEXCOORD1;"
        "centroid float4 t2:TEXCOORD2;centroid float4 t3:TEXCOORD3;};"
        "O main(uint id:SV_VertexID){O o=(O)0;float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);"
        "o.p=float4(p,0,1);o.d0=float4(.25,.5,.75,1);o.f=float4(1,8388608,0,0);return o;}";
    static const D3D_SHADER_MACRO flat[] = {{"FLATQ", "nointerpolation"}, {NULL, NULL}};
    ID3D11Texture2D *target = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11VertexShader *vs[2] = {0};
    D3D11_TEXTURE2D_DESC td = {0};
    uint32_t generic[256], warm[256];
    uint32_t dynamic[4][4];
    PSConsts pc; PsKey key;
    ID3DBlob *blob = NULL, *error = NULL;
    unsigned i, j;
    ULONGLONG deadline;
    if (cache_failure) {
        HANDLE file;
        wchar_t blocked[NV_SHADER_CACHE_PATH];
        CHECK(swprintf_s(blocked, NV_SHADER_CACHE_PATH, L"%s\\blocked", dir) > 0);
        file = CreateFileW(blocked, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        CHECK(file != INVALID_HANDLE_VALUE); CloseHandle(file);
        CHECK(SetEnvironmentVariableW(L"RECOMP_SHADER_CACHE_DIR", blocked));
    } else CHECK(!write_fixture(1, 0));
    CHECK(nv2a_d3d_init());
    CHECK(s_spec_enabled);
    if (!cache_failure) CHECK(s_spec_known_count == 8 && s_spec_warm_head == 8);
    /* Render-thread lookup races existing workers exactly as normal draws do. */
    deadline = GetTickCount64() + 30000;
    for (i = 0; i < 8; i++) {
        fixture_key(&pc, &key, i / 4, i % 4);
        while (!spec_ps(&pc, i % 4)) { CHECK(GetTickCount64() < deadline); Sleep(1); }
    }
    CHECK(s_spec_ready == 8); /* Duplicated manifest state compiled only once. */
    for (i = 0; i < 2; i++) {
        CHECK(SUCCEEDED(D3DCompile(vs_source, sizeof vs_source - 1, "test-vs", i ? flat : NULL,
                                  NULL, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)));
        CHECK(SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(blob),
                                    ID3D10Blob_GetBufferSize(blob), NULL, &vs[i])));
        ID3D10Blob_Release(blob); blob = NULL;
        if (error) { ID3D10Blob_Release(error); error = NULL; }
    }
    td.Width = td.Height = 16; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &target)));
    CHECK(SUCCEEDED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)target, NULL, &rtv)));
    td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &staging)));
    for (i = 0; i < 8; i++) for (j = 0; j < 4; j++) {
        fixture_key(&pc, &key, i / 4, i % 4);
        pc.cf0[0][0] = 0.25f * (j + 1); pc.cf0[0][1] = 0.5f;
        pc.cf0[0][2] = 0.75f; pc.cf0[0][3] = j & 1 ? 0.25f : 1.0f;
        pc.alpha_ref = j & 2 ? 128 : 0;
        CHECK(!render_pixels(s_psv[i % 4], vs[i & 1], &pc, rtv, target, staging, generic));
        CHECK(!render_pixels(spec_ps(&pc, i % 4), vs[i & 1], &pc, rtv, target, staging, warm));
        CHECK(!memcmp(generic, warm, sizeof generic));
        {
            static const uint32_t combiner_expected[] = {0xFF10408Fu, 0x4020408Fu, 0xFF30408Fu, 0xFF204080u};
            uint32_t expected = i < 4 ? 0xFF4080BFu : combiner_expected[j];
            unsigned channel;
            /* Raster interpolation/UNORM conversion can round a half unit
             * down: centroid diffuse 0.5 produced 127, flat produced 128.
             * The generic-versus-specialised image check above stays exact. */
            for (channel = 0; channel < 4; channel++) {
                int difference = (int)((warm[0] >> (channel * 8)) & 255) -
                                 (int)((expected >> (channel * 8)) & 255);
                if (difference < -1 || difference > 1)
                    fprintf(stderr, "GPU pixel=%08X expected=%08X state=%u variant=%u sample=%u\n",
                            warm[0], expected, i / 4, i % 4, j);
                CHECK(difference >= -1 && difference <= 1);
            }
            if (i >= 4) dynamic[i % 4][j] = warm[0];
        }
    }
    for (i = 0; i < 4; i++) for (j = 1; j < 4; j++)
        CHECK(dynamic[i][j] != dynamic[i][j - 1]); /* Factors and alpha reference remain dynamic. */
    if (!cache_failure) {
        unsigned n;
        /* A new live key must be remembered and saved by idle workers. */
        fixture_key(&pc, &key, 0, 0); pc.dot_map = key.dot_map = 1;
        while (!spec_ps(&pc, 0)) { CHECK(GetTickCount64() < deadline); Sleep(1); }
        for (;;) {
            if (nv_ps_manifest_read(path, family, records, &n) && n == 9) break;
            CHECK(GetTickCount64() < deadline); Sleep(10);
        }
        CHECK(s_spec_known_count == 9);
    }
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(target); ID3D11Texture2D_Release(staging);
    for (i = 0; i < 2; i++) ID3D11VertexShader_Release(vs[i]);
    printf("PASS: 32 exact GPU comparisons, all four variants, dynamic factors/alpha_ref, %s\n",
           cache_failure ? "unwritable-cache lazy fallback" : "import deduplication/concurrent live lookup/idle persistence");
    return 0;
}

static int verify_constants(ID3D11Buffer *stage, const PSConsts *pc)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)s_cb_ps);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &mapped)));
    CHECK(!memcmp(mapped.pData, pc, sizeof *pc));
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
    return 0;
}

static int double_compare(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static int test_constants(int benchmark)
{
    D3D_FEATURE_LEVEL level;
    D3D11_BUFFER_DESC desc = {0};
    ID3D11Buffer *stage;
    PSConsts pc = {0}, previous;
    unsigned i;
    CHECK(SUCCEEDED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                     D3D11_SDK_VERSION, &s_dev, &level, &s_ctx)));
    s_cb_ps = make_buffer(sizeof pc, D3D11_BIND_CONSTANT_BUFFER);
    CHECK(s_cb_ps);
    watched_ps_buffer = (ID3D11Resource *)s_cb_ps;
    desc.ByteWidth = sizeof pc; desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &desc, NULL, &stage)));
    fail_ps_map = 1;
    upload_ps_constants(&pc);
    CHECK(ps_map_calls == 1 && !s_uploaded_pc_valid);
    upload_ps_constants(&pc);
    CHECK(ps_map_calls == 2 && s_uploaded_pc_valid);
    CHECK(!verify_constants(stage, &pc));
    for (i = 0; i < 1000; i++) upload_ps_constants(&pc);
    CHECK(ps_map_calls == 2);
    /* Every byte matters: dynamic values, signed zero, NaN payloads, and
     * texture/depth scales cannot be suppressed by an incomplete key. */
    for (i = 0; i < sizeof pc; i++) {
        ((unsigned char *)&pc)[i] ^= (unsigned char)(i % 255 + 1);
        upload_ps_constants(&pc);
        CHECK(ps_map_calls == 3 + i);
        upload_ps_constants(&pc);
        CHECK(ps_map_calls == 3 + i);
        CHECK(!verify_constants(stage, &pc));
    }
    previous = pc;
    pc.alpha_ref ^= 1; fail_ps_map = 1;
    upload_ps_constants(&pc);
    CHECK(!memcmp(&s_uploaded_pc, &previous, sizeof previous));
    CHECK(!verify_constants(stage, &previous));
    upload_ps_constants(&pc);
    CHECK(!verify_constants(stage, &pc));
    CHECK(!memcmp(&s_uploaded_pc, &pc, sizeof pc));
    printf("PASS: pixel constants initial/retry/identical/all %zu bytes exact on GPU\n", sizeof pc);
    if (benchmark) {
        double baseline[7], reused[7];
        LARGE_INTEGER frequency;
        unsigned round, pass, iteration;
        QueryPerformanceFrequency(&frequency);
        for (round = 0; round < 7; round++) for (pass = 0; pass < 2; pass++) {
            int reuse = (round + pass) & 1;
            int64_t started = qpc();
            for (iteration = 0; iteration < 20000; iteration++) {
                if (reuse) upload_ps_constants(&pc);
                else upload_cb(s_cb_ps, &pc, sizeof pc);
            }
            (reuse ? reused : baseline)[round] = (qpc() - started) * 1000.0 / frequency.QuadPart;
            CHECK(!verify_constants(stage, &pc));
        }
        printf("CONSTANTS_AB {\"iterations_per_pass\":20000,\"bytes\":%zu,\"passes\":[", sizeof pc);
        for (round = 0; round < 7; round++)
            printf("%s{\"baseline_ms\":%.6f,\"reused_ms\":%.6f}", round ? "," : "", baseline[round], reused[round]);
        qsort(baseline, 7, sizeof(double), double_compare);
        qsort(reused, 7, sizeof(double), double_compare);
        printf("],\"baseline_median_ms\":%.6f,\"reused_median_ms\":%.6f}\n", baseline[3], reused[3]);
    }
    ID3D11Buffer_Release(stage);
    ID3D11Buffer_Release(s_cb_ps); ID3D11DeviceContext_Release(s_ctx); ID3D11Device_Release(s_dev);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2); CHECK(!setup());
    if (!strcmp(argv[1], "manifest")) return test_manifest();
    if (!strcmp(argv[1], "scheduler")) return test_scheduler();
    if (!strcmp(argv[1], "persistence")) return test_persistence();
    if (!strcmp(argv[1], "gpu")) return test_gpu(0);
    if (!strcmp(argv[1], "gpu_cache_failure")) return test_gpu(1);
    if (!strcmp(argv[1], "constants")) return test_constants(0);
    if (!strcmp(argv[1], "constants_bench")) return test_constants(1);
    return test_gate(argv[1]);
}
