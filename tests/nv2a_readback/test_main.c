/* Real staging copies/pixels with controlled asynchronous API failures. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static int inspect, hold_query, fail_query, fail_create_query, fail_map, fail_rtv;
static unsigned maps, queries, creates, flushes, bad_flags, clock_calls;

static HRESULT watched_map(ID3D11DeviceContext *ctx, ID3D11Resource *resource,
                           UINT sub, D3D11_MAP type, UINT flags, D3D11_MAPPED_SUBRESOURCE *out)
{
    if (inspect && type == D3D11_MAP_READ) {
        maps++;
        if (flags != D3D11_MAP_FLAG_DO_NOT_WAIT) bad_flags++;
        if (fail_map) { fail_map--; return DXGI_ERROR_WAS_STILL_DRAWING; }
    }
    return ctx->lpVtbl->Map(ctx, resource, sub, type, flags, out);
}

static HRESULT watched_query(ID3D11DeviceContext *ctx, ID3D11Asynchronous *query,
                             void *data, UINT size, UINT flags)
{
    if (inspect) {
        queries++;
        if (flags != D3D11_ASYNC_GETDATA_DONOTFLUSH) bad_flags++;
        if (hold_query) { if (data && size == sizeof(BOOL)) *(BOOL *)data = FALSE; return S_FALSE; }
        if (fail_query) { fail_query--; return E_FAIL; }
    }
    return ctx->lpVtbl->GetData(ctx, query, data, size, flags);
}

static HRESULT watched_create_query(ID3D11Device *dev, const D3D11_QUERY_DESC *desc, ID3D11Query **out)
{
    if (inspect) {
        creates++;
        if (fail_create_query) { fail_create_query--; *out = NULL; return E_OUTOFMEMORY; }
    }
    return dev->lpVtbl->CreateQuery(dev, desc, out);
}

static HRESULT watched_rtv(ID3D11Device *dev, ID3D11Resource *resource,
                           const D3D11_RENDER_TARGET_VIEW_DESC *desc, ID3D11RenderTargetView **out)
{
    if (inspect && fail_rtv) { fail_rtv--; *out = NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateRenderTargetView(dev, resource, desc, out);
}

static void watched_flush(ID3D11DeviceContext *ctx)
{
    if (inspect) flushes++;
    ctx->lpVtbl->Flush(ctx);
}

static BOOL WINAPI watched_clock(LARGE_INTEGER *time)
{
    if (inspect) clock_calls++;
    return QueryPerformanceCounter(time);
}

#undef ID3D11DeviceContext_Map
#define ID3D11DeviceContext_Map watched_map
#undef ID3D11DeviceContext_GetData
#define ID3D11DeviceContext_GetData watched_query
#undef ID3D11Device_CreateQuery
#define ID3D11Device_CreateQuery watched_create_query
#undef ID3D11Device_CreateRenderTargetView
#define ID3D11Device_CreateRenderTargetView watched_rtv
#undef ID3D11DeviceContext_Flush
#define ID3D11DeviceContext_Flush watched_flush
#define QueryPerformanceCounter watched_clock
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
#undef QueryPerformanceCounter

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) {}
int xbox_GetDisplayMode(uint32_t *pitch, uint32_t *bpp, uint32_t *w, uint32_t *h) { (void)pitch; (void)bpp; (void)w; (void)h; return 0; }
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void reset_counts(void)
{
    maps = queries = creates = flushes = bad_flags = clock_calls = 0;
}

static Surface *new_surface(unsigned i, unsigned bpp, unsigned w, unsigned h, unsigned padding)
{
    uint32_t va = 0x10000 + i * 0x4000, pitch = w * bpp + padding;
    memset(ram + va, 0x55, (size_t)pitch * h);
    return surface_get(va, pitch, bpp == 2 ? 3 : 8, w, h, 0);
}

static void draw_color(Surface *s, int green)
{
    float color[4] = {green ? 0.0f : 1.0f, green ? 1.0f : 0.0f, 0, 1};
    ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s->rtv, color);
    s->ms_dirty = 1; s->gpu_dirty = 1;
}

static int complete(void)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    while (nv2a_d3d_writeback_busy()) { CHECK(GetTickCount64() < deadline); Sleep(0); }
    CHECK(!s_wb_mask && !s_wb_pending && !s_wb_event_pending);
    return 0;
}

static int pixels_match(Surface *s, int green, int later_row)
{
    unsigned x, y;
    for (y = 0; y < s->h; y++) {
        const uint8_t *row = ram + s->va + (size_t)y * s->pitch;
        for (x = 0; x < s->w; x++) {
            if (s->bpp == 4) {
                uint32_t value, expected = later_row && y == 0 ? 0xFF0000FFu : green ? 0xFF00FF00u : 0xFFFF0000u;
                memcpy(&value, row + x * 4, 4); CHECK(value == expected);
            } else {
                uint16_t value, expected = later_row && y == 0 ? 0x001Fu : green ? 0x07E0u : 0xF800u;
                memcpy(&value, row + x * 2, 2); CHECK(value == expected);
            }
        }
        for (x = s->w * s->bpp; x < s->pitch; x++) CHECK(row[x] == 0x55);
    }
    return 0;
}

static void late_blue_row(Surface *s)
{
    unsigned x;
    uint8_t *row = ram + s->va;
    for (x = 0; x < s->w; x++) {
        if (s->bpp == 4) { uint32_t blue = 0xFF0000FFu; memcpy(row + x * 4, &blue, 4); }
        else { uint16_t blue = 0x001Fu; memcpy(row + x * 2, &blue, 2); }
    }
}

static int test_batch(void)
{
    Surface *surfaces[8];
    unsigned i;
    for (i = 0; i < 8; i++) {
        surfaces[i] = new_surface(i, i & 1 ? 2 : 4, i & 2 ? 32 : 16, i & 4 ? 16 : 8, 16);
        CHECK(surfaces[i]); draw_color(surfaces[i], i & 1);
    }
    reset_counts(); inspect = 1;
    writeback_surfaces(); CHECK(s_wb_event && s_wb_event_pending && flushes == 1);
    CHECK(s_wb_mask == 0xFF);
    hold_query = 1;
    for (i = 0; i < 64; i++) CHECK(nv2a_d3d_writeback_busy());
    CHECK(queries == 64 && maps == 0 && flushes == 1 && !bad_flags);
    for (i = 0; i < 8; i++) late_blue_row(surfaces[i]);
    hold_query = 0; CHECK(!complete());
    CHECK(maps == 8 && !bad_flags && flushes == 1);
    for (i = 0; i < 8; i++) CHECK(!pixels_match(surfaces[i], i & 1, 1));
    puts("PASS: eight real GPU copies, one query per unfinished poll and zero Maps, exact16/32-bit pixels/padding/late-row merge");
    return 0;
}

static int test_query_create_retry(void)
{
    Surface *s = new_surface(0, 4, 16, 16, 16); CHECK(s);
    draw_color(s, 0); reset_counts(); inspect = 1; fail_create_query = 1;
    writeback_surfaces(); CHECK(creates == 1 && !s_wb_event && !s_wb_event_pending);
    CHECK(!complete()); CHECK(maps && !queries && flushes == 1 && !bad_flags);
    CHECK(!pixels_match(s, 0, 0));
    draw_color(s, 1); reset_counts();
    writeback_surfaces(); CHECK(creates == 1 && s_wb_event_pending);
    CHECK(!complete()); CHECK(queries && maps == 1 && flushes == 1);
    CHECK(!pixels_match(s, 1, 0));
    puts("PASS: failed event allocation uses real Map completion, then retries query allocation on the next batch");
    return 0;
}

static int test_query_failure(void)
{
    Surface *s = new_surface(0, 4, 32, 16, 0); CHECK(s);
    draw_color(s, 0); inspect = 1; reset_counts(); writeback_surfaces();
    fail_query = 1; CHECK(!complete());
    CHECK(queries == 1 && maps && flushes == 1 && !bad_flags);
    CHECK(!pixels_match(s, 0, 0));
    draw_color(s, 1); reset_counts(); writeback_surfaces();
    CHECK(!complete()); CHECK(queries && maps == 1); CHECK(!pixels_match(s, 1, 0));
    puts("PASS: event GetData failure falls back to real Map and leaves future event batches usable");
    return 0;
}

static int test_map_retry(void)
{
    Surface *s = new_surface(0, 4, 16, 16, 16); CHECK(s);
    draw_color(s, 0); inspect = 1; reset_counts(); writeback_surfaces();
    /* Wait for the actual event, then force the staging Map to remain busy. */
    {
        BOOL done = FALSE;
        ULONGLONG deadline = GetTickCount64() + 5000;
        while (s_ctx->lpVtbl->GetData(s_ctx, (ID3D11Asynchronous *)s_wb_event, &done, sizeof done,
                                     D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) {
            CHECK(GetTickCount64() < deadline); Sleep(0);
        }
    }
    fail_map = 1;
    CHECK(nv2a_d3d_writeback_busy());
    CHECK(s_wb_pending && s_wb_mask == 1 && s_wb[0].pending && !s_wb_event_pending);
    late_blue_row(s);
    CHECK(!complete()); CHECK(maps == 2 && !bad_flags && flushes == 1);
    CHECK(!pixels_match(s, 0, 1));
    puts("PASS: event readiness cannot fabricate Map completion; busy Map retries and preserves later CPU pixels");
    return 0;
}

static int test_drop_resize(void)
{
    Surface *s[2], *grown;
    unsigned i, j;
    for (i = 0; i < 2; i++) { s[i] = new_surface(i, 4, 16, 16, 0); CHECK(s[i]); draw_color(s[i], 0); }
    inspect = 1; reset_counts(); writeback_surfaces(); hold_query = 1;
    CHECK(nv2a_d3d_writeback_busy());
    surface_release(s[0]); CHECK(s_wb_mask == 2 && s_wb_pending);
    CHECK(nv2a_d3d_writeback_busy());
    surface_release(s[1]); CHECK(!s_wb_mask && !s_wb_pending && !s_wb_event_pending);
    CHECK(!nv2a_d3d_writeback_busy()); CHECK(!maps);
    hold_query = 0;
    s[0] = new_surface(0, 4, 16, 16, 0); CHECK(s[0]); draw_color(s[0], 0);
    writeback_surfaces(); CHECK(s_wb_pending);
    /* Same address/pitch grows height: release cancels the old slot's pixels. */
    for (j = 0; j < 16 * 32; j++) ((uint32_t *)(ram + s[0]->va))[j] = 0xFF0000FFu;
    grown = surface_get(s[0]->va, 64, 8, 16, 32, 0); CHECK(grown == s[0]);
    CHECK(!nv2a_d3d_writeback_busy());
    for (j = 0; j < 16 * 32; j++) CHECK(((uint32_t *)(ram + grown->va))[j] == 0xFF0000FFu);
    draw_color(grown, 1); writeback_surfaces();
    CHECK(s_wb[grown - s_surf].h == 32 && s_wb_pending);
    CHECK(!complete()); CHECK(!pixels_match(grown, 1, 0));
    puts("PASS: dropping all copied resources cancels query wait; resized address never inherits old pixels and reuses a resized staging slot");
    return 0;
}

static int test_down_rtv_retry(void)
{
    Surface *s;
    ID3D11Texture2D *retained;
    s_scale = 2;
    s = new_surface(0, 4, 16, 16, 0); CHECK(s && s->iw == 32 && s->ih == 32);
    draw_color(s, 0); inspect = 1; reset_counts(); fail_rtv = 1;
    writeback_surfaces();
    CHECK(!fail_rtv && s->gpu_dirty && !s_wb_pending && !flushes);
    CHECK(s_wb[0].down && !s_wb[0].down_rtv);
    retained = s_wb[0].down;
    writeback_surfaces();
    CHECK(!s->gpu_dirty && s_wb_pending && s_wb[0].down == retained && s_wb[0].down_rtv);
    CHECK(!complete()); CHECK(flushes == 1 && !pixels_match(s, 0, 0));
    puts("PASS: failed downsample view retains dirty pixels and texture; next batch retries view and reads exact GPU pixels");
    return 0;
}

static int test_trace(int enabled)
{
    Surface *s = new_surface(0, 4, 16, 16, 0);
    FILE *log = tmpfile();
    int saved;
    char text[4096] = {0}, *marker;
    unsigned long long polls;
    double gap;
    CHECK(s && log);
    saved = _dup(_fileno(stderr)); CHECK(saved >= 0);
    CHECK(_dup2(_fileno(log), _fileno(stderr)) == 0);
    draw_color(s, 0); inspect = 1; reset_counts(); writeback_surfaces();
    hold_query = 1; CHECK(nv2a_d3d_writeback_busy());
    Sleep(12); CHECK(nv2a_d3d_writeback_busy());
    hold_query = 0; CHECK(!complete());
    fflush(stderr); CHECK(_dup2(saved, _fileno(stderr)) == 0); _close(saved);
    rewind(log); fread(text, 1, sizeof text - 1, log); fclose(log);
    marker = strstr(text, "phase=readback_pending");
    if (enabled) {
        CHECK(marker && strstr(marker, "poll_count=") && strstr(marker, "max_poll_gap_ms="));
        CHECK(sscanf(strstr(marker, "poll_count="), "poll_count=%llu max_poll_gap_ms=%lf", &polls, &gap) == 2);
        CHECK(polls >= 3 && gap >= 8 && clock_calls);
    } else CHECK(!marker && !clock_calls && !s_wb_hitch_polls && !s_wb_hitch_pending);
    printf("PASS: trace_%s reports actual poll count/gap only when enabled; trace-off performs zero QPC calls\n", enabled ? "on" : "off");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc >= 2);
    ram = calloc(1, 0x100000); CHECK(ram);
    _putenv_s("RECOMP_MSAA", argc > 2 ? argv[2] : "1");
    _putenv_s("RECOMP_RENDER_SCALE", "1");
    _putenv_s("RECOMP_D3D_PROFILE", "");
    _putenv_s("RECOMP_PS_SPEC", "0");
    _putenv_s("RECOMP_FPS_LIMIT", "0");
    _putenv_s("RECOMP_HITCH_TRACE", !strcmp(argv[1], "trace_on") ? "1" : "0");
    CHECK(nv2a_d3d_init());
    if (!strcmp(argv[1], "batch")) return test_batch();
    if (!strcmp(argv[1], "query_create_retry")) return test_query_create_retry();
    if (!strcmp(argv[1], "query_failure")) return test_query_failure();
    if (!strcmp(argv[1], "map_retry")) return test_map_retry();
    if (!strcmp(argv[1], "drop_resize")) return test_drop_resize();
    if (!strcmp(argv[1], "down_rtv_retry")) return test_down_rtv_retry();
    if (!strcmp(argv[1], "trace_off") || !strcmp(argv[1], "trace_on")) return test_trace(!strcmp(argv[1], "trace_on"));
    CHECK(0);
}

/* These renderer-only fixtures never request executor frame replay. */
void nv2a_pb_interp_enable(int on) { (void)on; abort(); }
long nv2a_pb_frames_done(void) { abort(); return 0; }
int nv2a_pb_replay(float t) { (void)t; abort(); return 0; }
