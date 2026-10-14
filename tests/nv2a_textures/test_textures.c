/* Drive the real cache and inspect its GPU pixels, including failed uploads. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail_srv;
static size_t fail_alloc_size;
static int fail_alloc;
static size_t pixel_allocations;

static HRESULT create_srv(ID3D11Device *device, ID3D11Resource *resource,
                          const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                          ID3D11ShaderResourceView **view)
{
    if (fail_srv) {
        fail_srv--;
        *view = NULL;
        return E_OUTOFMEMORY;
    }
    return device->lpVtbl->CreateShaderResourceView(device, resource, desc, view);
}

static void *allocate_pixels(size_t bytes)
{
    pixel_allocations++;
    if (fail_alloc && bytes == fail_alloc_size) {
        fail_alloc--;
        return NULL;
    }
    return malloc(bytes);
}

#undef ID3D11Device_CreateShaderResourceView
#define ID3D11Device_CreateShaderResourceView create_srv
#define malloc allocate_pixels
#ifndef NV2A_TEXTURE_RENDERER
#define NV2A_TEXTURE_RENDERER "../../src/video/nv2a_d3d11.c"
#endif
#include NV2A_TEXTURE_RENDERER
#include "../nv2a_test_host.h"
#undef malloc

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t read_mip(ID3D11ShaderResourceView *view, UINT mip)
{
    ID3D11Resource *resource = NULL;
    ID3D11Texture2D *source = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t pixel = 0xDEADBEEFu;
    if (!view) return pixel;
    ID3D11ShaderResourceView_GetResource(view, &resource);
    if (FAILED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D,
                                            (void **)&source))) {
        ID3D11Resource_Release(resource);
        return pixel;
    }
    ID3D11Resource_Release(resource);
    ID3D11Texture2D_GetDesc(source, &desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &desc, NULL, &staging))) {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging,
                                       (ID3D11Resource *)source);
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, mip,
                                             D3D11_MAP_READ, 0, &mapped))) {
            memcpy(&pixel, mapped.pData, sizeof pixel);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, mip);
        }
        ID3D11Texture2D_Release(staging);
    }
    ID3D11Texture2D_Release(source);
    return pixel;
}

static int test_srv_retry(void)
{
    NvD3DTexture texture = {0};
    ID3D11ShaderResourceView *view;
    size_t count = s_tex_count, bytes = s_tex_bytes;
    unsigned i;
    texture.addr = 0x20000;
    texture.color = 0x12;
    texture.format = 0x00011200;
    texture.width = texture.height = 4;
    texture.pitch = 16; texture.levels = 1;
    for (i = 0; i < 16; i++) ((uint32_t *)(ram + texture.addr))[i] = 0xFF00FF00u;
    fail_srv = 1;
    CHECK(texture_get(&texture) == NULL);
    CHECK(fail_srv == 0);
    CHECK(s_tex_count == count && s_tex_bytes == bytes);
    /* Retry in the same kick: the failed view must never become a cache hit. */
    view = texture_get(&texture);
    CHECK(view && read_mip(view, 0) == 0xFF00FF00u);
    CHECK(texture_get(&texture) == view);
    nv2a_d3d_kick();
    CHECK(texture_get(&texture) == view);
    CHECK(s_tex_count == count + 1);
    CHECK(s_tex_bytes == bytes + 16 * 4);
    puts("PASS: failed texture SRV creation retries immediately without a poisoned cache entry");
    return 0;
}

static int test_mip_retry(void)
{
    NvD3DTexture texture = {0};
    ID3D11ShaderResourceView *view;
    size_t count = s_tex_count, bytes = s_tex_bytes;
    unsigned i;
    texture.addr = 0x30000;
    texture.color = 0x06;
    texture.format = 0x03320600;
    texture.width = texture.height = 8; texture.levels = 2;
    for (i = 0; i < 64; i++) ((uint32_t *)(ram + texture.addr))[i] = 0xFFFF0000u;
    for (i = 64; i < 80; i++) ((uint32_t *)(ram + texture.addr))[i] = 0xFF0000FFu;
    /* Generated mips reuse the top-level scratch. An allocation failure must
     * still leave the complete previous GPU image visible and retryable. */
    free(s_up_pixels); s_up_pixels = NULL; s_up_cap = 0;
    fail_alloc_size = 8 * 8 * 4;
    fail_alloc = 1;
    CHECK(texture_get(&texture) == NULL);
    CHECK(fail_alloc == 0);
    CHECK(s_tex_count == count && s_tex_bytes == bytes);
    view = texture_get(&texture);
    CHECK(view);
    CHECK(read_mip(view, 0) == 0xFFFF0000u);
    for (i = 1; i < 4; i++) CHECK(read_mip(view, i) == 0xFF0000FFu);
    CHECK(s_tex_count == count + 1);
    CHECK(s_tex_bytes == bytes + (64 + 16 + 4 + 1) * 4);
    /* Also retry a failed update of an existing resource. */
    for (i = 64; i < 80; i++) ((uint32_t *)(ram + texture.addr))[i] = 0xFF00FF00u;
    nv2a_d3d_kick();
    free(s_up_pixels); s_up_pixels = NULL; s_up_cap = 0;
    fail_alloc = 1;
    CHECK(texture_get(&texture) == view);
    CHECK(fail_alloc == 0 && s_tex_count == count + 1);
    for (i = 1; i < 4; i++) CHECK(read_mip(view, i) == 0xFF0000FFu);
    CHECK(texture_get(&texture) == view);
    for (i = 1; i < 4; i++) CHECK(read_mip(view, i) == 0xFF00FF00u);
    puts("PASS: scratch allocation failure cannot publish incomplete mips and retries existing uploads");
    return 0;
}

static int test_palette_identity(void)
{
    NvD3DTexture texture = {0};
    ID3D11ShaderResourceView *short_view, *long_view;
    texture.addr = 0x40000;
    texture.color = 0x0B; texture.format = 0x02210B00;
    texture.width = texture.height = 4; texture.levels = 1;
    texture.palette = 0x41000; texture.palette_len = 32;
    memset(ram + texture.addr, 7, 16);
    ((uint32_t *)(ram + texture.palette))[7] = 0xFFFF0000u;
    short_view = texture_get(&texture);
    CHECK(short_view && read_mip(short_view, 0) == 0xFFFF0000u);
    /* PALETTE_LENGTH is a separate register from FORMAT and may change
     * between draws of a kick. Their validation/hash spans differ. */
    texture.palette_len = 256;
    long_view = texture_get(&texture);
    CHECK(long_view && long_view != short_view);
    CHECK(read_mip(long_view, 0) == 0xFFFF0000u);
    texture.palette_len = 32;
    CHECK(texture_get(&texture) == short_view);
    puts("PASS: independently changed palette length cannot bypass cache validation");
    return 0;
}

static uint32_t random_word(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

#include "native_bc_tests.h"
#include "mip_generation_tests.h"

static int test_decode_equivalence(void)
{
    uint32_t state = 0x12345678u;
    uint8_t *data = malloc(65536 + 64);
    uint32_t *allocation = malloc(65536 + 16), *decoded;
    const unsigned formats[] = { 0x06, 0x07, 0x0C, 0x0E, 0x0F };
    const unsigned dimensions[] = { 1, 2, 3, 4, 5, 7, 8, 12, 16, 20, 64, 128 };
    unsigned i, offset, format, width, height, w, h, x, y;
    CHECK(data);
    CHECK(allocation);
    decoded = allocation + 1;
    for (i = 0; i < 65536 + 64; i++) data[i] = (uint8_t)random_word(&state);
    /* Validate complete rectangles, unaligned sources, short mips and both
     * compressed alpha modes against independent per-texel addressing. */
    for (format = 0; format < 5; format++)
        for (width = 0; width < sizeof dimensions / sizeof dimensions[0]; width++)
            for (height = 0; height < sizeof dimensions / sizeof dimensions[0]; height++)
                for (offset = 0; offset < 8; offset++) {
                    NvD3DTexture texture = {0};
                    TexLayout layout;
                    w = dimensions[width]; h = dimensions[height];
                    if (format < 2 && ((w & (w - 1)) || (h & (h - 1)))) continue;
                    texture.color = formats[format];
                    texture.width = w; texture.height = h; texture.levels = 1;
                    CHECK(tex_layout(&texture, &layout));
                    decoded[-1] = decoded[w * h] = 0xDEADC0DEu;
                    decode_level(&texture, &layout, data + offset, 0, NULL, decoded);
                    CHECK(decoded[-1] == 0xDEADC0DEu && decoded[w * h] == 0xDEADC0DEu);
                    for (y = 0; y < h; y++)
                        for (x = 0; x < w; x++) {
                            uint32_t expected;
                            if (layout.kind == K_DXT)
                                CHECK(d3d8_dxt_decode_texel(data + offset, texture.color,
                                                           x, y, w, &expected));
                            else {
                                uint32_t address = swizzle_offset(x, y, w, h);
                                expected = texel(texture.color, data + offset + address * 4, NULL);
                            }
                            CHECK(decoded[y * w + x] == expected);
                        }
        }
    free(data);
    free(allocation);
    puts("PASS: tiled32-bit and DXT uploads match per-texel decoding across sizes, mips and alignments");
    return 0;
}

/* Original row-by-row implementation retained only as a benchmark reference. */
static void decode_before(const NvD3DTexture *t, const TexLayout *L, const uint8_t *src,
                          int lv, const uint32_t *pal, uint32_t *out)
{
    uint32_t w = L->level_w[lv], h = L->level_h[lv], x, y;
    if (L->kind == K_DXT) {
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bx, by, px[16];
        for (by = 0; by < bh; by++)
            for (bx = 0; bx < bw; bx++) {
                d3d8_dxt_decode_block(src + ((size_t)by * bw + bx) * L->bpp, t->color, px);
                for (y = 0; y < 4 && by * 4 + y < h; y++)
                    for (x = 0; x < 4 && bx * 4 + x < w; x++)
                        out[(size_t)(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
            }
    } else {
        uint32_t mx, my, oy = 0;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (y = 0; y < h; y++) {
            uint32_t ox = 0, *dst = out + (size_t)y * w;
            for (x = 0; x < w; x++) {
                dst[x] = texel(t->color, src + (size_t)(oy + ox) * L->bpp, pal);
                ox = (ox - mx) & mx;
            }
            oy = (oy - my) & my;
        }
    }
}

typedef void (*DecodeFunction)(const NvD3DTexture *, const TexLayout *, const uint8_t *,
                               int, const uint32_t *, uint32_t *);
static volatile uint32_t decode_sink;

static __declspec(noinline) double time_decode(DecodeFunction function,
    const NvD3DTexture *texture, const TexLayout *layout, uint8_t *data,
    uint32_t *out, unsigned repetitions)
{
    LARGE_INTEGER frequency;
    int64_t start, elapsed;
    unsigned i;
    QueryPerformanceFrequency(&frequency);
    start = qpc();
    for (i = 0; i < repetitions; i++) {
        data[0]++;
        function(texture, layout, data, 0, NULL, out);
        decode_sink ^= out[i & 255];
    }
    elapsed = qpc() - start;
    return elapsed / (double)frequency.QuadPart;
}

static int benchmark_decode(void)
{
    const unsigned formats[] = { 0x06, 0x07, 0x0C, 0x0E, 0x0F };
    uint8_t *data = malloc(4 * 1024 * 1024 + 64);
    uint32_t *out = malloc(4 * 1024 * 1024);
    uint32_t state = 0x1A2B3C4Du;
    unsigned i, sample, item, repetitions;
    double scalar[7], optimized[7];
    CHECK(data);
    CHECK(out);
    for (i = 0; i < 4 * 1024 * 1024 + 64; i++) data[i] = (uint8_t)random_word(&state);
    for (item = 0; item < 5; item++) {
        NvD3DTexture texture = {0};
        TexLayout layout;
        texture.color = formats[item];
        texture.width = texture.height = 1024; texture.levels = 1;
        CHECK(tex_layout(&texture, &layout));
        repetitions = 8;
        for (sample = 0; sample < 7; sample++) {
            scalar[sample] = time_decode(decode_before, &texture, &layout, data + 1, out, repetitions);
            optimized[sample] = time_decode(decode_level, &texture, &layout, data + 1, out, repetitions);
        }
        for (i = 0; i < 7; i++)
            for (sample = i + 1; sample < 7; sample++) {
                double t;
                if (scalar[sample] < scalar[i]) { t = scalar[i]; scalar[i] = scalar[sample]; scalar[sample] = t; }
                if (optimized[sample] < optimized[i]) { t = optimized[i]; optimized[i] = optimized[sample]; optimized[sample] = t; }
            }
        printf("DECODE format %02X 1024x1024: before %.3f ms, selected %.3f ms per upload (%.2fx)\n",
               formats[item], scalar[3] * 1000 / repetitions, optimized[3] * 1000 / repetitions,
               scalar[3] / optimized[3]);
    }
    free(data);
    free(out);
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    if (!strcmp(argv[1], "decode_equivalence")) return test_decode_equivalence();
    if (!strcmp(argv[1], "benchmark_decode")) return benchmark_decode();
    if (!strcmp(argv[1], "mip_inplace")) return test_mip_inplace();
    if (!strcmp(argv[1], "benchmark_mip_cpu")) return benchmark_mip_cpu();
    ram = calloc(1, (!strcmp(argv[1], "benchmark_native_bc") || !strcmp(argv[1], "benchmark_mip_upload")) ? 0x1000000 : 0x100000);
    CHECK(ram);
    _putenv_s("RECOMP_MSAA", "1");
    _putenv_s("RECOMP_RENDER_SCALE", "1");
    _putenv_s("RECOMP_PS_SPEC", "0");
    _putenv_s("RECOMP_FPS_LIMIT", "0");
    CHECK(nv2a_d3d_init());
    if (!strcmp(argv[1], "srv_retry")) return test_srv_retry();
    if (!strcmp(argv[1], "mip_retry")) return test_mip_retry();
    if (!strcmp(argv[1], "palette_identity")) return test_palette_identity();
    if (!strcmp(argv[1], "native_bc")) return test_native_bc();
    if (!strcmp(argv[1], "mip_chain")) return test_mip_chain();
    if (!strcmp(argv[1], "benchmark_mip_upload")) return benchmark_mip_upload();
    if (!strcmp(argv[1], "benchmark_native_bc")) return benchmark_native_bc();
    CHECK(0);
}
